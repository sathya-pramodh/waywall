#include "instance.h"
#include "inotify.h"
#include "server/server.h"
#include "server/ui.h"
#include "util.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zip.h>

#define K(x)                                                                                       \
    { #x, KEY_##x }

// TODO: This does not cover all possible keycodes.
static struct {
    const char *name;
    uint8_t code;
} key_mapping[] = {
    K(0),  K(1),  K(2),  K(3),  K(4),  K(5),  K(6),  K(7),  K(8),  K(9),   K(A),   K(B),
    K(C),  K(D),  K(E),  K(F),  K(G),  K(H),  K(I),  K(J),  K(K),  K(L),   K(M),   K(N),
    K(O),  K(P),  K(Q),  K(R),  K(S),  K(T),  K(U),  K(V),  K(W),  K(X),   K(Y),   K(Z),
    K(F1), K(F2), K(F3), K(F4), K(F5), K(F6), K(F7), K(F8), K(F9), K(F10), K(F11), K(F12),
};

#undef K

static const struct instance_options default_options = {
    .keys.atum_reset = KEY_F6,
    .keys.leave_preview = KEY_H,
    .auto_pause = false,
};

static inline uint8_t *
read_keycode(const char *name) {
    static const char prefix[] = "key.keyboard.";
    if (strlen(name) <= STATIC_STRLEN(prefix)) {
        return NULL;
    }

    for (size_t i = 0; i < STATIC_ARRLEN(key_mapping); i++) {
        if (strcasecmp(key_mapping[i].name, name + STATIC_STRLEN(prefix)) == 0) {
            return &key_mapping[i].code;
        }
    }

    return NULL;
}

static int
read_options(FILE *file, struct instance_options *opts) {
    if (fseek(file, 0, SEEK_SET) != 0) {
        ww_log_errno(LOG_ERROR, "failed to seek to start of options file");
        return 1;
    }

    char buf[4096];
    while (fgets(buf, STATIC_ARRLEN(buf), file)) {
        char *newline = strrchr(buf, '\n');
        if (newline) {
            *newline = '\0';
        }

        char *sep = strchr(buf, ':');
        if (!sep) {
            continue;
        }
        *sep = '\0';

        char *key = buf, *val = sep + 1;
        if (!*val) {
            continue;
        }
        if (strcmp(key, "key_Create New World") == 0) {
            uint8_t *code = read_keycode(val);
            if (!code) {
                ww_log(LOG_ERROR, "failed to read atum reset key: '%s'", val);
                return 1;
            }
            opts->keys.atum_reset = *code;
        } else if (strcmp(key, "key_Leave Preview") == 0) {
            uint8_t *code = read_keycode(val);
            if (!code) {
                ww_log(LOG_ERROR, "failed to read leave preview key: '%s'", val);
                return 1;
            }
            opts->keys.leave_preview = *code;
        } else if (strcmp(key, "pauseOnLostFocus") == 0) {
            if (strcmp(val, "false") == 0) {
                opts->auto_pause = false;
            } else if (strcmp(val, "true") == 0) {
                opts->auto_pause = true;
            } else {
                ww_log(LOG_ERROR, "invalid boolean in options file: '%s'", val);
                return 1;
            }
        } else if (strcmp(key, "f3PauseOnWorldLoad") == 0) {
            if (strcmp(val, "false") == 0) {
                opts->f3_pause = false;
            } else if (strcmp(val, "true") == 0) {
                opts->f3_pause = true;
            } else {
                ww_log(LOG_ERROR, "invalid boolean in options file: '%s'", val);
                return 1;
            }
        } else if (strcmp(key, "f1") == 0) {
            if (strcmp(val, "false") == 0) {
                opts->f1 = false;
            } else if (strcmp(val, "true") == 0) {
                opts->f1 = true;
            } else {
                ww_log(LOG_ERROR, "invalid boolean in options file: '%s'", val);
                return 1;
            }
        }
    }

    return ferror(file);
}

static inline int
parse_percent(char data[static 3]) {
    int x = data[0] - '0';
    if (x < 0 || x > 9) {
        return 0;
    }

    int y = data[1] - '0';
    if (y < 0 || y > 9) {
        return x;
    }

    int z = data[2] - '0';
    if (z < 0 || z > 9) {
        return x * 10 + y;
    }

    return 100;
}

static void
parse_state_output(struct instance *instance) {
    // The longest state which can currently be held by wpstateout.txt is 22 characters long:
    // "inworld,gamescreenopen"
    char buf[24];

    if (lseek(instance->state_fd, 0, SEEK_SET) == -1) {
        ww_log_errno(LOG_ERROR, "failed to seek wpstateout.txt in '%s'", instance->dir);
        return;
    }

    ssize_t n = read(instance->state_fd, buf, STATIC_STRLEN(buf));
    if (n == 0) {
        return;
    } else if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to read wpstateout.txt in '%s'", instance->dir);
        return;
    }
    buf[n] = '\0';

    struct instance_state next = instance->state;
    switch (buf[0]) {
    case 't': // title
        next.screen = SCREEN_TITLE;
        break;
    case 'w': // waiting
        next.screen = SCREEN_WAITING;
        break;
    case 'g': // generating,PERCENT
        next.screen = SCREEN_GENERATING;
        next.data.percent = parse_percent(buf + STATIC_STRLEN("generating,"));
        break;
    case 'p': // previewing,PERCENT
        next.screen = SCREEN_PREVIEWING;
        next.data.percent = parse_percent(buf + STATIC_STRLEN("previewing,"));
        if (instance->state.screen != SCREEN_PREVIEWING) {
            clock_gettime(CLOCK_MONOTONIC, &next.last_preview);
        }
        break;
    case 'i': // inworld,DATA
        next.screen = SCREEN_INWORLD;
        switch (buf[STATIC_STRLEN("inworld,")]) {
        case 'u': // unpaused
            next.data.inworld = INWORLD_UNPAUSED;
            break;
        case 'p': // paused
            next.data.inworld = INWORLD_PAUSED;
            break;
        case 'g': // gamescreenopen (menu)
            next.data.inworld = INWORLD_MENU;
            break;
        }
        break;
    default:
        ww_log(LOG_ERROR, "cannot parse wpstateout.txt: '%s'", buf);
        return;
    }

    instance->state = next;
}

static int
check_subdirs(const char *dirname) {
    static const char *should_exist[] = {
        "logs",
        "resourcepacks",
        "saves",
        "screenshots",
    };

    DIR *dir = opendir(dirname);
    if (!dir) {
        ww_log_errno(LOG_ERROR, "failed to open instance dir '%s'", dirname);
        return 1;
    }

    size_t found = 0;
    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        for (size_t i = 0; i < STATIC_ARRLEN(should_exist); i++) {
            if (strcmp(dirent->d_name, should_exist[i]) == 0) {
                found += 1;
                break;
            }
        }
    }

    closedir(dir);
    if (found != STATIC_ARRLEN(should_exist)) {
        ww_log(LOG_ERROR, "potential instance dir '%s' is missing directories", dirname);
        return 1;
    }
    return 0;
}

static int
process_mod_zip(const char *path, struct instance_mods *mods) {
    int err;
    zip_t *zip = zip_open(path, ZIP_RDONLY, &err);
    if (!zip) {
        zip_error_t error;
        zip_error_init_with_code(&error, err);
        ww_log(LOG_ERROR, "failed to read mod zip '%s': %s", path, zip_error_strerror(&error));
        zip_error_fini(&error);
        return 1;
    }

    for (int64_t i = 0; i < zip_get_num_entries(zip, 0); i++) {
        zip_stat_t stat;
        if (zip_stat_index(zip, i, 0, &stat) != 0) {
            zip_error_t *error = zip_get_error(zip);
            ww_log(LOG_ERROR, "failed to stat entry %" PRIi64 " of '%s': %s", i, path,
                   zip_error_strerror(error));
            zip_close(zip);
            return 1;
        }

        if (strcmp(stat.name, "me/voidxwalker/autoreset/") == 0) {
            // Atum
            mods->atum = true;
            break;
        } else if (strcmp(stat.name, "com/kingcontaria/standardsettings/") == 0) {
            // StandardSettings
            mods->standard_settings = true;
            break;
        } else if (strcmp(stat.name, "me/voidxwalker/worldpreview") == 0) {
            // WorldPreview
            mods->world_preview = true;
            continue;
        } else if (strcmp(stat.name, "me/voidxwalker/worldpreview/StateOutputHelper.class") == 0) {
            // WorldPreview with state output (3.x - 4.x)
            mods->state_output = true;
            mods->world_preview = true;
            break;
        } else if (strcmp(stat.name, "xyz/tildejustin/stateoutput/") == 0) {
            // Legacy state-output
            mods->state_output = true;
            break;
        } else if (strcmp(stat.name, "dev/tildejustin/stateoutput/") == 0) {
            // state-output
            mods->state_output = true;
            break;
        }
    }

    zip_close(zip);
    return 0;
}

static int
get_mods(const char *dirname, struct instance_mods *mods) {
    struct str dirbuf = {0};
    if (!str_append(&dirbuf, dirname)) {
        ww_log(LOG_ERROR, "instance path too long");
        return 1;
    }
    if (!str_append(&dirbuf, "/mods/")) {
        ww_log(LOG_ERROR, "instance path too long");
        return 1;
    }

    DIR *dir = opendir(dirbuf.data);
    if (!dir) {
        ww_log_errno(LOG_ERROR, "failed to open instance mods dir '%s'", dirbuf.data);
        return 1;
    }

    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        if (dirent->d_name[0] == '.') {
            continue;
        }

        const char *ext = strrchr(dirent->d_name, '.');
        if (!ext || strcmp(ext, ".jar") != 0) {
            continue;
        }

        struct str modbuf = {0};
        if (!str_append(&modbuf, dirbuf.data)) {
            ww_log(LOG_ERROR, "instance path too long");
            closedir(dir);
            return 1;
        }
        if (!str_append(&modbuf, dirent->d_name)) {
            ww_log(LOG_ERROR, "instance path too long");
            closedir(dir);
            return 1;
        }

        if (process_mod_zip(modbuf.data, mods) != 0) {
            closedir(dir);
            return 1;
        }
    }

    closedir(dir);
    return 0;
}

static int
get_options(const char *dirname, struct instance_mods mods, struct instance_options *opts) {
    struct str pathbuf = {0};
    if (!str_append(&pathbuf, dirname)) {
        ww_log(LOG_ERROR, "instance path too long");
        return 1;
    }
    if (!str_append(&pathbuf, "/options.txt")) {
        ww_log(LOG_ERROR, "instance path too long");
        return 1;
    }

    FILE *file = fopen(pathbuf.data, "r");
    if (!file) {
        ww_log_errno(LOG_ERROR, "failed to open '%s'", pathbuf.data);
        return 1;
    }

    if (read_options(file, opts) != 0) {
        fclose(file);
        return 1;
    }
    fclose(file);

    if (mods.standard_settings) {
        pathbuf = (struct str){0};
        ww_assert(str_append(&pathbuf, dirname));
        if (!str_append(&pathbuf, "/config/standardoptions.txt")) {
            ww_log(LOG_ERROR, "instance path too long");
            return 1;
        }

        // Follow the StandardSettings file chain, if there is one.
        file = fopen(pathbuf.data, "r");
        if (!file) {
            ww_log_errno(LOG_ERROR, "failed to open '%s'", pathbuf.data);
            return 1;
        }

        for (;;) {
            char buf[4096];
            if (fgets(buf, STATIC_ARRLEN(buf), file) == NULL) {
                if (feof(file)) {
                    ww_log(LOG_ERROR, "nothing to read from '%s'", pathbuf.data);
                    return 1;
                } else {
                    ww_log_errno(LOG_ERROR, "failed to read '%s'", pathbuf.data);
                    return 1;
                }
            }

            char *newline = strrchr(buf, '\n');
            if (newline) {
                *newline = '\0';
            }

            struct stat fstat;
            if (stat(buf, &fstat) != 0) {
                if (fseek(file, 0, SEEK_SET) == -1) {
                    ww_log_errno(LOG_ERROR, "failed to seek to start of '%s'", pathbuf.data);
                    return 1;
                }
                break;
            }

            fclose(file);
            file = fopen(buf, "r");
            if (!file) {
                ww_log_errno(LOG_ERROR, "failed to open '%s'", buf);
                return 1;
            }
        }

        if (read_options(file, opts) != 0) {
            fclose(file);
            return 1;
        }
        fclose(file);
    }

    return 0;
}

static int
get_version(struct server_view *view) {
    char *title = server_view_get_title(view);

    // TODO: Support snapshots?

    int version[3], n;
    if (strncmp(title, "Minecraft ", 10) == 0) {
        n = sscanf(title, "Minecraft %2d.%2d.%2d", &version[0], &version[1], &version[2]);
    } else if (strncmp(title, "Minecraft* ", 11) == 0) {
        n = sscanf(title, "Minecraft* %2d.%2d.%2d", &version[0], &version[1], &version[2]);
    } else {
        ww_log(LOG_ERROR, "failed to parse window title '%s'", title);
        free(title);
        return -1;
    }

    if (n != 3) {
        ww_log(LOG_ERROR, "failed to parse window title '%s'", title);
        free(title);
        return -1;
    }

    free(title);
    return version[1];
}

static int
open_state_file(const char *dir, struct instance_mods mods) {
    struct str pathbuf = {0};
    if (!str_append(&pathbuf, dir)) {
        ww_log(LOG_ERROR, "instance path too long");
        return -1;
    }

    const char *file = mods.state_output ? "/wpstateout.txt" : "/logs/latest.log";
    if (!str_append(&pathbuf, file)) {
        ww_log(LOG_ERROR, "instance path too long");
        return -1;
    }

    int fd = open(pathbuf.data, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open state file '%s'", pathbuf.data);
        return -1;
    }

    return fd;
}

static void
pause_instance(struct instance *instance) {
    static const struct syn_key keys[] = {
        {KEY_F3, true},
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F3, false},
    };

    server_view_send_keys(instance->view, keys, STATIC_ARRLEN(keys));
}

struct instance *
instance_create(struct server_view *view, struct inotify *inotify) {
    static_assert(sizeof(pid_t) <= sizeof(int));

    pid_t pid = server_view_get_pid(view);

    char buf[PATH_MAX];
    ssize_t n = snprintf(buf, STATIC_ARRLEN(buf), "/proc/%d/cwd", (int)pid);
    ww_assert(n < (ssize_t)STATIC_ARRLEN(buf));

    char dir[PATH_MAX];
    n = readlink(buf, dir, STATIC_ARRLEN(dir));
    if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to readlink instance dir (pid=%d)", (int)pid);
        return NULL;
    } else if (n >= (ssize_t)STATIC_ARRLEN(dir) - 1) {
        ww_log(LOG_ERROR, "instance dir too long (pid=%d)", (int)pid);
        return NULL;
    }
    dir[n] = '\0';

    // If this is a real Minecraft instance, it should have some normal directories. This does not
    // guarantee that it is actually an instance, but if you're trying to fool the detection then
    // it's your fault.
    if (check_subdirs(dir) != 0) {
        return NULL;
    }

    struct instance_mods mods = {0};
    if (get_mods(dir, &mods) != 0) {
        return NULL;
    }
    // TODO: support instances without state output
    if (!mods.state_output) {
        ww_log(LOG_ERROR, "instance '%s' has no state output", dir);
        return NULL;
    }

    struct instance_options opts = default_options;
    if (get_options(dir, mods, &opts) != 0) {
        return NULL;
    }

    int version = get_version(view);
    if (version == -1) {
        return NULL;
    }

    int state_fd = open_state_file(dir, mods);
    if (state_fd == -1) {
        return NULL;
    }

    struct instance *instance = calloc(1, sizeof(*instance));
    if (!instance) {
        ww_log(LOG_ERROR, "failed to allocate instance");
        return NULL;
    }

    instance->dir = strdup(dir);
    instance->pid = pid;
    instance->mods = mods;
    instance->opts = opts;
    instance->version = version;
    instance->state_fd = state_fd;
    instance->state_wd = -1;
    instance->view = view;

    instance->state.screen = SCREEN_TITLE;

    return instance;
}

void
instance_destroy(struct instance *instance) {
    free(instance->dir);
    free(instance);
}

char *
instance_get_state_path(struct instance *instance) {
    struct str pathbuf = {0};
    if (!str_append(&pathbuf, instance->dir)) {
        ww_log(LOG_ERROR, "instance path too long");
        return NULL;
    }

    const char *file = instance->mods.state_output ? "/wpstateout.txt" : "/logs/latest.log";
    if (!str_append(&pathbuf, file)) {
        ww_log(LOG_ERROR, "instance path too long");
        return NULL;
    }

    return strdup(pathbuf.data);
}

bool
instance_reset(struct instance *instance) {
    int screen = instance->state.screen;

    if (!server_view_has_focus(instance->view)) {
        // Resetting on the dirt screen can cause a reset to occur after the dirt screen ends.
        if (screen == SCREEN_WAITING || screen == SCREEN_GENERATING) {
            return false;
        }
    }

    // TODO: atum click fix is only necessary on older atum versions?

    uint8_t reset_key = instance->state.screen == SCREEN_PREVIEWING
                            ? instance->opts.keys.leave_preview
                            : instance->opts.keys.atum_reset;
    const struct syn_key keys[] = {
        {reset_key, true},
        {reset_key, false},
    };
    server_view_send_keys(instance->view, keys, STATIC_ARRLEN(keys));

    instance->state.screen = SCREEN_WAITING;
    instance->state.data.percent = 0;
    return true;
}

void
instance_state_update(struct instance *instance) {
    int prev = instance->state.screen;

    if (instance->mods.state_output) {
        parse_state_output(instance);
    } else {
        // TODO
    }

    int current = instance->state.screen;

    if (current == SCREEN_PREVIEWING && prev != SCREEN_PREVIEWING) {
        if (!instance->opts.f3_pause) {
            pause_instance(instance);
        }
    } else if (current == SCREEN_INWORLD && prev != SCREEN_INWORLD) {
        if (!server_view_has_focus(instance->view)) {
            if (!instance->opts.f3_pause) {
                pause_instance(instance);
            }
        } else {
            if (instance->opts.f1) {
                static const struct syn_key keys[] = {
                    {KEY_F1, true},
                    {KEY_F1, false},
                };
                server_view_send_keys(instance->view, keys, STATIC_ARRLEN(keys));
            }
        }
    }
}

void
instance_unpause(struct instance *instance) {
    static const struct syn_key keys[] = {
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F1, true},
        {KEY_F1, false},
    };

    server_view_send_keys(instance->view, keys, instance->opts.f1 ? 4 : 2);
}