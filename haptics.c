#include "dialmenu.h"

#include <errno.h>
#include <hidapi.h>
#include <string.h>

/*
 * Surface Dial haptics are HID output report 1 on the simple haptic
 * controller, not an evdev event and not something the kernel driver sends.
 *
 *   byte 0  report id 1
 *   byte 1  repeat count (0 plays once)
 *   byte 2  manual trigger: 3 click, 4 or 5 buzz
 *   byte 3  retrigger period, low byte (the dial's own value is 80)
 *   byte 4  retrigger period, high byte
 *
 * hid_send_output_report is a SET_REPORT on the control channel. hid_write
 * uses the interrupt OUT endpoint, which this Bluetooth dial does not have,
 * so a successful hid_write never reaches the motor.
 *
 * Feature report 1 carries the resolution and the auto-trigger waveform.
 * Auto-trigger 3 at the default 3600 steps per revolution clicks on every
 * tenth of a degree, so it stays off. A manual click is sent when the menu
 * opens and each time the highlight moves. Value 2 is not one of the dial's
 * waveforms (3, 4, 5) and must not be written.
 */
static hid_device *device;
static gint64 next_try;
static gboolean hid_ready;
static gboolean logged_ready;
static gboolean logged_missing;

static void keep_auto_trigger_off(hid_device *dial) {
    unsigned char report[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
    int got = hid_get_feature_report(dial, report, sizeof report);
    unsigned steps = (unsigned)report[1] | ((unsigned)report[2] << 8);
    if (got < 5 || steps == 0 || steps > 3600) {
        return;
    }
    if (report[4] == 0) {
        return;
    }
    /* Only disarm the fine-step buzz. A coarser resolution may keep its detent. */
    if (steps != 3600 && report[4] != 0x02) {
        return;
    }
    report[0] = 0x01;
    report[4] = 0x00;
    hid_send_feature_report(dial, report, (size_t)got);
}

static gboolean ensure_device(void) {
    if (device) {
        return TRUE;
    }
    gint64 now = g_get_monotonic_time();
    if (now < next_try) {
        return FALSE;
    }
    next_try = now + 2 * G_TIME_SPAN_SECOND;
    if (!hid_ready) {
        hid_init();
        hid_ready = TRUE;
    }
    errno = 0;
    device = hid_open(0x045e, 0x091b, NULL);
    if (!device) {
        if (!logged_missing) {
            if (errno == EACCES || errno == EPERM) {
                g_warning("the Surface Dial hidraw is not writable, so haptics stay off");
            } else {
                g_warning("Surface Dial haptics are unavailable until the dial is connected");
            }
            logged_missing = TRUE;
        }
        return FALSE;
    }
    keep_auto_trigger_off(device);
    if (!logged_ready) {
        g_message("Surface Dial haptics ready");
        logged_ready = TRUE;
    }
    logged_missing = FALSE;
    return TRUE;
}

void haptics_pulse(gboolean enabled) {
    if (!enabled) {
        return;
    }
    if (!ensure_device()) {
        return;
    }
    unsigned char report[5] = {0x01, 0x00, 0x03, 80, 0x00};
    if (hid_send_output_report(device, report, sizeof report) < 0) {
        hid_close(device);
        device = NULL;
        next_try = 0;
    }
}

void haptics_shutdown(void) {
    if (device) {
        hid_close(device);
        device = NULL;
    }
    if (hid_ready) {
        hid_exit();
        hid_ready = FALSE;
    }
}
