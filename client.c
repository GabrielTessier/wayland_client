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
#include <sys/mman.h>
#include <sys/stat.h>

#define DEBUG_PRINT(fd, color, type, format, ...)             \
    do {                                                \
        fprintf(fd, "%s[%s]\e[0m ", color, type);       \
        fprintf(fd, format __VA_OPT__(, ) __VA_ARGS__); \
    } while (0)

#define DEBUG(format, ...) DEBUG_PRINT(stderr, "\e[0;34m", "DEBUG", format, __VA_ARGS__)
#define LOG(format, ...) DEBUG_PRINT(stderr, "\e[0;34m", "LOG", format, __VA_ARGS__)
#define WARN(format, ...) DEBUG_PRINT(stderr, "\e[0;33m", "WARNING", format, __VA_ARGS__)
#define ERROR(format, ...) DEBUG_PRINT(stderr, "\e[0;31m", "ERROR", format, __VA_ARGS__)

int skip(int fd, uint32_t len) {
    char buf[1024];
    while (len > 0) {
        ssize_t n = read(fd, buf, len < sizeof(buf) ? len : sizeof(buf));
        if (n < 0) {
            perror("read");
            return -1;
        }
        len -= n;
    }
    return 0;
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

#define OBJECT_TYPE_LIST_X                      \
    X(IFACE_UNKNOWN)                            \
    X(IFACE_WL_DISPLAY)                         \
    X(IFACE_WL_REGISTRY)                        \
    X(IFACE_WL_CALLBACK)                        \
    X(IFACE_WL_COMPOSITOR)                      \
    X(IFACE_WL_SURFACE)                         \
    X(IFACE_WL_SHM)                             \
    X(IFACE_WL_SHM_POOL)                        \
    X(IFACE_WL_BUFFER)                          \
    X(IFACE_XDG_WM_BASE)                        \
    X(IFACE_XDG_SURFACE)                        \
    X(IFACE_XDG_TOPLEVEL)

enum object_type_e {
#define X(name) name,
  OBJECT_TYPE_LIST_X
  #undef X
};
typedef enum object_type_e object_type_t;

char *object_type_to_string(object_type_t type) {
#define X(name) case (name): return #name;
  switch (type) {OBJECT_TYPE_LIST_X}
#undef X
}

typedef struct {
    object_type_t type;
    uint32_t id;
} object_t;

int last_used_id = 1;

uint32_t nb_object = 1;
object_t *objects = &((object_t){.type = IFACE_WL_DISPLAY, .id = 1});

uint32_t width = 0, height = 0;

uint32_t new_object(object_type_t type) {
    if (nb_object == 1) {
        objects = malloc(sizeof(object_t) * (nb_object+1));
        objects[0].type = IFACE_WL_DISPLAY;
        objects[0].id = 1;
        DEBUG("%s : %d\n", object_type_to_string(IFACE_WL_DISPLAY), 1);
    } else {
        objects = realloc(objects, sizeof(object_t) * (nb_object+1));
    }
    nb_object++;
    last_used_id++;
    objects[nb_object-1].type = type;
    objects[nb_object - 1].id = last_used_id;
    DEBUG("%s : %d\n", object_type_to_string(type), last_used_id);
    return last_used_id;
}

object_type_t get_object_type(uint32_t id) {
    for (int i = 0; i < nb_object; i++) {
        if (objects[i].id == id) return objects[i].type;
    }
    return IFACE_UNKNOWN;
}

uint32_t get_object_id(object_type_t type) {
    for (int i = 0; i < nb_object; i++) {
        if (objects[i].type == type) return objects[i].id;
    }
    return 0;
}


void call(int fd, int obj_id, int function_id, uint32_t param_len, char *param) {
    uint32_t m[2] = {obj_id, ((8 + param_len) << 16) | function_id};
    write(fd, m, sizeof(m));
    object_type_t type = get_object_type(obj_id);
    LOG("CALL : obj_id : %s(%d), function_id : %d\n", object_type_to_string(type), obj_id, function_id);
    DEBUG("param_len = %d\n", param_len);
    if (param_len != 0) {
        write(fd, param, param_len);
        DEBUG("WR : param : %d\n", *((uint32_t*)param));
    }
}

void call_string(int fd, uint32_t obj_id, int function_id, uint32_t string_len, char *str) {
    uint32_t padding = (4 - string_len % 4) % 4;
    uint32_t total = 8 + sizeof(uint32_t) + string_len + padding;
    uint8_t buf[total];
    memset(buf, 0, total);
    uint8_t *p = buf;
    uint32_t length_code = (total << 16) | function_id;
    memcpy(p, &obj_id, 4);       p += 4;
    memcpy(p, &length_code, 4);  p += 4;
    memcpy(p, &string_len, 4);   p += 4;
    memcpy(p, str, string_len);  p += string_len;
    write(fd, buf, total);
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


struct callback_s {
    void (*fnc)(int fd, struct callback_s callback_elem, uint32_t serial);
    uint32_t orig_id;
    uint32_t id;
    void *extra_data;
    uint32_t extra_data_size;
};
typedef struct callback_s callback_t;
uint32_t nb_callbacks = 0;
callback_t *callbacks = NULL;
void new_callback(int fd, uint32_t orig_id, void (*fnc)(int, callback_t, uint32_t), void* extra_data, uint32_t extra_data_size) {
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
    callbacks[nb_callbacks - 1].extra_data = extra_data;
    callbacks[nb_callbacks - 1].extra_data_size = extra_data_size;
    call(fd, 1, WL_DISPLAY_SYNC, sizeof(uint32_t), (char*)&id);
}

void call_wl_registry_bind(int fd, int obj_id, uint32_t name,
                           uint32_t interface_len, char *interface,
                           uint32_t version, uint32_t new_id) {
    uint32_t padding = (4 - interface_len % 4) % 4;
    uint32_t total_length = 8 + sizeof(name) + sizeof(interface_len) +
                            interface_len + padding + sizeof(version) +
                            sizeof(new_id);
    uint32_t m[2] = {
        obj_id, (total_length<<16) | WL_REGISTRY_BIND};
    uint8_t buf[total_length];
    memset(buf, 0, total_length);
    uint8_t *p = buf;

    uint32_t length_code = (total_length << 16) | WL_REGISTRY_BIND;
    memcpy(p, &obj_id, 4);                p += 4;
    memcpy(p, &length_code, 4);           p += 4;
    memcpy(p, &name, 4);                  p += 4;
    memcpy(p, &interface_len, 4);         p += 4;
    memcpy(p, interface, interface_len);  p += interface_len;
    p += padding;
    memcpy(p, &version, 4);               p += 4;
    memcpy(p, &new_id, 4);                p += 4;

    write(fd, buf, total_length);

    object_type_t type = get_object_type(obj_id);
    LOG("bind : %s(%d), %d, %d, %d, %s, %d, %d\n", object_type_to_string(type), obj_id, WL_REGISTRY_BIND, name, interface_len, interface, version, new_id);
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

uint32_t compositor_create_surface(int fd, uint32_t compositor_id) {
    DEBUG("Compositor create surface\n");
    uint32_t surface_id = new_object(IFACE_WL_SURFACE);
    // wl_compositor::create_surface
    call(fd, compositor_id, 0, sizeof(uint32_t), (char *)&surface_id);
    DEBUG("Compositor create surface END\n");
    return surface_id;
}

void xdg_configure(int fd, uint32_t surface_id) {
    DEBUG("XDG_wm_base\n");
    uint32_t xdg_wm_base_id = get_object_id(IFACE_XDG_WM_BASE);
    uint32_t xdg_surface_id = new_object(IFACE_XDG_SURFACE);
    struct {
        uint32_t new_id, surface_id;
    } data = {
        .new_id = xdg_surface_id,
        .surface_id = surface_id,
    };
    // xdg_wm_base::get_xdg_surface
    call(fd, xdg_wm_base_id, 2, sizeof(data), (char *)&data);

    uint32_t xdg_toplevel_id = new_object(IFACE_XDG_TOPLEVEL);
    DEBUG("toplevel id : %d\n", xdg_toplevel_id);
    // xdg_surface::get_toplevel
    call(fd, xdg_surface_id, 1, sizeof(uint32_t), (char *)&xdg_toplevel_id);

    // xdg_toplevel::set_title
    call_string(fd, xdg_toplevel_id, 2, 7, "coucou");
    // xdg_toplevel::set_app_id
    call_string(fd, xdg_toplevel_id, 3, 7, "app.id");
    DEBUG("XDG_wm_base END\n");
}

void wl_buffer_callback(int fd, callback_t callback, uint32_t serial) {
    DEBUG("Buffer callback (serial=%u)\n", serial);
    uint32_t surface_id = get_object_id(IFACE_WL_SURFACE);
    DEBUG("XDG_wm_base\n");
    
    uint32_t xdg_wm_base_id = get_object_id(IFACE_XDG_WM_BASE);
    uint32_t xdg_surface_id = get_object_id(IFACE_XDG_SURFACE);

    uint32_t configure_serial = 0;
    if (callback.extra_data_size == sizeof(uint32_t)) {
        configure_serial = *((uint32_t *)callback.extra_data);
    } else {
        ERROR("wl_buffer_callback need a uint32_t in extra_data");
    }

    // xdg_surface::ack_configure
    call(fd, xdg_surface_id, 4, sizeof(uint32_t), (char *)&configure_serial);

    struct {
        uint32_t buffer_id;
        int32_t x, y;
    } data_attach = { .buffer_id = callback.orig_id, .x = 0, .y = 0 };
    // wl_surface::attach
    call(fd, surface_id, 1, sizeof(data_attach), (char *)&data_attach);

    struct {
        int32_t x, y, width, height;
    } data_damage_buffer = { .x = 0, .y = 0, .width = width, .height = height };
    // wl_surface::damage_buffer
    call(fd, surface_id, 9, sizeof(data_damage_buffer), (char *)&data_damage_buffer);

    // wl_surface::commit
    call(fd, surface_id, 6, 0, NULL);
    
    DEBUG("XDG_wm_base END\n");
    DEBUG("Buffer callback END\n");
}

uint8_t wl_display(int fd, header_t hdr) {
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
        object_type_t type = get_object_type(id);
        LOG("wl_display.delete_id: %u, type : %s\n", id, object_type_to_string(type));
        return 1;
    default:
        /* unknown display event – skip body */
        if (skip(fd, hdr.body) < 0) {
            perror("skip");
        }
        return 0;
    }
}

uint8_t wl_registry(int fd, header_t hdr) {
    uint32_t name;
    char *interface;
    uint32_t version;
    switch (hdr.opcode) {
    case WL_REGISTRY_EV_GLOBAL:
        /* uint32 name, string interface, uint32 version */
        read_all(fd, &name, 4);
        interface = read_wl_string(fd);
        read_all(fd, &version, 4);
        DEBUG("global: name=%-3u  version=%-2u  %s\n", name, version, interface);
        if (strcmp(interface, "wl_compositor") == 0) {
            uint32_t compositor_id = new_object(IFACE_WL_COMPOSITOR);
            DEBUG("New wl_compositor id : %d\n", compositor_id);
            call_wl_registry_bind(fd, hdr.obj_id, name, 14, interface, version,
                                  compositor_id);
            /* new_callback(fd, compositor_id, compositor_callback); */
        } else if (strcmp(interface, "wl_shm") == 0) {
            uint32_t shm_id = new_object(IFACE_WL_SHM);
            DEBUG("New wl_shm id : %d\n", shm_id);
            call_wl_registry_bind(fd, hdr.obj_id, name, 7, interface, version,
                                  shm_id);
        } else if (strcmp(interface, "xdg_wm_base") == 0) {
            uint32_t xdg_wm_base_id = new_object(IFACE_XDG_WM_BASE);
            DEBUG("New xdg_wm_base_id : %d\n", xdg_wm_base_id);
            call_wl_registry_bind(fd, hdr.obj_id, name, 12, interface, version,
                                  xdg_wm_base_id);
        }
        free(interface);
        return 1;
    case WL_REGISTRY_EV_GLOBAL_REMOVE:
        read_all(fd, &name, 4);
        DEBUG("global_remove: name=%u\n", name);
        return 1;
    default:
        if (skip(fd, hdr.body) < 0) {
            perror("skip");
        }
        return 0;
    }
}

void wl_shm_config(int fd, uint32_t wl_shm_id, uint32_t configure_serial) {
    DEBUG("Shm config\n");
    shm_unlink("client_shm");
    int shm_fd = shm_open("client_shm", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (shm_fd < 0) {
        perror("shm_open");
        return;
    }

    uint32_t format = 1;  // 32-bit RGB format, [31:0] x:R:G:B 8:8:8:8 little endian
    uint32_t stride = width * 4;   // 40 bytes per row
    uint32_t shm_size = stride * height;
    DEBUG("shm info : width : %d, height : %d, size : %d", width, height, shm_size);
    
    if (ftruncate(shm_fd, shm_size) < 0) {
        perror("ftruncate");
        return;
    }


    // Create a memory-mapped region for the file.
    void* shm_mem = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_mem == MAP_FAILED) {
        perror("mmap");
    } else {
        if (width < 1000) {
            memset(shm_mem, 255, shm_size);
        } else {
            memset(shm_mem, 0, shm_size);
        }
        munmap(shm_mem, shm_size);
    }
    
    
    // wl_surface::create_pool
    struct msghdr msg = { 0 };
    struct cmsghdr *cmsg;

    uint32_t pool_id = new_object(IFACE_WL_SHM_POOL);
    uint32_t m[4] = {wl_shm_id, ((8 + 8) << 16) | 0, pool_id,
                     shm_size};
    struct iovec io = {.iov_base = m, .iov_len = sizeof(m)};
    union {         /* Ancillary data buffer, wrapped in a union
                       in order to ensure it is suitably aligned */
        char buf[CMSG_SPACE(sizeof(shm_fd))];
        struct cmsghdr align;
    } u;
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(shm_fd));
    memcpy(CMSG_DATA(cmsg), &shm_fd, sizeof(shm_fd));
    DEBUG("Sending create_pool: obj=%u opcode=0 size=16 new_id=%u shm_size=%u\n",
        wl_shm_id, pool_id, shm_size);
    DEBUG("  raw: %08x %08x %08x %08x\n", m[0], m[1], m[2], m[3]);
    ssize_t sent = sendmsg(fd, &msg, 0);
    if (sent < 0) {
      perror("sendmsg");
      close(shm_fd);
      return;
    }
    DEBUG("sendmsg sent %zd bytes\n", sent);

    uint32_t buffer_id = new_object(IFACE_WL_BUFFER);
    
    struct __attribute__((packed)) {
        uint32_t new_id;
        int32_t offset, width, height, stride;
        uint32_t format;
    } data = {.new_id = buffer_id,
              .offset = 0,
              .width = width,
              .height = height,
              .stride = stride,
              .format = format};
    // wl_shm_pool::create_buffer
    call(fd, pool_id, 0, sizeof(data), (char *)&data);

    uint32_t *extra_callback = (uint32_t *)malloc(sizeof(configure_serial));
    *extra_callback = configure_serial;
    new_callback(fd, buffer_id, wl_buffer_callback, extra_callback, sizeof(configure_serial));
    
    DEBUG("Shm config END\n");
}

void callback_get_registry(int fd, callback_t callback, uint32_t serial) {
    DEBUG("Sync done (serial=%u) - all globals received.\n", serial);
    uint32_t compositor_id = get_object_id(IFACE_WL_COMPOSITOR);
    uint32_t surface_id = compositor_create_surface(fd, compositor_id);
    xdg_configure(fd, surface_id);
    // wl_surface::commit
    call(fd, surface_id, 6, 0, NULL);
}

static uint8_t fini = 0;

int main(void) {
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

    {
        uint32_t id = new_object(IFACE_WL_REGISTRY);
        call(fd, 1, WL_DISPLAY_GET_REGISTRY, sizeof(uint32_t), (char*)&id);
    }
    
    new_callback(fd, 1, callback_get_registry, NULL, 0);
    
    while (!fini) {
        header_t hdr = read_header(fd);
        if (hdr.obj_id == 0) {
            continue;
        }
        
        uint32_t serial;
        uint8_t ok = 0;

        // LOG("Request id(%d), opcode(%d), len(%d)\n", hdr.obj_id, hdr.opcode, hdr.msg_len);
        enum object_type_e type = get_object_type(hdr.obj_id);
        LOG("%s event\n", object_type_to_string(type));
        switch (type) {
        case IFACE_WL_DISPLAY:
            ok = wl_display(fd, hdr);
            break;
        case IFACE_WL_REGISTRY:
            ok = wl_registry(fd, hdr);
            break;
        case IFACE_WL_CALLBACK:
            if (hdr.opcode == 0) {
                read_all(fd, &serial, 4);
                printf("\n");
                DEBUG("callback event serial = %u\n", serial);
                ok = 1;
                for (int i = 0; i < nb_callbacks; i++) {
                    if (callbacks[i].id == hdr.obj_id) {
                        callbacks[i].fnc(fd, callbacks[i], serial);
                        if ((callbacks[i].extra_data_size == 0 &&
                             callbacks[i].extra_data != NULL) ||
                            (callbacks[i].extra_data_size != 0 &&
                             callbacks[i].extra_data == NULL)) {
                            object_type_t type = get_object_type(callbacks[i].orig_id);
                            ERROR("callback with wrong extra parameter (from "
                                  "%s(%d))",
                                  object_type_to_string(type),
                                  callbacks[i].orig_id);
                        }
                        if (callbacks[i].extra_data_size != 0) {
                            free(callbacks[i].extra_data);
                        }
                    }
                }
            }
            break;
        case IFACE_XDG_SURFACE:
            if (hdr.opcode == 0) {
                uint32_t configure_serial;
                read_all(fd, &configure_serial, 4);
                DEBUG("configure event serial = %u\n", serial);
                uint32_t wl_shm_id = get_object_id(IFACE_WL_SHM);
                wl_shm_config(fd, wl_shm_id, configure_serial);
                ok = 1;
            }
            break;
        case IFACE_XDG_TOPLEVEL:
            if (hdr.opcode == 1) { // exit
                fini = 1;
                ok = 1;
            } else if (hdr.opcode == 0) {
                int32_t array_len;
                read_all(fd, &width, 4);
                read_all(fd, &height, 4);
                read_all(fd, &array_len, 4);
                uint8_t array[array_len];
                read_all(fd, &array, array_len);
                uint32_t total_read = 4 + 4 + 4 + array_len;
                if ((array_len % 4) != 0) {
                    skip(fd, 4 - (array_len % 4));
                    DEBUG("skip: %d\n", 4 - (array_len % 4));
                    total_read += 4 - (array_len % 4);
                }
                DEBUG("read : %d\n", total_read);
                DEBUG("width: %d, height: %d, array_len: %d\n", width, height,
                      array_len);
                for (uint32_t i = 0; i < array_len; i++) {
                    DEBUG("  %d\n", array[i]);
                }
                DEBUG("body : %d\n", hdr.body);
                // FIXME
                DEBUG("skip : %d\n", hdr.body - total_read);
                skip(fd, hdr.body - total_read);
                ok = 1;
            }
            break;
        case IFACE_WL_COMPOSITOR:
            break;
        case IFACE_WL_SURFACE:
            break;
        case IFACE_WL_BUFFER:
          break;
        case IFACE_WL_SHM:
            if (hdr.opcode == 0) {
                read_all(fd, &serial, 4);  // format
                DEBUG("FIXME: wl_shm format (%d)\n", serial);
                ok = 1;
            }
            break;
        case IFACE_WL_SHM_POOL:
          break;
        case IFACE_XDG_WM_BASE:
            if (hdr.opcode == 0) { // ping
                read_all(fd, &serial, 4); 
                // pong
                call(fd, hdr.obj_id, 3, sizeof(serial), (char *)&serial);
                ok = 1;
            }
            break;
        default:
            WARN("Unknown object id (%d)\n", hdr.obj_id);
            break;
        }
        if (ok)
          continue;

        /* anything else: skip */
        WARN("Unknown event : object type %s id = %d, opcode = %d\n", object_type_to_string(type), hdr.obj_id, hdr.opcode);
        if (skip(fd, hdr.body) < 0) {
            perror("skip");
            fini = 1;
        }
    }

    close(fd);
    shm_unlink("client_shm");
    return 0;
}
