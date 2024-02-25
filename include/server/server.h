#ifndef WAYWALL_SERVER_SERVER_H
#define WAYWALL_SERVER_SERVER_H

#include <wayland-client-core.h>
#include <wayland-server-core.h>

struct server_backend {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_array shm_formats; // data: uint32_t

    struct wl_shm *shm;

    struct {
        struct wl_signal shm_format; // data: uint32_t *
    } events;
};

struct server {
    struct wl_display *display;
    struct server_backend backend;

    struct server_shm_g *shm;
};

struct server *server_create();
void server_destroy(struct server *server);

#endif