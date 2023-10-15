#ifndef WAYWALL_COMPOSITOR_PUB_COMPOSITOR_H
#define WAYWALL_COMPOSITOR_PUB_COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

// TODO: make headless size configurable
#define HEADLESS_WIDTH 1920
#define HEADLESS_HEIGHT 1080

struct compositor;

#ifndef WAYWALL_COMPOSITOR_IMPL

/*
 *  Contains all compositor state. Many objects are hidden in the public definition.
 */
struct compositor {
    struct comp_input *input;
    struct comp_render *render;
};

#endif

struct compositor_button_event {
    uint32_t button;
    uint32_t time_msec;
    bool state;
};

struct compositor_key_event {
    const xkb_keysym_t *syms;
    int nsyms;
    uint32_t modifiers;
    uint32_t time_msec;
    bool state;
};

struct compositor_motion_event {
    double x, y;
    uint32_t time_msec;
};

struct synthetic_key {
    uint8_t keycode;
    bool state;
};

struct compositor_config {
    int repeat_rate, repeat_delay;
    float floating_opacity;
    float background_color[4];
    bool confine_pointer;
    const char *cursor_theme;
    int cursor_size;
    bool stop_on_close;
};

struct compositor *compositor_create(struct compositor_config config);

void compositor_destroy(struct compositor *compositor);

struct wl_event_loop *compositor_get_loop(struct compositor *compositor);

void compositor_load_config(struct compositor *compositor, struct compositor_config config);

bool compositor_run(struct compositor *compositor, int display_file_fd);

void compositor_stop(struct compositor *compositor);

#endif