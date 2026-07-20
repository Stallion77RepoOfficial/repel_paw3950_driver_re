#define _XOPEN_SOURCE_EXTENDED 1

#include <hidapi/hidapi.h>
#include <hidapi/hidapi_darwin.h>
#include <libusb.h>
#include <ApplicationServices/ApplicationServices.h>
#include <curses.h>
#include <fcntl.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define TARGET_VID 0x04F3
#define TARGET_PID 0x026F
#define WIRED_PID 0x026E
#define BLUETOOTH_PID 0x026D

#define DPI_LEVELS 7
#define DPI_COLOR_REPORT_SLOTS 8
#define BUTTON_COUNT 6
#define LEGACY_LIGHTING_MODE_MIN 0
#define LEGACY_LIGHTING_MODE_MAX 9
#define LEGACY_LIGHTING_MODE_COUNT 10
#define LIGHTING_MODE_COUNT 3
#define DEFAULT_LIGHTING_MODE_INDEX 0
#define LIGHTING_SPEED_MAX 2
#define LIGHTING_BRIGHTNESS_MAX 3
#define SLEEP_TIMEOUT_MINUTES_MIN 1
#define SLEEP_TIMEOUT_MINUTES_MAX 8
#define DEFAULT_SLEEP_TIMEOUT_MINUTES 1
#define MACRO_DEFAULT_DELAY_MS 50
#define MACRO_MAX_DELAY_MS 4095
#define MACRO_CHUNK_SIZE 25
#define MACRO_MAX_CHUNKS 15
#define MACRO_MAX_PAYLOAD_SIZE (MACRO_CHUNK_SIZE * MACRO_MAX_CHUNKS)
#define MACRO_EVENT_SIZE 3
#define MACRO_EVENTS_PER_STEP 2
#define MACRO_MAX_STEPS \
    ((MACRO_CHUNK_SIZE * MACRO_MAX_CHUNKS) / \
     (MACRO_EVENT_SIZE * MACRO_EVENTS_PER_STEP))
#define MACRO_VISIBLE_STEPS 8
#define MACRO_LOOP_TYPE_COUNTED 2
#define MACRO_LOOP_COUNT_ONCE 1
#define DPI_MIN 50
#define DPI_MAX 30000
#define DPI_STEP 50
#define DPI_LARGE_STEP 500
#define REPORT_SIZE 32
#define STATE_FILE ".mousectl-state"
#define INSTANCE_LOCK_FILE ".mousectl.lock"
#define POLLING_OPTION_COUNT ((int)(sizeof(polling_options) / sizeof(polling_options[0])))

#define OPEN_RETRIES 3
#define REPORT_SEND_RETRIES 3
#define ACK_READ_RETRIES 4
#define VERIFY_RETRIES 2
#define ACK_DELAY_US 60000
#define MACRO_FIRST_ACK_DELAY_US 120000
#define RETRY_DELAY_US 150000
#define BETWEEN_REPORTS_US 100000
#define SETTLE_DELAY_US 350000
#define TRANSPORT_SCAN_INTERVAL_MS 100
#define DISCONNECT_GRACE_MS 500
#define UI_REFRESH_MS 100

enum transfer_result {
    TRANSFER_FAILED = 0,
    TRANSFER_SENT = 1,
    TRANSFER_ACKNOWLEDGED = 2
};

enum macro_event_kind {
    MACRO_EVENT_KEYBOARD = 0,
    MACRO_EVENT_MODIFIER,
    MACRO_EVENT_MOUSE_BUTTON,
    MACRO_EVENT_KIND_COUNT
};

struct polling_option {
    int hz;
    unsigned char code;
};

struct lighting_mode_option {
    const char *name;
    unsigned char protocol_id;
    int has_speed;
    int has_brightness;
};

enum device_mode {
    MODE_NONE = 0,
    MODE_WIRED,
    MODE_BLUETOOTH,
    MODE_RECEIVER,
    MODE_COUNT
};

enum transfer_backend {
    BACKEND_HIDAPI = 0,
    BACKEND_LIBUSB
};

enum ui_tab {
    TAB_GENERAL = 0,
    TAB_BUTTONS,
    TAB_SENSOR,
    TAB_LIGHTING,
    TAB_POWER,
    TAB_MACROS,
    TAB_COUNT
};

struct mode_spec {
    enum device_mode mode;
    unsigned short pid;
    int max_polling_hz;
    const char *name;
    enum transfer_backend backend;
    int polling_cap_from_status;
    int enumeration_proves_connection;
    int current_max_polling_hz;
    int current_max_polling_valid;
};

struct device_status {
    int available;
    int report_valid;
    int active_level_valid;
    int polling_valid;
    int lighting_mode_valid;
    enum device_mode mode;
    int active_level;
    int polling_hz;
    int lighting_mode_index;
    int battery_percent;
    int max_polling_hz;
    int max_polling_valid;
};

struct save_result {
    enum transfer_result dpi_x;
    enum transfer_result dpi_y;
    enum transfer_result dpi_colors;
    enum transfer_result polling;
    enum transfer_result lod;
    enum transfer_result sensor_advanced;
    enum transfer_result macros;
    enum transfer_result buttons;
    enum transfer_result lighting;
    enum transfer_result power;
    int live_status_read;
    int live_values_checked;
    int live_values_match;
};

struct macro_step {
    unsigned char kind;
    unsigned char code;
    unsigned char uppercase;
    unsigned short delay_ms;
};

struct macro_definition {
    int step_count;
    struct macro_step steps[MACRO_MAX_STEPS];
};

struct ui_state {
    int dpi[DPI_LEVELS];
    uint32_t dpi_colors[DPI_COLOR_REPORT_SLOTS];
    int active_level;
    int polling_index;
    int lod_mm;
    int angle_snap;
    int motion_sync;
    int ripple_control;
    unsigned char buttons[BUTTON_COUNT][3];
    int lighting_mode_index;
    int lighting_speed[LIGHTING_MODE_COUNT];
    int lighting_brightness[LIGHTING_MODE_COUNT];
    int sleep_timeout_minutes;
    struct macro_definition macros[BUTTON_COUNT];
    int macro_target;
    int buttons_dirty;
    int macros_dirty;
    int lighting_dirty;
    int dpi_dirty;
    int dpi_colors_dirty;
    int lod_dirty;
    int sensor_advanced_dirty;
    int power_dirty;
    int last_battery;
    long long last_seen_ms;
    int active_level_dirty;
    int polling_dirty;
    enum device_mode polling_synced_mode;
    struct device_status device;
    char message[256];
};

static const struct polling_option polling_options[] = {
    {125, 0x08}, {250, 0x04}, {500, 0x02}, {1000, 0x01},
    {2000, 0x11}, {4000, 0x12}, {8000, 0x14},
};

static struct mode_spec mode_specs[] = {
    {MODE_WIRED, WIRED_PID, 1000, "Wired",
     BACKEND_LIBUSB, 1, 1, 1000, 0},
    {MODE_BLUETOOTH, BLUETOOTH_PID, 125, "Bluetooth",
     BACKEND_HIDAPI, 0, 1, 125, 1},
    {MODE_RECEIVER, TARGET_PID, 8000, "2.4 GHz",
     BACKEND_HIDAPI, 0, 1, 8000, 1},
};

#define MODE_SPEC_COUNT ((int)(sizeof(mode_specs) / sizeof(mode_specs[0])))

static const int default_dpi[DPI_LEVELS] = {
    800, 1600, 2400, 3200, 6400, 12000, 16000,
};

static const uint32_t default_dpi_colors[DPI_COLOR_REPORT_SLOTS] = {
    0xff0000, 0x00ff00, 0x0000ff, 0x00ffff,
    0xffff00, 0xff00ff, 0xffffff, 0xffffff,
};

static const int default_lighting_speed[LIGHTING_MODE_COUNT] = {
    1, 1, 1,
};

static const int default_lighting_brightness[LIGHTING_MODE_COUNT] = {
    3, 1, 1,
};

static const unsigned char default_buttons[BUTTON_COUNT][3] = {
    {0x10, 0x01, 0x00}, {0x10, 0x02, 0x00},
    {0x10, 0x04, 0x00}, {0x10, 0x10, 0x00},
    {0x10, 0x08, 0x00}, {0x40, 0x01, 0x00},
};

static const char *button_names[BUTTON_COUNT] = {
    "Left", "Right", "Wheel", "Forward", "Backward", "DPI"
};

struct button_preset {
    const char *name;
    unsigned char value[3];
};

static const struct button_preset button_presets[] = {
    {"Left click", {0x10, 0x01, 0x00}},
    {"Right click", {0x10, 0x02, 0x00}},
    {"Wheel click", {0x10, 0x04, 0x00}},
    {"Backward", {0x10, 0x08, 0x00}},
    {"Forward", {0x10, 0x10, 0x00}},
    {"DPI loop", {0x40, 0x01, 0x00}},
};

#define BUTTON_PRESET_COUNT ((int)(sizeof(button_presets) / sizeof(button_presets[0])))
#define BUTTON_ACTION_COUNT (BUTTON_PRESET_COUNT + 1)

struct macro_input_option {
    const char *name;
    unsigned char kind;
    unsigned char code;
    wint_t captured_key;
    wint_t shifted_key;
    CGEventFlags native_flag;
    int native_button;
};

#define MACRO_KEY(name, code, captured, shifted) \
    {name, MACRO_EVENT_KEYBOARD, code, captured, shifted, 0, -1}
#define MACRO_NATIVE_KEY(name, code, flag) \
    {name, MACRO_EVENT_KEYBOARD, code, 0, 0, flag, -1}
#define MACRO_MODIFIER(name, code, flag) \
    {name, MACRO_EVENT_MODIFIER, code, 0, 0, flag, -1}
#define MACRO_MOUSE(name, code, button) \
    {name, MACRO_EVENT_MOUSE_BUTTON, code, 0, 0, 0, button}

static const struct macro_input_option macro_input_options[] = {
    MACRO_KEY("a", 0x04, L'a', L'A'), MACRO_KEY("b", 0x05, L'b', L'B'),
    MACRO_KEY("c", 0x06, L'c', L'C'), MACRO_KEY("d", 0x07, L'd', L'D'),
    MACRO_KEY("e", 0x08, L'e', L'E'), MACRO_KEY("f", 0x09, L'f', L'F'),
    MACRO_KEY("g", 0x0a, L'g', L'G'), MACRO_KEY("h", 0x0b, L'h', L'H'),
    MACRO_KEY("i", 0x0c, L'i', L'I'), MACRO_KEY("j", 0x0d, L'j', L'J'),
    MACRO_KEY("k", 0x0e, L'k', L'K'), MACRO_KEY("l", 0x0f, L'l', L'L'),
    MACRO_KEY("m", 0x10, L'm', L'M'), MACRO_KEY("n", 0x11, L'n', L'N'),
    MACRO_KEY("o", 0x12, L'o', L'O'), MACRO_KEY("p", 0x13, L'p', L'P'),
    MACRO_KEY("q", 0x14, L'q', L'Q'), MACRO_KEY("r", 0x15, L'r', L'R'),
    MACRO_KEY("s", 0x16, L's', L'S'), MACRO_KEY("t", 0x17, L't', L'T'),
    MACRO_KEY("u", 0x18, L'u', L'U'), MACRO_KEY("v", 0x19, L'v', L'V'),
    MACRO_KEY("w", 0x1a, L'w', L'W'), MACRO_KEY("x", 0x1b, L'x', L'X'),
    MACRO_KEY("y", 0x1c, L'y', L'Y'), MACRO_KEY("z", 0x1d, L'z', L'Z'),
    MACRO_KEY("1", 0x1e, L'1', L'!'), MACRO_KEY("2", 0x1f, L'2', L'@'),
    MACRO_KEY("3", 0x20, L'3', L'#'), MACRO_KEY("4", 0x21, L'4', L'$'),
    MACRO_KEY("5", 0x22, L'5', L'%'), MACRO_KEY("6", 0x23, L'6', L'^'),
    MACRO_KEY("7", 0x24, L'7', L'&'), MACRO_KEY("8", 0x25, L'8', L'*'),
    MACRO_KEY("9", 0x26, L'9', L'('), MACRO_KEY("0", 0x27, L'0', L')'),
    MACRO_KEY("Enter", 0x28, L'\n', 0),
    MACRO_KEY("Escape", 0x29, 27, 0),
    MACRO_KEY("Backspace", 0x2a, KEY_BACKSPACE, 0),
    MACRO_KEY("Tab", 0x2b, L'\t', 0),
    MACRO_KEY("Space", 0x2c, L' ', 0),
    MACRO_KEY("-", 0x2d, L'-', L'_'),
    MACRO_KEY("=", 0x2e, L'=', L'+'),
    MACRO_KEY("[", 0x2f, L'[', L'{'),
    MACRO_KEY("]", 0x30, L']', L'}'),
    MACRO_KEY("\\", 0x31, L'\\', L'|'),
    MACRO_KEY("Non-US #", 0x32, 0, 0),
    MACRO_KEY(";", 0x33, L';', L':'),
    MACRO_KEY("'", 0x34, L'\'', L'\"'),
    MACRO_KEY("`", 0x35, L'`', L'~'),
    MACRO_KEY(",", 0x36, L',', L'<'),
    MACRO_KEY(".", 0x37, L'.', L'>'),
    MACRO_KEY("/", 0x38, L'/', L'?'),
    MACRO_NATIVE_KEY("Caps Lock", 0x39, kCGEventFlagMaskAlphaShift),
    MACRO_KEY("F1", 0x3a, KEY_F(1), 0), MACRO_KEY("F2", 0x3b, KEY_F(2), 0),
    MACRO_KEY("F3", 0x3c, KEY_F(3), 0), MACRO_KEY("F4", 0x3d, KEY_F(4), 0),
    MACRO_KEY("F5", 0x3e, KEY_F(5), 0), MACRO_KEY("F6", 0x3f, KEY_F(6), 0),
    MACRO_KEY("F7", 0x40, KEY_F(7), 0), MACRO_KEY("F8", 0x41, KEY_F(8), 0),
    MACRO_KEY("F9", 0x42, KEY_F(9), 0), MACRO_KEY("F10", 0x43, KEY_F(10), 0),
    MACRO_KEY("F11", 0x44, KEY_F(11), 0), MACRO_KEY("F12", 0x45, KEY_F(12), 0),
    MACRO_KEY("Print Screen", 0x46, 0, 0),
    MACRO_KEY("Scroll Lock", 0x47, 0, 0),
    MACRO_KEY("Pause", 0x48, 0, 0),
    MACRO_KEY("Insert", 0x49, KEY_IC, 0),
    MACRO_KEY("Home", 0x4a, KEY_HOME, 0),
    MACRO_KEY("Page Up", 0x4b, KEY_PPAGE, 0),
    MACRO_KEY("Delete", 0x4c, KEY_DC, 0),
    MACRO_KEY("End", 0x4d, KEY_END, 0),
    MACRO_KEY("Page Down", 0x4e, KEY_NPAGE, 0),
    MACRO_KEY("Right", 0x4f, KEY_RIGHT, 0),
    MACRO_KEY("Left", 0x50, KEY_LEFT, 0),
    MACRO_KEY("Down", 0x51, KEY_DOWN, 0),
    MACRO_KEY("Up", 0x52, KEY_UP, 0),
    MACRO_KEY("Num Lock", 0x53, 0, 0),
    MACRO_KEY("Keypad /", 0x54, 0, 0), MACRO_KEY("Keypad *", 0x55, 0, 0),
    MACRO_KEY("Keypad -", 0x56, 0, 0), MACRO_KEY("Keypad +", 0x57, 0, 0),
    MACRO_KEY("Keypad Enter", 0x58, 0, 0),
    MACRO_KEY("Keypad 1", 0x59, 0, 0), MACRO_KEY("Keypad 2", 0x5a, 0, 0),
    MACRO_KEY("Keypad 3", 0x5b, 0, 0), MACRO_KEY("Keypad 4", 0x5c, 0, 0),
    MACRO_KEY("Keypad 5", 0x5d, 0, 0), MACRO_KEY("Keypad 6", 0x5e, 0, 0),
    MACRO_KEY("Keypad 7", 0x5f, 0, 0), MACRO_KEY("Keypad 8", 0x60, 0, 0),
    MACRO_KEY("Keypad 9", 0x61, 0, 0), MACRO_KEY("Keypad 0", 0x62, 0, 0),
    MACRO_KEY("Keypad .", 0x63, 0, 0),
    MACRO_KEY("Non-US \\|", 0x64, L'§', L'±'),
    MACRO_KEY("Application", 0x65, 0, 0),
    MACRO_KEY("Keyboard Power", 0x66, 0, 0),
    MACRO_KEY("Keypad =", 0x67, 0, 0),
    MACRO_KEY("F13", 0x68, KEY_F(13), 0), MACRO_KEY("F14", 0x69, KEY_F(14), 0),
    MACRO_KEY("F15", 0x6a, KEY_F(15), 0), MACRO_KEY("F16", 0x6b, KEY_F(16), 0),
    MACRO_KEY("F17", 0x6c, KEY_F(17), 0), MACRO_KEY("F18", 0x6d, KEY_F(18), 0),
    MACRO_KEY("F19", 0x6e, KEY_F(19), 0), MACRO_KEY("F20", 0x6f, KEY_F(20), 0),
    MACRO_KEY("F21", 0x70, KEY_F(21), 0), MACRO_KEY("F22", 0x71, KEY_F(22), 0),
    MACRO_KEY("F23", 0x72, KEY_F(23), 0), MACRO_KEY("F24", 0x73, KEY_F(24), 0),
    MACRO_KEY("Execute", 0x74, 0, 0), MACRO_KEY("Help", 0x75, KEY_HELP, 0),
    MACRO_KEY("Menu", 0x76, 0, 0), MACRO_KEY("Select", 0x77, 0, 0),
    MACRO_KEY("Stop", 0x78, 0, 0), MACRO_KEY("Again", 0x79, 0, 0),
    MACRO_KEY("Undo", 0x7a, 0, 0), MACRO_KEY("Cut", 0x7b, 0, 0),
    MACRO_KEY("Copy", 0x7c, 0, 0), MACRO_KEY("Paste", 0x7d, 0, 0),
    MACRO_KEY("Find", 0x7e, 0, 0), MACRO_KEY("Keyboard Mute", 0x7f, 0, 0),
    MACRO_KEY("Keyboard Volume Up", 0x80, 0, 0),
    MACRO_KEY("Keyboard Volume Down", 0x81, 0, 0),
    MACRO_KEY("Keypad ,", 0x85, 0, 0),
    MACRO_KEY("Keypad = (AS/400)", 0x86, 0, 0),
    MACRO_MODIFIER("Control", 0x01, kCGEventFlagMaskControl),
    MACRO_MODIFIER("Shift", 0x02, kCGEventFlagMaskShift),
    MACRO_MODIFIER("Option", 0x04, kCGEventFlagMaskAlternate),
    MACRO_MODIFIER("Command", 0x08, kCGEventFlagMaskCommand),
    MACRO_MODIFIER("Right Control", 0x10, 0),
    MACRO_MODIFIER("Right Shift", 0x20, 0),
    MACRO_MODIFIER("Right Option", 0x40, 0),
    MACRO_MODIFIER("Right Command", 0x80, 0),
    MACRO_MOUSE("Left click", 0x01, 0), MACRO_MOUSE("Right click", 0x02, 1),
    MACRO_MOUSE("Wheel click", 0x04, 2), MACRO_MOUSE("Backward", 0x08, 3),
    MACRO_MOUSE("Forward", 0x10, 4),
};

#define MACRO_INPUT_OPTION_COUNT \
    ((int)(sizeof(macro_input_options) / sizeof(macro_input_options[0])))

static const struct lighting_mode_option lighting_modes[LIGHTING_MODE_COUNT] = {
    {"Steady", 3, 0, 1},
    {"Breathing", 2, 1, 0},
    {"Neon", 4, 1, 0},
};

static unsigned short configuration_pid = TARGET_PID;
static hid_device *status_handle;
static unsigned short status_pid;
static libusb_context *usb_context;
static long long last_transport_scan_ms;
static unsigned int cached_transport_presence;
static unsigned char transaction_id = 1;

static void drain_input_reports(hid_device *handle);
static int polling_index_from_hz(int hz);

static int lighting_index_from_protocol_id(int protocol_id) {
    for (int i = 0; i < LIGHTING_MODE_COUNT; ++i) {
        if (lighting_modes[i].protocol_id == protocol_id) {
            return i;
        }
    }
    return -1;
}

static unsigned char next_transaction_id(void) {
    return transaction_id++;
}

static int hid_interface_number(libusb_device_handle *handle) {
    struct libusb_config_descriptor *configuration = NULL;
    libusb_device *device = libusb_get_device(handle);
    if (libusb_get_active_config_descriptor(device, &configuration) != 0) {
        return 0;
    }

    int interface_number = 0;
    for (int i = 0; i < configuration->bNumInterfaces; ++i) {
        const struct libusb_interface *interface = &configuration->interface[i];
        for (int j = 0; j < interface->num_altsetting; ++j) {
            const struct libusb_interface_descriptor *descriptor =
                &interface->altsetting[j];
            if (descriptor->bInterfaceClass != LIBUSB_CLASS_HID) {
                continue;
            }

            interface_number = descriptor->bInterfaceNumber;
            if (descriptor->bInterfaceSubClass == 0 &&
                descriptor->bInterfaceProtocol == 0) {
                libusb_free_config_descriptor(configuration);
                return interface_number;
            }
        }
    }

    libusb_free_config_descriptor(configuration);
    return interface_number;
}

static unsigned char checksum(const unsigned char *report, size_t length) {
    unsigned int sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum += report[i];
    }
    return (unsigned char)sum;
}

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static struct mode_spec *mode_spec(enum device_mode mode) {
    for (int i = 0; i < MODE_SPEC_COUNT; ++i) {
        if (mode_specs[i].mode == mode) {
            return &mode_specs[i];
        }
    }
    return NULL;
}

static struct mode_spec *mode_spec_for_pid(unsigned short pid) {
    for (int i = 0; i < MODE_SPEC_COUNT; ++i) {
        if (mode_specs[i].pid == pid) {
            return &mode_specs[i];
        }
    }
    return NULL;
}

static int maximum_polling_index(const struct device_status *device) {
    enum device_mode mode = device == NULL ? MODE_NONE : device->mode;
    const struct mode_spec *spec = mode_spec(mode);
    /* A mode whose cap comes from status must not use its baseline as a
     * measured limit before the first valid capability report. */
    if (device != NULL && device->available && spec != NULL &&
        spec->polling_cap_from_status && !device->max_polling_valid) {
        return -1;
    }
    int maximum = device != NULL && device->max_polling_hz > 0 ?
                  device->max_polling_hz :
                  (spec == NULL ? polling_options[0].hz :
                                  spec->max_polling_hz);
    int index = polling_index_from_hz(maximum);
    return index >= 0 ? index : 0;
}

static int polling_capacity_to_hz(unsigned char capability) {
    switch (capability) {
        case 1: return 1000;
        case 2: return 2000;
        case 4: return 4000;
        case 8: return 8000;
        default: return 0;
    }
}

static int polling_index_from_code(unsigned char code) {
    for (int i = 0; i < POLLING_OPTION_COUNT; ++i) {
        if (polling_options[i].code == code) {
            return i;
        }
    }
    return -1;
}

static int polling_index_from_hz(int hz) {
    for (int i = 0; i < POLLING_OPTION_COUNT; ++i) {
        if (polling_options[i].hz == hz) {
            return i;
        }
    }
    return -1;
}

static hid_device *open_configuration_interface_for_pid(unsigned short pid) {
    struct hid_device_info *devices = hid_enumerate(TARGET_VID, pid);
    hid_device *handle = NULL;

    for (struct hid_device_info *device = devices; device != NULL; device = device->next) {
        if (device->usage_page == 0x0001 && device->usage == 0x0006) {
            handle = hid_open_path(device->path);
            if (handle != NULL) {
                break;
            }
        }
    }

    hid_free_enumeration(devices);
    return handle;
}

static void close_status_handle(void) {
    if (status_handle != NULL) {
        hid_close(status_handle);
        status_handle = NULL;
        status_pid = 0;
    }
}

static unsigned int detect_transports(void) {
    long long now = monotonic_ms();
    if (now - last_transport_scan_ms < TRANSPORT_SCAN_INTERVAL_MS) {
        return cached_transport_presence;
    }

    unsigned int presence = 0;
    struct hid_device_info *devices = hid_enumerate(TARGET_VID, 0);
    for (struct hid_device_info *device = devices; device != NULL;
         device = device->next) {
        for (int i = 0; i < MODE_SPEC_COUNT; ++i) {
            if (device->product_id == mode_specs[i].pid) {
                presence |= 1u << mode_specs[i].mode;
            }
        }
    }
    hid_free_enumeration(devices);
    cached_transport_presence = presence;
    last_transport_scan_ms = now;
    return cached_transport_presence;
}

static void drain_input_reports(hid_device *handle) {
    unsigned char buffer[REPORT_SIZE];
    while (hid_read_timeout(handle, buffer, sizeof(buffer), 0) > 0) {
    }
}

static int response_is_ack(const unsigned char *response, int length) {
    /* GamingMS tests only response byte 1. */
    return length >= 2 && response[1] != 0;
}

static enum transfer_result send_report_once(hid_device *handle,
                                             unsigned char *report,
                                             size_t report_size,
                                             unsigned int first_ack_delay_us) {
    drain_input_reports(handle);
    int sent = 0;
    for (int attempt = 0; attempt < ACK_READ_RETRIES; ++attempt) {
        report[2] = next_transaction_id();
        report[4] = 0;
        /* GamingMS always checksums bytes 0..31. The lighting transfer has
         * one extra transport byte, which is not part of the checksum. */
        report[4] = checksum(report, REPORT_SIZE);

        int written = hid_send_feature_report(handle, report, report_size);
        if (written == (int)report_size) {
            sent = 1;
        }

        unsigned char response[REPORT_SIZE] = {0};
        response[0] = 0x06;
        /* cmd10 waits 120 ms after every send; all other commands wait
         * 60 ms.  Failed reads add a separate 60 ms retry backoff below. */
        usleep(first_ack_delay_us);

        int length = hid_get_feature_report(handle, response, sizeof(response));
        if (response_is_ack(response, length)) {
            return TRANSFER_ACKNOWLEDGED;
        }
        if (attempt + 1 < ACK_READ_RETRIES) {
            usleep(ACK_DELAY_US);
        }
    }

    return sent ? TRANSFER_SENT : TRANSFER_FAILED;
}

static enum transfer_result send_report_via_libusb(
    unsigned short pid,
    const unsigned char *template_report, size_t report_size,
    unsigned int first_ack_delay_us) {
    for (int open_attempt = 0; open_attempt < REPORT_SEND_RETRIES;
         ++open_attempt) {
        libusb_device_handle *handle =
            libusb_open_device_with_vid_pid(usb_context, TARGET_VID, pid);
        if (handle == NULL) {
            usleep(RETRY_DELAY_US);
            continue;
        }

        unsigned char report[REPORT_SIZE + 1];
        memcpy(report, template_report, report_size);
        int interface_number = hid_interface_number(handle);
        int sent = 0;

        for (int attempt = 0; attempt < ACK_READ_RETRIES; ++attempt) {
            report[2] = next_transaction_id();
            report[4] = 0;
            report[4] = checksum(report, REPORT_SIZE);
            int written = libusb_control_transfer(
                handle, 0x21, 0x09, 0x0300 | report[0], interface_number,
                report, (uint16_t)report_size, 1000);
            if (written == (int)report_size) {
                sent = 1;
            }
            unsigned char response[REPORT_SIZE] = {0};
            response[0] = 0x06;
            usleep(first_ack_delay_us);
            int length = libusb_control_transfer(
                handle, 0xa1, 0x01, 0x0300 | response[0],
                interface_number, response, sizeof(response), 1000);
            if (response_is_ack(response, length)) {
                libusb_close(handle);
                return TRANSFER_ACKNOWLEDGED;
            }
            if (attempt + 1 < ACK_READ_RETRIES) {
                usleep(ACK_DELAY_US);
            }
        }

        libusb_close(handle);
        if (sent) return TRANSFER_SENT;
        usleep(RETRY_DELAY_US);
    }

    return TRANSFER_FAILED;
}

static enum transfer_result send_report_with_size_and_delay(
    const unsigned char *template_report, size_t report_size,
    unsigned int first_ack_delay_us) {
    if (report_size < REPORT_SIZE || report_size > REPORT_SIZE + 1) {
        return TRANSFER_FAILED;
    }
    const struct mode_spec *spec = mode_spec_for_pid(configuration_pid);
    if (spec != NULL && spec->backend == BACKEND_LIBUSB) {
        return send_report_via_libusb(spec->pid, template_report, report_size,
                                      first_ack_delay_us);
    }
    for (int attempt = 0; attempt < REPORT_SEND_RETRIES; ++attempt) {
        hid_device *handle = NULL;
        for (int open_attempt = 0; open_attempt < OPEN_RETRIES && handle == NULL;
             ++open_attempt) {
            handle = open_configuration_interface_for_pid(configuration_pid);
            if (handle == NULL) {
                usleep(RETRY_DELAY_US);
            }
        }
        if (handle == NULL) {
            continue;
        }

        unsigned char report[REPORT_SIZE + 1];
        memcpy(report, template_report, report_size);

        enum transfer_result result =
            send_report_once(handle, report, report_size,
                             first_ack_delay_us);
        hid_close(handle);

        if (result >= TRANSFER_SENT) {
            return result;
        }
        usleep(RETRY_DELAY_US);
    }

    return TRANSFER_FAILED;
}

static enum transfer_result send_report_with_size(
    const unsigned char *template_report, size_t report_size) {
    return send_report_with_size_and_delay(template_report, report_size,
                                           ACK_DELAY_US);
}

static enum transfer_result send_report_with_retries(
    const unsigned char template_report[REPORT_SIZE]) {
    return send_report_with_size(template_report, REPORT_SIZE);
}

static int decode_status_report(const unsigned char *report, int length,
                                const struct mode_spec *spec,
                                struct device_status *status) {
    if (length < 10 || report[0] != 0x04) {
        return 0;
    }

    int polling_index = polling_index_from_code(report[3]);
    status->available = 1;
    status->report_valid = 1;
    status->active_level_valid = report[2] < DPI_LEVELS;
    status->active_level = status->active_level_valid ? report[2] : -1;
    status->polling_valid = polling_index >= 0;
    status->polling_hz = polling_index >= 0 ? polling_options[polling_index].hz : 0;
    /* Only the three effects proven useful on this mouse are exposed. */
    int lighting_index = lighting_index_from_protocol_id(report[6]);
    status->lighting_mode_valid = lighting_index >= 0;
    status->lighting_mode_index = lighting_index;
    status->battery_percent = report[5] == 0x02 ? 100 :
                              (report[4] <= 100 ? report[4] : -1);
    if (spec != NULL && spec->polling_cap_from_status) {
        int maximum_polling_hz = polling_capacity_to_hz(report[8]);
        if (maximum_polling_hz > 0) {
            /* Unknown selector values leave a cap learned from an earlier
             * queued report intact. */
            status->max_polling_hz = maximum_polling_hz;
            status->max_polling_valid = 1;
        }
    }
    return 1;
}

static int read_status_for_pid(unsigned short pid, struct device_status *status) {
    struct mode_spec *spec = mode_spec_for_pid(pid);
    if (spec == NULL) {
        return 0;
    }
    int opened = 0;
    if (status_handle == NULL || status_pid != pid) {
        close_status_handle();
        status_handle = open_configuration_interface_for_pid(pid);
        if (status_handle == NULL) {
            return 0;
        }
        status_pid = pid;
        opened = 1;
    }

    int found = 0;
    if (opened) {
        if (hid_set_nonblocking(status_handle, 1) < 0) {
            close_status_handle();
            return 0;
        }
    }
    for (;;) {
        unsigned char report[REPORT_SIZE] = {0};
        int length = hid_read(status_handle, report, sizeof(report));
        if (length < 0) {
            close_status_handle();
            return 0;
        }
        if (length == 0) {
            break;
        }
        if (decode_status_report(report, length, spec, status)) {
            found = 1;
        }
    }

    if (found) {
        configuration_pid = pid;
        status->mode = spec->mode;
        if (status->max_polling_valid) {
            spec->current_max_polling_hz = status->max_polling_hz;
            spec->current_max_polling_valid = 1;
        } else {
            status->max_polling_hz = spec->current_max_polling_hz;
            status->max_polling_valid = spec->current_max_polling_valid;
        }
    }
    return found;
}

static int read_live_status(struct device_status *status) {
    memset(status, 0, sizeof(*status));
    status->battery_percent = -1;
    unsigned int presence = detect_transports();

    /* Table order defines priority.  Every mode uses the same status path;
     * the table says whether enumeration alone proves a live connection. */
    for (int i = 0; i < MODE_SPEC_COUNT; ++i) {
        struct mode_spec *spec = &mode_specs[i];
        if ((presence & (1u << spec->mode)) == 0) {
            continue;
        }
        if (read_status_for_pid(spec->pid, status)) {
            return 1;
        }
        if (!spec->enumeration_proves_connection) {
            continue;
        }
        status->available = 1;
        status->mode = spec->mode;
        status->max_polling_hz = spec->current_max_polling_hz;
        status->max_polling_valid = spec->current_max_polling_valid;
        configuration_pid = spec->pid;
        return 1;
    }

    close_status_handle();
    return 0;
}

static int dpi_to_device_code(int dpi, unsigned short *code) {
    if (dpi < DPI_MIN || dpi > DPI_MAX || dpi % DPI_STEP != 0) {
        return 0;
    }
    *code = (unsigned short)(dpi / DPI_STEP - 1);
    return 1;
}

static int build_dpi_report(const int dpi[DPI_LEVELS], int active_level,
                            int y_axis, unsigned char report[REPORT_SIZE]) {
    if (active_level < 0 || active_level >= DPI_LEVELS) {
        return 0;
    }

    unsigned short codes[DPI_LEVELS];
    for (int i = 0; i < DPI_LEVELS; ++i) {
        if (!dpi_to_device_code(dpi[i], &codes[i])) {
            return 0;
        }
    }

    memset(report, 0, REPORT_SIZE);
    report[0] = 0x06;
    report[1] = 0x02;
    report[3] = y_axis ? 0x12 : 0x02;
    report[5] = (unsigned char)active_level;
    report[6] = 0x7f;

    for (int i = 0; i < DPI_LEVELS; ++i) {
        report[7 + i * 2] = (unsigned char)(codes[i] & 0xff);
        report[8 + i * 2] = (unsigned char)(codes[i] >> 8);
    }
    return 1;
}

static enum transfer_result save_dpi_axis(const int dpi[DPI_LEVELS],
                                          int active_level, int y_axis) {
    unsigned char report[REPORT_SIZE];
    if (!build_dpi_report(dpi, active_level, y_axis, report)) {
        return TRANSFER_FAILED;
    }
    return send_report_with_size(report, sizeof(report));
}

static void build_dpi_color_report(
        const uint32_t colors[DPI_COLOR_REPORT_SLOTS],
        unsigned char report[REPORT_SIZE]) {
    /* GamingMS FUN_00437080, 004379fc-00437bad: command 03 carries eight
     * independent RGB slots. Profile values are written as #RRGGBB, while
     * the packet stores the component bytes in R, G, B order. */
    memset(report, 0, REPORT_SIZE);
    report[0] = 0x06;
    report[1] = 0x03;
    report[3] = 0x01;
    for (int slot = 0; slot < DPI_COLOR_REPORT_SLOTS; ++slot) {
        uint32_t rgb = colors[slot];
        report[5 + slot * 3] = (unsigned char)(rgb >> 16);
        report[6 + slot * 3] = (unsigned char)(rgb >> 8);
        report[7 + slot * 3] = (unsigned char)rgb;
    }
}

static enum transfer_result save_dpi_colors(
        const uint32_t colors[DPI_COLOR_REPORT_SLOTS]) {
    unsigned char report[REPORT_SIZE];
    build_dpi_color_report(colors, report);
    return send_report_with_retries(report);
}

static enum transfer_result save_polling(int polling_index) {
    if (polling_index < 0 || polling_index >= POLLING_OPTION_COUNT) {
        return TRANSFER_FAILED;
    }

    unsigned char report[REPORT_SIZE] = {0};
    report[0] = 0x06;
    report[1] = 0x04;
    report[3] = 0x01;
    report[5] = polling_options[polling_index].code;
    return send_report_with_size(report, sizeof(report));
}

static void build_lod_report(int lod_mm,
                             unsigned char report[REPORT_SIZE]) {
    /* GamingMS FUN_00437080: command 05, LiftDetectionHeight 1/2. */
    memset(report, 0, REPORT_SIZE);
    report[0] = 0x06;
    report[1] = 0x05;
    report[3] = 0x01;
    report[5] = (unsigned char)lod_mm;
}

static enum transfer_result save_lod(int lod_mm) {
    if (lod_mm < 1 || lod_mm > 2) {
        return TRANSFER_FAILED;
    }
    unsigned char report[REPORT_SIZE];
    build_lod_report(lod_mm, report);
    return send_report_with_retries(report);
}

static void build_sensor_advanced_report(int angle_snap, int motion_sync,
                                         int ripple_control,
                                         unsigned char report[REPORT_SIZE]) {
    /* GamingMS FUN_00438340: command 08, DD4/DD5/DD6 in UI order. */
    memset(report, 0, REPORT_SIZE);
    report[0] = 0x06;
    report[1] = 0x08;
    report[3] = 0x01;
    report[5] = angle_snap ? 1 : 0;
    report[6] = motion_sync ? 1 : 0;
    report[7] = ripple_control ? 1 : 0;
}

static enum transfer_result save_sensor_advanced(int angle_snap,
                                                 int motion_sync,
                                                 int ripple_control) {
    unsigned char report[REPORT_SIZE];
    build_sensor_advanced_report(angle_snap, motion_sync, ripple_control,
                                 report);
    return send_report_with_retries(report);
}

static void build_button_report(const unsigned char buttons[BUTTON_COUNT][3],
                                unsigned char report[REPORT_SIZE]) {
    static const int packet_order[BUTTON_COUNT] = {0, 1, 2, 4, 3, 5};
    memset(report, 0, REPORT_SIZE);
    report[0] = 0x06;
    report[1] = 0x01;
    report[2] = 0;
    report[3] = 0x01;
    report[4] = 0;
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        memcpy(&report[5 + i * 3], buttons[packet_order[i]], 3);
    }
}

static enum transfer_result save_buttons(
        const unsigned char buttons[BUTTON_COUNT][3]) {
    unsigned char report[REPORT_SIZE];
    build_button_report(buttons, report);
    return send_report_with_retries(report);
}

static int build_lighting_report(const struct ui_state *state,
                                 unsigned char report[REPORT_SIZE + 1]) {
    int index = state->lighting_mode_index;
    if (index < 0 || index >= LIGHTING_MODE_COUNT ||
        state->lighting_speed[index] < 0 ||
        state->lighting_speed[index] > LIGHTING_SPEED_MAX ||
        state->lighting_brightness[index] < 0 ||
        state->lighting_brightness[index] > LIGHTING_BRIGHTNESS_MAX) {
        return 0;
    }

    memset(report, 0, REPORT_SIZE + 1);
    report[0] = 0x06;
    report[1] = 0x06;
    report[3] = 0x01;

    /* GamingMS runtime Apply path (FUN_00437080, 00437528-0043757a)
     * clears the 25-byte lighting payload and writes only these fields:
     *   payload[0] = 0
     *   payload[1] = raw OEM effect identifier
     *   payload[2] = (brightness + 1) << 4 | (speed + 1)
     * The color-bearing buffer built by BuildProfileDeviceBuffers is only a
     * profile cache; sending it here produces a different, invalid command. */
    unsigned char *payload = &report[5];
    payload[1] = lighting_modes[index].protocol_id;
    payload[2] =
        (unsigned char)(((state->lighting_brightness[index] + 1) << 4) |
                        ((state->lighting_speed[index] + 1) & 0x0f));
    return 1;
}

static enum transfer_result save_lighting(const struct ui_state *state) {
    unsigned char report[REPORT_SIZE + 1];
    if (!build_lighting_report(state, report)) {
        return TRANSFER_FAILED;
    }
    return send_report_with_size(report, sizeof(report));
}

static int build_power_report(int sleep_timeout_minutes,
                              unsigned char report[REPORT_SIZE]) {
    if (sleep_timeout_minutes < SLEEP_TIMEOUT_MINUTES_MIN ||
        sleep_timeout_minutes > SLEEP_TIMEOUT_MINUTES_MAX) {
        return 0;
    }

    /* GamingMS FUN_004380d0 (0043812e-004382a7): command 07 carries the
     * selected PowerDownTime verbatim in byte 5. The OEM selection table at
     * 0061a3b8 contains the supported minute values 1..8. */
    memset(report, 0, REPORT_SIZE);
    report[0] = 0x06;
    report[1] = 0x07;
    report[3] = 0x01;
    report[5] = (unsigned char)sleep_timeout_minutes;
    return 1;
}

static enum transfer_result save_power(const struct ui_state *state) {
    unsigned char report[REPORT_SIZE];
    if (!build_power_report(state->sleep_timeout_minutes, report)) {
        return TRANSFER_FAILED;
    }
    return send_report_with_retries(report);
}

static int button_uses_macro(const unsigned char value[3]) {
    int slot = value[1] & 0x0f;
    return value[0] == 0x90 && slot >= 1 && slot <= BUTTON_COUNT;
}

static int button_uses_own_macro(const unsigned char value[3], int button) {
    return button >= 0 && button < BUTTON_COUNT && button_uses_macro(value) &&
           (value[1] & 0x0f) == button + 1;
}

static enum transfer_result combine_transfer_results(
        enum transfer_result aggregate, enum transfer_result current) {
    if (aggregate == TRANSFER_FAILED || current == TRANSFER_FAILED) {
        return TRANSFER_FAILED;
    }
    if (aggregate == TRANSFER_SENT || current == TRANSFER_SENT) {
        return TRANSFER_SENT;
    }
    return TRANSFER_ACKNOWLEDGED;
}

static int macro_input_option_index(unsigned char kind, unsigned char code) {
    for (int i = 0; i < MACRO_INPUT_OPTION_COUNT; ++i) {
        if (macro_input_options[i].kind == kind &&
            macro_input_options[i].code == code) {
            return i;
        }
    }
    return -1;
}

static const char *macro_input_name(const struct macro_step *step) {
    int index = macro_input_option_index(step->kind, step->code);
    if (index < 0) return "Unknown";
    if (step->uppercase && step->kind == MACRO_EVENT_KEYBOARD &&
        step->code >= 0x04 && step->code <= 0x1d) {
        static char uppercase_name[2];
        uppercase_name[0] = (char)('A' + step->code - 0x04);
        uppercase_name[1] = '\0';
        return uppercase_name;
    }
    return macro_input_options[index].name;
}

static int macro_event_prefix(unsigned char kind) {
    static const unsigned char prefixes[MACRO_EVENT_KIND_COUNT] = {
        0x30, 0x20, 0x10,
    };
    return kind < MACRO_EVENT_KIND_COUNT ? prefixes[kind] : -1;
}

static int append_macro_event(unsigned char *payload, size_t capacity,
                              size_t *offset, int prefix, int released,
                              unsigned short delay, unsigned char code) {
    if (*offset + MACRO_EVENT_SIZE > capacity) {
        return 0;
    }
    payload[(*offset)++] =
        (unsigned char)(prefix | (released ? 0x80 : 0) | (delay >> 8));
    payload[(*offset)++] = (unsigned char)delay;
    payload[(*offset)++] = code;
    return 1;
}

static int build_macro_payload(const struct macro_definition *macro,
                               unsigned char *payload, size_t capacity) {
    if (macro->step_count <= 0 || macro->step_count > MACRO_MAX_STEPS ||
        payload == NULL) {
        return -1;
    }

    size_t offset = 0;
    unsigned char held_modifiers[MACRO_MAX_STEPS];
    int held_modifier_count = 0;
    for (int i = 0; i < macro->step_count; ++i) {
        int prefix = macro_event_prefix(macro->steps[i].kind);
        if (prefix < 0 ||
            macro_input_option_index(macro->steps[i].kind,
                                     macro->steps[i].code) < 0 ||
            macro->steps[i].delay_ms > MACRO_MAX_DELAY_MS) {
            return -1;
        }
        unsigned short delay = macro->steps[i].delay_ms;
        int implicit_shift = macro->steps[i].uppercase;
        for (int held = 0; held < held_modifier_count; ++held) {
            if (held_modifiers[held] & 0x22) implicit_shift = 0;
        }
        if (implicit_shift &&
            !append_macro_event(payload, capacity, &offset,
                                macro_event_prefix(MACRO_EVENT_MODIFIER),
                                0, 0, 0x02)) {
            return -1;
        }
        if (!append_macro_event(payload, capacity, &offset, prefix, 0,
                                delay, macro->steps[i].code)) {
            return -1;
        }
        if (macro->steps[i].kind == MACRO_EVENT_MODIFIER) {
            held_modifiers[held_modifier_count++] = macro->steps[i].code;
            if (i + 1 < macro->step_count) {
                continue;
            }
        } else if (!append_macro_event(payload, capacity, &offset, prefix, 1,
                                       0, macro->steps[i].code)) {
            return -1;
        }
        if (implicit_shift &&
            !append_macro_event(payload, capacity, &offset,
                                macro_event_prefix(MACRO_EVENT_MODIFIER),
                                1, 0, 0x02)) {
            return -1;
        }
        while (held_modifier_count > 0) {
            unsigned char code = held_modifiers[--held_modifier_count];
            int modifier_prefix = macro_event_prefix(MACRO_EVENT_MODIFIER);
            if (!append_macro_event(payload, capacity, &offset,
                                    modifier_prefix, 1, 0, code)) {
                return -1;
            }
        }
    }
    return (int)offset;
}

static int build_macro_chunk_report(unsigned char slot,
                                    const unsigned char *payload,
                                    int payload_size, int chunk,
                                    int chunk_count,
                                    unsigned char report[REPORT_SIZE]) {
    if (slot == 0 || slot > BUTTON_COUNT || payload == NULL ||
        payload_size <= 0 || chunk < 0 || chunk >= chunk_count ||
        chunk_count <= 0 || chunk_count > MACRO_MAX_CHUNKS) {
        return 0;
    }
    int offset = chunk * MACRO_CHUNK_SIZE;
    if (offset >= payload_size) {
        return 0;
    }
    int chunk_size = payload_size - offset;
    if (chunk_size > MACRO_CHUNK_SIZE) {
        chunk_size = MACRO_CHUNK_SIZE;
    }

    memset(report, 0, REPORT_SIZE);
    report[0] = 0x06;
    report[1] = 0x10;
    report[3] = (unsigned char)((chunk << 4) | chunk_count);
    report[5] = (unsigned char)(chunk_size + 1);
    report[6] = slot;
    memcpy(&report[7], &payload[offset], (size_t)chunk_size);
    return 1;
}

static void build_macro_reset_report(unsigned char report[REPORT_SIZE]) {
    memset(report, 0, REPORT_SIZE);
    report[0] = 0x06;
    report[1] = 0x11;
    report[3] = 0x01;
    report[5] = 0xff;
}

static enum transfer_result save_macros(const struct ui_state *state) {
    /* Validate every payload before cmd11 clears the device macro storage. */
    for (int button = 0; button < BUTTON_COUNT; ++button) {
        if (button_uses_macro(state->buttons[button]) &&
            !button_uses_own_macro(state->buttons[button], button)) {
            return TRANSFER_FAILED;
        }
        if (!button_uses_own_macro(state->buttons[button], button)) {
            continue;
        }
        unsigned char payload[MACRO_MAX_PAYLOAD_SIZE];
        int payload_size = build_macro_payload(&state->macros[button], payload,
                                               sizeof(payload));
        int chunk_count = payload_size > 0 ?
                          (payload_size + MACRO_CHUNK_SIZE - 1) /
                          MACRO_CHUNK_SIZE : 0;
        if (payload_size <= 0 || chunk_count > MACRO_MAX_CHUNKS) {
            return TRANSFER_FAILED;
        }
    }

    unsigned char reset[REPORT_SIZE];
    build_macro_reset_report(reset);

    enum transfer_result aggregate = send_report_with_retries(reset);
    if (aggregate == TRANSFER_FAILED) {
        return aggregate;
    }
    usleep(BETWEEN_REPORTS_US);

    for (int button = 0; button < BUTTON_COUNT; ++button) {
        if (!button_uses_own_macro(state->buttons[button], button)) {
            continue;
        }

        unsigned char payload[MACRO_MAX_PAYLOAD_SIZE];
        int payload_size = build_macro_payload(&state->macros[button], payload,
                                               sizeof(payload));
        if (payload_size <= 0) {
            return TRANSFER_FAILED;
        }
        int chunk_count = (payload_size + MACRO_CHUNK_SIZE - 1) /
                          MACRO_CHUNK_SIZE;
        if (chunk_count > MACRO_MAX_CHUNKS) {
            return TRANSFER_FAILED;
        }

        for (int chunk = 0; chunk < chunk_count; ++chunk) {
            unsigned char report[REPORT_SIZE];
            if (!build_macro_chunk_report((unsigned char)(button + 1),
                                          payload, payload_size, chunk,
                                          chunk_count, report)) {
                return TRANSFER_FAILED;
            }

            enum transfer_result result =
                send_report_with_size_and_delay(report, sizeof(report),
                                                MACRO_FIRST_ACK_DELAY_US);
            aggregate = combine_transfer_results(aggregate, result);
            if (aggregate == TRANSFER_FAILED) {
                return aggregate;
            }
            usleep(BETWEEN_REPORTS_US);
        }
    }
    return aggregate;
}

static int button_preset_index(const unsigned char value[3]) {
    for (int i = 0; i < BUTTON_PRESET_COUNT; ++i) {
        if (memcmp(value, button_presets[i].value, 3) == 0) {
            return i;
        }
    }
    return -1;
}

static int button_action_index(const struct ui_state *state, int button) {
    int preset = button_preset_index(state->buttons[button]);
    if (preset >= 0) {
        return preset;
    }
    if (button_uses_own_macro(state->buttons[button], button)) {
        return BUTTON_PRESET_COUNT;
    }
    return -1;
}

static void set_button_action(struct ui_state *state, int button, int action) {
    int was_macro = button_uses_macro(state->buttons[button]);
    if (action < BUTTON_PRESET_COUNT) {
        memcpy(state->buttons[button], button_presets[action].value, 3);
        if (was_macro) {
            state->macros_dirty = 1;
        }
        return;
    }
    state->buttons[button][0] = 0x90;
    state->buttons[button][1] =
        (unsigned char)((MACRO_LOOP_TYPE_COUNTED << 4) | (button + 1));
    state->buttons[button][2] = MACRO_LOOP_COUNT_ONCE;
    state->macros_dirty = 1;
}

static const char *button_action_name(const struct ui_state *state, int button,
                                      char *buffer, size_t buffer_size) {
    int action = button_action_index(state, button);
    if (action >= 0 && action < BUTTON_PRESET_COUNT) {
        return button_presets[action].name;
    }
    if (action == BUTTON_PRESET_COUNT) {
        const struct macro_definition *macro = &state->macros[button];
        if (macro->step_count == 1) {
            snprintf(buffer, buffer_size, "%s",
                     macro_input_name(&macro->steps[0]));
        } else {
            snprintf(buffer, buffer_size, "Macro (%d steps)",
                     macro->step_count);
        }
        return buffer;
    }
    return "Custom";
}

static int all_macro_assignments_valid(const struct ui_state *state) {
    for (int button = 0; button < BUTTON_COUNT; ++button) {
        if (button_uses_macro(state->buttons[button]) &&
            (!button_uses_own_macro(state->buttons[button], button) ||
             state->macros[button].step_count <= 0)) {
            return 0;
        }
        if (button_uses_own_macro(state->buttons[button], button)) {
            unsigned char payload[MACRO_MAX_PAYLOAD_SIZE];
            if (build_macro_payload(&state->macros[button], payload,
                                    sizeof(payload)) <= 0) {
                return 0;
            }
        }
    }
    return 1;
}

static unsigned char keyboard_code_from_key(wint_t key,
                                             unsigned char *modifier) {
    *modifier = 0;
    if (key == L'\r' || key == KEY_ENTER) key = L'\n';
    if (key == 127 || key == 8) key = KEY_BACKSPACE;
    for (int i = 0; i < MACRO_INPUT_OPTION_COUNT; ++i) {
        const struct macro_input_option *option = &macro_input_options[i];
        if (option->kind != MACRO_EVENT_KEYBOARD) continue;
        if (option->captured_key != 0 && key == option->captured_key) {
            return option->code;
        }
        if (option->shifted_key != 0 && key == option->shifted_key) {
            *modifier = 0x02;
            return option->code;
        }
    }
    return 0;
}

static unsigned char captured_letter_modifier(wint_t key,
                                               CGEventFlags flags) {
    int uppercase = key >= L'A' && key <= L'Z';
    int caps_lock = (flags & kCGEventFlagMaskAlphaShift) != 0;
    return uppercase != caps_lock ? 0x02 : 0;
}

static int valid_dpi_value(int dpi) {
    return dpi >= DPI_MIN && dpi <= DPI_MAX && dpi % DPI_STEP == 0;
}

static void reset_to_defaults(struct ui_state *state) {
    memcpy(state->dpi, default_dpi, sizeof(default_dpi));
    memcpy(state->dpi_colors, default_dpi_colors,
           sizeof(default_dpi_colors));
    state->active_level = 0;
    state->polling_index = polling_index_from_hz(1000);
    state->lod_mm = 2;
    state->angle_snap = 0;
    state->motion_sync = 1;
    state->ripple_control = 0;
    memcpy(state->buttons, default_buttons, sizeof(default_buttons));
    state->lighting_mode_index = DEFAULT_LIGHTING_MODE_INDEX;
    memcpy(state->lighting_speed, default_lighting_speed,
           sizeof(default_lighting_speed));
    memcpy(state->lighting_brightness, default_lighting_brightness,
           sizeof(default_lighting_brightness));
    state->sleep_timeout_minutes = DEFAULT_SLEEP_TIMEOUT_MINUTES;
    memset(state->macros, 0, sizeof(state->macros));
    state->macro_target = 0;
    state->last_battery = -1;
}

static void mark_all_settings_dirty(struct ui_state *state) {
    state->active_level_dirty = 1;
    state->polling_dirty = 1;
    state->buttons_dirty = 1;
    state->macros_dirty = 1;
    state->lighting_dirty = 1;
    state->dpi_dirty = 1;
    state->dpi_colors_dirty = 1;
    state->lod_dirty = 1;
    state->sensor_advanced_dirty = 1;
    state->power_dirty = 1;
}

static int has_pending_settings(const struct ui_state *state) {
    return state->active_level_dirty || state->polling_dirty ||
           state->buttons_dirty || state->macros_dirty ||
           state->lighting_dirty || state->dpi_dirty ||
           state->dpi_colors_dirty || state->lod_dirty ||
           state->sensor_advanced_dirty || state->power_dirty;
}

static int load_state(struct ui_state *state) {
    reset_to_defaults(state);
    /* Without a trustworthy local snapshot, every displayed default is
     * staged so Save cannot silently skip values that the UI presents. */
    mark_all_settings_dirty(state);

    FILE *file = fopen(STATE_FILE, "r");
    if (file == NULL) {
        return 0;
    }

    int active_level = 0;
    int polling_index = 0;
    int dpi[DPI_LEVELS];
    uint32_t dpi_colors[DPI_COLOR_REPORT_SLOTS];
    memcpy(dpi_colors, default_dpi_colors, sizeof(dpi_colors));
    int lod_mm = 2;
    int last_battery = -1;
    int angle_snap = 0;
    int motion_sync = 1;
    int ripple_control = 0;
    unsigned char buttons[BUTTON_COUNT][3];
    memcpy(buttons, default_buttons, sizeof(buttons));
    int lighting_mode_index = DEFAULT_LIGHTING_MODE_INDEX;
    int lighting_speed[LIGHTING_MODE_COUNT];
    int lighting_brightness[LIGHTING_MODE_COUNT];
    int unsupported_lighting_mode = 0;
    int sleep_timeout_minutes = DEFAULT_SLEEP_TIMEOUT_MINUTES;
    int removed_movement_setting = 0;
    memcpy(lighting_speed, default_lighting_speed, sizeof(lighting_speed));
    memcpy(lighting_brightness, default_lighting_brightness,
           sizeof(lighting_brightness));
    struct macro_definition macros[BUTTON_COUNT] = {0};
    char version[3] = {0};
    int valid = fscanf(file, "%2s %d %d", version, &active_level,
                       &polling_index) == 3;
    int is_vb = strcmp(version, "vB") == 0;
    int is_vc = strcmp(version, "vC") == 0;
    int is_vd = strcmp(version, "vD") == 0;
    int is_ve = strcmp(version, "vE") == 0;
    int is_vf = strcmp(version, "vF") == 0;
    int is_vg = strcmp(version, "vG") == 0;
    int is_vh = strcmp(version, "vH") == 0;
    int color_count = (is_vd || is_ve || is_vf || is_vg || is_vh) ?
                      DPI_COLOR_REPORT_SLOTS :
                      (is_vc ? DPI_LEVELS : 0);
    valid = valid &&
            (is_vb || is_vc || is_vd || is_ve || is_vf || is_vg || is_vh) &&
            fscanf(file, "%d %d %d %d %d", &last_battery, &lod_mm,
                   &angle_snap, &motion_sync, &ripple_control) == 5;
    if (valid && (is_vg || is_vh)) {
        valid = fscanf(file, "%d", &sleep_timeout_minutes) == 1;
    } else if (valid && (is_ve || is_vf)) {
        valid = fscanf(file, "%d %d", &sleep_timeout_minutes,
                       &removed_movement_setting) == 2 &&
                (removed_movement_setting == 0 ||
                 removed_movement_setting == 1);
    }

    for (int i = 0; valid && i < DPI_LEVELS; ++i) {
        valid = fscanf(file, "%d", &dpi[i]) == 1 && valid_dpi_value(dpi[i]);
    }
    for (int i = 0; valid && i < BUTTON_COUNT; ++i) {
        unsigned int a, b, c;
        valid = fscanf(file, "%u %u %u", &a, &b, &c) == 3 &&
                a <= 255 && b <= 255 && c <= 255;
        if (valid) {
            buttons[i][0] = (unsigned char)a;
            buttons[i][1] = (unsigned char)b;
            buttons[i][2] = (unsigned char)c;
        }
    }
    if (valid && (is_vf || is_vg || is_vh)) {
        int protocol_id;
        valid = fscanf(file, "%d", &protocol_id) == 1;
        lighting_mode_index = lighting_index_from_protocol_id(protocol_id);
        valid = valid && lighting_mode_index >= 0;
        for (int index = 0; valid && index < LIGHTING_MODE_COUNT; ++index) {
            valid = fscanf(file, "%d %d", &lighting_speed[index],
                           &lighting_brightness[index]) == 2;
        }
    } else if (valid && (is_ve || is_vd)) {
        int legacy_mode;
        valid = fscanf(file, "%d", &legacy_mode) == 1 &&
                legacy_mode >= LEGACY_LIGHTING_MODE_MIN &&
                legacy_mode <= LEGACY_LIGHTING_MODE_MAX;
        for (int mode = 0; valid && mode < LEGACY_LIGHTING_MODE_COUNT;
             ++mode) {
            int speed, brightness, legacy_direction = 0;
            int fields = is_vd ?
                         fscanf(file, "%d %d %d", &speed, &brightness,
                                &legacy_direction) :
                         fscanf(file, "%d %d", &speed, &brightness);
            valid = fields == (is_vd ? 3 : 2) &&
                    speed >= 0 && speed <= LIGHTING_SPEED_MAX &&
                    brightness >= 0 &&
                    brightness <= LIGHTING_BRIGHTNESS_MAX &&
                    (!is_vd || legacy_direction == 0 ||
                     legacy_direction == 1);
            int index = lighting_index_from_protocol_id(mode);
            if (valid && index >= 0) {
                lighting_speed[index] = speed;
                lighting_brightness[index] = brightness;
            }
        }
        if (valid && is_ve) {
            int legacy_direction;
            valid = fscanf(file, "%d", &legacy_direction) == 1 &&
                    (legacy_direction == 0 || legacy_direction == 1);
        }
        if (valid) {
            int index = lighting_index_from_protocol_id(legacy_mode);
            if (index >= 0) {
                lighting_mode_index = index;
            } else {
                lighting_mode_index = DEFAULT_LIGHTING_MODE_INDEX;
                unsupported_lighting_mode = 1;
            }
        }
    } else if (valid) {
        int enabled, legacy_mode, speed, brightness, direction;
        valid = fscanf(file, "%d %d %d %d %d", &enabled, &legacy_mode,
                       &speed, &brightness, &direction) == 5;
        valid = valid && (enabled == 0 || enabled == 1) &&
                legacy_mode >= LEGACY_LIGHTING_MODE_MIN &&
                legacy_mode <= LEGACY_LIGHTING_MODE_MAX &&
                speed >= 0 && speed <= LIGHTING_SPEED_MAX &&
                brightness >= 0 &&
                brightness <= LIGHTING_BRIGHTNESS_MAX &&
                (direction == 0 || direction == 1);
        if (valid) {
            int index = enabled ?
                        lighting_index_from_protocol_id(legacy_mode) : -1;
            if (index >= 0) {
                lighting_mode_index = index;
                lighting_speed[index] = speed;
                lighting_brightness[index] = brightness;
            } else {
                lighting_mode_index = DEFAULT_LIGHTING_MODE_INDEX;
                unsupported_lighting_mode = 1;
            }
        }
    }
    for (int i = 0; valid && i < color_count; ++i) {
        unsigned int color;
        valid = fscanf(file, "%x", &color) == 1 && color <= 0xffffff;
        if (valid) {
            dpi_colors[i] = color;
        }
    }
    for (int button = 0; valid && button < BUTTON_COUNT; ++button) {
        int step_count = 0;
        valid = fscanf(file, "%d", &step_count) == 1 &&
                step_count >= 0 && step_count <= MACRO_MAX_STEPS;
        macros[button].step_count = step_count;
        for (int step = 0; valid && step < step_count; ++step) {
            unsigned int kind = MACRO_EVENT_KEYBOARD, code, uppercase = 0,
                         delay;
            int fields = is_vh ?
                         fscanf(file, "%u %u %u %u", &kind, &code,
                                &uppercase, &delay) :
                         is_vg ? fscanf(file, "%u %u %u", &kind, &code,
                                       &delay) :
                         fscanf(file, "%u %u", &code, &delay);
            valid = fields == (is_vh ? 4 : is_vg ? 3 : 2) &&
                    kind <= 255 && code <= 255 && uppercase <= 1 &&
                    delay <= MACRO_MAX_DELAY_MS &&
                    (!uppercase ||
                     (kind == MACRO_EVENT_KEYBOARD && code >= 0x04 &&
                      code <= 0x1d)) &&
                    macro_input_option_index((unsigned char)kind,
                                             (unsigned char)code) >= 0;
            if (valid) {
                macros[button].steps[step].kind = (unsigned char)kind;
                macros[button].steps[step].code = (unsigned char)code;
                macros[button].steps[step].uppercase =
                    (unsigned char)uppercase;
                macros[button].steps[step].delay_ms = (unsigned short)delay;
            }
        }
    }
    if (valid) {
        char trailing[2];
        valid = fscanf(file, "%1s", trailing) != 1 && !ferror(file);
    }
    fclose(file);

    if (!valid || active_level < 0 || active_level >= DPI_LEVELS ||
        polling_index < 0 || polling_index >= POLLING_OPTION_COUNT ||
        lod_mm < 1 || lod_mm > 2 ||
        (angle_snap != 0 && angle_snap != 1) ||
        (motion_sync != 0 && motion_sync != 1) ||
        (ripple_control != 0 && ripple_control != 1) ||
        sleep_timeout_minutes < SLEEP_TIMEOUT_MINUTES_MIN ||
        sleep_timeout_minutes > SLEEP_TIMEOUT_MINUTES_MAX ||
        lighting_mode_index < 0 ||
        lighting_mode_index >= LIGHTING_MODE_COUNT) {
        return 0;
    }
    for (int mode = 0; mode < LIGHTING_MODE_COUNT; ++mode) {
        if (lighting_speed[mode] < 0 ||
            lighting_speed[mode] > LIGHTING_SPEED_MAX ||
            lighting_brightness[mode] < 0 ||
            lighting_brightness[mode] > LIGHTING_BRIGHTNESS_MAX) {
            return 0;
        }
    }
    for (int button = 0; button < BUTTON_COUNT; ++button) {
        if (button_uses_macro(buttons[button]) &&
            ((buttons[button][1] & 0x0f) != button + 1 ||
             macros[button].step_count <= 0)) {
            return 0;
        }
    }

    memcpy(state->dpi, dpi, sizeof(dpi));
    memcpy(state->dpi_colors, dpi_colors, sizeof(dpi_colors));
    state->active_level = active_level;
    state->polling_index = polling_index;
    state->lod_mm = lod_mm;
    state->angle_snap = angle_snap != 0;
    state->motion_sync = motion_sync != 0;
    state->ripple_control = ripple_control != 0;
    memcpy(state->buttons, buttons, sizeof(buttons));
    state->lighting_mode_index = lighting_mode_index;
    memcpy(state->lighting_speed, lighting_speed, sizeof(lighting_speed));
    memcpy(state->lighting_brightness, lighting_brightness,
           sizeof(lighting_brightness));
    state->sleep_timeout_minutes = sleep_timeout_minutes;
    state->active_level_dirty = 0;
    state->polling_dirty = 0;
    state->buttons_dirty = 0;
    state->macros_dirty = 0;
    state->lighting_dirty = unsupported_lighting_mode;
    state->dpi_dirty = 0;
    state->dpi_colors_dirty = 0;
    state->lod_dirty = 0;
    state->sensor_advanced_dirty = 0;
    state->power_dirty = (is_vg || is_vh) ? 0 : 1;
    memcpy(state->macros, macros, sizeof(macros));
    state->last_battery = last_battery >= 0 && last_battery <= 100 ?
                          last_battery : -1;
    return 1;
}

static int persist_state(const struct ui_state *state) {
    if (state->lighting_mode_index < 0 ||
        state->lighting_mode_index >= LIGHTING_MODE_COUNT) {
        return 0;
    }

    char temporary_path[256];
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", STATE_FILE);

    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return 0;
    }

    int success = fprintf(file, "vH %d %d %d %d %d %d %d %d",
                          state->active_level, state->polling_index,
                          state->last_battery, state->lod_mm,
                          state->angle_snap, state->motion_sync,
                          state->ripple_control,
                          state->sleep_timeout_minutes) > 0;
    for (int i = 0; success && i < DPI_LEVELS; ++i) {
        success = fprintf(file, " %d", state->dpi[i]) > 0;
    }
    for (int i = 0; success && i < BUTTON_COUNT; ++i) {
        success = fprintf(file, " %u %u %u", state->buttons[i][0],
                          state->buttons[i][1], state->buttons[i][2]) > 0;
    }
    if (success) {
        success = fprintf(file, " %u",
                          lighting_modes[state->lighting_mode_index].protocol_id) > 0;
    }
    for (int mode = 0; success && mode < LIGHTING_MODE_COUNT; ++mode) {
        success = fprintf(file, " %d %d", state->lighting_speed[mode],
                          state->lighting_brightness[mode]) > 0;
    }
    for (int i = 0; success && i < DPI_COLOR_REPORT_SLOTS; ++i) {
        success = fprintf(file, " %06X",
                          (unsigned int)state->dpi_colors[i]) > 0;
    }
    for (int button = 0; success && button < BUTTON_COUNT; ++button) {
        const struct macro_definition *macro = &state->macros[button];
        success = fprintf(file, " %d", macro->step_count) > 0;
        for (int step = 0; success && step < macro->step_count; ++step) {
            success = fprintf(file, " %u %u %u %u", macro->steps[step].kind,
                              macro->steps[step].code,
                              macro->steps[step].uppercase,
                              macro->steps[step].delay_ms) > 0;
        }
    }
    success = success && fputc('\n', file) != EOF;
    success = success && fflush(file) == 0;
    success = success && fsync(fileno(file)) == 0;
    success = fclose(file) == 0 && success;

    if (!success || rename(temporary_path, STATE_FILE) != 0) {
        unlink(temporary_path);
        return 0;
    }
    return 1;
}

static int refresh_device(struct ui_state *state, int show_message) {
    struct device_status status;
    enum device_mode previous_mode = state->device.available ?
                                     state->device.mode : MODE_NONE;
    if (show_message) {
        close_status_handle();
        last_transport_scan_ms = 0;
    }
    if (!read_live_status(&status)) {
        if (state->device.available &&
            monotonic_ms() - state->last_seen_ms >= DISCONNECT_GRACE_MS) {
            memset(&state->device, 0, sizeof(state->device));
            state->device.active_level = -1;
            state->device.battery_percent = -1;
            state->polling_synced_mode = MODE_NONE;
        }
        if (show_message) {
            snprintf(state->message, sizeof(state->message),
                     state->device.available ? "No new report; current mode kept."
                                             : "Device not found.");
        }
        return state->device.available;
    }

    state->last_seen_ms = monotonic_ms();

    if (!status.report_valid) {
        status.active_level = state->active_level;
        status.polling_hz = polling_options[state->polling_index].hz;
    }
    if (status.battery_percent < 0) {
        status.battery_percent = state->last_battery;
    } else {
        state->last_battery = status.battery_percent;
    }
    state->device = status;
    if (!state->active_level_dirty && status.report_valid &&
        status.active_level_valid) {
        state->active_level = status.active_level;
    }
    if (!state->lighting_dirty && status.report_valid &&
        status.lighting_mode_valid) {
        state->lighting_mode_index = status.lighting_mode_index;
    }

    /* DPI events may carry stale polling data; sync polling once per mode. */
    int polling_index = state->polling_index;
    if (status.polling_valid && !state->polling_dirty &&
        state->polling_synced_mode != status.mode) {
        polling_index = polling_index_from_hz(status.polling_hz);
        state->polling_synced_mode = status.mode;
    } else if (previous_mode != status.mode) {
        state->polling_synced_mode = MODE_NONE;
    }
    int maximum_index = maximum_polling_index(&status);
    if (maximum_index >= 0 && polling_index > maximum_index) {
        polling_index = maximum_index;
    }
    if (polling_index >= 0) {
        state->polling_index = polling_index;
    }

    if (show_message) {
        snprintf(state->message, sizeof(state->message), "Refreshed.");
    }
    return 1;
}

static void move_dpi(int *dpi, int delta) {
    int value = *dpi + delta;
    if (value < DPI_MIN) {
        value = DPI_MIN;
    } else if (value > DPI_MAX) {
        value = DPI_MAX;
    }

    value -= value % DPI_STEP;
    *dpi = value < DPI_MIN ? DPI_MIN : value;
}

static void draw_slider(int row, int value) {
    const int width = 38;
    int filled = (value - DPI_MIN) * width / (DPI_MAX - DPI_MIN);
    if (filled < 0) {
        filled = 0;
    } else if (filled > width) {
        filled = width;
    }

    mvaddch(row, 30, '[');
    for (int i = 0; i < width; ++i) {
        mvaddch(row, 31 + i, i < filled ? '=' : ' ');
    }
    mvaddch(row, 31 + width, ']');
    mvprintw(row, 33 + width, "%5d DPI", value);
}

static void draw_tabs(enum ui_tab tab) {
    static const char *names[TAB_COUNT] = {
        "General", "Buttons", "Sensor", "Lighting", "Power", "Macros"
    };
    int column = 2;
    for (int i = 0; i < TAB_COUNT; ++i) {
        if (i == (int)tab) attron(A_REVERSE);
        mvprintw(1, column, " %s ", names[i]);
        if (i == (int)tab) attroff(A_REVERSE);
        column += (int)strlen(names[i]) + 3;
    }
}

static void draw_common_help(int row) {
    mvprintw(row, 2,
             "Arrows: navigate/adjust | S: save | D: reset | R: refresh | Q: exit");
}

enum lighting_item {
    LIGHTING_ITEM_MODE,
    LIGHTING_ITEM_SPEED,
    LIGHTING_ITEM_BRIGHTNESS
};

static int lighting_has_speed(int index) {
    return index >= 0 && index < LIGHTING_MODE_COUNT &&
           lighting_modes[index].has_speed;
}

static int lighting_has_brightness(int index) {
    return index >= 0 && index < LIGHTING_MODE_COUNT &&
           lighting_modes[index].has_brightness;
}

static int lighting_item_count(int index) {
    return 1 + lighting_has_speed(index) + lighting_has_brightness(index);
}

static enum lighting_item lighting_item_at(int mode_index, int index) {
    if (index == 0) return LIGHTING_ITEM_MODE;
    index -= 1;
    if (lighting_has_speed(mode_index)) {
        if (index == 0) return LIGHTING_ITEM_SPEED;
        --index;
    }
    return LIGHTING_ITEM_BRIGHTNESS;
}

static int macro_target_index(const struct ui_state *state) {
    return state->macro_target >= 0 && state->macro_target < BUTTON_COUNT ?
           state->macro_target : 0;
}

static int tab_item_count(const struct ui_state *state, enum ui_tab tab) {
    if (tab == TAB_GENERAL) return DPI_LEVELS + 1;
    if (tab == TAB_BUTTONS) return BUTTON_COUNT;
    if (tab == TAB_SENSOR) return 4;
    if (tab == TAB_LIGHTING) {
        return lighting_item_count(state->lighting_mode_index);
    }
    if (tab == TAB_POWER) return 1;

    int target = macro_target_index(state);
    const struct macro_definition *macro = &state->macros[target];
    return 1 + macro->step_count +
           (macro->step_count < MACRO_MAX_STEPS ? 1 : 0);
}

static void draw_ui(const struct ui_state *state, enum ui_tab tab, int selected) {
    const int polling_row = 14;
    const int status_row = polling_row + 3;
    int item_count = tab_item_count(state, tab);
    if (selected < 0 || selected >= item_count) selected = 0;
    erase();

    draw_tabs(tab);

    if (state->device.available) {
        const struct mode_spec *spec = mode_spec(state->device.mode);
        const char *mode = spec == NULL ? "Unknown" : spec->name;
        char battery[16];
        if (state->device.battery_percent >= 0) {
            snprintf(battery, sizeof(battery), "%d%%",
                     state->device.battery_percent);
        } else {
            snprintf(battery, sizeof(battery), "--");
        }
        mvprintw(3, 2,
                 "Mode: %s | Battery: %s | DPI level: %d | Polling: %d Hz",
                 mode, battery, state->active_level,
                 polling_options[state->polling_index].hz);
    } else {
        mvprintw(3, 2, "Mode: Not connected");
    }

    if (tab == TAB_BUTTONS) {
        mvprintw(5, 2, "Button assignments:");
        for (int i = 0; i < BUTTON_COUNT; ++i) {
            char action_buffer[32];
            if (selected == i) attron(A_REVERSE);
            mvprintw(7 + i, 4, "%-10s < %-14s >", button_names[i],
                     button_action_name(state, i, action_buffer,
                                        sizeof(action_buffer)));
            if (selected == i) attroff(A_REVERSE);
        }
        draw_common_help(15);
        mvprintw(18, 2, "%s", state->message);
        refresh();
        return;
    }

    if (tab == TAB_SENSOR) {
        const char *labels[] = {
            "Lift-off distance", "Angle snap", "Motion sync", "Ripple control"
        };
        const char *descriptions[] = {
            "Tracking stop height after lifting the mouse.",
            "Straightens nearly horizontal/vertical movement.",
            "Aligns sensor samples with USB report timing.",
            "Reduces high-DPI ripple/jitter; may add smoothing."
        };
        char lod_value[8];
        snprintf(lod_value, sizeof(lod_value), "%d mm", state->lod_mm);
        const char *values[] = {
            lod_value,
            state->angle_snap ? "On" : "Off",
            state->motion_sync ? "On" : "Off",
            state->ripple_control ? "On" : "Off"
        };
        mvprintw(5, 2, "Sensor settings:");
        for (int i = 0; i < 4; ++i) {
            if (selected == i) attron(A_REVERSE);
            mvprintw(7 + i, 4, "%-24s < %s >", labels[i], values[i]);
            if (selected == i) attroff(A_REVERSE);
        }
        mvprintw(12, 2, "%s", descriptions[selected]);
        draw_common_help(13);
        mvprintw(15, 2, "%s", state->message);
        refresh();
        return;
    }

    if (tab == TAB_LIGHTING) {
        int mode_index = state->lighting_mode_index;
        if (mode_index < 0 || mode_index >= LIGHTING_MODE_COUNT) {
            mode_index = DEFAULT_LIGHTING_MODE_INDEX;
        }
        mvprintw(4, 2, "Lighting settings:");
        int count = lighting_item_count(mode_index);
        for (int i = 0; i < count; ++i) {
            enum lighting_item item = lighting_item_at(mode_index, i);
            const char *label;
            char value[32];
            if (item == LIGHTING_ITEM_MODE) {
                label = "Mode";
                snprintf(value, sizeof(value), "%s", lighting_modes[mode_index].name);
            } else if (item == LIGHTING_ITEM_SPEED) {
                label = "Speed";
                snprintf(value, sizeof(value), "%d",
                         state->lighting_speed[mode_index] + 1);
            } else {
                label = "Brightness";
                snprintf(value, sizeof(value), "%d",
                         state->lighting_brightness[mode_index] + 1);
            }
            if (selected == i) attron(A_REVERSE);
            mvprintw(6 + i, 4, "%-24s < %s >", label, value);
            if (selected == i) attroff(A_REVERSE);
        }
        mvprintw(10, 2, "%s",
                 lighting_has_speed(mode_index) ?
                 "Speed controls the selected lighting effect." :
                 "Brightness controls the selected lighting effect.");
        draw_common_help(12);
        mvprintw(14, 2, "%s", state->message);
        refresh();
        return;
    }

    if (tab == TAB_POWER) {
        char timeout_value[24];
        snprintf(timeout_value, sizeof(timeout_value), "%d minute%s",
                 state->sleep_timeout_minutes,
                 state->sleep_timeout_minutes == 1 ? "" : "s");
        mvprintw(4, 2, "Power management:");
        if (selected == 0) attron(A_REVERSE);
        mvprintw(6, 4, "%-32s < %s >", "Sleep after inactivity",
                 timeout_value);
        if (selected == 0) attroff(A_REVERSE);
        mvprintw(10, 2,
                 "Battery-saving sleep timer; the device supports 1-8 minutes.");
        draw_common_help(12);
        mvprintw(14, 2, "%s", state->message);
        refresh();
        return;
    }

    if (tab == TAB_MACROS) {
        int target = macro_target_index(state);
        const struct macro_definition *macro = &state->macros[target];
        mvprintw(5, 2, "Input / macro editor (%d/%d steps):",
                 macro->step_count, MACRO_MAX_STEPS);
        if (selected == 0) attron(A_REVERSE);
        mvprintw(7, 4, "Target button              < %s >",
                 button_names[target]);
        if (selected == 0) attroff(A_REVERSE);
        int first_step = selected > MACRO_VISIBLE_STEPS ?
                         selected - MACRO_VISIBLE_STEPS : 0;
        for (int row = 0; row < MACRO_VISIBLE_STEPS; ++row) {
            int i = first_step + row;
            if (selected == i + 1) attron(A_REVERSE);
            if (i < macro->step_count) {
                mvprintw(8 + row, 4, "Step %-2d  %-14s  delay %4u ms", i + 1,
                         macro_input_name(&macro->steps[i]),
                         macro->steps[i].delay_ms);
            } else if (i == macro->step_count) {
                mvprintw(8 + row, 4, "+ Add step");
            }
            if (selected == i + 1) attroff(A_REVERSE);
        }
        mvprintw(17, 2,
                 "Arrows: choose input | -/+: delay | K: capture key/click");
        mvprintw(18, 2,
                 "A: add | C: delete | X: clear | Modifiers hold for next input");
        draw_common_help(19);
        mvprintw(21, 2, "%s", state->message);
        refresh();
        return;
    }

    int maximum_polling = maximum_polling_index(&state->device);
    if (maximum_polling >= 0) {
        mvprintw(4, 2, "Maximum polling: %d Hz | DPI: %d-%d",
                 polling_options[maximum_polling].hz, DPI_MIN, DPI_MAX);
    } else {
        mvprintw(4, 2, "Maximum polling: awaiting report | DPI: %d-%d",
                 DPI_MIN, DPI_MAX);
    }

    mvprintw(5, 2, "DPI profiles:");
    for (int i = 0; i < DPI_LEVELS; ++i) {
        if (selected == i) {
            attron(A_REVERSE);
        }
        mvprintw(7 + i, 4, "Level %d%s  #%06X", i,
                 state->active_level == i ? "  *" : "   ",
                 (unsigned int)state->dpi_colors[i]);
        draw_slider(7 + i, state->dpi[i]);
        if (selected == i) {
            attroff(A_REVERSE);
        }
    }

    if (selected == DPI_LEVELS) {
        attron(A_REVERSE);
    }
    mvprintw(polling_row, 4, "Polling rate                 < %d Hz >",
             polling_options[state->polling_index].hz);
    if (selected == DPI_LEVELS) {
        attroff(A_REVERSE);
    }

    mvprintw(status_row, 2,
             "O/P: DPI -/+500 | Space: active | E: edit #RRGGBB");
    draw_common_help(status_row + 1);
    mvprintw(status_row + 2, 2, "%s", state->message);
    refresh();
}

static void initialize_curses(void) {
    setlocale(LC_CTYPE, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    mousemask(ALL_MOUSE_EVENTS, NULL);
    timeout(UI_REFRESH_MS);
    curs_set(0);
}

static struct save_result perform_save(const struct ui_state *state) {
    struct save_result result = {0};

    /* Do not verify against a status report queued before these writes. */
    close_status_handle();
    last_transport_scan_ms = 0;

    int macro_upload_needed = state->macros_dirty;
    int polling_upload_needed =
        state->polling_dirty &&
        maximum_polling_index(&state->device) != 0;
    int dpi_changed = state->dpi_dirty || state->active_level_dirty;

    /* GamingMS Apply order: cmd01, cmd02 X/Y, cmd03, cmd04, cmd05,
     * cmd06, cmd07, cmd08, then macro data. */
    result.buttons = state->buttons_dirty ? save_buttons(state->buttons) :
                                           TRANSFER_ACKNOWLEDGED;
    if (state->buttons_dirty) usleep(BETWEEN_REPORTS_US);
    result.dpi_x = dpi_changed ?
                   save_dpi_axis(state->dpi, state->active_level, 0) :
                   TRANSFER_ACKNOWLEDGED;
    if (dpi_changed) usleep(BETWEEN_REPORTS_US);
    result.dpi_y = dpi_changed ?
                   save_dpi_axis(state->dpi, state->active_level, 1) :
                   TRANSFER_ACKNOWLEDGED;
    if (dpi_changed) usleep(BETWEEN_REPORTS_US);
    int dpi_colors_upload_needed = state->dpi_colors_dirty;
    result.dpi_colors = dpi_colors_upload_needed ?
                        save_dpi_colors(state->dpi_colors) :
                        TRANSFER_ACKNOWLEDGED;
    if (dpi_colors_upload_needed) usleep(BETWEEN_REPORTS_US);
    result.polling = polling_upload_needed ?
                     save_polling(state->polling_index) :
                     TRANSFER_ACKNOWLEDGED;
    if (polling_upload_needed) usleep(BETWEEN_REPORTS_US);
    result.lod = state->lod_dirty ? save_lod(state->lod_mm) :
                                   TRANSFER_ACKNOWLEDGED;
    if (state->lod_dirty) usleep(BETWEEN_REPORTS_US);
    result.lighting = state->lighting_dirty ? save_lighting(state) :
                                              TRANSFER_ACKNOWLEDGED;
    if (state->lighting_dirty) usleep(BETWEEN_REPORTS_US);
    /* Lighting and power are independent OEM settings.  cmd07 is sent only
     * when its own state changes, regardless of the selected light effect. */
    int power_upload_needed = state->power_dirty;
    result.power = power_upload_needed ? save_power(state) :
                                         TRANSFER_ACKNOWLEDGED;
    if (power_upload_needed) usleep(BETWEEN_REPORTS_US);
    result.sensor_advanced = state->sensor_advanced_dirty ?
                             save_sensor_advanced(state->angle_snap,
                                                  state->motion_sync,
                                                  state->ripple_control) :
                             TRANSFER_ACKNOWLEDGED;
    if (state->sensor_advanced_dirty) usleep(BETWEEN_REPORTS_US);
    result.macros = macro_upload_needed ? save_macros(state) :
                                          TRANSFER_ACKNOWLEDGED;
    usleep(SETTLE_DELAY_US);

    for (int attempt = 0; attempt < VERIFY_RETRIES; ++attempt) {
        struct device_status live;
        if (read_live_status(&live)) {
            result.live_status_read = 1;
            int required = 0;
            int checked = 0;
            int matches = 1;
            if (state->active_level_dirty) {
                ++required;
                if (live.report_valid && live.active_level_valid) {
                    ++checked;
                    matches &= live.active_level == state->active_level;
                }
            }
            if (polling_upload_needed) {
                ++required;
                if (live.report_valid && live.polling_valid) {
                    ++checked;
                    matches &= live.polling_hz ==
                               polling_options[state->polling_index].hz;
                }
            }
            if (state->lighting_dirty) {
                ++required;
                if (live.report_valid && live.lighting_mode_valid) {
                    ++checked;
                    matches &= live.lighting_mode_index ==
                               state->lighting_mode_index;
                }
            }
            if (required == 0) {
                result.live_values_match = 1;
                break;
            }
            if (checked == required) {
                result.live_values_checked = 1;
                result.live_values_match = matches;
                if (matches) break;
            }
        }
        if (attempt + 1 < VERIFY_RETRIES) {
            usleep(SETTLE_DELAY_US);
        }
    }
    return result;
}

static int all_transfers_sent(const struct save_result *result) {
    return result->dpi_x >= TRANSFER_SENT &&
           result->dpi_y >= TRANSFER_SENT &&
           result->dpi_colors >= TRANSFER_SENT &&
           result->polling >= TRANSFER_SENT &&
           result->lod >= TRANSFER_SENT &&
           result->sensor_advanced >= TRANSFER_SENT &&
           result->macros >= TRANSFER_SENT &&
           result->buttons >= TRANSFER_SENT &&
           result->lighting >= TRANSFER_SENT &&
           result->power >= TRANSFER_SENT;
}

static int all_transfers_acknowledged(const struct save_result *result) {
    return result->dpi_x == TRANSFER_ACKNOWLEDGED &&
           result->dpi_y == TRANSFER_ACKNOWLEDGED &&
           result->dpi_colors == TRANSFER_ACKNOWLEDGED &&
           result->polling == TRANSFER_ACKNOWLEDGED &&
           result->lod == TRANSFER_ACKNOWLEDGED &&
           result->sensor_advanced == TRANSFER_ACKNOWLEDGED &&
           result->macros == TRANSFER_ACKNOWLEDGED &&
           result->buttons == TRANSFER_ACKNOWLEDGED &&
           result->lighting == TRANSFER_ACKNOWLEDGED &&
           result->power == TRANSFER_ACKNOWLEDGED;
}

static int save_settings(struct ui_state *state) {
    if (!state->device.available) {
        snprintf(state->message, sizeof(state->message),
                 "Save skipped: device is not connected.");
        return 0;
    }
    if (!all_macro_assignments_valid(state)) {
        snprintf(state->message, sizeof(state->message),
                 "Save blocked: a macro assignment is empty or uses the wrong slot.");
        return 0;
    }

    struct save_result result = perform_save(state);

    if (!all_transfers_sent(&result)) {
        snprintf(state->message, sizeof(state->message),
                 "Save failed after multiple connection and transmission attempts.");
        return 0;
    }

    if (result.live_values_checked && !result.live_values_match) {
        snprintf(state->message, sizeof(state->message),
                 "Save rejected: device readback does not match the requested value.");
        return 0;
    }

    int local_saved = persist_state(state);

    if (result.live_status_read) {
        state->device.available = 1;
        state->device.active_level = state->active_level;
        state->device.polling_hz = polling_options[state->polling_index].hz;
        if (state->lighting_dirty) {
            state->device.lighting_mode_index = state->lighting_mode_index;
            state->device.lighting_mode_valid = 1;
        }
    }

    if (!local_saved) {
        snprintf(state->message, sizeof(state->message),
                 "Settings were transmitted, but local state could not be saved; retry S.");
        return 0;
    }

    int has_unverifiable_changes = state->buttons_dirty || state->macros_dirty ||
                                   state->dpi_dirty ||
                                   state->dpi_colors_dirty ||
                                   state->lighting_dirty ||
                                   state->lod_dirty ||
                                   state->sensor_advanced_dirty ||
                                   state->power_dirty;
    if (all_transfers_acknowledged(&result) && result.live_values_match &&
        !has_unverifiable_changes) {
        snprintf(state->message, sizeof(state->message),
                 "Settings saved, acknowledged and verified successfully.");
    } else if (result.live_values_match && !has_unverifiable_changes) {
        snprintf(state->message, sizeof(state->message),
                 "Settings applied and verified. Some firmware ACK responses were not observed.");
    } else if (all_transfers_acknowledged(&result)) {
        snprintf(state->message, sizeof(state->message),
                 "Settings acknowledged; these fields have no live readback.");
    } else {
        snprintf(state->message, sizeof(state->message),
                 "Settings transmitted; these fields have no live readback.");
    }
    state->active_level_dirty = 0;
    state->polling_dirty = 0;
    state->polling_synced_mode = state->device.mode;
    state->buttons_dirty = 0;
    state->macros_dirty = 0;
    state->lighting_dirty = 0;
    state->dpi_dirty = 0;
    state->dpi_colors_dirty = 0;
    state->lod_dirty = 0;
    state->sensor_advanced_dirty = 0;
    state->power_dirty = 0;
    return 1;
}

static int add_macro_step(struct ui_state *state) {
    int target = macro_target_index(state);
    struct macro_definition *macro = &state->macros[target];
    if (macro->step_count >= MACRO_MAX_STEPS) {
        return 0;
    }
    macro->steps[macro->step_count].kind = macro_input_options[0].kind;
    macro->steps[macro->step_count].code = macro_input_options[0].code;
    macro->steps[macro->step_count].uppercase = 0;
    macro->steps[macro->step_count].delay_ms = MACRO_DEFAULT_DELAY_MS;
    ++macro->step_count;
    state->macros_dirty = 1;
    return 1;
}

static int insert_macro_step(struct ui_state *state, int target, int step,
                             unsigned char kind, unsigned char code) {
    struct macro_definition *macro = &state->macros[target];
    if (step < 0 || step > macro->step_count ||
        macro->step_count >= MACRO_MAX_STEPS ||
        macro_input_option_index(kind, code) < 0) {
        return 0;
    }
    memmove(&macro->steps[step + 1], &macro->steps[step],
            (size_t)(macro->step_count - step) * sizeof(macro->steps[0]));
    macro->steps[step].kind = kind;
    macro->steps[step].code = code;
    macro->steps[step].uppercase = 0;
    macro->steps[step].delay_ms = MACRO_DEFAULT_DELAY_MS;
    ++macro->step_count;
    state->macros_dirty = 1;
    return 1;
}

static unsigned char mouse_code_from_event(const MEVENT *event) {
    int native_button = -1;
    if (event->bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)) native_button = 0;
    if (event->bstate & (BUTTON3_CLICKED | BUTTON3_PRESSED)) native_button = 1;
    if (event->bstate & (BUTTON2_CLICKED | BUTTON2_PRESSED)) native_button = 2;
#ifdef BUTTON4_CLICKED
    if (event->bstate & (BUTTON4_CLICKED | BUTTON4_PRESSED)) native_button = 3;
#endif
#ifdef BUTTON5_CLICKED
    if (event->bstate & (BUTTON5_CLICKED | BUTTON5_PRESSED)) native_button = 4;
#endif
    for (int i = 0; i < MACRO_INPUT_OPTION_COUNT; ++i) {
        if (macro_input_options[i].kind == MACRO_EVENT_MOUSE_BUTTON &&
            macro_input_options[i].native_button == native_button) {
            return macro_input_options[i].code;
        }
    }
    return 0;
}

struct native_capture_state {
    CGEventFlags flags;
    int buttons[5];
};

static struct native_capture_state native_capture_state(void) {
    struct native_capture_state state = {
        .flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState),
    };
    for (int button = 0; button < 5; ++button) {
        state.buttons[button] = CGEventSourceButtonState(
            kCGEventSourceStateCombinedSessionState, (CGMouseButton)button);
    }
    return state;
}

static int capture_native_input(const struct native_capture_state *baseline,
                                unsigned char *kind, unsigned char *code) {
    struct native_capture_state current = native_capture_state();
    for (int i = 0; i < MACRO_INPUT_OPTION_COUNT; ++i) {
        const struct macro_input_option *option = &macro_input_options[i];
        int matched = 0;
        if (option->native_flag != 0) {
            CGEventFlags changed = current.flags ^ baseline->flags;
            matched = option->kind == MACRO_EVENT_KEYBOARD ?
                      (changed & option->native_flag) != 0 :
                      (current.flags & ~baseline->flags &
                       option->native_flag) != 0;
        } else if (option->native_button >= 0) {
            int button = option->native_button;
            matched = current.buttons[button] && !baseline->buttons[button];
        }
        if (matched) {
            *kind = option->kind;
            *code = option->code;
            return 1;
        }
    }
    return 0;
}

static void clear_macro_assignment(struct ui_state *state, int button) {
    if (button < 0 || button >= BUTTON_COUNT) {
        return;
    }
    memset(&state->macros[button], 0, sizeof(state->macros[button]));
    state->macros_dirty = 1;
}

static void restore_all_button_defaults(struct ui_state *state) {
    memcpy(state->buttons, default_buttons, sizeof(default_buttons));
    state->buttons_dirty = 1;
}

static void reset_current_tab(struct ui_state *state, enum ui_tab tab) {
    if (tab == TAB_GENERAL) {
        memcpy(state->dpi, default_dpi, sizeof(default_dpi));
        memcpy(state->dpi_colors, default_dpi_colors,
               sizeof(default_dpi_colors));
        state->active_level = 0;
        int default_polling_index = polling_index_from_hz(1000);
        if (state->device.available) {
            int maximum_index = maximum_polling_index(&state->device);
            if (maximum_index >= 0 && default_polling_index > maximum_index) {
                default_polling_index = maximum_index;
            }
        }
        state->polling_index = default_polling_index;
        state->dpi_dirty = 1;
        state->dpi_colors_dirty = 1;
        state->active_level_dirty = 1;
        state->polling_dirty = 1;
    } else if (tab == TAB_BUTTONS) {
        restore_all_button_defaults(state);
    } else if (tab == TAB_SENSOR) {
        state->lod_mm = 2;
        state->angle_snap = 0;
        state->motion_sync = 1;
        state->ripple_control = 0;
        state->lod_dirty = 1;
        state->sensor_advanced_dirty = 1;
    } else if (tab == TAB_LIGHTING) {
        state->lighting_mode_index = DEFAULT_LIGHTING_MODE_INDEX;
        memcpy(state->lighting_speed, default_lighting_speed,
               sizeof(default_lighting_speed));
        memcpy(state->lighting_brightness, default_lighting_brightness,
               sizeof(default_lighting_brightness));
        state->lighting_dirty = 1;
    } else if (tab == TAB_POWER) {
        state->sleep_timeout_minutes = DEFAULT_SLEEP_TIMEOUT_MINUTES;
        state->power_dirty = 1;
    } else {
        clear_macro_assignment(state, macro_target_index(state));
    }
    snprintf(state->message, sizeof(state->message),
             "%s reset to defaults; press S to save.",
             tab == TAB_GENERAL ? "General settings" :
             tab == TAB_BUTTONS ? "Button assignments" :
             tab == TAB_SENSOR ? "Sensor settings" :
             tab == TAB_LIGHTING ? "Lighting settings" :
             tab == TAB_POWER ? "Power settings" : "Selected macro");
}

static void delete_macro_step(struct ui_state *state, int step) {
    int target = macro_target_index(state);
    struct macro_definition *macro = &state->macros[target];
    if (step < 0 || step >= macro->step_count) {
        return;
    }
    memmove(&macro->steps[step], &macro->steps[step + 1],
            (size_t)(macro->step_count - step - 1) * sizeof(macro->steps[0]));
    --macro->step_count;
    memset(&macro->steps[macro->step_count], 0, sizeof(macro->steps[0]));
    state->macros_dirty = 1;
}

static void move_macro_input(struct ui_state *state, int step, int delta) {
    int target = macro_target_index(state);
    struct macro_definition *macro = &state->macros[target];
    if (step < 0 || step >= macro->step_count) {
        return;
    }
    int index = macro_input_option_index(macro->steps[step].kind,
                                         macro->steps[step].code);
    if (index < 0) index = 0;
    index = (index + delta + MACRO_INPUT_OPTION_COUNT) %
            MACRO_INPUT_OPTION_COUNT;
    macro->steps[step].kind = macro_input_options[index].kind;
    macro->steps[step].code = macro_input_options[index].code;
    macro->steps[step].uppercase = 0;
    state->macros_dirty = 1;
}

static void move_macro_delay(struct ui_state *state, int step, int delta) {
    int target = macro_target_index(state);
    struct macro_definition *macro = &state->macros[target];
    if (step < 0 || step >= macro->step_count) {
        return;
    }
    int delay = macro->steps[step].delay_ms + delta;
    if (delay < 0) delay = 0;
    if (delay > MACRO_MAX_DELAY_MS) delay = MACRO_MAX_DELAY_MS;
    macro->steps[step].delay_ms = (unsigned short)delay;
    state->macros_dirty = 1;
}

static int parse_hex_color(const char *text, uint32_t *color) {
    const char *hex = text[0] == '#' ? text + 1 : text;
    if (strlen(hex) != 6) {
        return 0;
    }

    uint32_t value = 0;
    for (int i = 0; i < 6; ++i) {
        unsigned char c = (unsigned char)hex[i];
        unsigned int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return 0;
        }
        value = (value << 4) | digit;
    }
    *color = value;
    return 1;
}

static void edit_dpi_color(struct ui_state *state, int level) {
    char input[8] = {0};
    move(20, 2);
    clrtoeol();
    printw("Level %d color (#RRGGBB): ", level);
    echo();
    curs_set(1);
    timeout(-1);
    int result = getnstr(input, (int)sizeof(input) - 1);
    timeout(UI_REFRESH_MS);
    curs_set(0);
    noecho();

    uint32_t color;
    if (result == OK && parse_hex_color(input, &color)) {
        state->dpi_colors[level] = color;
        state->dpi_colors_dirty = 1;
        snprintf(state->message, sizeof(state->message),
                 "DPI level %d color set to #%06X; press S to save.",
                 level, (unsigned int)color);
    } else {
        snprintf(state->message, sizeof(state->message),
                 "Invalid color. Enter exactly #RRGGBB, for example #FF8000.");
    }
}

static int run_ui(void) {
    struct ui_state state;
    memset(&state, 0, sizeof(state));
    state.device.active_level = -1;
    state.device.battery_percent = -1;
    int state_loaded = load_state(&state);
    refresh_device(&state, 0);
    if (!state_loaded) {
        snprintf(state.message, sizeof(state.message),
                 "No valid saved state; defaults are staged. Press S to apply.");
    } else if (has_pending_settings(&state)) {
        snprintf(state.message, sizeof(state.message),
                 "Saved state migrated; adjusted settings are staged. Press S to apply.");
    } else {
        snprintf(state.message, sizeof(state.message), "Ready.");
    }

    int selected = 0;
    enum ui_tab tab = TAB_GENERAL;
    initialize_curses();

    for (;;) {
        if (tab == TAB_LIGHTING) {
            int count = lighting_item_count(state.lighting_mode_index);
            if (selected >= count) selected = count - 1;
        }
        draw_ui(&state, tab, selected);
        int key = getch();

        if (key == ERR) {
            refresh_device(&state, 0);
            continue;
        }

        if (key == 'q' || key == 'Q' || key == 27) {
            break;
        }

        if (key == '\t' || key == KEY_BTAB) {
            tab = key == KEY_BTAB ? (tab + TAB_COUNT - 1) % TAB_COUNT :
                                    (tab + 1) % TAB_COUNT;
            selected = 0;
            continue;
        }

        int item_count = tab_item_count(&state, tab);

        if (key == KEY_UP) {
            selected = (selected + item_count - 1) % item_count;
        } else if (key == KEY_DOWN) {
            selected = (selected + 1) % item_count;
        } else if (key == 's' || key == 'S') {
            save_settings(&state);
        } else if (key == 'r' || key == 'R') {
            refresh_device(&state, 1);
        } else if (key == 'd' || key == 'D') {
            reset_current_tab(&state, tab);
            if (tab == TAB_MACROS) selected = 0;
        } else if (tab == TAB_BUTTONS &&
                   (key == KEY_LEFT || key == KEY_RIGHT)) {
            int action = button_action_index(&state, selected);
            if (action < 0) action = 0;
            action = (action + (key == KEY_RIGHT ? 1 : BUTTON_ACTION_COUNT - 1)) %
                     BUTTON_ACTION_COUNT;
            set_button_action(&state, selected, action);
            state.buttons_dirty = 1;
        } else if (tab == TAB_SENSOR &&
                   (key == KEY_LEFT || key == KEY_RIGHT)) {
            if (selected == 0) {
                state.lod_mm = state.lod_mm == 1 ? 2 : 1;
                state.lod_dirty = 1;
            } else {
                if (selected == 1) state.angle_snap = !state.angle_snap;
                if (selected == 2) state.motion_sync = !state.motion_sync;
                if (selected == 3) state.ripple_control = !state.ripple_control;
                state.sensor_advanced_dirty = 1;
            }
        } else if (tab == TAB_LIGHTING &&
                   (key == KEY_LEFT || key == KEY_RIGHT)) {
            enum lighting_item item = lighting_item_at(state.lighting_mode_index,
                                                       selected);
            int delta = key == KEY_RIGHT ? 1 : -1;
            if (item == LIGHTING_ITEM_MODE) {
                state.lighting_mode_index =
                    (state.lighting_mode_index + delta +
                     LIGHTING_MODE_COUNT) % LIGHTING_MODE_COUNT;
            } else if (item == LIGHTING_ITEM_SPEED) {
                int index = state.lighting_mode_index;
                state.lighting_speed[index] =
                (state.lighting_speed[index] + delta +
                 LIGHTING_SPEED_MAX + 1) %
                (LIGHTING_SPEED_MAX + 1);
            } else {
                int index = state.lighting_mode_index;
                state.lighting_brightness[index] =
                (state.lighting_brightness[index] + delta +
                 LIGHTING_BRIGHTNESS_MAX + 1) %
                (LIGHTING_BRIGHTNESS_MAX + 1);
            }
            state.lighting_dirty = 1;
        } else if (tab == TAB_POWER &&
                   (key == KEY_LEFT || key == KEY_RIGHT)) {
            int delta = key == KEY_RIGHT ? 1 : -1;
            int range = SLEEP_TIMEOUT_MINUTES_MAX -
                        SLEEP_TIMEOUT_MINUTES_MIN + 1;
            state.sleep_timeout_minutes =
                SLEEP_TIMEOUT_MINUTES_MIN +
                (state.sleep_timeout_minutes -
                 SLEEP_TIMEOUT_MINUTES_MIN + delta + range) % range;
            state.power_dirty = 1;
        } else if (tab == TAB_MACROS && selected == 0 &&
                   (key == KEY_LEFT || key == KEY_RIGHT)) {
            int delta = key == KEY_RIGHT ? 1 : BUTTON_COUNT - 1;
            state.macro_target =
                (macro_target_index(&state) + delta) % BUTTON_COUNT;
            selected = 0;
        } else if (tab == TAB_MACROS && (key == 'a' || key == 'A')) {
            if (add_macro_step(&state)) {
                int target = macro_target_index(&state);
                selected = state.macros[target].step_count;
                snprintf(state.message, sizeof(state.message),
                         "Step added; choose a keyboard, modifier or click input.");
            } else {
                snprintf(state.message, sizeof(state.message),
                         "Macro is already at the %d-step limit.", MACRO_MAX_STEPS);
            }
        } else if (tab == TAB_MACROS && selected > 0 &&
                   (key == 'c' || key == 'C')) {
            delete_macro_step(&state, selected - 1);
            int target = macro_target_index(&state);
            int count = state.macros[target].step_count;
            if (selected > count + 1) selected = count + 1;
        } else if (tab == TAB_MACROS && (key == 'x' || key == 'X')) {
            int target = macro_target_index(&state);
            clear_macro_assignment(&state, target);
            selected = 0;
            snprintf(state.message, sizeof(state.message),
                     "%s macro cleared; press S to save.",
                     button_names[target]);
        } else if (tab == TAB_MACROS && selected > 0 &&
                   (key == KEY_LEFT || key == KEY_RIGHT)) {
            int step = selected - 1;
            int target = macro_target_index(&state);
            if (step == state.macros[target].step_count &&
                !add_macro_step(&state)) {
                continue;
            }
            move_macro_input(&state, step, key == KEY_RIGHT ? 1 : -1);
        } else if (tab == TAB_MACROS && selected > 0 &&
                   (key == '-' || key == '+')) {
            move_macro_delay(&state, selected - 1, key == '+' ? 10 : -10);
        } else if (tab == TAB_MACROS && selected > 0 &&
                   (key == 'k' || key == 'K')) {
            int step = selected - 1;
            int target = macro_target_index(&state);
            int adding = step ==
                         state.macros[target].step_count;
            snprintf(state.message, sizeof(state.message),
                     "Press a keyboard key or mouse button for step %d.",
                     step + 1);
            draw_ui(&state, tab, selected);
            unsigned char kind = MACRO_EVENT_KEYBOARD;
            unsigned char modifier = 0;
            unsigned char code = 0;
            wint_t captured_key = 0;
            struct native_capture_state baseline = native_capture_state();
            timeout(20);
            while (code == 0) {
                wint_t captured = 0;
                int capture_status = get_wch(&captured);
                if (capture_status == KEY_CODE_YES && captured == KEY_MOUSE) {
                    MEVENT event;
                    if (getmouse(&event) == OK) {
                        kind = MACRO_EVENT_MOUSE_BUTTON;
                        code = mouse_code_from_event(&event);
                    }
                } else if (capture_status == OK ||
                           capture_status == KEY_CODE_YES) {
                    captured_key = captured;
                    code = keyboard_code_from_key(captured, &modifier);
                    if (code >= 0x04 && code <= 0x1d) {
                        CGEventFlags flags = CGEventSourceFlagsState(
                            kCGEventSourceStateCombinedSessionState);
                        modifier = captured_letter_modifier(captured, flags);
                    }
                } else if (capture_native_input(&baseline, &kind, &code)) {
                    break;
                }
            }
            timeout(UI_REFRESH_MS);
            if (code != 0) {
                int required = modifier != 0 ? 2 : 1;
                int available = MACRO_MAX_STEPS -
                                state.macros[target].step_count + !adding;
                if (available < required) {
                    snprintf(state.message, sizeof(state.message),
                             "Not enough room in the %d-step macro limit.",
                             MACRO_MAX_STEPS);
                    continue;
                }
                if (!adding) {
                    delete_macro_step(&state, step);
                }
                if (modifier != 0) {
                    insert_macro_step(&state, target, step,
                                      MACRO_EVENT_MODIFIER, modifier);
                    ++step;
                }
                insert_macro_step(&state, target, step, kind, code);
                if (kind == MACRO_EVENT_KEYBOARD && code >= 0x04 &&
                    code <= 0x1d) {
                    state.macros[target].steps[step].uppercase =
                        captured_key >= L'A' && captured_key <= L'Z';
                }
                state.macros_dirty = 1;
                snprintf(state.message, sizeof(state.message),
                         "Captured %s%s for step %d.",
                         modifier != 0 ? "Shift + " : "",
                         macro_input_name(&state.macros[target].steps[step]),
                         modifier != 0 ? step : step + 1);
            } else {
                snprintf(state.message, sizeof(state.message),
                         "That key or mouse button was not reported by the terminal.");
            }
        } else if (tab != TAB_GENERAL) {
            continue;
        } else if (selected < DPI_LEVELS && (key == 'e' || key == 'E')) {
            edit_dpi_color(&state, selected);
        } else if (key == ' ' && selected < DPI_LEVELS) {
            state.active_level = selected;
            state.active_level_dirty = 1;
            snprintf(state.message, sizeof(state.message),
                     "DPI level %d will become active after saving.", selected);
        } else if (selected < DPI_LEVELS && (key == 'o' || key == 'O')) {
            move_dpi(&state.dpi[selected], -DPI_LARGE_STEP);
            state.dpi_dirty = 1;
        } else if (selected < DPI_LEVELS && (key == 'p' || key == 'P')) {
            move_dpi(&state.dpi[selected], DPI_LARGE_STEP);
            state.dpi_dirty = 1;
        } else if (selected < DPI_LEVELS && (key == KEY_LEFT || key == '-')) {
            move_dpi(&state.dpi[selected], -DPI_STEP);
            state.dpi_dirty = 1;
        } else if (selected < DPI_LEVELS && (key == KEY_RIGHT || key == '+')) {
            move_dpi(&state.dpi[selected], DPI_STEP);
            state.dpi_dirty = 1;
        } else if (selected == DPI_LEVELS &&
                   key == KEY_LEFT && state.polling_index > 0) {
            --state.polling_index;
            state.polling_dirty = 1;
        } else if (selected == DPI_LEVELS && key == KEY_RIGHT) {
            int maximum_index = maximum_polling_index(&state.device);
            if (maximum_index >= 0 && state.polling_index < maximum_index) {
                ++state.polling_index;
                state.polling_dirty = 1;
            }
        }
    }

    endwin();
    close_status_handle();
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 1) {
        fprintf(stderr, "Usage: %s\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "mousectl requires an interactive terminal.\n");
        return EXIT_FAILURE;
    }

    int instance_lock = open(INSTANCE_LOCK_FILE, O_CREAT | O_RDWR, 0600);
    if (instance_lock < 0 || flock(instance_lock, LOCK_EX | LOCK_NB) != 0) {
        if (instance_lock >= 0) {
            close(instance_lock);
        }
        fprintf(stderr, "Another mousectl instance is already running.\n");
        return EXIT_FAILURE;
    }

    if (hid_init() != 0) {
        fprintf(stderr, "Failed to initialize HIDAPI.\n");
        close(instance_lock);
        return EXIT_FAILURE;
    }
    if (libusb_init(&usb_context) != 0) {
        fprintf(stderr, "Failed to initialize USB access.\n");
        hid_exit();
        close(instance_lock);
        return EXIT_FAILURE;
    }

    hid_darwin_set_open_exclusive(0);
    int result = run_ui();
    libusb_exit(usb_context);
    hid_exit();
    close(instance_lock);
    return result;
}
