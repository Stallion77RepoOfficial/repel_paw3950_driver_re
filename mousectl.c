#define _XOPEN_SOURCE_EXTENDED 1

#include <hidapi/hidapi.h>
#include <hidapi/hidapi_darwin.h>
#include <libusb.h>
#include <ApplicationServices/ApplicationServices.h>
#include <ctype.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#define RECEIVER_PID 0x026F
#define WIRED_PID 0x026E
#define BLUETOOTH_PID 0x026D

#define REPORT_SIZE 32
#define STATUS_REPORT_ID 0x04
#define INFO_REPORT_ID 0x05
#define CONFIG_REPORT_ID 0x06
#define PAIRING_REPORT_ID 0xBC
#define PAIRING_REPORT_SIZE 8
#define PAIRING_START 0x03
#define CLEAR_ALL 0xFF
#define DPI_AXIS_X 0x02
#define DPI_AXIS_Y 0x12

#define DPI_LEVELS 7
#define DPI_COLOR_SLOTS 8
#define DPI_ALL_LEVELS ((1u << DPI_LEVELS) - 1)
#define DPI_MIN 50
#define DPI_MAX 30000
#define DPI_STEP 50
#define DPI_LARGE_STEP 500
#define DPI_LOCK_MIN 200
#define DPI_LOCK_STEP 100
#define DPI_LOCK_LEVELS 11
#define BUTTON_COUNT 6
#define DEBOUNCE_MAX_MS 30
#define SLEEP_MINUTES_MIN 1
#define SLEEP_MINUTES_MAX 8
#define LIGHTING_MODE_COUNT 4
#define DEFAULT_LIGHTING_MODE 1
#define LIGHTING_SPEED_MAX 2
#define LIGHTING_BRIGHTNESS_MAX 3
#define FIRE_INTERVAL_MIN 5
#define FIRE_INTERVAL_MAX 255
#define FIRE_INTERVAL_STEP 5
#define REPEAT_COUNT_MAX 255
#define MOVE_DISTANCE_MAX 127
#define MACRO_DEFAULT_DELAY_MS 50
#define MACRO_MAX_DELAY_MS 4095
#define MACRO_DEFAULT_MOVE 10
#define MACRO_CHUNK_SIZE 25
#define MACRO_MAX_CHUNKS 15
#define MACRO_MAX_PAYLOAD_SIZE (MACRO_CHUNK_SIZE * MACRO_MAX_CHUNKS)
#define MACRO_EVENT_SIZE 3
#define MACRO_MAX_STEPS (MACRO_MAX_PAYLOAD_SIZE / (MACRO_EVENT_SIZE * 2))
#define MACRO_VISIBLE_STEPS 8
#define MACRO_HEADER_ROWS 2

#define STATE_FILE_NAME ".mousectl-state"
#define LOCK_FILE_NAME ".mousectl.lock"
#define STATE_HEADER "mousectl-state 2"
#define STATE_LINE_MAX 8192

#define OPEN_RETRIES 3
#define ACK_READ_RETRIES 4
#define VERIFY_RETRIES 2
#define ACK_DELAY_US 60000
#define MACRO_ACK_DELAY_US 120000
#define RETRY_DELAY_US 150000
#define BETWEEN_REPORTS_US 100000
#define SETTLE_DELAY_US 350000
#define TRANSPORT_SCAN_INTERVAL_MS 100
#define DISCONNECT_GRACE_MS 500
#define UI_REFRESH_MS 100
#define PAIRING_TIMEOUT_MS 30000

#define HINT_ROW 18
#define MESSAGE_ROW 21

enum command {
    CMD_BUTTONS = 0x01,
    CMD_DPI = 0x02,
    CMD_DPI_COLORS = 0x03,
    CMD_POLLING = 0x04,
    CMD_LIFT_OFF = 0x05,
    CMD_LIGHTING = 0x06,
    CMD_POWER = 0x07,
    CMD_SENSOR = 0x08,
    CMD_DEBOUNCE = 0x09,
    CMD_FACTORY_RESET = 0x0F,
    CMD_MACRO_DATA = 0x10,
    CMD_MACRO_CLEAR = 0x11
};

enum transfer_result {
    TRANSFER_FAILED = 0,
    TRANSFER_SENT,
    TRANSFER_ACKNOWLEDGED
};

enum dirty_flag {
    DIRTY_BUTTONS = 1u << 0,
    DIRTY_DPI = 1u << 1,
    DIRTY_ACTIVE_LEVEL = 1u << 2,
    DIRTY_DPI_COLORS = 1u << 3,
    DIRTY_POLLING = 1u << 4,
    DIRTY_LIFT_OFF = 1u << 5,
    DIRTY_DEBOUNCE = 1u << 6,
    DIRTY_LIGHTING = 1u << 7,
    DIRTY_POWER = 1u << 8,
    DIRTY_SENSOR = 1u << 9,
    DIRTY_MACROS = 1u << 10,
    DIRTY_ALL = (1u << 11) - 1
};

enum device_mode {
    MODE_NONE = 0,
    MODE_WIRED,
    MODE_BLUETOOTH,
    MODE_RECEIVER
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
    TAB_DEVICE,
    TAB_COUNT
};

enum general_row {
    GENERAL_ROW_AXES = DPI_LEVELS,
    GENERAL_ROW_POLLING,
    GENERAL_ROW_COUNT
};

enum sensor_row {
    SENSOR_ROW_LIFT_OFF = 0,
    SENSOR_ROW_DEBOUNCE,
    SENSOR_ROW_ANGLE_SNAP,
    SENSOR_ROW_MOTION_SYNC,
    SENSOR_ROW_RIPPLE,
    SENSOR_ROW_COUNT
};

enum power_row {
    POWER_ROW_SLEEP = 0,
    POWER_ROW_LIGHT_OFF,
    POWER_ROW_COUNT
};

enum device_row {
    DEVICE_ROW_PAIR = 0,
    DEVICE_ROW_RESET,
    DEVICE_ROW_COUNT
};

enum dpi_axes {
    AXES_LINKED = 0,
    AXES_X,
    AXES_Y,
    AXES_COUNT
};

enum lighting_item {
    LIGHTING_ITEM_MODE,
    LIGHTING_ITEM_SPEED,
    LIGHTING_ITEM_BRIGHTNESS
};

enum macro_event_kind {
    MACRO_EVENT_KEYBOARD = 0,
    MACRO_EVENT_MODIFIER,
    MACRO_EVENT_MOUSE_BUTTON,
    MACRO_EVENT_MOVE_X,
    MACRO_EVENT_MOVE_Y,
    MACRO_EVENT_KIND_COUNT
};

enum macro_loop {
    MACRO_LOOP_HOLD = 1,
    MACRO_LOOP_COUNT = 2,
    MACRO_LOOP_TOGGLE = 4
};

enum button_type {
    BUTTON_TYPE_DISABLED = 0x00,
    BUTTON_TYPE_MOUSE = 0x10,
    BUTTON_TYPE_FIRE = 0x30,
    BUTTON_TYPE_DPI = 0x40,
    BUTTON_TYPE_DPI_LOCK = 0x50,
    BUTTON_TYPE_LIGHTING = 0x60,
    BUTTON_TYPE_KEY = 0x70,
    BUTTON_TYPE_MEDIA = 0x80,
    BUTTON_TYPE_MACRO = 0x90
};

enum button_action {
    ACTION_LEFT = 0,
    ACTION_RIGHT,
    ACTION_MIDDLE,
    ACTION_BACKWARD,
    ACTION_FORWARD,
    ACTION_DPI_LOOP,
    ACTION_DPI_UP,
    ACTION_DPI_DOWN,
    ACTION_DPI_LOCK,
    ACTION_TRIPLE_CLICK,
    ACTION_FIRE,
    ACTION_LIGHTING_SWITCH,
    ACTION_KEY,
    ACTION_MEDIA,
    ACTION_MACRO,
    ACTION_DISABLED,
    ACTION_COUNT
};

enum state_load {
    STATE_MISSING = 0,
    STATE_LOADED,
    STATE_IMPORTED,
    STATE_INVALID
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

struct mode_spec {
    enum device_mode mode;
    unsigned short pid;
    int max_polling_hz;
    const char *name;
    enum transfer_backend backend;
    int reports_online;
    int reports_polling_cap;
};

struct device_status {
    int connected;
    int online;
    int charge_state;
    enum device_mode mode;
    int active_level;
    int polling_hz;
    int lighting_mode;
    int battery_percent;
    int max_polling_hz;
    int sensor_id;
    unsigned int report_count;
};

struct macro_step {
    unsigned char kind;
    unsigned char code;
    unsigned char uppercase;
    unsigned short delay_ms;
};

struct macro_definition {
    int step_count;
    unsigned char loop_type;
    unsigned char loop_count;
    struct macro_step steps[MACRO_MAX_STEPS];
};

struct settings {
    int dpi[2][DPI_LEVELS];
    int dpi_linked;
    unsigned int dpi_enabled;
    uint32_t dpi_colors[DPI_COLOR_SLOTS];
    int active_level;
    int polling_index;
    int lift_off_mm;
    int debounce_ms;
    int angle_snap;
    int motion_sync;
    int ripple_control;
    unsigned char buttons[BUTTON_COUNT][3];
    int lighting_mode;
    int lighting_speed[LIGHTING_MODE_COUNT];
    int lighting_brightness[LIGHTING_MODE_COUNT];
    int sleep_minutes;
    int light_off_on_move;
    struct macro_definition macros[BUTTON_COUNT];
};

struct ui_state {
    struct settings settings;
    unsigned int dirty;
    int dpi_axis;
    int macro_target;
    int last_battery;
    int firmware_checked;
    long long last_seen_ms;
    struct device_status device;
    char firmware_tag[5];
    char message[256];
};

struct action_info {
    const char *name;
    unsigned char code[3];
};

struct media_key {
    const char *name;
    unsigned short usage;
};

struct sensor_info {
    unsigned char id;
    const char *name;
};

struct macro_input_option {
    const char *name;
    unsigned char kind;
    unsigned char code;
    wint_t captured_key;
    wint_t shifted_key;
    CGEventFlags native_flag;
    int native_button;
};

struct captured_input {
    unsigned char kind;
    unsigned char code;
    unsigned char modifiers;
    int uppercase;
};

struct native_capture_state {
    CGEventFlags flags;
    int buttons[5];
};

struct channel {
    enum transfer_backend backend;
    hid_device *hid;
    libusb_device_handle *usb;
    int interface_number;
};

struct setting_writer {
    unsigned int flags;
    const char *name;
    enum transfer_result (*write)(const struct settings *settings);
};

static const struct polling_option polling_options[] = {
    {125, 0x08}, {250, 0x04}, {500, 0x02}, {1000, 0x01},
    {2000, 0x11}, {4000, 0x12}, {8000, 0x14},
};

#define POLLING_OPTION_COUNT ((int)(sizeof(polling_options) / sizeof(polling_options[0])))

static const struct mode_spec mode_specs[] = {
    {MODE_WIRED, WIRED_PID, 1000, "Wired", BACKEND_LIBUSB, 0, 1},
    {MODE_BLUETOOTH, BLUETOOTH_PID, 125, "Bluetooth", BACKEND_HIDAPI, 0, 0},
    {MODE_RECEIVER, RECEIVER_PID, 8000, "2.4 GHz", BACKEND_HIDAPI, 1, 1},
};

#define MODE_SPEC_COUNT ((int)(sizeof(mode_specs) / sizeof(mode_specs[0])))

static const int default_dpi[DPI_LEVELS] = {
    800, 1600, 2400, 3200, 6400, 12000, 16000,
};

static const uint32_t default_dpi_colors[DPI_COLOR_SLOTS] = {
    0xff0000, 0x00ff00, 0x0000ff, 0x00ffff,
    0xffff00, 0xff00ff, 0xffffff, 0xffffff,
};

static const unsigned char default_buttons[BUTTON_COUNT][3] = {
    {0x10, 0x01, 0x00}, {0x10, 0x02, 0x00},
    {0x10, 0x04, 0x00}, {0x10, 0x10, 0x00},
    {0x10, 0x08, 0x00}, {0x40, 0x01, 0x00},
};

static const char *button_names[BUTTON_COUNT] = {
    "Left", "Right", "Wheel", "Forward", "Backward", "DPI"
};

static const struct action_info actions[ACTION_COUNT] = {
    [ACTION_LEFT] = {"Left click", {0x10, 0x01, 0x00}},
    [ACTION_RIGHT] = {"Right click", {0x10, 0x02, 0x00}},
    [ACTION_MIDDLE] = {"Wheel click", {0x10, 0x04, 0x00}},
    [ACTION_BACKWARD] = {"Backward", {0x10, 0x08, 0x00}},
    [ACTION_FORWARD] = {"Forward", {0x10, 0x10, 0x00}},
    [ACTION_DPI_LOOP] = {"DPI loop", {0x40, 0x01, 0x00}},
    [ACTION_DPI_UP] = {"DPI +", {0x40, 0x02, 0x00}},
    [ACTION_DPI_DOWN] = {"DPI -", {0x40, 0x03, 0x00}},
    [ACTION_DPI_LOCK] = {"DPI lock", {0x50, 0x04, 0x00}},
    [ACTION_TRIPLE_CLICK] = {"Triple click", {0x30, 0x32, 0x03}},
    [ACTION_FIRE] = {"Fire key", {0x30, 0x1e, 0x00}},
    [ACTION_LIGHTING_SWITCH] = {"Lighting switch", {0x60, 0x04, 0x00}},
    [ACTION_KEY] = {"Key", {0x70, 0x00, 0x04}},
    [ACTION_MEDIA] = {"Media", {0x80, 0xcd, 0x00}},
    [ACTION_MACRO] = {"Macro", {0x90, 0x00, 0x00}},
    [ACTION_DISABLED] = {"Disabled", {0x00, 0x00, 0x00}},
};

static const struct media_key media_keys[] = {
    {"Play/Pause", 0x00cd}, {"Next track", 0x00b5},
    {"Previous track", 0x00b6}, {"Stop", 0x00b7},
    {"Mute", 0x00e2}, {"Volume up", 0x00e9}, {"Volume down", 0x00ea},
    {"Media player", 0x0183}, {"Mail", 0x018a},
    {"Calculator", 0x0192}, {"My computer", 0x0194},
};

#define MEDIA_KEY_COUNT ((int)(sizeof(media_keys) / sizeof(media_keys[0])))

static const struct sensor_info sensors[] = {
    {0x04, "PAW3104"}, {0x11, "PAW3311"}, {0x12, "PAW3212"},
    {0x25, "PAW3325"}, {0x27, "PAW3327"}, {0x35, "PAW3335"},
    {0x50, "PAW3950"}, {0x60, "PAW3360"}, {0x70, "PAW3370"},
    {0x89, "PAW3389"}, {0x95, "PAW3395"}, {0x99, "PAW3399"},
};

#define SENSOR_COUNT ((int)(sizeof(sensors) / sizeof(sensors[0])))

static const struct lighting_mode_option lighting_modes[LIGHTING_MODE_COUNT] = {
    {"Off", 0, 0, 0},
    {"Steady", 3, 0, 1},
    {"Breathing", 2, 1, 0},
    {"Neon", 4, 1, 0},
};

static const int default_lighting_speed[LIGHTING_MODE_COUNT] = {
    0, 1, 1, 1,
};

static const int default_lighting_brightness[LIGHTING_MODE_COUNT] = {
    0, 3, 1, 1,
};

#define MACRO_KEY(name, code, captured, shifted) \
    {name, MACRO_EVENT_KEYBOARD, code, captured, shifted, 0, -1}
#define MACRO_NATIVE_KEY(name, code, flag) \
    {name, MACRO_EVENT_KEYBOARD, code, 0, 0, flag, -1}
#define MACRO_MODIFIER(name, code, flag) \
    {name, MACRO_EVENT_MODIFIER, code, 0, 0, flag, -1}
#define MACRO_MOUSE(name, code, button) \
    {name, MACRO_EVENT_MOUSE_BUTTON, code, 0, 0, 0, button}
#define MACRO_MOVE(name, kind, code) \
    {name, kind, code, 0, 0, 0, -1}

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
    MACRO_MOVE("Move left", MACRO_EVENT_MOVE_X, 256 - MACRO_DEFAULT_MOVE),
    MACRO_MOVE("Move right", MACRO_EVENT_MOVE_X, MACRO_DEFAULT_MOVE),
    MACRO_MOVE("Move up", MACRO_EVENT_MOVE_Y, 256 - MACRO_DEFAULT_MOVE),
    MACRO_MOVE("Move down", MACRO_EVENT_MOVE_Y, MACRO_DEFAULT_MOVE),
};

#define MACRO_INPUT_OPTION_COUNT \
    ((int)(sizeof(macro_input_options) / sizeof(macro_input_options[0])))

static unsigned short configuration_pid = RECEIVER_PID;
static hid_device *status_handle;
static unsigned short status_pid;
static libusb_context *usb_context;
static long long last_transport_scan_ms;
static unsigned int cached_transport_presence;
static unsigned char transaction_id = 1;
static char state_path[PATH_MAX];
static char lock_path[PATH_MAX];

static int clamp_int(int value, int minimum, int maximum) {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static int wrap_int(int value, int count) {
    return ((value % count) + count) % count;
}

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static unsigned char checksum(const unsigned char *report, size_t length) {
    unsigned int sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum += report[i];
    }
    return (unsigned char)sum;
}

static unsigned char next_transaction_id(void) {
    return transaction_id++;
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

static int polling_capacity_to_hz(unsigned char capability) {
    switch (capability) {
        case 1: return 1000;
        case 2: return 2000;
        case 4: return 4000;
        case 8: return 8000;
        default: return 0;
    }
}

static int lighting_index_from_protocol_id(int protocol_id) {
    for (int i = 0; i < LIGHTING_MODE_COUNT; ++i) {
        if (lighting_modes[i].protocol_id == protocol_id) {
            return i;
        }
    }
    return -1;
}

static const struct mode_spec *mode_spec(enum device_mode mode) {
    for (int i = 0; i < MODE_SPEC_COUNT; ++i) {
        if (mode_specs[i].mode == mode) {
            return &mode_specs[i];
        }
    }
    return NULL;
}

static const struct mode_spec *mode_spec_for_pid(unsigned short pid) {
    for (int i = 0; i < MODE_SPEC_COUNT; ++i) {
        if (mode_specs[i].pid == pid) {
            return &mode_specs[i];
        }
    }
    return NULL;
}

static const char *sensor_name(int id, char *buffer, size_t buffer_size) {
    for (int i = 0; i < SENSOR_COUNT; ++i) {
        if (sensors[i].id == id) {
            return sensors[i].name;
        }
    }
    if (id == 0) {
        return "--";
    }
    snprintf(buffer, buffer_size, "0x%02X", id);
    return buffer;
}

static int maximum_polling_index(const struct device_status *device) {
    if (!device->connected || device->max_polling_hz <= 0) {
        return POLLING_OPTION_COUNT - 1;
    }
    int index = polling_index_from_hz(device->max_polling_hz);
    return index >= 0 ? index : 0;
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

static hid_device *open_configuration_interface(unsigned short pid) {
    struct hid_device_info *devices = hid_enumerate(TARGET_VID, pid);
    hid_device *handle = NULL;

    for (struct hid_device_info *device = devices;
         device != NULL && handle == NULL; device = device->next) {
        if (device->usage_page == 0x0001 && device->usage == 0x0006) {
            handle = hid_open_path(device->path);
        }
    }

    hid_free_enumeration(devices);
    return handle;
}

static int channel_open(unsigned short pid, struct channel *channel) {
    const struct mode_spec *spec = mode_spec_for_pid(pid);
    memset(channel, 0, sizeof(*channel));
    channel->backend = spec != NULL ? spec->backend : BACKEND_HIDAPI;
    if (channel->backend == BACKEND_LIBUSB) {
        channel->usb = libusb_open_device_with_vid_pid(usb_context, TARGET_VID, pid);
        if (channel->usb == NULL) {
            return 0;
        }
        channel->interface_number = hid_interface_number(channel->usb);
        return 1;
    }
    channel->hid = open_configuration_interface(pid);
    return channel->hid != NULL;
}

static void channel_close(struct channel *channel) {
    if (channel->hid != NULL) {
        hid_close(channel->hid);
    }
    if (channel->usb != NULL) {
        libusb_close(channel->usb);
    }
    memset(channel, 0, sizeof(*channel));
}

static int channel_set_feature(struct channel *channel,
                               const unsigned char *report, size_t size) {
    if (size > REPORT_SIZE) {
        return 0;
    }
    if (channel->backend == BACKEND_LIBUSB) {
        unsigned char buffer[REPORT_SIZE];
        memcpy(buffer, report, size);
        return libusb_control_transfer(
                   channel->usb, 0x21, 0x09, (uint16_t)(0x0300 | report[0]),
                   (uint16_t)channel->interface_number, buffer,
                   (uint16_t)size, 1000) == (int)size;
    }
    return hid_send_feature_report(channel->hid, report, size) == (int)size;
}

static int channel_get_feature(struct channel *channel, unsigned char *report,
                               size_t size) {
    if (channel->backend == BACKEND_LIBUSB) {
        return libusb_control_transfer(
            channel->usb, 0xa1, 0x01, (uint16_t)(0x0300 | report[0]),
            (uint16_t)channel->interface_number, report, (uint16_t)size, 1000);
    }
    return hid_get_feature_report(channel->hid, report, size);
}

static enum transfer_result transfer_config_report(
        const unsigned char template_report[REPORT_SIZE],
        unsigned int ack_delay_us) {
    for (int open_attempt = 0; open_attempt < OPEN_RETRIES; ++open_attempt) {
        struct channel channel;
        if (!channel_open(configuration_pid, &channel)) {
            usleep(RETRY_DELAY_US);
            continue;
        }

        unsigned char report[REPORT_SIZE];
        memcpy(report, template_report, sizeof(report));
        enum transfer_result result = TRANSFER_FAILED;
        for (int attempt = 0; attempt < ACK_READ_RETRIES; ++attempt) {
            report[2] = next_transaction_id();
            report[4] = 0;
            report[4] = checksum(report, sizeof(report));
            if (channel_set_feature(&channel, report, sizeof(report))) {
                result = TRANSFER_SENT;
            }

            usleep(ack_delay_us);
            unsigned char response[REPORT_SIZE] = {CONFIG_REPORT_ID};
            if (channel_get_feature(&channel, response, sizeof(response)) >= 2 &&
                response[1] != 0) {
                result = TRANSFER_ACKNOWLEDGED;
                break;
            }
            if (attempt + 1 < ACK_READ_RETRIES) {
                usleep(ACK_DELAY_US);
            }
        }

        channel_close(&channel);
        if (result != TRANSFER_FAILED) {
            return result;
        }
        usleep(RETRY_DELAY_US);
    }
    return TRANSFER_FAILED;
}

static enum transfer_result combine_transfer_results(
        enum transfer_result aggregate, enum transfer_result current) {
    return aggregate < current ? aggregate : current;
}

static void begin_command(unsigned char report[REPORT_SIZE],
                          enum command command, unsigned char parameter) {
    memset(report, 0, REPORT_SIZE);
    report[0] = CONFIG_REPORT_ID;
    report[1] = (unsigned char)command;
    report[3] = parameter;
}

static enum transfer_result send_command(enum command command,
                                         const unsigned char *payload,
                                         size_t size) {
    unsigned char report[REPORT_SIZE];
    begin_command(report, command, 0x01);
    memcpy(&report[5], payload, size);
    return transfer_config_report(report, ACK_DELAY_US);
}

static int send_pairing_request(void) {
    struct channel channel;
    if (!channel_open(RECEIVER_PID, &channel)) {
        return 0;
    }
    unsigned char report[PAIRING_REPORT_SIZE] = {PAIRING_REPORT_ID, PAIRING_START};
    int sent = channel_set_feature(&channel, report, sizeof(report));
    channel_close(&channel);
    return sent;
}

static int read_firmware_tag(unsigned short pid, char tag[5]) {
    struct channel channel;
    if (!channel_open(pid, &channel)) {
        return 0;
    }
    unsigned char report[REPORT_SIZE] = {INFO_REPORT_ID};
    int length = channel_get_feature(&channel, report, sizeof(report));
    channel_close(&channel);
    if (length < 5 || report[0] != INFO_REPORT_ID) {
        return 0;
    }
    for (int i = 0; i < 4; ++i) {
        if (!isprint(report[1 + i])) {
            return 0;
        }
        tag[i] = (char)report[1 + i];
    }
    tag[4] = '\0';
    return 1;
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
        const struct mode_spec *spec = mode_spec_for_pid(device->product_id);
        if (spec != NULL) {
            presence |= 1u << spec->mode;
        }
    }
    hid_free_enumeration(devices);
    cached_transport_presence = presence;
    last_transport_scan_ms = now;
    return presence;
}

static void device_status_clear(struct device_status *status) {
    memset(status, 0, sizeof(*status));
    status->active_level = -1;
    status->lighting_mode = -1;
    status->battery_percent = -1;
}

static void device_status_init(struct device_status *status,
                               const struct mode_spec *spec) {
    device_status_clear(status);
    status->connected = 1;
    status->online = 1;
    status->mode = spec->mode;
    status->max_polling_hz = spec->max_polling_hz;
}

static int decode_status_report(const unsigned char *report, int length,
                                const struct mode_spec *spec,
                                struct device_status *status) {
    if (length < 10 || report[0] != STATUS_REPORT_ID) {
        return 0;
    }

    ++status->report_count;
    status->online = !spec->reports_online || report[1] != 0;
    if (!status->online) {
        return 1;
    }

    int polling_index = polling_index_from_code(report[3]);
    status->active_level = report[2] < DPI_LEVELS ? report[2] : -1;
    status->polling_hz = polling_index >= 0 ? polling_options[polling_index].hz : 0;
    status->battery_percent = report[5] == 0x02 ? 100 :
                              (report[4] <= 100 ? report[4] : -1);
    status->charge_state = report[5] <= 0x02 ? report[5] : 0;
    status->lighting_mode = lighting_index_from_protocol_id(report[6]);
    int capacity = polling_capacity_to_hz(report[8]);
    if (spec->reports_polling_cap && capacity > 0) {
        status->max_polling_hz = capacity;
    }
    status->sensor_id = report[9];
    return 1;
}

static void read_status_reports(const struct mode_spec *spec,
                                struct device_status *status) {
    if (status_handle == NULL || status_pid != spec->pid) {
        close_status_handle();
        status_handle = open_configuration_interface(spec->pid);
        if (status_handle == NULL) {
            return;
        }
        if (hid_set_nonblocking(status_handle, 1) < 0) {
            close_status_handle();
            return;
        }
        status_pid = spec->pid;
    }

    for (;;) {
        unsigned char report[REPORT_SIZE] = {0};
        int length = hid_read(status_handle, report, sizeof(report));
        if (length < 0) {
            close_status_handle();
            return;
        }
        if (length == 0) {
            return;
        }
        decode_status_report(report, length, spec, status);
    }
}

static int read_live_status(const struct device_status *previous,
                            struct device_status *status) {
    unsigned int presence = detect_transports();
    for (int i = 0; i < MODE_SPEC_COUNT; ++i) {
        const struct mode_spec *spec = &mode_specs[i];
        if ((presence & (1u << spec->mode)) == 0) {
            continue;
        }
        if (previous->connected && previous->mode == spec->mode) {
            *status = *previous;
        } else {
            device_status_init(status, spec);
        }
        read_status_reports(spec, status);
        configuration_pid = spec->pid;
        return 1;
    }

    close_status_handle();
    return 0;
}

static int macro_kind_is_move(unsigned char kind) {
    return kind == MACRO_EVENT_MOVE_X || kind == MACRO_EVENT_MOVE_Y;
}

static int macro_input_option_index(unsigned char kind, unsigned char code) {
    for (int i = 0; i < MACRO_INPUT_OPTION_COUNT; ++i) {
        const struct macro_input_option *option = &macro_input_options[i];
        if (option->kind != kind) {
            continue;
        }
        if (macro_kind_is_move(kind) ? (option->code & 0x80) == (code & 0x80) :
                                       option->code == code) {
            return i;
        }
    }
    return -1;
}

static int macro_step_valid(const struct macro_step *step) {
    if (step->delay_ms > MACRO_MAX_DELAY_MS ||
        macro_input_option_index(step->kind, step->code) < 0) {
        return 0;
    }
    if (macro_kind_is_move(step->kind)) {
        int distance = (signed char)step->code;
        return distance != 0 && abs(distance) <= MOVE_DISTANCE_MAX &&
               !step->uppercase;
    }
    return !step->uppercase ||
           (step->kind == MACRO_EVENT_KEYBOARD && step->code >= 0x04 &&
            step->code <= 0x1d);
}

static int macro_loop_valid(int loop_type, int loop_count) {
    return (loop_type == MACRO_LOOP_COUNT && loop_count >= 1 &&
            loop_count <= REPEAT_COUNT_MAX) ||
           ((loop_type == MACRO_LOOP_HOLD || loop_type == MACRO_LOOP_TOGGLE) &&
            loop_count >= 0 && loop_count <= REPEAT_COUNT_MAX);
}

static const char *macro_step_name(const struct macro_step *step, char *buffer,
                                   size_t buffer_size) {
    int index = macro_input_option_index(step->kind, step->code);
    if (index < 0) {
        return "Unknown";
    }
    if (macro_kind_is_move(step->kind)) {
        snprintf(buffer, buffer_size, "%s %d", macro_input_options[index].name,
                 abs((signed char)step->code));
        return buffer;
    }
    if (step->uppercase && step->kind == MACRO_EVENT_KEYBOARD &&
        step->code >= 0x04 && step->code <= 0x1d) {
        snprintf(buffer, buffer_size, "%c", 'A' + step->code - 0x04);
        return buffer;
    }
    return macro_input_options[index].name;
}

static int macro_event_prefix(unsigned char kind) {
    static const unsigned char prefixes[MACRO_EVENT_KIND_COUNT] = {
        0x30, 0x20, 0x10, 0x40, 0x50,
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
    if (macro->step_count <= 0 || macro->step_count > MACRO_MAX_STEPS) {
        return -1;
    }

    int modifier_prefix = macro_event_prefix(MACRO_EVENT_MODIFIER);
    unsigned char held_modifiers[MACRO_MAX_STEPS];
    int held_count = 0;
    size_t offset = 0;
    for (int i = 0; i < macro->step_count; ++i) {
        const struct macro_step *step = &macro->steps[i];
        int prefix = macro_event_prefix(step->kind);
        if (prefix < 0 || !macro_step_valid(step)) {
            return -1;
        }
        int implicit_shift = step->uppercase;
        for (int held = 0; held < held_count; ++held) {
            if (held_modifiers[held] & 0x22) implicit_shift = 0;
        }
        if (implicit_shift &&
            !append_macro_event(payload, capacity, &offset, modifier_prefix,
                                0, 0, 0x02)) {
            return -1;
        }
        if (!append_macro_event(payload, capacity, &offset, prefix, 0,
                                step->delay_ms, step->code)) {
            return -1;
        }
        if (step->kind == MACRO_EVENT_MODIFIER) {
            held_modifiers[held_count++] = step->code;
            if (i + 1 < macro->step_count) {
                continue;
            }
        } else if (!macro_kind_is_move(step->kind) &&
                   !append_macro_event(payload, capacity, &offset, prefix, 1,
                                       0, step->code)) {
            return -1;
        }
        if (implicit_shift &&
            !append_macro_event(payload, capacity, &offset, modifier_prefix,
                                1, 0, 0x02)) {
            return -1;
        }
        while (held_count > 0) {
            if (!append_macro_event(payload, capacity, &offset,
                                    modifier_prefix, 1, 0,
                                    held_modifiers[--held_count])) {
                return -1;
            }
        }
    }
    return (int)offset;
}

static int button_is_macro(const unsigned char code[3]) {
    return code[0] == BUTTON_TYPE_MACRO;
}

static int button_uses_own_macro(const unsigned char code[3], int button) {
    return button_is_macro(code) && (code[1] & 0x0f) == button + 1;
}

static void macro_button_code(const struct macro_definition *macro, int button,
                              unsigned char code[3]) {
    code[0] = BUTTON_TYPE_MACRO;
    code[1] = (unsigned char)((macro->loop_type << 4) | (button + 1));
    code[2] = macro->loop_type == MACRO_LOOP_COUNT ? macro->loop_count : 0;
}

static enum transfer_result write_buttons(const struct settings *settings) {
    static const int packet_order[BUTTON_COUNT] = {0, 1, 2, 4, 3, 5};
    unsigned char payload[REPORT_SIZE - 5];
    memset(payload, 0xff, sizeof(payload));
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        memcpy(&payload[i * 3], settings->buttons[packet_order[i]], 3);
    }
    return send_command(CMD_BUTTONS, payload, sizeof(payload));
}

static enum transfer_result write_dpi(const struct settings *settings) {
    enum transfer_result result = TRANSFER_ACKNOWLEDGED;
    for (int axis = 0; axis < 2 && result != TRANSFER_FAILED; ++axis) {
        const int *dpi = settings->dpi[settings->dpi_linked ? 0 : axis];
        unsigned char report[REPORT_SIZE];
        begin_command(report, CMD_DPI, axis == 0 ? DPI_AXIS_X : DPI_AXIS_Y);
        report[5] = (unsigned char)settings->active_level;
        report[6] = (unsigned char)settings->dpi_enabled;
        for (int i = 0; i < DPI_LEVELS; ++i) {
            unsigned int code = (unsigned int)(dpi[i] / DPI_STEP - 1);
            report[7 + i * 2] = (unsigned char)code;
            report[8 + i * 2] = (unsigned char)(code >> 8);
        }
        if (axis > 0) {
            usleep(BETWEEN_REPORTS_US);
        }
        result = combine_transfer_results(
            result, transfer_config_report(report, ACK_DELAY_US));
    }
    return result;
}

static enum transfer_result write_dpi_colors(const struct settings *settings) {
    unsigned char payload[DPI_COLOR_SLOTS * 3];
    for (int slot = 0; slot < DPI_COLOR_SLOTS; ++slot) {
        uint32_t rgb = settings->dpi_colors[slot];
        payload[slot * 3] = (unsigned char)(rgb >> 16);
        payload[slot * 3 + 1] = (unsigned char)(rgb >> 8);
        payload[slot * 3 + 2] = (unsigned char)rgb;
    }
    return send_command(CMD_DPI_COLORS, payload, sizeof(payload));
}

static enum transfer_result write_polling(const struct settings *settings) {
    unsigned char code = polling_options[settings->polling_index].code;
    return send_command(CMD_POLLING, &code, 1);
}

static enum transfer_result write_lift_off(const struct settings *settings) {
    unsigned char height = (unsigned char)settings->lift_off_mm;
    return send_command(CMD_LIFT_OFF, &height, 1);
}

static enum transfer_result write_debounce(const struct settings *settings) {
    unsigned char debounce = (unsigned char)settings->debounce_ms;
    return send_command(CMD_DEBOUNCE, &debounce, 1);
}

static enum transfer_result write_lighting(const struct settings *settings) {
    int mode = settings->lighting_mode;
    unsigned char payload[4] = {
        0, lighting_modes[mode].protocol_id,
        (unsigned char)(((settings->lighting_brightness[mode] + 1) << 4) |
                        (settings->lighting_speed[mode] + 1)),
        0,
    };
    return send_command(CMD_LIGHTING, payload, sizeof(payload));
}

static enum transfer_result write_power(const struct settings *settings) {
    unsigned char payload[2] = {
        (unsigned char)settings->sleep_minutes,
        (unsigned char)settings->light_off_on_move,
    };
    return send_command(CMD_POWER, payload, sizeof(payload));
}

static enum transfer_result write_sensor(const struct settings *settings) {
    unsigned char payload[3] = {
        (unsigned char)settings->angle_snap,
        (unsigned char)settings->motion_sync,
        (unsigned char)settings->ripple_control,
    };
    return send_command(CMD_SENSOR, payload, sizeof(payload));
}

static enum transfer_result write_macros(const struct settings *settings) {
    unsigned char payloads[BUTTON_COUNT][MACRO_MAX_PAYLOAD_SIZE];
    int sizes[BUTTON_COUNT] = {0};
    for (int button = 0; button < BUTTON_COUNT; ++button) {
        if (!button_is_macro(settings->buttons[button])) {
            continue;
        }
        sizes[button] = build_macro_payload(&settings->macros[button],
                                            payloads[button],
                                            sizeof(payloads[button]));
        if (sizes[button] <= 0) {
            return TRANSFER_FAILED;
        }
    }

    unsigned char clear = CLEAR_ALL;
    enum transfer_result aggregate = send_command(CMD_MACRO_CLEAR, &clear, 1);
    for (int button = 0; button < BUTTON_COUNT && aggregate != TRANSFER_FAILED;
         ++button) {
        int chunk_count = (sizes[button] + MACRO_CHUNK_SIZE - 1) / MACRO_CHUNK_SIZE;
        for (int chunk = 0; chunk < chunk_count && aggregate != TRANSFER_FAILED;
             ++chunk) {
            int offset = chunk * MACRO_CHUNK_SIZE;
            int length = sizes[button] - offset < MACRO_CHUNK_SIZE ?
                         sizes[button] - offset : MACRO_CHUNK_SIZE;
            unsigned char report[REPORT_SIZE];
            begin_command(report, CMD_MACRO_DATA,
                          (unsigned char)((chunk << 4) | chunk_count));
            report[5] = (unsigned char)(length + 1);
            report[6] = (unsigned char)(button + 1);
            memcpy(&report[7], &payloads[button][offset], (size_t)length);
            usleep(BETWEEN_REPORTS_US);
            aggregate = combine_transfer_results(
                aggregate, transfer_config_report(report, MACRO_ACK_DELAY_US));
        }
    }
    return aggregate;
}

static const struct setting_writer setting_writers[] = {
    {DIRTY_BUTTONS, "buttons", write_buttons},
    {DIRTY_DPI | DIRTY_ACTIVE_LEVEL, "DPI", write_dpi},
    {DIRTY_DPI_COLORS, "DPI colors", write_dpi_colors},
    {DIRTY_POLLING, "polling", write_polling},
    {DIRTY_LIFT_OFF, "lift-off", write_lift_off},
    {DIRTY_DEBOUNCE, "debounce", write_debounce},
    {DIRTY_LIGHTING, "lighting", write_lighting},
    {DIRTY_POWER, "power", write_power},
    {DIRTY_SENSOR, "sensor", write_sensor},
    {DIRTY_MACROS, "macros", write_macros},
};

#define SETTING_WRITER_COUNT \
    ((int)(sizeof(setting_writers) / sizeof(setting_writers[0])))

static int action_of_code(const unsigned char code[3]) {
    switch (code[0]) {
        case BUTTON_TYPE_DISABLED: return ACTION_DISABLED;
        case BUTTON_TYPE_FIRE:
            return code[1] == actions[ACTION_TRIPLE_CLICK].code[1] &&
                   code[2] == actions[ACTION_TRIPLE_CLICK].code[2] ?
                   ACTION_TRIPLE_CLICK : ACTION_FIRE;
        case BUTTON_TYPE_DPI_LOCK: return ACTION_DPI_LOCK;
        case BUTTON_TYPE_LIGHTING: return ACTION_LIGHTING_SWITCH;
        case BUTTON_TYPE_KEY: return ACTION_KEY;
        case BUTTON_TYPE_MEDIA: return ACTION_MEDIA;
        case BUTTON_TYPE_MACRO: return ACTION_MACRO;
        default: break;
    }
    for (int action = 0; action < ACTION_COUNT; ++action) {
        if (memcmp(actions[action].code, code, 2) == 0) {
            return action;
        }
    }
    return -1;
}

static int media_key_index(const unsigned char code[3]) {
    unsigned int usage = code[1] | (unsigned int)code[2] << 8;
    for (int i = 0; i < MEDIA_KEY_COUNT; ++i) {
        if (media_keys[i].usage == usage) {
            return i;
        }
    }
    return -1;
}

static const char *keyboard_key_name(unsigned char code, char *buffer,
                                     size_t buffer_size) {
    int index = macro_input_option_index(MACRO_EVENT_KEYBOARD, code);
    if (index >= 0) {
        return macro_input_options[index].name;
    }
    snprintf(buffer, buffer_size, "0x%02X", code);
    return buffer;
}

static unsigned char next_keyboard_code(unsigned char code, int delta) {
    int indices[MACRO_INPUT_OPTION_COUNT];
    int count = 0;
    int current = -1;
    for (int i = 0; i < MACRO_INPUT_OPTION_COUNT; ++i) {
        if (macro_input_options[i].kind != MACRO_EVENT_KEYBOARD) continue;
        if (macro_input_options[i].code == code) current = count;
        indices[count++] = i;
    }
    int next = current < 0 ? 0 : wrap_int(current + delta, count);
    return macro_input_options[indices[next]].code;
}

static const char *macro_loop_name(const struct macro_definition *macro,
                                   char *buffer, size_t buffer_size) {
    if (macro->loop_type == MACRO_LOOP_HOLD) return "While held";
    if (macro->loop_type == MACRO_LOOP_TOGGLE) return "Until pressed again";
    if (macro->loop_count <= 1) return "Once";
    snprintf(buffer, buffer_size, "%d times", macro->loop_count);
    return buffer;
}

static const char *describe_button(const struct settings *settings, int button,
                                   char *buffer, size_t buffer_size) {
    const unsigned char *code = settings->buttons[button];
    int action = action_of_code(code);
    if (action == ACTION_DPI_LOCK) {
        snprintf(buffer, buffer_size, "DPI lock %d",
                 DPI_LOCK_MIN + code[1] * DPI_LOCK_STEP);
    } else if (action == ACTION_FIRE) {
        if (code[2] == 0) {
            snprintf(buffer, buffer_size, "Fire key %d ms, while held", code[1]);
        } else {
            snprintf(buffer, buffer_size, "Fire key %d ms, %d clicks",
                     code[1], code[2]);
        }
    } else if (action == ACTION_KEY) {
        static const char *modifier_names[8] = {
            "Ctrl", "Shift", "Opt", "Cmd", "RCtrl", "RShift", "ROpt", "RCmd"
        };
        char key_buffer[8];
        size_t used = 0;
        buffer[0] = '\0';
        for (int bit = 0; bit < 8; ++bit) {
            if (code[1] & (1u << bit)) {
                int written = snprintf(buffer + used, buffer_size - used, "%s+",
                                       modifier_names[bit]);
                if (written > 0 && (size_t)written < buffer_size - used) {
                    used += (size_t)written;
                }
            }
        }
        snprintf(buffer + used, buffer_size - used, "%s",
                 keyboard_key_name(code[2], key_buffer, sizeof(key_buffer)));
    } else if (action == ACTION_MEDIA) {
        int index = media_key_index(code);
        if (index >= 0) {
            snprintf(buffer, buffer_size, "%s", media_keys[index].name);
        } else {
            snprintf(buffer, buffer_size, "Media 0x%04X",
                     code[1] | (unsigned int)code[2] << 8);
        }
    } else if (action == ACTION_MACRO) {
        const struct macro_definition *macro = &settings->macros[button];
        char loop_buffer[16];
        char step_buffer[24];
        const char *loop = macro_loop_name(macro, loop_buffer, sizeof(loop_buffer));
        if (!button_uses_own_macro(code, button)) {
            snprintf(buffer, buffer_size, "Macro slot %d (invalid)", code[1] & 0x0f);
        } else if (macro->step_count == 1) {
            snprintf(buffer, buffer_size, "Macro: %s, %s",
                     macro_step_name(&macro->steps[0], step_buffer,
                                     sizeof(step_buffer)), loop);
        } else {
            snprintf(buffer, buffer_size, "Macro %d steps, %s",
                     macro->step_count, loop);
        }
    } else if (action >= 0) {
        snprintf(buffer, buffer_size, "%s", actions[action].name);
    } else {
        snprintf(buffer, buffer_size, "Custom %02X %02X %02X",
                 code[0], code[1], code[2]);
    }
    return buffer;
}

static void default_settings(struct settings *settings) {
    memset(settings, 0, sizeof(*settings));
    for (int axis = 0; axis < 2; ++axis) {
        memcpy(settings->dpi[axis], default_dpi, sizeof(default_dpi));
    }
    settings->dpi_linked = 1;
    settings->dpi_enabled = DPI_ALL_LEVELS;
    memcpy(settings->dpi_colors, default_dpi_colors, sizeof(default_dpi_colors));
    settings->polling_index = polling_index_from_hz(1000);
    settings->lift_off_mm = 2;
    settings->debounce_ms = 2;
    settings->motion_sync = 1;
    memcpy(settings->buttons, default_buttons, sizeof(default_buttons));
    settings->lighting_mode = DEFAULT_LIGHTING_MODE;
    memcpy(settings->lighting_speed, default_lighting_speed,
           sizeof(default_lighting_speed));
    memcpy(settings->lighting_brightness, default_lighting_brightness,
           sizeof(default_lighting_brightness));
    settings->sleep_minutes = 1;
    for (int button = 0; button < BUTTON_COUNT; ++button) {
        settings->macros[button].loop_type = MACRO_LOOP_COUNT;
        settings->macros[button].loop_count = 1;
    }
}

static const char *validate_settings(const struct settings *settings) {
    static char problem[96];
    int has_left_click = 0;
    for (int button = 0; button < BUTTON_COUNT; ++button) {
        const unsigned char *code = settings->buttons[button];
        if (memcmp(code, actions[ACTION_LEFT].code, 3) == 0) {
            has_left_click = 1;
        }
        if (!button_is_macro(code)) {
            continue;
        }
        unsigned char payload[MACRO_MAX_PAYLOAD_SIZE];
        if (!button_uses_own_macro(code, button) ||
            build_macro_payload(&settings->macros[button], payload,
                                sizeof(payload)) <= 0) {
            snprintf(problem, sizeof(problem),
                     "the %s macro is empty, too long or uses another slot.",
                     button_names[button]);
            return problem;
        }
    }
    if (!has_left_click) {
        return "at least one button must stay on Left click.";
    }
    if ((settings->dpi_enabled & (1u << settings->active_level)) == 0) {
        return "the active DPI level is switched off.";
    }
    return NULL;
}

static int resolve_paths(void) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return 0;
    }
    int state_length = snprintf(state_path, sizeof(state_path), "%s/%s", home,
                                STATE_FILE_NAME);
    int lock_length = snprintf(lock_path, sizeof(lock_path), "%s/%s", home,
                               LOCK_FILE_NAME);
    return state_length > 0 && (size_t)state_length < sizeof(state_path) &&
           lock_length > 0 && (size_t)lock_length < sizeof(lock_path);
}

static int read_int(const char **cursor, int base, long minimum, long maximum,
                    int *value) {
    char *end;
    errno = 0;
    long parsed = strtol(*cursor, &end, base);
    if (end == *cursor || errno != 0 || parsed < minimum || parsed > maximum) {
        return 0;
    }
    *cursor = end;
    *value = (int)parsed;
    return 1;
}

static int read_dpi(const char **cursor, int *dpi) {
    return read_int(cursor, 10, DPI_MIN, DPI_MAX, dpi) && *dpi % DPI_STEP == 0;
}

static int at_line_end(const char *cursor) {
    while (isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return *cursor == '\0';
}

static int read_macro_steps(const char **cursor, struct macro_definition *macro,
                            int step_count) {
    if (step_count < 0 || step_count > MACRO_MAX_STEPS) {
        return 0;
    }
    macro->step_count = step_count;
    for (int i = 0; i < step_count; ++i) {
        int kind, code, uppercase, delay;
        if (!read_int(cursor, 10, 0, MACRO_EVENT_KIND_COUNT - 1, &kind) ||
            !read_int(cursor, 10, 0, 255, &code) ||
            !read_int(cursor, 10, 0, 1, &uppercase) ||
            !read_int(cursor, 10, 0, MACRO_MAX_DELAY_MS, &delay)) {
            return 0;
        }
        struct macro_step *step = &macro->steps[i];
        step->kind = (unsigned char)kind;
        step->code = (unsigned char)code;
        step->uppercase = (unsigned char)uppercase;
        step->delay_ms = (unsigned short)delay;
        if (!macro_step_valid(step)) {
            return 0;
        }
    }
    return 1;
}

static int import_legacy_state(const char *line, struct ui_state *state) {
    static const unsigned char legacy_lighting[3] = {3, 2, 4};
    struct settings *settings = &state->settings;
    const char *cursor = line + 2;
    int ok = read_int(&cursor, 10, 0, DPI_LEVELS - 1, &settings->active_level) &&
             read_int(&cursor, 10, 0, POLLING_OPTION_COUNT - 1,
                      &settings->polling_index) &&
             read_int(&cursor, 10, -1, 100, &state->last_battery) &&
             read_int(&cursor, 10, 1, 2, &settings->lift_off_mm) &&
             read_int(&cursor, 10, 0, 1, &settings->angle_snap) &&
             read_int(&cursor, 10, 0, 1, &settings->motion_sync) &&
             read_int(&cursor, 10, 0, 1, &settings->ripple_control) &&
             read_int(&cursor, 10, SLEEP_MINUTES_MIN, SLEEP_MINUTES_MAX,
                      &settings->sleep_minutes);
    for (int i = 0; ok && i < DPI_LEVELS; ++i) {
        ok = read_dpi(&cursor, &settings->dpi[0][i]);
    }
    for (int i = 0; ok && i < BUTTON_COUNT * 3; ++i) {
        int value;
        ok = read_int(&cursor, 10, 0, 255, &value);
        settings->buttons[i / 3][i % 3] = (unsigned char)value;
    }
    int protocol_id = -1;
    ok = ok && read_int(&cursor, 10, 0, 255, &protocol_id);
    settings->lighting_mode = lighting_index_from_protocol_id(protocol_id);
    ok = ok && settings->lighting_mode > 0;
    for (int i = 0; ok && i < 3; ++i) {
        int index = lighting_index_from_protocol_id(legacy_lighting[i]);
        ok = read_int(&cursor, 10, 0, LIGHTING_SPEED_MAX,
                      &settings->lighting_speed[index]) &&
             read_int(&cursor, 10, 0, LIGHTING_BRIGHTNESS_MAX,
                      &settings->lighting_brightness[index]);
    }
    for (int i = 0; ok && i < DPI_COLOR_SLOTS; ++i) {
        int color;
        ok = read_int(&cursor, 16, 0, 0xffffff, &color);
        settings->dpi_colors[i] = (uint32_t)color;
    }
    for (int button = 0; ok && button < BUTTON_COUNT; ++button) {
        int step_count;
        ok = read_int(&cursor, 10, 0, MACRO_MAX_STEPS, &step_count) &&
             read_macro_steps(&cursor, &settings->macros[button], step_count);
        const unsigned char *code = settings->buttons[button];
        if (ok && button_is_macro(code) &&
            macro_loop_valid(code[1] >> 4, code[2])) {
            settings->macros[button].loop_type = (unsigned char)(code[1] >> 4);
            settings->macros[button].loop_count = code[2];
        }
    }
    if (!ok || !at_line_end(cursor)) {
        return STATE_INVALID;
    }
    memcpy(settings->dpi[1], settings->dpi[0], sizeof(settings->dpi[0]));
    state->dirty = DIRTY_DEBOUNCE;
    return STATE_IMPORTED;
}

static int read_state_lines(FILE *file, struct ui_state *state) {
    struct settings *settings = &state->settings;
    unsigned int loaded = 0;
    int pending = 0;
    int macro_lines = 0;
    int dpi_y_loaded = 0;
    char line[STATE_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(file)) {
            return STATE_INVALID;
        }
        char key[32];
        int consumed = 0;
        if (sscanf(line, "%31s%n", key, &consumed) != 1) {
            continue;
        }
        const char *cursor = line + consumed;
        int ok = 1;
        int value = 0;
        if (strcmp(key, "pending") == 0) {
            ok = read_int(&cursor, 10, 0, DIRTY_ALL, &pending);
        } else if (strcmp(key, "active_level") == 0) {
            ok = read_int(&cursor, 10, 0, DPI_LEVELS - 1, &settings->active_level);
            loaded |= DIRTY_ACTIVE_LEVEL;
        } else if (strcmp(key, "polling_hz") == 0) {
            ok = read_int(&cursor, 10, 0, 100000, &value) &&
                 (settings->polling_index = polling_index_from_hz(value)) >= 0;
            loaded |= DIRTY_POLLING;
        } else if (strcmp(key, "dpi_x") == 0 || strcmp(key, "dpi_y") == 0) {
            int axis = key[4] == 'y';
            for (int i = 0; ok && i < DPI_LEVELS; ++i) {
                ok = read_dpi(&cursor, &settings->dpi[axis][i]);
            }
            if (axis == 0) loaded |= DIRTY_DPI;
            else dpi_y_loaded = 1;
        } else if (strcmp(key, "dpi_linked") == 0) {
            ok = read_int(&cursor, 10, 0, 1, &settings->dpi_linked);
        } else if (strcmp(key, "dpi_enabled") == 0) {
            ok = read_int(&cursor, 10, 1, DPI_ALL_LEVELS, &value);
            settings->dpi_enabled = (unsigned int)value;
        } else if (strcmp(key, "dpi_colors") == 0) {
            for (int i = 0; ok && i < DPI_COLOR_SLOTS; ++i) {
                ok = read_int(&cursor, 16, 0, 0xffffff, &value);
                settings->dpi_colors[i] = (uint32_t)value;
            }
            loaded |= DIRTY_DPI_COLORS;
        } else if (strcmp(key, "lift_off_mm") == 0) {
            ok = read_int(&cursor, 10, 1, 2, &settings->lift_off_mm);
            loaded |= DIRTY_LIFT_OFF;
        } else if (strcmp(key, "debounce_ms") == 0) {
            ok = read_int(&cursor, 10, 0, DEBOUNCE_MAX_MS, &settings->debounce_ms);
            loaded |= DIRTY_DEBOUNCE;
        } else if (strcmp(key, "sensor") == 0) {
            ok = read_int(&cursor, 10, 0, 1, &settings->angle_snap) &&
                 read_int(&cursor, 10, 0, 1, &settings->motion_sync) &&
                 read_int(&cursor, 10, 0, 1, &settings->ripple_control);
            loaded |= DIRTY_SENSOR;
        } else if (strcmp(key, "buttons") == 0) {
            for (int i = 0; ok && i < BUTTON_COUNT * 3; ++i) {
                ok = read_int(&cursor, 16, 0, 255, &value);
                settings->buttons[i / 3][i % 3] = (unsigned char)value;
            }
            loaded |= DIRTY_BUTTONS;
        } else if (strcmp(key, "lighting_mode") == 0) {
            ok = read_int(&cursor, 10, 0, 255, &value) &&
                 (settings->lighting_mode = lighting_index_from_protocol_id(value)) >= 0;
            loaded |= DIRTY_LIGHTING;
        } else if (strcmp(key, "lighting") == 0) {
            int index = -1;
            ok = read_int(&cursor, 10, 0, 255, &value) &&
                 (index = lighting_index_from_protocol_id(value)) >= 0 &&
                 read_int(&cursor, 10, 0, LIGHTING_SPEED_MAX,
                          &settings->lighting_speed[index]) &&
                 read_int(&cursor, 10, 0, LIGHTING_BRIGHTNESS_MAX,
                          &settings->lighting_brightness[index]);
        } else if (strcmp(key, "power") == 0) {
            ok = read_int(&cursor, 10, SLEEP_MINUTES_MIN, SLEEP_MINUTES_MAX,
                          &settings->sleep_minutes) &&
                 read_int(&cursor, 10, 0, 1, &settings->light_off_on_move);
            loaded |= DIRTY_POWER;
        } else if (strcmp(key, "battery") == 0) {
            ok = read_int(&cursor, 10, -1, 100, &state->last_battery);
        } else if (strcmp(key, "macro") == 0) {
            int button, loop_type, loop_count, step_count;
            ok = read_int(&cursor, 10, 0, BUTTON_COUNT - 1, &button) &&
                 read_int(&cursor, 10, 0, 15, &loop_type) &&
                 read_int(&cursor, 10, 0, REPEAT_COUNT_MAX, &loop_count) &&
                 macro_loop_valid(loop_type, loop_count) &&
                 read_int(&cursor, 10, 0, MACRO_MAX_STEPS, &step_count) &&
                 read_macro_steps(&cursor, &settings->macros[button], step_count);
            if (ok) {
                settings->macros[button].loop_type = (unsigned char)loop_type;
                settings->macros[button].loop_count = (unsigned char)loop_count;
                ++macro_lines;
            }
        }
        if (!ok || !at_line_end(cursor)) {
            return STATE_INVALID;
        }
    }
    if (ferror(file)) {
        return STATE_INVALID;
    }
    if (macro_lines == BUTTON_COUNT) {
        loaded |= DIRTY_MACROS;
    }
    if (settings->dpi_linked) {
        memcpy(settings->dpi[1], settings->dpi[0], sizeof(settings->dpi[0]));
    } else if (!dpi_y_loaded) {
        loaded &= ~DIRTY_DPI;
    }
    state->dirty = (DIRTY_ALL & ~loaded) | (unsigned int)pending;
    return STATE_LOADED;
}

static int load_state(struct ui_state *state) {
    default_settings(&state->settings);
    state->dirty = DIRTY_ALL;
    state->last_battery = -1;

    int imported = 0;
    FILE *file = fopen(state_path, "r");
    if (file == NULL) {
        file = fopen(STATE_FILE_NAME, "r");
        imported = file != NULL;
    }
    if (file == NULL) {
        return STATE_MISSING;
    }

    int result = STATE_INVALID;
    char line[STATE_LINE_MAX];
    if (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "vH ", 3) == 0) {
            result = import_legacy_state(line, state);
        } else if (strcmp(line, STATE_HEADER) == 0) {
            result = read_state_lines(file, state);
        }
    }
    fclose(file);

    if (result == STATE_INVALID) {
        default_settings(&state->settings);
        state->dirty = DIRTY_ALL;
        state->last_battery = -1;
    }
    return result == STATE_LOADED && imported ? STATE_IMPORTED : result;
}

static int persist_state(const struct ui_state *state) {
    const struct settings *settings = &state->settings;
    char temporary_path[PATH_MAX + 8];
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", state_path);

    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return 0;
    }

    fprintf(file, "%s\n", STATE_HEADER);
    fprintf(file, "pending %u\n", state->dirty);
    fprintf(file, "active_level %d\n", settings->active_level);
    fprintf(file, "polling_hz %d\n", polling_options[settings->polling_index].hz);
    for (int axis = 0; axis < 2; ++axis) {
        fprintf(file, "dpi_%c", axis == 0 ? 'x' : 'y');
        for (int i = 0; i < DPI_LEVELS; ++i) {
            fprintf(file, " %d", settings->dpi[axis][i]);
        }
        fputc('\n', file);
    }
    fprintf(file, "dpi_linked %d\n", settings->dpi_linked);
    fprintf(file, "dpi_enabled %u\n", settings->dpi_enabled);
    fprintf(file, "dpi_colors");
    for (int i = 0; i < DPI_COLOR_SLOTS; ++i) {
        fprintf(file, " %06X", (unsigned int)settings->dpi_colors[i]);
    }
    fprintf(file, "\nlift_off_mm %d\n", settings->lift_off_mm);
    fprintf(file, "debounce_ms %d\n", settings->debounce_ms);
    fprintf(file, "sensor %d %d %d\n", settings->angle_snap,
            settings->motion_sync, settings->ripple_control);
    fprintf(file, "buttons");
    for (int i = 0; i < BUTTON_COUNT * 3; ++i) {
        fprintf(file, " %02X", settings->buttons[i / 3][i % 3]);
    }
    fprintf(file, "\nlighting_mode %d\n",
            lighting_modes[settings->lighting_mode].protocol_id);
    for (int mode = 0; mode < LIGHTING_MODE_COUNT; ++mode) {
        fprintf(file, "lighting %d %d %d\n", lighting_modes[mode].protocol_id,
                settings->lighting_speed[mode],
                settings->lighting_brightness[mode]);
    }
    fprintf(file, "power %d %d\n", settings->sleep_minutes,
            settings->light_off_on_move);
    fprintf(file, "battery %d\n", state->last_battery);
    for (int button = 0; button < BUTTON_COUNT; ++button) {
        const struct macro_definition *macro = &settings->macros[button];
        fprintf(file, "macro %d %d %d %d", button, macro->loop_type,
                macro->loop_count, macro->step_count);
        for (int step = 0; step < macro->step_count; ++step) {
            fprintf(file, " %u %u %u %u", macro->steps[step].kind,
                    macro->steps[step].code, macro->steps[step].uppercase,
                    macro->steps[step].delay_ms);
        }
        fputc('\n', file);
    }

    int success = !ferror(file) && fflush(file) == 0 && fsync(fileno(file)) == 0;
    success = fclose(file) == 0 && success;
    if (!success || rename(temporary_path, state_path) != 0) {
        unlink(temporary_path);
        return 0;
    }
    return 1;
}

static void format_pending(unsigned int dirty, char *buffer, size_t buffer_size) {
    size_t used = 0;
    buffer[0] = '\0';
    for (int i = 0; i < SETTING_WRITER_COUNT; ++i) {
        if ((dirty & setting_writers[i].flags) == 0) {
            continue;
        }
        int written = snprintf(buffer + used, buffer_size - used, "%s%s",
                               used > 0 ? ", " : "", setting_writers[i].name);
        if (written < 0 || (size_t)written >= buffer_size - used) {
            break;
        }
        used += (size_t)written;
    }
}

static int refresh_device(struct ui_state *state, int force) {
    if (force) {
        close_status_handle();
        last_transport_scan_ms = 0;
        state->firmware_checked = 0;
    }

    struct device_status status;
    if (!read_live_status(&state->device, &status)) {
        if (state->device.connected &&
            monotonic_ms() - state->last_seen_ms >= DISCONNECT_GRACE_MS) {
            device_status_clear(&state->device);
            state->firmware_tag[0] = '\0';
            state->firmware_checked = 0;
        }
        return state->device.connected;
    }

    if (!state->device.connected || state->device.mode != status.mode) {
        state->firmware_tag[0] = '\0';
        state->firmware_checked = 0;
    }
    state->last_seen_ms = monotonic_ms();
    state->device = status;
    if (status.battery_percent >= 0) {
        state->last_battery = status.battery_percent;
    }

    struct settings *settings = &state->settings;
    if (!(state->dirty & DIRTY_ACTIVE_LEVEL) && status.active_level >= 0 &&
        (settings->dpi_enabled & (1u << status.active_level))) {
        settings->active_level = status.active_level;
    }
    if (!(state->dirty & DIRTY_LIGHTING) && status.lighting_mode >= 0) {
        settings->lighting_mode = status.lighting_mode;
    }
    int reported_polling = polling_index_from_hz(status.polling_hz);
    if (!(state->dirty & DIRTY_POLLING) && reported_polling >= 0) {
        settings->polling_index = reported_polling;
    }
    int maximum = maximum_polling_index(&status);
    if (settings->polling_index > maximum) {
        settings->polling_index = maximum;
    }

    if (!state->firmware_checked) {
        state->firmware_checked = 1;
        if (!read_firmware_tag(configuration_pid, state->firmware_tag)) {
            state->firmware_tag[0] = '\0';
        }
    }
    return 1;
}

static unsigned int verify_live_status(struct ui_state *state,
                                       unsigned int written) {
    unsigned int checked = written & (DIRTY_DPI | DIRTY_ACTIVE_LEVEL |
                                      DIRTY_POLLING | DIRTY_LIGHTING);
    if (checked == 0) {
        return 0;
    }

    const struct settings *settings = &state->settings;
    unsigned int mismatched = 0;
    for (int attempt = 0; attempt < VERIFY_RETRIES; ++attempt) {
        usleep(SETTLE_DELAY_US);
        struct device_status live;
        if (!read_live_status(&state->device, &live) ||
            live.report_count == state->device.report_count || !live.online) {
            continue;
        }
        state->device = live;
        mismatched = 0;
        if ((checked & (DIRTY_DPI | DIRTY_ACTIVE_LEVEL)) &&
            live.active_level >= 0 && live.active_level != settings->active_level) {
            mismatched |= DIRTY_ACTIVE_LEVEL;
        }
        if ((checked & DIRTY_POLLING) && live.polling_hz > 0 &&
            live.polling_hz != polling_options[settings->polling_index].hz) {
            mismatched |= DIRTY_POLLING;
        }
        if ((checked & DIRTY_LIGHTING) &&
            live.lighting_mode != settings->lighting_mode) {
            mismatched |= DIRTY_LIGHTING;
        }
        if (mismatched == 0) {
            return 0;
        }
    }
    return mismatched;
}

static int save_settings(struct ui_state *state) {
    if (!state->device.connected) {
        snprintf(state->message, sizeof(state->message),
                 "Save skipped: device is not connected.");
        return 0;
    }
    if (!state->device.online) {
        snprintf(state->message, sizeof(state->message),
                 "Mouse is asleep or off: move it to wake it, then press S again.");
        return 0;
    }
    const char *problem = validate_settings(&state->settings);
    if (problem != NULL) {
        snprintf(state->message, sizeof(state->message), "Save blocked: %s",
                 problem);
        return 0;
    }
    if (state->dirty == 0) {
        snprintf(state->message, sizeof(state->message), "Nothing to save.");
        return 1;
    }

    close_status_handle();
    last_transport_scan_ms = 0;

    unsigned int written = 0;
    int all_acknowledged = 1;
    const char *failed = NULL;
    for (int i = 0; i < SETTING_WRITER_COUNT; ++i) {
        const struct setting_writer *writer = &setting_writers[i];
        if ((state->dirty & writer->flags) == 0) {
            continue;
        }
        enum transfer_result result = TRANSFER_ACKNOWLEDGED;
        if (writer->write != write_polling ||
            maximum_polling_index(&state->device) > 0) {
            result = writer->write(&state->settings);
        }
        if (result == TRANSFER_FAILED) {
            failed = writer->name;
            break;
        }
        all_acknowledged &= result == TRANSFER_ACKNOWLEDGED;
        written |= state->dirty & writer->flags;
        state->dirty &= ~writer->flags;
        usleep(BETWEEN_REPORTS_US);
    }

    unsigned int mismatched = verify_live_status(state, written);
    state->dirty |= mismatched;
    int persisted = written == 0 || persist_state(state);

    char pending[128];
    format_pending(state->dirty, pending, sizeof(pending));
    if (failed != NULL) {
        snprintf(state->message, sizeof(state->message),
                 "Save stopped: no answer while writing %s. Still unsaved: %s.",
                 failed, pending);
    } else if (mismatched != 0) {
        snprintf(state->message, sizeof(state->message),
                 "Written, but the mouse reports a different %s; still marked unsaved.",
                 pending);
    } else if (!persisted) {
        snprintf(state->message, sizeof(state->message),
                 "Written to the mouse, but %s could not be updated.", state_path);
    } else if (all_acknowledged) {
        snprintf(state->message, sizeof(state->message),
                 "Saved and acknowledged by the mouse.");
    } else {
        snprintf(state->message, sizeof(state->message),
                 "Saved; some acknowledgements were not received.");
    }
    return failed == NULL && mismatched == 0;
}

static void draw_tabs(enum ui_tab tab) {
    static const char *names[TAB_COUNT] = {
        "General", "Buttons", "Sensor", "Lighting", "Power", "Macros", "Device"
    };
    int column = 2;
    for (int i = 0; i < TAB_COUNT; ++i) {
        if (i == (int)tab) attron(A_REVERSE);
        mvprintw(1, column, " %s ", names[i]);
        if (i == (int)tab) attroff(A_REVERSE);
        column += (int)strlen(names[i]) + 3;
    }
}

static void format_battery(const struct ui_state *state, char *buffer,
                           size_t buffer_size) {
    const struct device_status *device = &state->device;
    if (device->battery_percent >= 0) {
        snprintf(buffer, buffer_size, "%d%%%s", device->battery_percent,
                 device->charge_state == 1 ? " charging" :
                 device->charge_state == 2 ? " full" : "");
    } else if (state->last_battery >= 0) {
        snprintf(buffer, buffer_size, "%d%% (last seen)", state->last_battery);
    } else {
        snprintf(buffer, buffer_size, "--");
    }
}

static void draw_header(const struct ui_state *state, enum ui_tab tab) {
    draw_tabs(tab);
    if (state->dirty != 0) {
        char pending[128];
        format_pending(state->dirty, pending, sizeof(pending));
        mvprintw(2, 2, "Unsaved: %s", pending);
    }

    if (!state->device.connected) {
        mvprintw(3, 2, "Mode: Not connected");
        return;
    }
    const struct mode_spec *spec = mode_spec(state->device.mode);
    char battery[32];
    format_battery(state, battery, sizeof(battery));
    if (!state->device.online) {
        mvprintw(3, 2, "Mode: %s | Mouse asleep or off, move it to wake it | Battery: %s",
                 spec->name, battery);
        return;
    }
    mvprintw(3, 2, "Mode: %s | Battery: %s | DPI level: %d | Polling: %d Hz",
             spec->name, battery, state->settings.active_level + 1,
             polling_options[state->settings.polling_index].hz);
}

static void draw_footer(const struct ui_state *state, const char *hint,
                        const char *detail) {
    if (hint != NULL) mvprintw(HINT_ROW, 2, "%s", hint);
    if (detail != NULL) mvprintw(HINT_ROW + 1, 2, "%s", detail);
    mvprintw(HINT_ROW + 2, 2,
             "Tab: tab | Up/Down: select | S: save | D: defaults | R: refresh | Q: quit");
    mvprintw(MESSAGE_ROW, 2, "%s", state->message);
}

static void draw_choice(int row, int selected, const char *label,
                        const char *value) {
    if (selected) attron(A_REVERSE);
    mvprintw(row, 4, "%-26s < %s >", label, value);
    if (selected) attroff(A_REVERSE);
}

static void draw_dpi_bar(int row, int column, int width, int value) {
    int filled = clamp_int((value - DPI_MIN) * width / (DPI_MAX - DPI_MIN), 0, width);
    mvaddch(row, column, '[');
    for (int i = 0; i < width; ++i) {
        mvaddch(row, column + 1 + i, i < filled ? '=' : ' ');
    }
    mvaddch(row, column + 1 + width, ']');
}

static const char *dpi_axes_name(const struct ui_state *state) {
    if (state->settings.dpi_linked) return "Linked (X = Y)";
    return state->dpi_axis == 0 ? "Separate, editing X" : "Separate, editing Y";
}

static void draw_general(const struct ui_state *state, int selected) {
    const struct settings *settings = &state->settings;
    int maximum = maximum_polling_index(&state->device);
    mvprintw(4, 2, "DPI %d-%d in steps of %d | Max polling: %d Hz",
             DPI_MIN, DPI_MAX, DPI_STEP, polling_options[maximum].hz);
    mvprintw(5, 2, "DPI levels (* = active):");
    int axis = settings->dpi_linked ? 0 : state->dpi_axis;
    for (int i = 0; i < DPI_LEVELS; ++i) {
        int row = 6 + i;
        if (selected == i) attron(A_REVERSE);
        mvprintw(row, 4, "Level %d %c %-3s #%06X", i + 1,
                 settings->active_level == i ? '*' : ' ',
                 (settings->dpi_enabled >> i) & 1 ? "on" : "off",
                 (unsigned int)settings->dpi_colors[i]);
        draw_dpi_bar(row, 26, 34, settings->dpi[axis][i]);
        if (settings->dpi_linked) {
            mvprintw(row, 63, "%5d DPI", settings->dpi[0][i]);
        } else {
            mvprintw(row, 63, "X%5d Y%5d", settings->dpi[0][i], settings->dpi[1][i]);
        }
        if (selected == i) attroff(A_REVERSE);
    }

    char polling[16];
    snprintf(polling, sizeof(polling), "%d Hz",
             polling_options[settings->polling_index].hz);
    draw_choice(14, selected == GENERAL_ROW_AXES, "DPI axes", dpi_axes_name(state));
    draw_choice(15, selected == GENERAL_ROW_POLLING, "Polling rate", polling);
    draw_footer(state,
                "Left/Right: 50 DPI | O/P: 500 DPI | Space: active | T: on/off | E: color",
                "DPI axes: Linked writes X to both axes; Separate edits one axis.");
}

static void draw_buttons(const struct ui_state *state, int selected) {
    mvprintw(5, 2, "Button assignments:");
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        char description[48];
        if (selected == i) attron(A_REVERSE);
        mvprintw(7 + i, 4, "%-9s < %-30s >", button_names[i],
                 describe_button(&state->settings, i, description,
                                 sizeof(description)));
        if (selected == i) attroff(A_REVERSE);
    }
    mvprintw(14, 2, "Fire key repeats left click; DPI lock holds 200-1200 DPI.");
    mvprintw(15, 2, "Key: 1 Ctrl  2 Shift  3 Option  4 Command  K capture key");
    draw_footer(state,
                "Left/Right: action | -/+: key, media, DPI or interval | [/]: repeats",
                "Macro steps are edited in the Macros tab.");
}

static void draw_sensor(const struct ui_state *state, int selected) {
    static const char *labels[SENSOR_ROW_COUNT] = {
        "Lift-off distance", "Button debounce", "Angle snap", "Motion sync",
        "Ripple control"
    };
    static const char *descriptions[SENSOR_ROW_COUNT] = {
        "Height at which tracking stops after lifting the mouse.",
        "Delay before a switch press counts; raise it if clicks double.",
        "Straightens nearly horizontal/vertical movement.",
        "Aligns sensor samples with USB report timing.",
        "Reduces high-DPI ripple/jitter; may add smoothing."
    };
    const struct settings *settings = &state->settings;
    char lift_off[8];
    char debounce[8];
    snprintf(lift_off, sizeof(lift_off), "%d mm", settings->lift_off_mm);
    snprintf(debounce, sizeof(debounce), "%d ms", settings->debounce_ms);
    const char *values[SENSOR_ROW_COUNT] = {
        lift_off, debounce,
        settings->angle_snap ? "On" : "Off",
        settings->motion_sync ? "On" : "Off",
        settings->ripple_control ? "On" : "Off"
    };
    mvprintw(5, 2, "Sensor settings:");
    for (int i = 0; i < SENSOR_ROW_COUNT; ++i) {
        draw_choice(7 + i, selected == i, labels[i], values[i]);
    }
    draw_footer(state, descriptions[selected], NULL);
}

static int lighting_item_count(int mode) {
    return 1 + lighting_modes[mode].has_speed + lighting_modes[mode].has_brightness;
}

static enum lighting_item lighting_item_at(int mode, int index) {
    if (index == 0) return LIGHTING_ITEM_MODE;
    if (lighting_modes[mode].has_speed && index == 1) return LIGHTING_ITEM_SPEED;
    return LIGHTING_ITEM_BRIGHTNESS;
}

static void draw_lighting(const struct ui_state *state, int selected) {
    const struct settings *settings = &state->settings;
    int mode = settings->lighting_mode;
    mvprintw(5, 2, "Lighting settings:");
    for (int i = 0; i < lighting_item_count(mode); ++i) {
        enum lighting_item item = lighting_item_at(mode, i);
        char value[16];
        if (item == LIGHTING_ITEM_MODE) {
            snprintf(value, sizeof(value), "%s", lighting_modes[mode].name);
        } else if (item == LIGHTING_ITEM_SPEED) {
            snprintf(value, sizeof(value), "%d", settings->lighting_speed[mode] + 1);
        } else {
            snprintf(value, sizeof(value), "%d",
                     settings->lighting_brightness[mode] + 1);
        }
        draw_choice(7 + i, selected == i,
                    item == LIGHTING_ITEM_MODE ? "Mode" :
                    item == LIGHTING_ITEM_SPEED ? "Speed" : "Brightness", value);
    }
    draw_footer(state, "The effect uses the DPI level colors from the General tab.",
                NULL);
}

static void draw_power(const struct ui_state *state, int selected) {
    const struct settings *settings = &state->settings;
    char sleep_value[24];
    snprintf(sleep_value, sizeof(sleep_value), "%d minute%s",
             settings->sleep_minutes, settings->sleep_minutes == 1 ? "" : "s");
    mvprintw(5, 2, "Power management:");
    draw_choice(7, selected == POWER_ROW_SLEEP, "Sleep after inactivity",
                sleep_value);
    draw_choice(8, selected == POWER_ROW_LIGHT_OFF, "Lighting off while moving",
                settings->light_off_on_move ? "On" : "Off");
    draw_footer(state,
                selected == POWER_ROW_SLEEP ?
                "Battery-saving sleep timer; the mouse supports 1-8 minutes." :
                "Turns the lighting off while the mouse is moving to save power.",
                NULL);
}

static int macro_target_index(const struct ui_state *state) {
    return clamp_int(state->macro_target, 0, BUTTON_COUNT - 1);
}

static void draw_macros(const struct ui_state *state, int selected) {
    int target = macro_target_index(state);
    const struct macro_definition *macro = &state->settings.macros[target];
    char loop_buffer[16];
    mvprintw(4, 2, "Macro editor (%d/%d steps):", macro->step_count,
             MACRO_MAX_STEPS);
    draw_choice(6, selected == 0, "Target button", button_names[target]);
    draw_choice(7, selected == 1, "Repeat",
                macro_loop_name(macro, loop_buffer, sizeof(loop_buffer)));

    int step_selected = selected - MACRO_HEADER_ROWS;
    int first_step = step_selected >= MACRO_VISIBLE_STEPS ?
                     step_selected - MACRO_VISIBLE_STEPS + 1 : 0;
    for (int row = 0; row < MACRO_VISIBLE_STEPS; ++row) {
        int i = first_step + row;
        char name[24];
        if (step_selected == i) attron(A_REVERSE);
        if (i < macro->step_count) {
            mvprintw(8 + row, 4, "Step %-2d  %-20s  delay %4u ms", i + 1,
                     macro_step_name(&macro->steps[i], name, sizeof(name)),
                     macro->steps[i].delay_ms);
        } else if (i == macro->step_count && i < MACRO_MAX_STEPS) {
            mvprintw(8 + row, 4, "+ Add step");
        }
        if (step_selected == i) attroff(A_REVERSE);
    }
    mvprintw(16, 2, "A: add | C: delete | X: clear | K: capture key/click");
    draw_footer(state,
                "Left/Right: input | -/+: delay or repeats | [/]: move distance",
                "Modifiers stay held until the next input. Assign it in Buttons.");
}

static void draw_device(const struct ui_state *state, int selected) {
    const struct device_status *device = &state->device;
    char sensor_buffer[8];
    if (device->connected) {
        const struct mode_spec *spec = mode_spec(device->mode);
        mvprintw(5, 2, "Connection: %s (USB PID %04X)", spec->name, spec->pid);
        mvprintw(6, 2, "Mouse: %s", device->online ? "online" : "asleep or off");
        mvprintw(7, 2, "Sensor: %s | Max polling: %d Hz | Firmware tag: %s",
                 sensor_name(device->sensor_id, sensor_buffer, sizeof(sensor_buffer)),
                 device->max_polling_hz,
                 state->firmware_tag[0] != '\0' ? state->firmware_tag : "--");
    } else {
        mvprintw(5, 2, "Connection: none");
    }
    mvprintw(8, 2, "State file: ~/%s", STATE_FILE_NAME);

    draw_choice(10, selected == DEVICE_ROW_PAIR, "Pair with 2.4 GHz receiver",
                "Enter");
    draw_choice(11, selected == DEVICE_ROW_RESET, "Factory reset", "Enter");
    draw_footer(state,
                selected == DEVICE_ROW_PAIR ?
                "Re-pairs the mouse with the receiver (hold L+Wheel+R for 3 s first)." :
                "Restores the mouse firmware defaults, then writes mousectl defaults.",
                "Enter: run the selected action");
}

static int tab_item_count(const struct ui_state *state, enum ui_tab tab) {
    switch (tab) {
        case TAB_GENERAL: return GENERAL_ROW_COUNT;
        case TAB_BUTTONS: return BUTTON_COUNT;
        case TAB_SENSOR: return SENSOR_ROW_COUNT;
        case TAB_LIGHTING: return lighting_item_count(state->settings.lighting_mode);
        case TAB_POWER: return POWER_ROW_COUNT;
        case TAB_DEVICE: return DEVICE_ROW_COUNT;
        default: break;
    }
    const struct macro_definition *macro =
        &state->settings.macros[macro_target_index(state)];
    return MACRO_HEADER_ROWS + macro->step_count +
           (macro->step_count < MACRO_MAX_STEPS ? 1 : 0);
}

static void draw_ui(const struct ui_state *state, enum ui_tab tab, int selected) {
    erase();
    draw_header(state, tab);
    switch (tab) {
        case TAB_GENERAL: draw_general(state, selected); break;
        case TAB_BUTTONS: draw_buttons(state, selected); break;
        case TAB_SENSOR: draw_sensor(state, selected); break;
        case TAB_LIGHTING: draw_lighting(state, selected); break;
        case TAB_POWER: draw_power(state, selected); break;
        case TAB_MACROS: draw_macros(state, selected); break;
        default: draw_device(state, selected); break;
    }
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

static int confirm(struct ui_state *state, enum ui_tab tab, int selected,
                   const char *question) {
    snprintf(state->message, sizeof(state->message), "%s [y/N]", question);
    draw_ui(state, tab, selected);
    timeout(-1);
    int key = getch();
    timeout(UI_REFRESH_MS);
    return key == 'y' || key == 'Y';
}

static unsigned char keyboard_code_from_key(wint_t key, unsigned char *modifier) {
    *modifier = 0;
    if (key == L'\r' || key == KEY_ENTER) key = L'\n';
    if (key == 127 || key == 8) key = KEY_BACKSPACE;
    if (key >= 1 && key <= 26 && key != L'\t' && key != L'\n') {
        *modifier = 0x01;
        return (unsigned char)(0x04 + key - 1);
    }
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

static unsigned char captured_letter_modifier(wint_t key, CGEventFlags flags) {
    int uppercase = key >= L'A' && key <= L'Z';
    int caps_lock = (flags & kCGEventFlagMaskAlphaShift) != 0;
    return uppercase != caps_lock ? 0x02 : 0;
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

static int capture_input(struct captured_input *input) {
    memset(input, 0, sizeof(*input));
    input->kind = MACRO_EVENT_KEYBOARD;
    struct native_capture_state baseline = native_capture_state();
    raw();
    timeout(20);
    for (;;) {
        wint_t key = 0;
        int status = get_wch(&key);
        if (status == KEY_CODE_YES && key == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                input->kind = MACRO_EVENT_MOUSE_BUTTON;
                input->code = mouse_code_from_event(&event);
            }
            break;
        }
        if (status == OK || status == KEY_CODE_YES) {
            input->code = keyboard_code_from_key(key, &input->modifiers);
            if ((key >= L'a' && key <= L'z') || (key >= L'A' && key <= L'Z')) {
                input->modifiers = captured_letter_modifier(
                    key, CGEventSourceFlagsState(
                             kCGEventSourceStateCombinedSessionState));
                input->uppercase = key >= L'A' && key <= L'Z';
            }
            break;
        }
        if (capture_native_input(&baseline, &input->kind, &input->code)) {
            break;
        }
    }
    cbreak();
    timeout(UI_REFRESH_MS);
    return input->code != 0;
}

static void edit_dpi_color(struct ui_state *state, int level) {
    char input[8] = {0};
    move(MESSAGE_ROW, 2);
    clrtoeol();
    printw("Level %d color (#RRGGBB): ", level + 1);
    echo();
    curs_set(1);
    timeout(-1);
    int result = getnstr(input, (int)sizeof(input) - 1);
    timeout(UI_REFRESH_MS);
    curs_set(0);
    noecho();

    const char *hex = input[0] == '#' ? input + 1 : input;
    int valid = result == OK && strlen(hex) == 6;
    for (int i = 0; valid && i < 6; ++i) {
        valid = isxdigit((unsigned char)hex[i]) != 0;
    }
    unsigned long color = valid ? strtoul(hex, NULL, 16) : 0;
    if (valid) {
        state->settings.dpi_colors[level] = (uint32_t)color;
        state->dirty |= DIRTY_DPI_COLORS;
        snprintf(state->message, sizeof(state->message),
                 "DPI level %d color set to #%06lX; press S to save.",
                 level + 1, color);
    } else {
        snprintf(state->message, sizeof(state->message),
                 "Invalid color. Enter exactly #RRGGBB, for example #FF8000.");
    }
}

static void move_dpi(int *dpi, int delta) {
    int value = clamp_int(*dpi + delta, DPI_MIN, DPI_MAX);
    *dpi = value - value % DPI_STEP;
}

static void adjust_dpi(struct ui_state *state, int level, int delta) {
    struct settings *settings = &state->settings;
    for (int axis = 0; axis < 2; ++axis) {
        if (settings->dpi_linked || axis == state->dpi_axis) {
            move_dpi(&settings->dpi[axis][level], delta);
        }
    }
    state->dirty |= DIRTY_DPI;
}

static void toggle_dpi_level(struct ui_state *state, int level) {
    struct settings *settings = &state->settings;
    unsigned int mask = settings->dpi_enabled ^ (1u << level);
    if ((mask & DPI_ALL_LEVELS) == 0) {
        snprintf(state->message, sizeof(state->message),
                 "At least one DPI level must stay on.");
        return;
    }
    settings->dpi_enabled = mask;
    state->dirty |= DIRTY_DPI;
    for (int step = 0; step < DPI_LEVELS &&
         (mask & (1u << settings->active_level)) == 0; ++step) {
        settings->active_level = (settings->active_level + 1) % DPI_LEVELS;
        state->dirty |= DIRTY_ACTIVE_LEVEL;
    }
}

static void cycle_dpi_axes(struct ui_state *state, int delta) {
    struct settings *settings = &state->settings;
    int current = settings->dpi_linked ? AXES_LINKED :
                  (state->dpi_axis == 0 ? AXES_X : AXES_Y);
    int next = wrap_int(current + delta, AXES_COUNT);
    if (next == AXES_LINKED) {
        memcpy(settings->dpi[1], settings->dpi[0], sizeof(settings->dpi[0]));
        settings->dpi_linked = 1;
        state->dirty |= DIRTY_DPI;
        return;
    }
    if (settings->dpi_linked) {
        settings->dpi_linked = 0;
        state->dirty |= DIRTY_DPI;
    }
    state->dpi_axis = next == AXES_X ? 0 : 1;
}

static void handle_general_key(struct ui_state *state, int selected, int key) {
    struct settings *settings = &state->settings;
    if (selected == GENERAL_ROW_AXES) {
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            cycle_dpi_axes(state, key == KEY_RIGHT ? 1 : -1);
        }
        return;
    }
    if (selected == GENERAL_ROW_POLLING) {
        if (key == KEY_LEFT && settings->polling_index > 0) {
            --settings->polling_index;
            state->dirty |= DIRTY_POLLING;
        } else if (key == KEY_RIGHT &&
                   settings->polling_index < maximum_polling_index(&state->device)) {
            ++settings->polling_index;
            state->dirty |= DIRTY_POLLING;
        }
        return;
    }

    if (key == KEY_LEFT || key == '-') {
        adjust_dpi(state, selected, -DPI_STEP);
    } else if (key == KEY_RIGHT || key == '+' || key == '=') {
        adjust_dpi(state, selected, DPI_STEP);
    } else if (key == 'o' || key == 'O') {
        adjust_dpi(state, selected, -DPI_LARGE_STEP);
    } else if (key == 'p' || key == 'P') {
        adjust_dpi(state, selected, DPI_LARGE_STEP);
    } else if (key == 't' || key == 'T') {
        toggle_dpi_level(state, selected);
    } else if (key == 'e' || key == 'E') {
        edit_dpi_color(state, selected);
    } else if (key == ' ') {
        if ((settings->dpi_enabled & (1u << selected)) == 0) {
            snprintf(state->message, sizeof(state->message),
                     "Turn level %d on (T) before making it active.", selected + 1);
            return;
        }
        settings->active_level = selected;
        state->dirty |= DIRTY_ACTIVE_LEVEL;
        snprintf(state->message, sizeof(state->message),
                 "DPI level %d will become active after saving.", selected + 1);
    }
}

static void set_button_action(struct ui_state *state, int button, int action) {
    struct settings *settings = &state->settings;
    int was_macro = button_is_macro(settings->buttons[button]);
    if (action == ACTION_MACRO) {
        macro_button_code(&settings->macros[button], button,
                          settings->buttons[button]);
    } else {
        memcpy(settings->buttons[button], actions[action].code, 3);
    }
    state->dirty |= DIRTY_BUTTONS;
    if (was_macro || action == ACTION_MACRO) {
        state->dirty |= DIRTY_MACROS;
    }
}

static int adjust_macro_loop(struct ui_state *state, int button, int delta,
                             int count_only) {
    static const unsigned char loop_order[] = {
        MACRO_LOOP_COUNT, MACRO_LOOP_HOLD, MACRO_LOOP_TOGGLE,
    };
    struct macro_definition *macro = &state->settings.macros[button];
    if (count_only) {
        if (macro->loop_type != MACRO_LOOP_COUNT) {
            return 0;
        }
        macro->loop_count =
            (unsigned char)clamp_int(macro->loop_count + delta, 1, REPEAT_COUNT_MAX);
    } else {
        int current = 0;
        for (int i = 0; i < 3; ++i) {
            if (loop_order[i] == macro->loop_type) current = i;
        }
        macro->loop_type = loop_order[wrap_int(current + delta, 3)];
        if (macro->loop_count == 0) macro->loop_count = 1;
    }
    unsigned char *code = state->settings.buttons[button];
    if (button_uses_own_macro(code, button)) {
        macro_button_code(macro, button, code);
        state->dirty |= DIRTY_BUTTONS;
    }
    return 1;
}

static int adjust_button(struct ui_state *state, int button, int delta,
                         int secondary) {
    unsigned char *code = state->settings.buttons[button];
    switch (code[0]) {
        case BUTTON_TYPE_DPI_LOCK:
            if (secondary) return 0;
            code[1] = (unsigned char)clamp_int(code[1] + delta, 0,
                                               DPI_LOCK_LEVELS - 1);
            break;
        case BUTTON_TYPE_FIRE:
            if (secondary) {
                code[2] = (unsigned char)clamp_int(code[2] + delta, 0,
                                                   REPEAT_COUNT_MAX);
            } else {
                code[1] = (unsigned char)clamp_int(
                    code[1] + delta * FIRE_INTERVAL_STEP, FIRE_INTERVAL_MIN,
                    FIRE_INTERVAL_MAX);
            }
            break;
        case BUTTON_TYPE_KEY:
            if (secondary) return 0;
            code[2] = next_keyboard_code(code[2], delta);
            break;
        case BUTTON_TYPE_MEDIA: {
            if (secondary) return 0;
            int index = wrap_int(media_key_index(code) + delta, MEDIA_KEY_COUNT);
            code[1] = (unsigned char)media_keys[index].usage;
            code[2] = (unsigned char)(media_keys[index].usage >> 8);
            break;
        }
        case BUTTON_TYPE_MACRO:
            return adjust_macro_loop(state, button, delta, secondary);
        default:
            return 0;
    }
    state->dirty |= DIRTY_BUTTONS;
    return 1;
}

static void capture_button_key(struct ui_state *state, enum ui_tab tab,
                               int button) {
    snprintf(state->message, sizeof(state->message),
             "Press the key for the %s button.", button_names[button]);
    draw_ui(state, tab, button);
    struct captured_input input;
    if (!capture_input(&input) || input.kind != MACRO_EVENT_KEYBOARD) {
        snprintf(state->message, sizeof(state->message),
                 "Only keyboard keys can be captured here.");
        return;
    }
    unsigned char *code = state->settings.buttons[button];
    int was_macro = button_is_macro(code);
    code[0] = BUTTON_TYPE_KEY;
    code[1] = input.modifiers;
    code[2] = input.code;
    state->dirty |= DIRTY_BUTTONS | (was_macro ? DIRTY_MACROS : 0);
    char description[48];
    snprintf(state->message, sizeof(state->message), "%s button now sends %s.",
             button_names[button],
             describe_button(&state->settings, button, description,
                             sizeof(description)));
}

static void handle_buttons_key(struct ui_state *state, enum ui_tab tab,
                               int selected, int key) {
    unsigned char *code = state->settings.buttons[selected];
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        int action = action_of_code(code);
        if (action < 0) action = 0;
        set_button_action(state, selected,
                          wrap_int(action + (key == KEY_RIGHT ? 1 : -1), ACTION_COUNT));
    } else if (key == '-' || key == '+' || key == '=') {
        if (!adjust_button(state, selected, key == '-' ? -1 : 1, 0)) {
            snprintf(state->message, sizeof(state->message),
                     "This action has no adjustable value.");
        }
    } else if (key == '[' || key == ']') {
        if (!adjust_button(state, selected, key == '[' ? -1 : 1, 1)) {
            snprintf(state->message, sizeof(state->message),
                     "[/] sets fire key clicks or macro repeat counts.");
        }
    } else if (key >= '1' && key <= '4') {
        if (code[0] != BUTTON_TYPE_KEY) {
            snprintf(state->message, sizeof(state->message),
                     "Modifiers apply to the Key action only.");
            return;
        }
        code[1] ^= (unsigned char)(1u << (key - '1'));
        state->dirty |= DIRTY_BUTTONS;
    } else if (key == 'k' || key == 'K') {
        capture_button_key(state, tab, selected);
    }
}

static void handle_sensor_key(struct ui_state *state, int selected, int key) {
    struct settings *settings = &state->settings;
    if (key != KEY_LEFT && key != KEY_RIGHT) {
        return;
    }
    int delta = key == KEY_RIGHT ? 1 : -1;
    if (selected == SENSOR_ROW_LIFT_OFF) {
        settings->lift_off_mm = settings->lift_off_mm == 1 ? 2 : 1;
        state->dirty |= DIRTY_LIFT_OFF;
    } else if (selected == SENSOR_ROW_DEBOUNCE) {
        settings->debounce_ms = clamp_int(settings->debounce_ms + delta, 0,
                                          DEBOUNCE_MAX_MS);
        state->dirty |= DIRTY_DEBOUNCE;
    } else {
        int *toggle = selected == SENSOR_ROW_ANGLE_SNAP ? &settings->angle_snap :
                      selected == SENSOR_ROW_MOTION_SYNC ? &settings->motion_sync :
                      &settings->ripple_control;
        *toggle = !*toggle;
        state->dirty |= DIRTY_SENSOR;
    }
}

static void handle_lighting_key(struct ui_state *state, int selected, int key) {
    struct settings *settings = &state->settings;
    if (key != KEY_LEFT && key != KEY_RIGHT) {
        return;
    }
    int delta = key == KEY_RIGHT ? 1 : -1;
    int mode = settings->lighting_mode;
    enum lighting_item item = lighting_item_at(mode, selected);
    if (item == LIGHTING_ITEM_MODE) {
        settings->lighting_mode = wrap_int(mode + delta, LIGHTING_MODE_COUNT);
    } else if (item == LIGHTING_ITEM_SPEED) {
        settings->lighting_speed[mode] =
            wrap_int(settings->lighting_speed[mode] + delta, LIGHTING_SPEED_MAX + 1);
    } else {
        settings->lighting_brightness[mode] =
            wrap_int(settings->lighting_brightness[mode] + delta,
                     LIGHTING_BRIGHTNESS_MAX + 1);
    }
    state->dirty |= DIRTY_LIGHTING;
}

static void handle_power_key(struct ui_state *state, int selected, int key) {
    struct settings *settings = &state->settings;
    if (key != KEY_LEFT && key != KEY_RIGHT) {
        return;
    }
    if (selected == POWER_ROW_SLEEP) {
        int range = SLEEP_MINUTES_MAX - SLEEP_MINUTES_MIN + 1;
        settings->sleep_minutes = SLEEP_MINUTES_MIN +
            wrap_int(settings->sleep_minutes - SLEEP_MINUTES_MIN +
                     (key == KEY_RIGHT ? 1 : -1), range);
    } else {
        settings->light_off_on_move = !settings->light_off_on_move;
    }
    state->dirty |= DIRTY_POWER;
}

static int insert_macro_step(struct ui_state *state, int target, int step,
                             unsigned char kind, unsigned char code) {
    struct macro_definition *macro = &state->settings.macros[target];
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
    state->dirty |= DIRTY_MACROS;
    return 1;
}

static void delete_macro_step(struct ui_state *state, int target, int step) {
    struct macro_definition *macro = &state->settings.macros[target];
    if (step < 0 || step >= macro->step_count) {
        return;
    }
    memmove(&macro->steps[step], &macro->steps[step + 1],
            (size_t)(macro->step_count - step - 1) * sizeof(macro->steps[0]));
    --macro->step_count;
    memset(&macro->steps[macro->step_count], 0, sizeof(macro->steps[0]));
    state->dirty |= DIRTY_MACROS;
}

static void clear_macro(struct ui_state *state, int target) {
    struct macro_definition *macro = &state->settings.macros[target];
    memset(macro->steps, 0, sizeof(macro->steps));
    macro->step_count = 0;
    state->dirty |= DIRTY_MACROS;
}

static void move_macro_input(struct ui_state *state, int target, int step,
                             int delta) {
    struct macro_step *macro_step = &state->settings.macros[target].steps[step];
    int index = macro_input_option_index(macro_step->kind, macro_step->code);
    index = wrap_int((index < 0 ? 0 : index) + delta, MACRO_INPUT_OPTION_COUNT);
    macro_step->kind = macro_input_options[index].kind;
    macro_step->code = macro_input_options[index].code;
    macro_step->uppercase = 0;
    state->dirty |= DIRTY_MACROS;
}

static void capture_macro_step(struct ui_state *state, enum ui_tab tab,
                               int selected) {
    int target = macro_target_index(state);
    struct macro_definition *macro = &state->settings.macros[target];
    int step = selected - MACRO_HEADER_ROWS;
    int adding = step == macro->step_count;
    snprintf(state->message, sizeof(state->message),
             "Press a keyboard key or mouse button for step %d.", step + 1);
    draw_ui(state, tab, selected);

    struct captured_input input;
    if (!capture_input(&input)) {
        snprintf(state->message, sizeof(state->message),
                 "That key or mouse button is not supported.");
        return;
    }
    int modifier_steps = 0;
    for (int bit = 0; bit < 8; ++bit) {
        modifier_steps += (input.modifiers >> bit) & 1;
    }
    if (MACRO_MAX_STEPS - macro->step_count + !adding < modifier_steps + 1) {
        snprintf(state->message, sizeof(state->message),
                 "Not enough room in the %d-step macro limit.", MACRO_MAX_STEPS);
        return;
    }
    if (!adding) {
        delete_macro_step(state, target, step);
    }
    for (int bit = 0; bit < 8; ++bit) {
        if (input.modifiers & (1u << bit)) {
            insert_macro_step(state, target, step++, MACRO_EVENT_MODIFIER,
                              (unsigned char)(1u << bit));
        }
    }
    insert_macro_step(state, target, step, input.kind, input.code);
    macro->steps[step].uppercase = (unsigned char)(input.kind == MACRO_EVENT_KEYBOARD &&
                                                   input.uppercase &&
                                                   input.code >= 0x04 &&
                                                   input.code <= 0x1d);
    char name[24];
    snprintf(state->message, sizeof(state->message), "Captured %s for step %d.",
             macro_step_name(&macro->steps[step], name, sizeof(name)), step + 1);
}

static void handle_macros_key(struct ui_state *state, enum ui_tab tab,
                              int *selected, int key) {
    int target = macro_target_index(state);
    struct macro_definition *macro = &state->settings.macros[target];
    int step = *selected - MACRO_HEADER_ROWS;

    if (key == 'a' || key == 'A') {
        if (insert_macro_step(state, target, macro->step_count,
                              macro_input_options[0].kind,
                              macro_input_options[0].code)) {
            *selected = MACRO_HEADER_ROWS + macro->step_count - 1;
            snprintf(state->message, sizeof(state->message),
                     "Step added; choose a key, modifier, click or movement.");
        } else {
            snprintf(state->message, sizeof(state->message),
                     "Macro is already at the %d-step limit.", MACRO_MAX_STEPS);
        }
    } else if (key == 'x' || key == 'X') {
        clear_macro(state, target);
        *selected = 0;
        snprintf(state->message, sizeof(state->message),
                 "%s macro cleared; press S to save.", button_names[target]);
    } else if (*selected == 0 && (key == KEY_LEFT || key == KEY_RIGHT)) {
        state->macro_target = wrap_int(target + (key == KEY_RIGHT ? 1 : -1),
                                       BUTTON_COUNT);
    } else if (*selected == 1) {
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            adjust_macro_loop(state, target, key == KEY_RIGHT ? 1 : -1, 0);
        } else if (key == '-' || key == '+' || key == '=' || key == '[' || key == ']') {
            if (!adjust_macro_loop(state, target,
                                   key == '-' || key == '[' ? -1 : 1, 1)) {
                snprintf(state->message, sizeof(state->message),
                         "Repeat counts apply to the counted repeat mode.");
            }
        }
    } else if (*selected >= MACRO_HEADER_ROWS) {
        if (key == 'k' || key == 'K') {
            capture_macro_step(state, tab, *selected);
            return;
        }
        if (step == macro->step_count) {
            if ((key == KEY_LEFT || key == KEY_RIGHT) &&
                insert_macro_step(state, target, step, macro_input_options[0].kind,
                                  macro_input_options[0].code)) {
                move_macro_input(state, target, step, key == KEY_RIGHT ? 0 : -1);
            }
            return;
        }
        struct macro_step *macro_step = &macro->steps[step];
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            move_macro_input(state, target, step, key == KEY_RIGHT ? 1 : -1);
        } else if (key == '-' || key == '+' || key == '=') {
            macro_step->delay_ms = (unsigned short)clamp_int(
                macro_step->delay_ms + (key == '-' ? -10 : 10), 0,
                MACRO_MAX_DELAY_MS);
            state->dirty |= DIRTY_MACROS;
        } else if ((key == '[' || key == ']') && macro_kind_is_move(macro_step->kind)) {
            int distance = (signed char)macro_step->code;
            int magnitude = clamp_int(abs(distance) + (key == ']' ? 1 : -1), 1,
                                      MOVE_DISTANCE_MAX);
            macro_step->code = (unsigned char)(distance < 0 ? 256 - magnitude :
                                                              magnitude);
            state->dirty |= DIRTY_MACROS;
        } else if (key == 'c' || key == 'C') {
            delete_macro_step(state, target, step);
        }
    }
}

static void pair_receiver(struct ui_state *state, enum ui_tab tab, int selected) {
    if ((detect_transports() & (1u << MODE_RECEIVER)) == 0) {
        snprintf(state->message, sizeof(state->message),
                 "Plug in the 2.4 GHz receiver first.");
        return;
    }
    if (!confirm(state, tab, selected,
                 "Hold Left+Wheel+Right for 3 s until the LED blinks fast. Start pairing?")) {
        snprintf(state->message, sizeof(state->message), "Pairing cancelled.");
        return;
    }
    close_status_handle();
    if (!send_pairing_request()) {
        snprintf(state->message, sizeof(state->message),
                 "Pairing request failed: the receiver did not accept it.");
        return;
    }
    if (state->device.mode != MODE_RECEIVER) {
        snprintf(state->message, sizeof(state->message),
                 "Pairing request sent to the receiver.");
        return;
    }

    unsigned int reports_before = state->device.report_count;
    long long deadline = monotonic_ms() + PAIRING_TIMEOUT_MS;
    while (monotonic_ms() < deadline) {
        snprintf(state->message, sizeof(state->message),
                 "Pairing... %lld s left (Q stops waiting).",
                 (deadline - monotonic_ms() + 999) / 1000);
        draw_ui(state, tab, selected);
        int key = getch();
        if (key == 'q' || key == 'Q' || key == 27) {
            snprintf(state->message, sizeof(state->message),
                     "Stopped waiting; the receiver may still pair.");
            return;
        }
        refresh_device(state, 0);
        if (state->device.mode == MODE_RECEIVER && state->device.online &&
            state->device.report_count != reports_before) {
            snprintf(state->message, sizeof(state->message), "Pairing succeeded.");
            return;
        }
    }
    snprintf(state->message, sizeof(state->message),
             "Pairing timed out: no mouse answered within %d s.",
             PAIRING_TIMEOUT_MS / 1000);
}

static void factory_reset(struct ui_state *state, enum ui_tab tab, int selected) {
    if (!state->device.connected || !state->device.online) {
        snprintf(state->message, sizeof(state->message),
                 "Factory reset needs a connected, awake mouse.");
        return;
    }
    if (!confirm(state, tab, selected,
                 "Restore factory defaults? Buttons, DPI, lighting and macros are reset.")) {
        snprintf(state->message, sizeof(state->message), "Factory reset cancelled.");
        return;
    }
    close_status_handle();
    unsigned char all = CLEAR_ALL;
    if (send_command(CMD_FACTORY_RESET, &all, 1) == TRANSFER_FAILED) {
        snprintf(state->message, sizeof(state->message),
                 "Factory reset failed: the mouse did not answer.");
        return;
    }
    usleep(SETTLE_DELAY_US);
    default_settings(&state->settings);
    state->dirty = DIRTY_ALL;
    if (save_settings(state)) {
        snprintf(state->message, sizeof(state->message),
                 "Factory reset done and mousectl defaults written.");
    }
}

static void reset_current_tab(struct ui_state *state, enum ui_tab tab) {
    struct settings *settings = &state->settings;
    struct settings defaults;
    default_settings(&defaults);
    const char *name = NULL;
    if (tab == TAB_GENERAL) {
        memcpy(settings->dpi, defaults.dpi, sizeof(defaults.dpi));
        memcpy(settings->dpi_colors, defaults.dpi_colors, sizeof(defaults.dpi_colors));
        settings->dpi_linked = defaults.dpi_linked;
        settings->dpi_enabled = defaults.dpi_enabled;
        settings->active_level = defaults.active_level;
        settings->polling_index = defaults.polling_index;
        int maximum = maximum_polling_index(&state->device);
        if (settings->polling_index > maximum) settings->polling_index = maximum;
        state->dirty |= DIRTY_DPI | DIRTY_ACTIVE_LEVEL | DIRTY_DPI_COLORS |
                        DIRTY_POLLING;
        name = "General settings";
    } else if (tab == TAB_BUTTONS) {
        for (int button = 0; button < BUTTON_COUNT; ++button) {
            if (button_is_macro(settings->buttons[button])) {
                state->dirty |= DIRTY_MACROS;
            }
        }
        memcpy(settings->buttons, defaults.buttons, sizeof(defaults.buttons));
        state->dirty |= DIRTY_BUTTONS;
        name = "Button assignments";
    } else if (tab == TAB_SENSOR) {
        settings->lift_off_mm = defaults.lift_off_mm;
        settings->debounce_ms = defaults.debounce_ms;
        settings->angle_snap = defaults.angle_snap;
        settings->motion_sync = defaults.motion_sync;
        settings->ripple_control = defaults.ripple_control;
        state->dirty |= DIRTY_LIFT_OFF | DIRTY_DEBOUNCE | DIRTY_SENSOR;
        name = "Sensor settings";
    } else if (tab == TAB_LIGHTING) {
        settings->lighting_mode = defaults.lighting_mode;
        memcpy(settings->lighting_speed, defaults.lighting_speed,
               sizeof(defaults.lighting_speed));
        memcpy(settings->lighting_brightness, defaults.lighting_brightness,
               sizeof(defaults.lighting_brightness));
        state->dirty |= DIRTY_LIGHTING;
        name = "Lighting settings";
    } else if (tab == TAB_POWER) {
        settings->sleep_minutes = defaults.sleep_minutes;
        settings->light_off_on_move = defaults.light_off_on_move;
        state->dirty |= DIRTY_POWER;
        name = "Power settings";
    } else if (tab == TAB_MACROS) {
        clear_macro(state, macro_target_index(state));
        name = "Selected macro";
    }
    if (name == NULL) {
        snprintf(state->message, sizeof(state->message),
                 "Use Factory reset to restore the mouse itself.");
    } else {
        snprintf(state->message, sizeof(state->message),
                 "%s reset to defaults; press S to save.", name);
    }
}

static void handle_tab_key(struct ui_state *state, enum ui_tab tab,
                           int *selected, int key) {
    switch (tab) {
        case TAB_GENERAL: handle_general_key(state, *selected, key); break;
        case TAB_BUTTONS: handle_buttons_key(state, tab, *selected, key); break;
        case TAB_SENSOR: handle_sensor_key(state, *selected, key); break;
        case TAB_LIGHTING: handle_lighting_key(state, *selected, key); break;
        case TAB_POWER: handle_power_key(state, *selected, key); break;
        case TAB_MACROS: handle_macros_key(state, tab, selected, key); break;
        default:
            if (key == '\n' || key == '\r' || key == KEY_ENTER) {
                if (*selected == DEVICE_ROW_PAIR) pair_receiver(state, tab, *selected);
                else factory_reset(state, tab, *selected);
            }
            break;
    }
}

static int run_ui(void) {
    struct ui_state state;
    memset(&state, 0, sizeof(state));
    device_status_clear(&state.device);
    int loaded = load_state(&state);
    refresh_device(&state, 0);
    if (loaded == STATE_MISSING) {
        snprintf(state.message, sizeof(state.message),
                 "No saved state; defaults are staged. Press S to apply.");
    } else if (loaded == STATE_INVALID) {
        snprintf(state.message, sizeof(state.message),
                 "Saved state was unreadable; defaults are staged. Press S to apply.");
    } else if (loaded == STATE_IMPORTED) {
        snprintf(state.message, sizeof(state.message),
                 "Previous state imported; new settings are staged. Press S to apply.");
    } else if (state.dirty != 0) {
        snprintf(state.message, sizeof(state.message),
                 "Some settings were never written; press S to apply them.");
    } else {
        snprintf(state.message, sizeof(state.message), "Ready.");
    }

    int selected = 0;
    int quit_armed = 0;
    enum ui_tab tab = TAB_GENERAL;
    initialize_curses();

    for (;;) {
        int item_count = tab_item_count(&state, tab);
        selected = clamp_int(selected, 0, item_count - 1);
        draw_ui(&state, tab, selected);
        int key = getch();
        if (key == ERR) {
            refresh_device(&state, 0);
            continue;
        }

        if (key == 'q' || key == 'Q' || key == 27) {
            if (state.dirty == 0 || quit_armed) {
                break;
            }
            quit_armed = 1;
            snprintf(state.message, sizeof(state.message),
                     "Unsaved changes. Press Q again to quit without saving.");
            continue;
        }
        quit_armed = 0;

        if (key == '\t' || key == KEY_BTAB) {
            tab = key == KEY_BTAB ? (tab + TAB_COUNT - 1) % TAB_COUNT :
                                    (tab + 1) % TAB_COUNT;
            selected = 0;
        } else if (key == KEY_UP) {
            selected = wrap_int(selected - 1, item_count);
        } else if (key == KEY_DOWN) {
            selected = wrap_int(selected + 1, item_count);
        } else if (key == 's' || key == 'S') {
            save_settings(&state);
        } else if (key == 'r' || key == 'R') {
            snprintf(state.message, sizeof(state.message),
                     refresh_device(&state, 1) ? "Refreshed." : "Device not found.");
        } else if (key == 'd' || key == 'D') {
            reset_current_tab(&state, tab);
            if (tab == TAB_MACROS) selected = 0;
        } else {
            handle_tab_key(&state, tab, &selected, key);
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
    if (!resolve_paths()) {
        fprintf(stderr, "mousectl needs HOME to locate its state file.\n");
        return EXIT_FAILURE;
    }

    int instance_lock = open(lock_path, O_CREAT | O_RDWR, 0600);
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
