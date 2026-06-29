#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#define DEBUG(fd, color, type, format, ...)             \
    do {                                                \
        fprintf(fd, "%s[%s]\e[0m ", color, type);       \
        fprintf(fd, format __VA_OPT__(, ) __VA_ARGS__); \
    } while (0)

#define LOG(format, ...) DEBUG(stderr, "\e[0;34m", "LOG", format, __VA_ARGS__)
#define WARN(format, ...) DEBUG(stderr, "\e[0;33m", "WARNING", format, __VA_ARGS__)
#define ERROR(format, ...) DEBUG(stderr, "\e[0;31m", "ERROR", format, __VA_ARGS__)

int skip(int fd, uint32_t len) {
    char *emp = malloc(len);
    ssize_t nbread = 0;
    while (len - nbread > 0) {
        ssize_t nr = read(fd, emp, len);
        if (nr < 0) {
            perror("read");
            return -1;
        }
        nbread += nr;
    }
    return nbread;
}

/*
 * Wayland wire format:
 *   uint32_t  object_id   (1 = wl_display)
 *   uint16_t  opcode      (0 = wl_display.sync)
 *   uint16_t  size        (total message size in bytes)
 *   uint32_t  new_id      (id for the wl_callback object, e.g. 2)
 */

static ssize_t read_all(int fd, void *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, ((char *)buf) + total, len - total, 0);
        if (n <= 0) return n == 0 ? (ssize_t)total : n;
        total += n;
    }
    return (ssize_t)total;
}

static char *read_wl_string(int fd)
{
    uint32_t len;
    read_all(fd, &len, 4);
    char *buf = calloc(1, len + 4);   /* +4 so padding never overflows */
    read_all(fd, buf, len);
    uint32_t pad = (4 - (len % 4)) % 4;
    if (pad) {
        uint8_t sink[3];
        read_all(fd, sink, pad);
    }
    return buf;   /* caller must free() */
}

enum object_type_e {
  IFACE_UNKNOWN,
  IFACE_WL_DISPLAY,
  IFACE_WL_REGISTRY,
  IFACE_WL_CALLBACK,
  IFACE_WL_COMPOSITOR,
  IFACE_WL_SURFACE,
  IFACE_WL_SHM,
};

typedef enum object_type_e object_type_t;

typedef struct {
    object_type_t type;
    int id;
} object_t;

int last_used_id = 1;

uint32_t nb_object = 1;
object_t *objects = &((object_t){.type = IFACE_WL_DISPLAY, .id = 1});

uint32_t new_object(object_type_t type) {
    if (nb_object == 1) {
        objects = malloc(sizeof(object_t) * (nb_object+1));
        objects[0].type = IFACE_WL_DISPLAY;
        objects[0].id = 1;
        LOG("%d : %d\n", IFACE_WL_DISPLAY, 1);
    } else {
        objects = realloc(objects, sizeof(object_t) * (nb_object+1));
    }
    nb_object++;
    last_used_id++;
    objects[nb_object-1].type = type;
    objects[nb_object - 1].id = last_used_id;
    LOG("%d : %d\n", type, last_used_id);
    return last_used_id;
}

object_type_t get_object_type(int id) {
    for (int i = 0; i < nb_object; i++) {
        if (objects[i].id == id) return objects[i].type;
    }
    return IFACE_UNKNOWN;
}


void call(int fd, int obj_id, int function_id, uint32_t param_len, char *param) {
    uint32_t m[2] = {obj_id, ((8 + param_len) << 16) | function_id};
    write(fd, m, sizeof(m));
    LOG("CALL : obj_id : %d, function_id : %d\n", obj_id, function_id);
    LOG("param_len = %d\n", param_len);
    if (param_len != 0) {
        write(fd, param, param_len);
        LOG("WR : param : %d\n", *((uint32_t*)param));
    }
}

/* wl_display opcodes (requests) */
#define WL_DISPLAY_SYNC         0
#define WL_DISPLAY_GET_REGISTRY 1

/* wl_display opcodes (events) */
#define WL_DISPLAY_EV_ERROR     0
#define WL_DISPLAY_EV_DELETE_ID 1

/* wl_regisrty opcodes (requests) */
#define WL_REGISTRY_BIND 0

/* wl_registry opcodes (events) */
#define WL_REGISTRY_EV_GLOBAL        0
#define WL_REGISTRY_EV_GLOBAL_REMOVE 1


/* Every Wayland message starts with an 8-byte header */
typedef struct {
    uint32_t obj_id;
    uint32_t opcode;
    uint32_t msg_len;  /* total, including header */
    uint32_t body;     /* bytes still to read     */
} header_t;


typedef struct {
    void (*fnc)(int fd, uint32_t orig_id, uint32_t serial);
    uint32_t orig_id;
    uint32_t id;
} callback_t;
uint32_t nb_callbacks = 0;
callback_t *callbacks = NULL;
void new_callback(int fd, uint32_t orig_id, void (*fnc)(int, uint32_t, uint32_t)) {
    uint32_t id = new_object(IFACE_WL_CALLBACK);
    if (callbacks == NULL) {
        callbacks = malloc(sizeof(callback_t));
        nb_callbacks = 1;
    } else {
        nb_callbacks++;
        callbacks = realloc(callbacks, sizeof(callback_t)*nb_callbacks);
    }
    callbacks[nb_callbacks-1].fnc = fnc;
    callbacks[nb_callbacks - 1].id = id;
    callbacks[nb_callbacks - 1].orig_id = orig_id;
    call(fd, 1, WL_DISPLAY_SYNC, sizeof(uint32_t), (char*)&id);
}

void call_wl_registry_bind(int fd, int obj_id, uint32_t name, uint32_t interface_len, char *interface, uint32_t version, uint32_t new_id) {
    uint32_t m[2] = {
        obj_id,
        ((8+sizeof(name)+sizeof(interface_len)+interface_len+4-interface_len%4+sizeof(version)+sizeof(new_id)) << 16) | WL_REGISTRY_BIND
    };
    write(fd, m, sizeof(m));
    write(fd, &name, sizeof(name));
    write(fd, &interface_len, sizeof(interface_len));
    write(fd, interface, interface_len);
    if (interface_len % 4 != 0) {
        uint32_t zero = 0;
        write(fd, &zero, 4 - interface_len % 4);
        LOG("write %d zero\n", 4 - interface_len % 4);
    }
    write(fd, &version, sizeof(version));
    write(fd, &new_id, sizeof(new_id));

    LOG("bind : %d, %d, %d, %d, %s, %d, %d\n", obj_id, WL_REGISTRY_BIND, name, interface_len, interface, version, new_id);
}

header_t read_header(int fd) {
    uint32_t hdr[2];
    uint8_t r = 0;
    if (read_all(fd, hdr, 8) < 8) {
        return (header_t) {0};
    }
    uint32_t msg_len = hdr[1] >> 16;
    return (header_t) {
        .obj_id = hdr[0],
        .opcode = hdr[1] & 0xFFFF,
        .msg_len = msg_len,
        .body = msg_len - 8,
    };
}

void compositor_callback(int fd, uint32_t orig_id, uint32_t serial) {
    LOG("Compositor bind (serial=%u)\n", serial);
    uint32_t surface_id = new_object(IFACE_WL_SURFACE);
    // wl_compositor::create_surface
    call(fd, orig_id, 0, sizeof(uint32_t), (char *)&surface_id);
    // wl_surface::attach
    /* uint32_t attach_len; */
    /* char *attach_param; */
    /* call(fd, surface_id, 1, attach_len, attach_param); */
    LOG("Compositor bind END (serial=%u)\n", serial);
}

void wl_shm_callback(int fd, uint32_t orig_id, uint32_t serial) {
    LOG("Shm bind (serial=%u)\n", serial);
    LOG("Shm bind END (serial=%u)\n", serial);
}

uint8_t wl_display(int fd, int devnull, header_t hdr) {
    int32_t id;
    uint32_t code;
    char *msg;
    switch (hdr.opcode) {
    case WL_DISPLAY_EV_ERROR:
        /* int32 object_id, uint32 code, string message */
        read_all(fd, &id, 4);
        read_all(fd, &code, 4);
        msg = read_wl_string(fd);
        ERROR("wl_display.error: obj=%d code=%u msg=%s\n", id, code, msg);
        free(msg);
        return 1;
    case WL_DISPLAY_EV_DELETE_ID:
        read_all(fd, &id, 4);
        LOG("wl_display.delete_id: %u\n", id);
        return 1;
    default:
        /* unknown display event – skip body */
        while (hdr.body) {
            /* hdr.body -= splice(fd, NULL, devnull, NULL, hdr.body, 0); */
            int sk = skip(fd, hdr.body);
            if (sk < 0) {
                perror("skip");
                return 0;
            }
            hdr.body -= sk;
        }
        return 0;
    }
}

uint8_t wl_registry(int fd, int devnull, header_t hdr) {
    uint32_t name;
    char *interface;
    uint32_t version;
    switch (hdr.opcode) {
    case WL_REGISTRY_EV_GLOBAL:
        /* uint32 name, string interface, uint32 version */
        read_all(fd, &name, 4);
        interface = read_wl_string(fd);
        read_all(fd, &version, 4);
        LOG("global: name=%-3u  version=%-2u  %s\n", name, version, interface);
        if (strcmp(interface, "wl_compositor") == 0) {
            uint32_t compositor_id = new_object(IFACE_WL_COMPOSITOR);
            LOG("New wl_compositor id : %d\n", compositor_id);
            call_wl_registry_bind(fd, hdr.obj_id, name, 14, interface, version,
                                  compositor_id);
            new_callback(fd, compositor_id, compositor_callback);
        } else if (strcmp(interface, "wl_shm") == 0) {
            uint32_t shm_id = new_object(IFACE_WL_SHM);
            LOG("New wl_shm id : %d\n", shm_id);
            call_wl_registry_bind(fd, hdr.obj_id, name, 7, interface, version,
                                  shm_id);
            new_callback(fd, 0, wl_shm_callback);
        }
        free(interface);
        return 1;
    case WL_REGISTRY_EV_GLOBAL_REMOVE:
        read_all(fd, &name, 4);
        LOG("global_remove: name=%u\n", name);
        return 1;
    default:
        /* unknown registry event – skip body */
        while (hdr.body) {
            /* hdr.body -= splice(fd, NULL, devnull, NULL, hdr.body, 0); */
            int sk = skip(fd, hdr.body);
            if (sk < 0) {
                perror("skip");
                return 0;
            }
            hdr.body -= sk;
        }
        return 0;
    }
}

void callback_get_registry(int fd, uint32_t orig_id, uint32_t serial) {
    LOG("Sync done (serial=%u) - all globals received.\n", serial);
}

int main(void)
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg) {
      ERROR("No XDG_RUNTIME_DIR in env\n");
      return 1;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/wayland-1", xdg);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    LOG("Connected to %s\n\n", path);

    int devnull = open("/dev/null", O_WRONLY);

    {
        uint32_t id = new_object(IFACE_WL_REGISTRY);
        call(fd, 1, WL_DISPLAY_GET_REGISTRY, sizeof(uint32_t), (char*)&id);
    }
    
    new_callback(fd, 1, callback_get_registry);

    uint8_t fini = 0;
    
    while (!fini) {
        header_t hdr = read_header(fd);
        if (hdr.obj_id == 0) {
            continue;
        }
        
        uint32_t serial;
        uint8_t ok = 0;

        // LOG("Request id(%d), opcode(%d), len(%d)\n", hdr.obj_id, hdr.opcode, hdr.msg_len);
        
        switch (get_object_type(hdr.obj_id)) {
        case IFACE_WL_DISPLAY:
            ok = wl_display(fd, devnull, hdr);
            break;
        case IFACE_WL_REGISTRY:
            ok = wl_registry(fd, devnull, hdr);
            break;
        case IFACE_WL_CALLBACK:
            if (hdr.opcode == 0) {
                read_all(fd, &serial, 4);
                printf("\n");
                ok = 1;
                for (int i = 0; i < nb_callbacks; i++) {
                    if (callbacks[i].id == hdr.obj_id) {
                        callbacks[i].fnc(fd, callbacks[i].orig_id, serial);
                    }
                }
                /* fini = 1; */
            }
            break;
        default:
            WARN("Unknown object id (%d)\n", hdr.obj_id);
            break;
        }
        if (ok)
          continue;

        /* anything else: skip */
        WARN("Unknown event : object id = %d, opcode = %d\n", hdr.obj_id, hdr.opcode);
        while (hdr.body) {
            /* ssize_t splice_len = splice(fd, NULL, devnull, NULL, hdr.body, 0); */
            /* if (splice_len < 0) { */
            /*     perror("splice"); */
            /* } else { */
            /*     LOG("splice : %ld\n", splice_len); */
            /*     hdr.body -= splice_len; */
            /* } */
            int sk = skip(fd, hdr.body);
            if (sk < 0) {
              perror("skip");
              fini = 1;
            }
            hdr.body -= sk;
        }
    }
    close(devnull);
    close(fd);
    return 0;
}
