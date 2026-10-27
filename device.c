#include "dialmenu.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

static DialEventFn event_fn;
static gpointer event_user;
static GThread *thread;
static volatile int running;

typedef struct {
    int kind;
    int value;
} DialEvent;

static gboolean deliver(gpointer data) {
    DialEvent *event = data;
    if (running && event_fn) {
        event_fn(event->kind, event->value, event_user);
    }
    g_free(event);
    return G_SOURCE_REMOVE;
}

static void post(int kind, int value) {
    DialEvent *event = g_new(DialEvent, 1);
    event->kind = kind;
    event->value = value;
    g_idle_add(deliver, event);
}

static gboolean open_dial(struct libevdev **device, char *path, size_t path_size) {
    const char *forced = g_getenv("DIALMENU_DEVICE");
    if (forced && forced[0]) {
        int fd = open(forced, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            return FALSE;
        }
        if (libevdev_new_from_fd(fd, device) < 0) {
            close(fd);
            return FALSE;
        }
        g_strlcpy(path, forced, path_size);
        return TRUE;
    }

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return FALSE;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof candidate, "/dev/input/%s", entry->d_name);
        int fd = open(candidate, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        struct libevdev *dev = NULL;
        if (libevdev_new_from_fd(fd, &dev) < 0) {
            close(fd);
            continue;
        }
        const char *name = libevdev_get_name(dev);
        if (name && strcmp(name, "Surface Dial System Multi Axis") == 0) {
            *device = dev;
            g_strlcpy(path, candidate, path_size);
            closedir(dir);
            return TRUE;
        }
        libevdev_free(dev);
        close(fd);
    }
    closedir(dir);
    return FALSE;
}

static void handle_input_event(const struct input_event *event) {
    if (event->type == EV_KEY && event->code == BTN_0 && event->value != 2) {
        post(DIAL_BUTTON, event->value);
    } else if (event->type == EV_REL && event->code == REL_DIAL && event->value != 0) {
        post(DIAL_ROTATE, event->value);
    }
}

static gpointer device_thread(gpointer unused) {
    (void)unused;
    gboolean complained = FALSE;
    while (running) {
        struct libevdev *device = NULL;
        char path[PATH_MAX];
        if (!open_dial(&device, path, sizeof path)) {
            if (!complained) {
                g_warning("Surface Dial is not connected.");
                complained = TRUE;
            }
            for (int i = 0; i < 10 && running; i++) {
                g_usleep(100 * 1000);
            }
            continue;
        }
        complained = FALSE;
        int fd = libevdev_get_fd(device);
        int grabbed = libevdev_grab(device, LIBEVDEV_GRAB);
        g_message("listening to %s%s", path, grabbed == 0 ? "" : " (could not grab it)");

        while (running) {
            struct pollfd pollfd = {.fd = fd, .events = POLLIN};
            int result = poll(&pollfd, 1, 400);
            if (!running) {
                break;
            }
            if (result == 0) {
                continue;
            }
            if (result < 0) {
                if (errno == EINTR) continue;
                break;
            }
            struct input_event event;
            int rc = LIBEVDEV_READ_STATUS_SUCCESS;
            do {
                rc = libevdev_next_event(device, LIBEVDEV_READ_FLAG_NORMAL, &event);
                if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                    while ((rc = libevdev_next_event(device, LIBEVDEV_READ_FLAG_SYNC, &event)) ==
                           LIBEVDEV_READ_STATUS_SYNC) {
                        handle_input_event(&event);
                    }
                }
                if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                    handle_input_event(&event);
                } else {
                    break;
                }
            } while (rc == LIBEVDEV_READ_STATUS_SUCCESS);
            if (rc != -EAGAIN && rc != LIBEVDEV_READ_STATUS_SUCCESS && rc != LIBEVDEV_READ_STATUS_SYNC) {
                break;
            }
        }
        libevdev_grab(device, LIBEVDEV_UNGRAB);
        libevdev_free(device);
        close(fd);
        if (running) {
            g_message("Surface Dial disconnected");
        }
    }
    return NULL;
}

void dial_device_start(DialEventFn fn, gpointer user) {
    event_fn = fn;
    event_user = user;
    running = 1;
    thread = g_thread_new("dial-device", device_thread, NULL);
}

void dial_device_stop(void) {
    if (!thread) {
        return;
    }
    running = 0;
    g_thread_join(thread);
    thread = NULL;
}
