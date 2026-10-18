#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"
#define DEFAULT_CONFIG "mouse.conf"
/* Delay between consecutive attempts to find the device */
#define RECONNECT_DELAY_MS 1000
/* Wait after a config change so that editors can finish writing the file */
#define RELOAD_DEBOUNCE_MS 150
/* Time given to the compositor to pick up the virtual device before grabbing */
#define UINPUT_SETTLE_MS 300
#define MAX_BINDINGS 128
#define MAX_COMMAND 512
#define MAX_CONFIG_LINE 1024
#define MAX_MICE 32
#define MAX_WATCHES 2

/* Device access management via udev rules */
#define RULES_DIR "/etc/udev/rules.d"
#define RULE_PREFIX "99-mbtn-"
#define RULE_SUFFIX ".rules"
#define ACCESS_GROUP "mbtn-input"

/* uinput access (needed only when blocking is used) */
#define UINPUT_PATH "/dev/uinput"
#define UINPUT_RULE_FILE RULES_DIR "/" RULE_PREFIX "uinput" RULE_SUFFIX
#define MODULES_FILE "/etc/modules-load.d/mbtn-uinput.conf"
/* Name prefix of the virtual device created for blocking */
#define VIRTUAL_PREFIX "mbtn virtual: "

/* Supported button codes: BTN_MISC (0x100) .. BTN_MISC + 63 (0x13f) */
#define BUTTON_BASE BTN_MISC
#define BUTTON_SLOTS 64

enum trigger_type {
    TRIG_WHEEL_UP,
    TRIG_WHEEL_DOWN,
    TRIG_WHEEL_LEFT,
    TRIG_WHEEL_RIGHT,
    TRIG_CLICK, /* on button press */
    TRIG_TAP    /* on release, if the button was not used in a combination */
};

struct binding {
    uint64_t modifiers;
    enum trigger_type type;
    int button_index; /* -1 for wheel triggers */
    int block;        /* '!' prefix: the trigger event is not forwarded */
    char command[MAX_COMMAND]; /* may be empty when block is set */
};

struct config {
    char device_name[256];
    int wheel_delay_ms;  /* horizontal wheel */
    int scroll_delay_ms; /* vertical wheel */
    struct binding bindings[MAX_BINDINGS];
    size_t binding_count;
    int needs_wheel;
    int needs_hwheel;
    uint64_t needs_buttons; /* buttons the device must provide */
    uint64_t tracked;       /* buttons whose state must be tracked */
    uint64_t swallow;       /* buttons whose press is deferred (level 2) */
    int needs_grab;         /* any blocking is configured */
};

struct state {
    uint64_t held;
    uint64_t used;           /* held buttons that already took part in a combination */
    uint64_t pending;        /* swallowed buttons: press not forwarded yet */
    uint64_t blocked_down;   /* buttons whose press was blocked: drop the release too */
    uint64_t forwarded_down; /* buttons whose press was forwarded to the client */
    long long last_hwheel;
    long long last_wheel;
    int dropping;            /* discarding events up to the next SYN_REPORT */
};

/* Virtual uinput device used to forward events while the real one is grabbed */
struct sink {
    int fd; /* -1 when blocking is not active */
    int dirty;
    int write_failed;
};

struct watch {
    int wd;
    char name[NAME_MAX + 1];
};

struct reloader {
    int fd; /* inotify descriptor, -1 when auto-reload is unavailable */
    struct watch watches[MAX_WATCHES];
    size_t count;
};

struct button_name {
    const char *name;
    int code;
};

static const struct button_name button_names[] = {
    {"LEFT", BTN_LEFT},       {"RIGHT", BTN_RIGHT}, {"MIDDLE", BTN_MIDDLE},
    {"SIDE", BTN_SIDE},       {"EXTRA", BTN_EXTRA}, {"FORWARD", BTN_FORWARD},
    {"BACK", BTN_BACK},       {"TASK", BTN_TASK},   {"0", BTN_0},
    {"1", BTN_1},             {"2", BTN_2},         {"3", BTN_3},
    {"4", BTN_4},             {"5", BTN_5},         {"6", BTN_6},
    {"7", BTN_7},             {"8", BTN_8},         {"9", BTN_9},
};

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NLONGS(bits) (((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG)

/* A mouse-like input device found under /dev/input */
struct mouse {
    char path[300];
    int event_number;
    char name[256];
    struct input_id id;
    unsigned long key_bits[NLONGS(KEY_MAX + 1)];
    int has_wheel;
    int has_hwheel;
    int granted; /* an mbtn udev rule exists for this VID:PID */
};

/* Set by the signal handler to stop --monitor */
static volatile sig_atomic_t stop_requested;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

static int test_bit(int bit, const unsigned long *array)
{
    return (array[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;

    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                          text[length - 1] == '\r' || text[length - 1] == '\n'))
        text[--length] = '\0';

    return text;
}

/* Returns the button index (0..63) or -1. Accepts LEFT, BTN_LEFT, CODE_0x118 */
static int button_from_name(const char *name)
{
    if (strncasecmp(name, "BTN_", 4) == 0)
        name += 4;

    for (size_t i = 0; i < sizeof(button_names) / sizeof(button_names[0]); i++) {
        if (strcasecmp(name, button_names[i].name) == 0)
            return button_names[i].code - BUTTON_BASE;
    }

    if (strncasecmp(name, "CODE_", 5) == 0) {
        char *end = NULL;
        errno = 0;
        long code = strtol(name + 5, &end, 0);
        if (errno || end == name + 5 || *end)
            return -1;
        if (code < BUTTON_BASE || code >= BUTTON_BASE + BUTTON_SLOTS)
            return -1;
        return (int)(code - BUTTON_BASE);
    }

    return -1;
}

static const char *button_label(int code, char *buffer, size_t capacity)
{
    for (size_t i = 0; i < sizeof(button_names) / sizeof(button_names[0]); i++) {
        if (button_names[i].code == code)
            return button_names[i].name;
    }
    snprintf(buffer, capacity, "CODE_0x%x", code);
    return buffer;
}

static int parse_wheel(const char *token, enum trigger_type *type)
{
    if (strcasecmp(token, "WHEEL_UP") == 0)
        *type = TRIG_WHEEL_UP;
    else if (strcasecmp(token, "WHEEL_DOWN") == 0)
        *type = TRIG_WHEEL_DOWN;
    else if (strcasecmp(token, "WHEEL_LEFT") == 0)
        *type = TRIG_WHEEL_LEFT;
    else if (strcasecmp(token, "WHEEL_RIGHT") == 0)
        *type = TRIG_WHEEL_RIGHT;
    else
        return -1;
    return 0;
}

/* Recognizes <BUTTON>_CLICK and <BUTTON>_TAP */
static int parse_button_trigger(const char *token, enum trigger_type *type, int *index)
{
    char base[64];
    size_t length = strlen(token);

    if (length >= 6 && strcasecmp(token + length - 6, "_CLICK") == 0) {
        *type = TRIG_CLICK;
        length -= 6;
    } else if (length >= 4 && strcasecmp(token + length - 4, "_TAP") == 0) {
        *type = TRIG_TAP;
        length -= 4;
    } else {
        return -1;
    }

    if (length == 0 || length >= sizeof(base))
        return -1;
    memcpy(base, token, length);
    base[length] = '\0';

    *index = button_from_name(base);
    return *index < 0 ? -1 : 0;
}

static int parse_delay(const char *value, int *out)
{
    char *end = NULL;
    errno = 0;
    long delay = strtol(value, &end, 10);
    if (errno || end == value || *trim(end) || delay < 0 || delay > INT_MAX)
        return -1;
    *out = (int)delay;
    return 0;
}

/* Parses "swallow = SIDE, EXTRA" */
static int parse_swallow(struct config *config, char *list)
{
    char *saveptr = NULL;
    int any = 0;

    for (char *token = strtok_r(list, ",", &saveptr); token;
         token = strtok_r(NULL, ",", &saveptr)) {
        token = trim(token);
        int index = button_from_name(token);
        if (index < 0)
            return -1;

        uint64_t bit = 1ULL << index;
        config->swallow |= bit;
        config->tracked |= bit;
        config->needs_buttons |= bit;
        any = 1;
    }

    if (!any)
        return -1;
    config->needs_grab = 1;
    return 0;
}

static int parse_binding(struct config *config, char *name, char *command)
{
    uint64_t modifiers = 0;
    int have_trigger = 0;
    int block = 0;
    enum trigger_type type = TRIG_WHEEL_UP;
    int button_index = -1;
    char *saveptr = NULL;

    if (*name == '!') {
        block = 1;
        name = trim(name + 1);
    }

    char *token = strtok_r(name, "+", &saveptr);

    while (token) {
        token = trim(token);
        enum trigger_type parsed_type;
        int index;

        if (parse_wheel(token, &parsed_type) == 0) {
            if (have_trigger)
                return -1;
            type = parsed_type;
            have_trigger = 1;
            if (type == TRIG_WHEEL_UP || type == TRIG_WHEEL_DOWN)
                config->needs_wheel = 1;
            else
                config->needs_hwheel = 1;
        } else if (parse_button_trigger(token, &parsed_type, &index) == 0) {
            if (have_trigger)
                return -1;
            type = parsed_type;
            button_index = index;
            have_trigger = 1;
        } else {
            index = button_from_name(token);
            if (index < 0)
                return -1;
            uint64_t bit = 1ULL << index;
            if (modifiers & bit)
                return -1;
            modifiers |= bit;
        }
        token = strtok_r(NULL, "+", &saveptr);
    }

    /* An empty command is allowed only for blocking-only bindings */
    if (!have_trigger || config->binding_count == MAX_BINDINGS || (!*command && !block))
        return -1;

    if (button_index >= 0) {
        uint64_t bit = 1ULL << button_index;
        if (modifiers & bit)
            return -1;
        config->needs_buttons |= bit;
        if (type == TRIG_TAP)
            config->tracked |= bit;
    }
    config->needs_buttons |= modifiers;
    config->tracked |= modifiers;

    if (*command && command[0] == '"') {
        size_t length = strlen(command);
        if (length < 2 || command[length - 1] != '"')
            return -1;
        command[length - 1] = '\0';
        command++;
        if (!*command && !block)
            return -1;
    }

    if (strlen(command) >= MAX_COMMAND)
        return -1;

    for (size_t i = 0; i < config->binding_count; i++) {
        const struct binding *other = &config->bindings[i];
        if (other->modifiers == modifiers && other->type == type &&
            other->button_index == button_index)
            return -1;
    }

    if (block)
        config->needs_grab = 1;

    struct binding *binding = &config->bindings[config->binding_count++];
    binding->modifiers = modifiers;
    binding->type = type;
    binding->button_index = button_index;
    binding->block = block;
    strcpy(binding->command, command);
    return 0;
}

static int load_config(const char *path, struct config *config)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Cannot open config %s: %s\n", path, strerror(errno));
        return -1;
    }

    memset(config, 0, sizeof(*config));
    char line[MAX_CONFIG_LINE];
    unsigned int line_number = 0;
    int have_device_name = 0;
    int result = 0;

    while (fgets(line, sizeof(line), file)) {
        line_number++;
        char *entry = trim(line);
        if (!*entry || *entry == '#')
            continue;

        char *equals = strchr(entry, '=');
        if (!equals) {
            result = -1;
            break;
        }
        *equals = '\0';
        char *name = trim(entry);
        char *value = trim(equals + 1);

        if (strcmp(name, "device_name") == 0) {
            if (!*value || strlen(value) >= sizeof(config->device_name)) {
                result = -1;
                break;
            }
            strcpy(config->device_name, value);
            have_device_name = 1;
        } else if (strcmp(name, "wheel_delay") == 0) {
            if (parse_delay(value, &config->wheel_delay_ms) < 0) {
                result = -1;
                break;
            }
        } else if (strcmp(name, "scroll_delay") == 0) {
            if (parse_delay(value, &config->scroll_delay_ms) < 0) {
                result = -1;
                break;
            }
        } else if (strcmp(name, "swallow") == 0) {
            if (parse_swallow(config, value) < 0) {
                result = -1;
                break;
            }
        } else if (parse_binding(config, name, value) < 0) {
            result = -1;
            break;
        }
    }

    if (ferror(file))
        result = -1;
    fclose(file);

    if (result < 0) {
        fprintf(stderr, "Invalid config %s at line %u\n", path, line_number);
        return -1;
    }

    if (!have_device_name || config->binding_count == 0) {
        fprintf(stderr, "Config %s needs device_name and at least one binding\n", path);
        return -1;
    }

    /* '!' on a *_TAP trigger only makes sense for swallowed buttons */
    for (size_t i = 0; i < config->binding_count; i++) {
        const struct binding *binding = &config->bindings[i];
        if (binding->block && binding->type == TRIG_TAP &&
            !(config->swallow & (1ULL << binding->button_index))) {
            char buffer[32];
            fprintf(stderr,
                    "Config %s: '!%s_TAP' requires %s in the 'swallow' directive\n", path,
                    button_label(BUTTON_BASE + binding->button_index, buffer, sizeof(buffer)),
                    buffer);
            return -1;
        }
    }

    return 0;
}

/*
 * Checks the device capabilities required by the configured bindings.
 * This skips the other event nodes of a device that share the same name.
 */
static int has_required_caps(int fd, const struct config *config)
{
    unsigned long rel_bits[NLONGS(REL_MAX + 1)];
    unsigned long key_bits[NLONGS(KEY_MAX + 1)];

    memset(rel_bits, 0, sizeof(rel_bits));
    memset(key_bits, 0, sizeof(key_bits));

    if (config->needs_wheel || config->needs_hwheel) {
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0)
            return 0;
        if (config->needs_wheel && !test_bit(REL_WHEEL, rel_bits))
            return 0;
        if (config->needs_hwheel && !test_bit(REL_HWHEEL, rel_bits))
            return 0;
    }

    if (config->needs_buttons) {
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
            return 0;
        for (int i = 0; i < BUTTON_SLOTS; i++) {
            if ((config->needs_buttons >> i) & 1ULL &&
                !test_bit(BUTTON_BASE + i, key_bits))
                return 0;
        }
    }

    return 1;
}

/*
 * Scans /dev/input/event* and returns the fd of the matching device, or -1.
 * The path of the opened node is stored in found_path.
 */
static int find_device(const struct config *config, char *found_path, size_t capacity)
{
    DIR *dir = opendir(INPUT_DIR);
    if (!dir)
        return -1;

    int found_fd = -1;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", INPUT_DIR, de->d_name);

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;

        char name[256] = "";
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
            close(fd);
            continue;
        }

        if (strcmp(name, config->device_name) == 0 && has_required_caps(fd, config)) {
            snprintf(found_path, capacity, "%s", path);
            found_fd = fd;
            break;
        }

        close(fd);
    }

    closedir(dir);
    return found_fd;
}

/* ------------------------------------------------------------------ */
/*  Virtual device (uinput) used for blocking                          */
/* ------------------------------------------------------------------ */

static void sink_emit(struct sink *sink, int type, int code, int value)
{
    if (sink->fd < 0)
        return;

    struct input_event event;
    memset(&event, 0, sizeof(event));
    event.type = (unsigned short)type;
    event.code = (unsigned short)code;
    event.value = value;

    if (write(sink->fd, &event, sizeof(event)) != (ssize_t)sizeof(event) &&
        !sink->write_failed) {
        fprintf(stderr, "Write to the virtual device failed: %s\n", strerror(errno));
        sink->write_failed = 1;
    }
}

static void forward_event(struct sink *sink, const struct input_event *ev)
{
    if (sink->fd < 0)
        return;
    sink_emit(sink, ev->type, ev->code, ev->value);
    sink->dirty = 1;
}

/* Emits SYN_REPORT, but only if something was forwarded since the last one */
static void sink_flush(struct sink *sink)
{
    if (sink->fd >= 0 && sink->dirty) {
        sink_emit(sink, EV_SYN, SYN_REPORT, 0);
        sink->dirty = 0;
    }
}

/* Sends a complete click (press, report, release) of a button */
static void sink_click(struct sink *sink, int code)
{
    sink_emit(sink, EV_KEY, code, 1);
    sink_emit(sink, EV_SYN, SYN_REPORT, 0);
    sink_emit(sink, EV_KEY, code, 0);
    sink->dirty = 1;
}

static void sink_close(struct sink *sink)
{
    if (sink->fd >= 0) {
        ioctl(sink->fd, UI_DEV_DESTROY);
        close(sink->fd);
    }
    sink->fd = -1;
    sink->dirty = 0;
}

/*
 * Creates a virtual device that mirrors the capabilities of the source
 * device. Returns NULL on success or a message describing the failure
 * (errno is preserved for the caller).
 */
static const char *sink_open(struct sink *sink, int src_fd, const char *src_name)
{
    static const struct {
        int type;
        int max;
        unsigned long request;
    } copied[] = {
        {EV_KEY, KEY_MAX, UI_SET_KEYBIT},
        {EV_REL, REL_MAX, UI_SET_RELBIT},
        {EV_MSC, MSC_MAX, UI_SET_MSCBIT},
    };

    const char *error = NULL;
    int ufd = open(UINPUT_PATH, O_WRONLY | O_CLOEXEC);
    if (ufd < 0)
        return "cannot open " UINPUT_PATH " (see --grant-uinput)";

    struct input_id id;
    unsigned long ev_bits[NLONGS(EV_MAX + 1)];
    memset(ev_bits, 0, sizeof(ev_bits));

    if (ioctl(src_fd, EVIOCGID, &id) < 0 ||
        ioctl(src_fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        error = "cannot read the device capabilities";
        goto fail;
    }

    if (test_bit(EV_ABS, ev_bits))
        fprintf(stderr, "Warning: absolute axes (EV_ABS) are not forwarded\n");

    for (size_t i = 0; i < sizeof(copied) / sizeof(copied[0]); i++) {
        if (!test_bit(copied[i].type, ev_bits))
            continue;

        unsigned long bits[NLONGS(KEY_MAX + 1)];
        memset(bits, 0, sizeof(bits));

        if (ioctl(ufd, UI_SET_EVBIT, copied[i].type) < 0 ||
            ioctl(src_fd, EVIOCGBIT(copied[i].type, sizeof(bits)), bits) < 0) {
            error = "cannot copy the device capabilities";
            goto fail;
        }
        for (int code = 0; code <= copied[i].max; code++) {
            if (test_bit(code, bits) && ioctl(ufd, copied[i].request, code) < 0) {
                error = "cannot copy the device capabilities";
                goto fail;
            }
        }
    }

    unsigned long props[NLONGS(INPUT_PROP_MAX + 1)];
    memset(props, 0, sizeof(props));
    if (ioctl(src_fd, EVIOCGPROP(sizeof(props)), props) >= 0) {
        for (int prop = 0; prop <= INPUT_PROP_MAX; prop++) {
            if (test_bit(prop, props))
                ioctl(ufd, UI_SET_PROPBIT, prop);
        }
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id = id;
    snprintf(setup.name, sizeof(setup.name), "%s%.*s", VIRTUAL_PREFIX,
             (int)(sizeof(setup.name) - 1 - strlen(VIRTUAL_PREFIX)), src_name);

    if (ioctl(ufd, UI_DEV_SETUP, &setup) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) {
        error = "cannot create the virtual device (is the uinput module loaded?)";
        goto fail;
    }

    sink->fd = ufd;
    sink->dirty = 0;
    sink->write_failed = 0;
    /* Give the compositor time to open the new device before we grab the real one */
    sleep_ms(UINPUT_SETTLE_MS);
    return NULL;

fail: {
    int saved = errno;
    close(ufd);
    errno = saved;
    return error;
}
}

/*
 * Finds and opens the configured device. When blocking is configured it
 * also creates the virtual device and grabs the real one. Returns the fd
 * or -1 (not found or setup failed; the caller retries later).
 */
static int open_input(const struct config *config, struct sink *sink, int *warned_missing,
                      int *reported_failure)
{
    char path[PATH_MAX];
    int fd = find_device(config, path, sizeof(path));

    if (fd < 0) {
        if (!*warned_missing) {
            fprintf(stderr, "Device \"%s\" not found. Waiting...\n", config->device_name);
            *warned_missing = 1;
        }
        return -1;
    }
    *warned_missing = 0;

    if (config->needs_grab) {
        char name[256] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);

        const char *error = sink_open(sink, fd, name);
        if (!error && ioctl(fd, EVIOCGRAB, 1) < 0)
            error = "cannot grab the device (is another program holding it?)";

        if (error) {
            int saved = errno;
            if (!*reported_failure) {
                fprintf(stderr, "Blocking setup failed for %s: %s: %s\n", path, error,
                        strerror(saved));
                *reported_failure = 1;
            }
            sink_close(sink);
            close(fd);
            return -1;
        }
    }
    *reported_failure = 0;

    fprintf(stderr, "Opened %s%s\n", path,
            config->needs_grab ? " (blocking enabled: device grabbed)" : "");
    return fd;
}

/* Closing the descriptor also releases the grab */
static void close_input(int *fd, struct sink *sink)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
    sink_close(sink);
}

/* ------------------------------------------------------------------ */
/*  Config auto-reload (inotify)                                       */
/* ------------------------------------------------------------------ */

static int reloader_add_watch(struct reloader *reloader, const char *file_path)
{
    char dir[PATH_MAX];
    const char *name;
    const char *slash = strrchr(file_path, '/');

    if (slash) {
        size_t length = (size_t)(slash - file_path);
        if (length == 0) {
            strcpy(dir, "/");
        } else {
            if (length >= sizeof(dir))
                return -1;
            memcpy(dir, file_path, length);
            dir[length] = '\0';
        }
        name = slash + 1;
    } else {
        strcpy(dir, ".");
        name = file_path;
    }

    if (!*name || strlen(name) > NAME_MAX)
        return -1;

    /* Editors usually replace the file (rename), so watch the directory */
    int wd = inotify_add_watch(reloader->fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd < 0)
        return -1;

    for (size_t i = 0; i < reloader->count; i++) {
        if (reloader->watches[i].wd == wd && strcmp(reloader->watches[i].name, name) == 0)
            return 0;
    }
    if (reloader->count >= MAX_WATCHES)
        return -1;

    reloader->watches[reloader->count].wd = wd;
    strcpy(reloader->watches[reloader->count].name, name);
    reloader->count++;
    return 0;
}

static void reloader_init(struct reloader *reloader, const char *config_path)
{
    memset(reloader, 0, sizeof(*reloader));
    reloader->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (reloader->fd < 0) {
        fprintf(stderr, "Auto-reload disabled: inotify_init1: %s\n", strerror(errno));
        return;
    }

    int ok = reloader_add_watch(reloader, config_path) == 0;

    /* If the config is a symlink, also watch the file it points to */
    char resolved[PATH_MAX];
    if (realpath(config_path, resolved) && strcmp(resolved, config_path) != 0)
        reloader_add_watch(reloader, resolved);

    if (!ok && reloader->count == 0) {
        fprintf(stderr, "Auto-reload disabled: cannot watch %s\n", config_path);
        close(reloader->fd);
        reloader->fd = -1;
    }
}

/* Drains pending inotify events. Returns 1 if the config file changed */
static int reloader_poll(struct reloader *reloader)
{
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    int changed = 0;

    for (;;) {
        ssize_t n = read(reloader->fd, buffer, sizeof(buffer));
        if (n <= 0)
            break;

        for (char *p = buffer; p < buffer + n;) {
            const struct inotify_event *event = (const struct inotify_event *)p;
            if (event->len) {
                for (size_t i = 0; i < reloader->count; i++) {
                    if (event->wd == reloader->watches[i].wd &&
                        strcmp(event->name, reloader->watches[i].name) == 0)
                        changed = 1;
                }
            }
            p += sizeof(*event) + event->len;
        }
    }

    return changed;
}

/* ------------------------------------------------------------------ */
/*  Device discovery and access state                                  */
/* ------------------------------------------------------------------ */

static void rule_path(char *buffer, size_t capacity, unsigned vendor, unsigned product)
{
    snprintf(buffer, capacity, "%s/%s%04x-%04x%s", RULES_DIR, RULE_PREFIX,
             vendor, product, RULE_SUFFIX);
}

/* True if an mbtn udev rule file exists for this VID:PID */
static int rule_exists(unsigned vendor, unsigned product)
{
    char path[PATH_MAX];
    rule_path(path, sizeof(path), vendor, product);
    return access(path, F_OK) == 0;
}

static int compare_mice(const void *a, const void *b)
{
    const struct mouse *x = a;
    const struct mouse *y = b;
    return x->event_number - y->event_number;
}

/*
 * Collects mouse-like devices (those that have BTN_LEFT), sorted by event
 * number. Virtual devices created by mbtn are skipped. Sets *denied to the
 * number of nodes that could not be opened due to missing permissions.
 * Returns the number of devices stored.
 */
static size_t collect_mice(struct mouse *mice, size_t capacity, int *denied)
{
    *denied = 0;

    DIR *dir = opendir(INPUT_DIR);
    if (!dir) {
        perror("Cannot open " INPUT_DIR);
        return 0;
    }

    size_t count = 0;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL && count < capacity) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;

        char *end = NULL;
        long number = strtol(de->d_name + 5, &end, 10);
        if (end == de->d_name + 5 || *end)
            continue;

        struct mouse *mouse = &mice[count];
        memset(mouse, 0, sizeof(*mouse));

        int written = snprintf(mouse->path, sizeof(mouse->path), "%s/%s", INPUT_DIR,
                               de->d_name);
        if (written < 0 || (size_t)written >= sizeof(mouse->path))
            continue;

        int fd = open(mouse->path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES)
                (*denied)++;
            continue;
        }

        unsigned long rel_bits[NLONGS(REL_MAX + 1)];
        memset(rel_bits, 0, sizeof(rel_bits));

        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(mouse->key_bits)), mouse->key_bits) < 0 ||
            !test_bit(BTN_LEFT, mouse->key_bits) ||
            ioctl(fd, EVIOCGID, &mouse->id) < 0) {
            close(fd);
            continue;
        }
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits);

        if (ioctl(fd, EVIOCGNAME(sizeof(mouse->name) - 1), mouse->name) < 0)
            snprintf(mouse->name, sizeof(mouse->name), "?");
        close(fd);

        if (strncmp(mouse->name, VIRTUAL_PREFIX, strlen(VIRTUAL_PREFIX)) == 0)
            continue;

        mouse->event_number = (int)number;
        mouse->has_wheel = test_bit(REL_WHEEL, rel_bits);
        mouse->has_hwheel = test_bit(REL_HWHEEL, rel_bits);
        mouse->granted = rule_exists(mouse->id.vendor, mouse->id.product);
        count++;
    }

    closedir(dir);
    qsort(mice, count, sizeof(mice[0]), compare_mice);
    return count;
}

/* Prints whether blocking can work: uinput node, access, rule, module autoload */
static void print_uinput_status(void)
{
    printf("uinput (needed only for blocking):\n");

    if (access(UINPUT_PATH, F_OK) == 0) {
        printf("    device node: %s present\n", UINPUT_PATH);
        printf("    write access (current session): %s\n",
               access(UINPUT_PATH, W_OK) == 0 ? "yes" : "no");
    } else {
        printf("    device node: %s missing (uinput module not loaded?)\n", UINPUT_PATH);
        printf("    write access (current session): no\n");
    }

    if (access(UINPUT_RULE_FILE, F_OK) == 0)
        printf("    access: granted (rule %s)\n", UINPUT_RULE_FILE);
    else
        printf("    access: not granted (see --grant-uinput)\n");

    printf("    module autoload: %s\n",
           access(MODULES_FILE, F_OK) == 0 ? "configured" : "not configured");
}

/* Prints all detected mice together with their buttons and access state */
static int list_devices(void)
{
    struct mouse mice[MAX_MICE];
    int denied = 0;
    size_t count = collect_mice(mice, MAX_MICE, &denied);

    if (count == 0) {
        fprintf(stderr, "No mouse-like devices found.%s\n",
                denied ? " Permission denied for some nodes (try sudo, --grant or the 'input' group)."
                       : "");
    }

    for (size_t i = 0; i < count; i++) {
        const struct mouse *mouse = &mice[i];

        printf("%s: device_name = %s\n", mouse->path, mouse->name);
        printf("    id: %04x:%04x\n", (unsigned)mouse->id.vendor,
               (unsigned)mouse->id.product);

        if (mouse->granted) {
            char path[PATH_MAX];
            rule_path(path, sizeof(path), mouse->id.vendor, mouse->id.product);
            printf("    access: granted (rule %s)\n", path);
        } else {
            printf("    access: not granted\n");
        }

        printf("    buttons:");
        for (int b = 0; b < BUTTON_SLOTS; b++) {
            if (test_bit(BUTTON_BASE + b, mouse->key_bits)) {
                char buffer[32];
                printf(" %s", button_label(BUTTON_BASE + b, buffer, sizeof(buffer)));
            }
        }
        printf("\n    wheel: vertical=%s horizontal=%s\n",
               mouse->has_wheel ? "yes" : "no", mouse->has_hwheel ? "yes" : "no");
    }

    if (count > 0)
        printf("\n");
    print_uinput_status();

    if (denied && count > 0)
        fprintf(stderr, "Note: %d input node(s) could not be opened (permission denied).\n",
                denied);
    return count == 0 ? 1 : 0;
}

/*
 * Shows a numbered menu of devices and asks the user to pick one.
 * only_granted: 0 = show all devices, 1 = show only devices with granted access.
 * Returns the index into mice[], or -1 when nothing was selected.
 */
static int pick_mouse(const struct mouse *mice, size_t count, int only_granted)
{
    size_t candidates[MAX_MICE];
    size_t candidate_count = 0;

    for (size_t i = 0; i < count; i++) {
        if (!only_granted || mice[i].granted)
            candidates[candidate_count++] = i;
    }

    if (candidate_count == 0) {
        if (only_granted)
            fprintf(stderr,
                    "No connected device has granted access.\n"
                    "For a disconnected mouse use: --revoke VID:PID\n");
        else
            fprintf(stderr, "No mouse-like devices found.\n");
        return -1;
    }

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "No device given and stdin is not a terminal; pass a device explicitly\n");
        return -1;
    }

    printf("%s:\n", only_granted ? "Devices with granted access" : "Detected devices");
    for (size_t i = 0; i < candidate_count; i++) {
        const struct mouse *mouse = &mice[candidates[i]];
        printf("  [%zu] %s  %s (%04x:%04x)%s\n", i + 1, mouse->path, mouse->name,
               (unsigned)mouse->id.vendor, (unsigned)mouse->id.product,
               mouse->granted ? " [granted]" : "");
    }

    for (;;) {
        printf("Select device number (q to quit): ");
        fflush(stdout);

        char line[64];
        if (!fgets(line, sizeof(line), stdin))
            return -1;

        char *text = trim(line);
        if (strcasecmp(text, "q") == 0)
            return -1;

        char *end = NULL;
        errno = 0;
        long choice = strtol(text, &end, 10);
        if (!errno && end != text && !*end && choice >= 1 &&
            (size_t)choice <= candidate_count)
            return (int)candidates[choice - 1];

        printf("Invalid choice.\n");
    }
}

/* ------------------------------------------------------------------ */
/*  Access management (udev rules + group)                             */
/* ------------------------------------------------------------------ */

/* Runs a program without a shell and waits for it to finish. 0 = success */
static int run_program(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;

    fprintf(stderr, "Command failed: %s\n", argv[0]);
    return -1;
}

/* Reloads udev rules and re-applies them to devices of one subsystem */
static int reload_udev(const char *subsystem)
{
    char match[64];
    snprintf(match, sizeof(match), "--subsystem-match=%s", subsystem);

    char *reload[] = {"udevadm", "control", "--reload-rules", NULL};
    char *trigger[] = {"udevadm", "trigger", match, NULL};

    if (run_program(reload) < 0)
        return -1;
    return run_program(trigger);
}

/* Returns non-zero if any mbtn rule file (mouse or uinput) is still present */
static int rules_remain(void)
{
    DIR *dir = opendir(RULES_DIR);
    if (!dir)
        return 0;

    size_t prefix_length = strlen(RULE_PREFIX);
    size_t suffix_length = strlen(RULE_SUFFIX);
    int remain = 0;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        size_t length = strlen(de->d_name);
        if (length > prefix_length + suffix_length &&
            strncmp(de->d_name, RULE_PREFIX, prefix_length) == 0 &&
            strcmp(de->d_name + length - suffix_length, RULE_SUFFIX) == 0) {
            remain = 1;
            break;
        }
    }

    closedir(dir);
    return remain;
}

/*
 * The value is written to a udev rule inside double quotes. Reject
 * characters whose handling (escaping, glob patterns) was not verified
 * against the udev documentation.
 */
static int is_safe_rule_value(const char *text)
{
    if (!*text)
        return 0;

    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\' || c == '*' ||
            c == '?' || c == '[' || c == ']' || c == '|')
            return 0;
    }
    return 1;
}

static int parse_vid_pid(const char *text, unsigned *vendor, unsigned *product)
{
    unsigned v, p;
    int consumed = 0;

    if (sscanf(text, "%x:%x%n", &v, &p, &consumed) != 2 || text[consumed] ||
        v > 0xffff || p > 0xffff)
        return -1;

    *vendor = v;
    *product = p;
    return 0;
}

/* Reads name and VID:PID; refuses devices without BTN_LEFT (not a mouse) */
static int read_identity(const char *path, char *name, size_t capacity,
                         struct input_id *id)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    unsigned long key_bits[NLONGS(KEY_MAX + 1)];
    memset(key_bits, 0, sizeof(key_bits));
    memset(name, 0, capacity);
    memset(id, 0, sizeof(*id));

    int ok = ioctl(fd, EVIOCGNAME(capacity - 1), name) >= 0 &&
             ioctl(fd, EVIOCGID, id) >= 0 &&
             ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0;
    close(fd);

    if (!ok) {
        fprintf(stderr, "%s is not an input event device\n", path);
        return -1;
    }
    if (!test_bit(BTN_LEFT, key_bits)) {
        fprintf(stderr,
                "%s does not look like a mouse (no BTN_LEFT); refusing to change its permissions\n",
                path);
        return -1;
    }
    return 0;
}

static int ensure_group(void)
{
    if (getgrnam(ACCESS_GROUP))
        return 0;

    char *create[] = {"groupadd", "--system", ACCESS_GROUP, NULL};
    return run_program(create);
}

static int write_text_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fputs(text, file);
    if (ferror(file) || fclose(file) != 0) {
        fprintf(stderr, "Error while writing %s\n", path);
        return -1;
    }
    printf("Wrote %s\n", path);
    return 0;
}

/* Removes the membership and the group, but only when no mbtn rule is left */
static int remove_group_if_unused(const char *user)
{
    if (rules_remain() || !getgrnam(ACCESS_GROUP))
        return 0;

    char *remove_user[] = {"gpasswd", "-d", (char *)user, ACCESS_GROUP, NULL};
    char *delete_group[] = {"groupdel", ACCESS_GROUP, NULL};

    run_program(remove_user); /* may fail if the user was not a member */
    if (run_program(delete_group) < 0)
        return -1;
    printf("Removed group %s\n", ACCESS_GROUP);
    return 0;
}

static int grant_access(const char *user, const char *name, const struct input_id *id)
{
    if (!is_safe_rule_value(name)) {
        fprintf(stderr,
                "Device name \"%s\" contains characters that cannot be safely written to a "
                "udev rule (control characters, quotes, backslash or any of *?[]|)\n",
                name);
        return 1;
    }

    if (ensure_group() < 0)
        return 1;

    char *add_user[] = {"usermod", "-aG", ACCESS_GROUP, (char *)user, NULL};
    if (run_program(add_user) < 0)
        return 1;

    char path[PATH_MAX];
    rule_path(path, sizeof(path), id->vendor, id->product);

    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Cannot write %s: %s\n", path, strerror(errno));
        return 1;
    }

    /*
     * Match keys use "==", assignment keys (GROUP, MODE) use a single "=".
     */
    fprintf(file,
            "# Generated by mbtn --grant. Remove with: mbtn --revoke %04x:%04x\n"
            "SUBSYSTEM==\"input\", KERNEL==\"event*\", ATTRS{name}==\"%s\", "
            "ATTRS{id/vendor}==\"%04x\", ATTRS{id/product}==\"%04x\", "
            "GROUP=\"" ACCESS_GROUP "\", MODE=\"0640\"\n",
            (unsigned)id->vendor, (unsigned)id->product, name,
            (unsigned)id->vendor, (unsigned)id->product);
    if (ferror(file) || fclose(file) != 0) {
        fprintf(stderr, "Error while writing %s\n", path);
        return 1;
    }
    printf("Wrote %s\n", path);

    if (reload_udev("input") < 0)
        return 1;

    printf("Done. User \"%s\" must log out and back in (or reboot) for the new "
           "group membership to take effect.\n", user);
    return 0;
}

static int revoke_access(const char *user, unsigned vendor, unsigned product)
{
    char path[PATH_MAX];
    rule_path(path, sizeof(path), vendor, product);

    if (unlink(path) < 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "Cannot remove %s: %s\n", path, strerror(errno));
            return 1;
        }
        fprintf(stderr, "No rule at %s (nothing to remove)\n", path);
    } else {
        printf("Removed %s\n", path);
    }

    if (remove_group_if_unused(user) < 0)
        return 1;
    return reload_udev("input") < 0 ? 1 : 0;
}

/*
 * Blocking needs write access to /dev/uinput. Members of the group can
 * create virtual input devices, i.e. inject input events of any kind,
 * including keyboard events.
 */
static int grant_uinput(const char *user)
{
    if (ensure_group() < 0)
        return 1;

    char *add_user[] = {"usermod", "-aG", ACCESS_GROUP, (char *)user, NULL};
    if (run_program(add_user) < 0)
        return 1;

    if (write_text_file(UINPUT_RULE_FILE,
                        "# Generated by mbtn --grant-uinput. Remove with: mbtn --revoke-uinput\n"
                        "KERNEL==\"uinput\", SUBSYSTEM==\"misc\", GROUP=\"" ACCESS_GROUP
                        "\", MODE=\"0660\"\n") < 0)
        return 1;

    if (write_text_file(MODULES_FILE,
                        "# Generated by mbtn --grant-uinput\nuinput\n") < 0)
        return 1;

    char *modprobe[] = {"modprobe", "uinput", NULL};
    if (run_program(modprobe) < 0)
        fprintf(stderr, "Warning: could not load the uinput module now\n");

    if (reload_udev("misc") < 0)
        return 1;

    printf("Done. Members of group %s can now create virtual input devices.\n"
           "User \"%s\" must log out and back in (or reboot) for the new group "
           "membership to take effect.\n", ACCESS_GROUP, user);
    return 0;
}

static int revoke_uinput(const char *user)
{
    const char *files[] = {UINPUT_RULE_FILE, MODULES_FILE};

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (unlink(files[i]) < 0) {
            if (errno != ENOENT) {
                fprintf(stderr, "Cannot remove %s: %s\n", files[i], strerror(errno));
                return 1;
            }
        } else {
            printf("Removed %s\n", files[i]);
        }
    }

    if (remove_group_if_unused(user) < 0)
        return 1;
    return reload_udev("misc") < 0 ? 1 : 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [config-file] [--replace-existing] (default: " DEFAULT_CONFIG ")\n"
            "       %s --list\n"
            "       %s --monitor [/dev/input/eventN]\n"
            "       sudo %s --grant [/dev/input/eventN] [--user NAME]\n"
            "       sudo %s --revoke [/dev/input/eventN | VID:PID] [--user NAME]\n"
            "       sudo %s --grant-uinput [--user NAME]\n"
            "       sudo %s --revoke-uinput [--user NAME]\n"
            "\n"
            "--list lists detected mouse-like devices and their access status.\n"
            "--monitor prints button and wheel events as config binding names.\n"
            "--grant adds an input access rule for a device and user (requires sudo).\n"
            "--revoke removes an input access rule (requires sudo).\n"
            "--grant-uinput allows the user to create virtual input devices; needed only\n"
            "for blocking ('!' and 'swallow' in the config) (requires sudo).\n"
            "--revoke-uinput removes that access again (requires sudo).\n"
            "--replace-existing stops other running mbtn instances.\n"
            "Without a device argument, --monitor and --grant show a device menu;\n"
            "--revoke shows only devices that currently have granted access.\n",
            program, program, program, program, program, program, program);
}

static int access_command(int argc, char **argv)
{
    int grant = strncmp(argv[1], "--grant", 7) == 0;
    int uinput = strcmp(argv[1], "--grant-uinput") == 0 ||
                 strcmp(argv[1], "--revoke-uinput") == 0;
    const char *target = NULL;
    const char *user = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            user = argv[++i];
        } else if (!target && !uinput && argv[i][0] != '-') {
            target = argv[i];
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (geteuid() != 0) {
        fprintf(stderr, "%s needs root privileges (run with sudo)\n", argv[1]);
        return 1;
    }

    if (!user)
        user = getenv("SUDO_USER");
    if (!user || !*user) {
        fprintf(stderr, "Cannot determine the target user; pass --user NAME\n");
        return 1;
    }
    struct passwd *pw = getpwnam(user);
    if (!pw || pw->pw_uid == 0) {
        fprintf(stderr, "Invalid target user \"%s\" (must exist and not be root)\n", user);
        return 1;
    }

    if (uinput)
        return grant ? grant_uinput(user) : revoke_uinput(user);

    char name[256];
    struct input_id id;

    if (!target) {
        /* No device given: show a menu of detected devices */
        struct mouse mice[MAX_MICE];
        int denied = 0;
        size_t count = collect_mice(mice, MAX_MICE, &denied);

        int choice = pick_mouse(mice, count, !grant);
        if (choice < 0)
            return 1;

        snprintf(name, sizeof(name), "%s", mice[choice].name);
        id = mice[choice].id;
    } else if (strncmp(target, "/dev/", 5) == 0) {
        if (read_identity(target, name, sizeof(name), &id) < 0)
            return 1;
    } else if (!grant) {
        unsigned vendor, product;
        if (parse_vid_pid(target, &vendor, &product) < 0) {
            fprintf(stderr, "Invalid target \"%s\": expected /dev/input/eventN or VID:PID\n",
                    target);
            return 2;
        }
        return revoke_access(user, vendor, product);
    } else {
        fprintf(stderr, "--grant needs a device path such as /dev/input/event5 "
                        "(or no argument for a menu)\n");
        return 2;
    }

    if (grant)
        return grant_access(user, name, &id);
    return revoke_access(user, id.vendor, id.product);
}

/* ------------------------------------------------------------------ */
/*  Monitor mode (helps with writing the config)                       */
/* ------------------------------------------------------------------ */

static void on_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/* Writes a mask of buttons as "SIDE+EXTRA" (config syntax) into buffer */
static void format_combo(uint64_t mask, char *buffer, size_t capacity)
{
    size_t length = 0;
    buffer[0] = '\0';

    for (int i = 0; i < BUTTON_SLOTS; i++) {
        if (!((mask >> i) & 1ULL))
            continue;

        char label[32];
        const char *text = button_label(BUTTON_BASE + i, label, sizeof(label));
        int written = snprintf(buffer + length, capacity - length, "%s%s",
                               length ? "+" : "", text);
        if (written < 0 || (size_t)written >= capacity - length)
            return;
        length += (size_t)written;
    }
}

/* Prints one monitor line: "<action> <MODIFIERS+>TRIGGER<note>" */
static void print_monitor_line(const char *action, uint64_t modifiers,
                               const char *trigger, const char *note)
{
    char combo[256];
    format_combo(modifiers, combo, sizeof(combo));
    printf("%-8s %s%s%s%s\n", action, combo, *combo ? "+" : "", trigger, note);
    fflush(stdout);
}

/*
 * Prints every event of one device in config syntax until interrupted.
 * Unlike the normal mode, all buttons are tracked here, not only the ones
 * used as modifiers in a config.
 */
static int monitor_device(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s%s\n", path, strerror(errno),
                errno == EACCES ? " (try sudo or --grant)" : "");
        return 1;
    }

    char name[256] = "";
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
        fprintf(stderr, "%s is not an input event device\n", path);
        close(fd);
        return 1;
    }

    /* No SA_RESTART, so a blocking read() returns with EINTR on Ctrl+C */
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    printf("Monitoring %s (%s). Press Ctrl+C to stop.\n", path, name);
    printf("Lines show the binding name to put in the config:\n"
           "  press    <MODIFIERS+><BUTTON>_CLICK\n"
           "  release  <MODIFIERS+><BUTTON>_TAP (only if the button was not part of a combination)\n"
           "  wheel    <MODIFIERS+>WHEEL_UP / WHEEL_DOWN / WHEEL_LEFT / WHEEL_RIGHT\n"
           "If nothing appears, a running mbtn with blocking enabled may hold the device\n"
           "(grabbed); stop it first.\n\n");
    fflush(stdout);

    uint64_t held = 0;
    uint64_t used = 0;
    struct input_event ev;

    while (!stop_requested) {
        ssize_t n = read(fd, &ev, sizeof(ev));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Read error: %s\n", strerror(errno));
            break;
        }
        if (n != (ssize_t)sizeof(ev))
            break;

        if (ev.type == EV_KEY) {
            if (ev.value != 0 && ev.value != 1)
                continue; /* ignore auto-repeat */

            if (ev.code < BUTTON_BASE || ev.code >= BUTTON_BASE + BUTTON_SLOTS) {
                if (ev.value == 1) {
                    char note[96];
                    snprintf(note, sizeof(note),
                             "key code 0x%x is outside 0x100..0x13f and cannot be used in the config",
                             ev.code);
                    printf("%-8s %s\n", "ignored", note);
                    fflush(stdout);
                }
                continue;
            }

            int index = ev.code - BUTTON_BASE;
            uint64_t bit = 1ULL << index;
            char label_buffer[32];
            const char *label = button_label(ev.code, label_buffer, sizeof(label_buffer));
            char trigger[64];

            if (ev.value == 1) {
                uint64_t others = held & ~bit;
                used |= others;
                snprintf(trigger, sizeof(trigger), "%s_CLICK", label);
                print_monitor_line("press", others, trigger, "");
                held |= bit;
                used &= ~bit;
            } else {
                held &= ~bit;
                if (!(used & bit)) {
                    snprintf(trigger, sizeof(trigger), "%s_TAP", label);
                    print_monitor_line("release", held, trigger, "");
                    used |= held;
                } else {
                    printf("%-8s %s (was part of a combination, no TAP)\n", "release", label);
                    fflush(stdout);
                }
                used &= ~bit;
            }
        } else if (ev.type == EV_REL) {
            const char *trigger = NULL;

            if (ev.code == REL_WHEEL)
                trigger = ev.value > 0 ? "WHEEL_UP" : (ev.value < 0 ? "WHEEL_DOWN" : NULL);
            else if (ev.code == REL_HWHEEL)
                trigger = ev.value < 0 ? "WHEEL_LEFT" : (ev.value > 0 ? "WHEEL_RIGHT" : NULL);

            if (trigger) {
                used |= held;
                print_monitor_line("wheel", held, trigger, "");
            }
        }
    }

    close(fd);
    printf("\nStopped.\n");
    return 0;
}

static int monitor_command(int argc, char **argv)
{
    const char *target = NULL;

    for (int i = 2; i < argc; i++) {
        if (!target && argv[i][0] != '-') {
            target = argv[i];
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (target) {
        if (strncmp(target, "/dev/", 5) != 0) {
            fprintf(stderr, "--monitor needs a device path such as /dev/input/event5 "
                            "(or no argument for a menu)\n");
            return 2;
        }
        return monitor_device(target);
    }

    struct mouse mice[MAX_MICE];
    int denied = 0;
    size_t count = collect_mice(mice, MAX_MICE, &denied);

    int choice = pick_mouse(mice, count, 0);
    if (choice < 0) {
        if (denied)
            fprintf(stderr, "Note: %d input node(s) could not be opened (permission denied).\n",
                    denied);
        return 1;
    }
    return monitor_device(mice[choice].path);
}

/* ------------------------------------------------------------------ */
/*  Event handling                                                     */
/* ------------------------------------------------------------------ */

/* Runs a command in the background so event reading is never blocked */
static void spawn_command(const char *command)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
}

static const struct binding *find_binding(const struct config *config, uint64_t modifiers,
                                          enum trigger_type type, int button_index)
{
    for (size_t i = 0; i < config->binding_count; i++) {
        const struct binding *binding = &config->bindings[i];
        if (binding->modifiers == modifiers && binding->type == type &&
            binding->button_index == button_index)
            return binding;
    }
    return NULL;
}

/* Blocking-only bindings have an empty command */
static void run_binding(const struct binding *binding)
{
    if (binding && binding->command[0])
        spawn_command(binding->command);
}

static void reset_state(struct state *state)
{
    memset(state, 0, sizeof(*state));
}

/*
 * The kernel buffer overflowed and events were lost. Release everything we
 * forwarded as pressed, forget all button state and skip the events up to
 * the next SYN_REPORT.
 */
static void handle_dropped(struct state *state, struct sink *sink)
{
    for (int i = 0; i < BUTTON_SLOTS; i++) {
        if ((state->forwarded_down >> i) & 1ULL) {
            sink_emit(sink, EV_KEY, BUTTON_BASE + i, 0);
            sink->dirty = 1;
        }
    }
    sink_flush(sink);

    state->held = 0;
    state->used = 0;
    state->pending = 0;
    state->blocked_down = 0;
    state->forwarded_down = 0;
    state->dropping = 1;
}

static void handle_button(const struct config *config, struct state *state,
                          struct sink *sink, const struct input_event *ev)
{
    int index = ev->code - BUTTON_BASE;
    uint64_t bit = 1ULL << index;

    if (ev->value == 1) {
        uint64_t others = state->held & ~bit;
        state->used |= others; /* held buttons took part in a combination */

        const struct binding *binding = find_binding(config, others, TRIG_CLICK, index);
        run_binding(binding);

        if (config->tracked & bit) {
            state->held |= bit;
            state->used &= ~bit;
        }

        if (binding && binding->block) {
            state->blocked_down |= bit; /* press and its release are dropped */
        } else if (config->swallow & bit) {
            state->pending |= bit;      /* forwarded as a click on a plain tap */
        } else {
            forward_event(sink, ev);
            state->forwarded_down |= bit;
        }
        return;
    }

    if (ev->value == 0) {
        int plain_tap = 0;
        int tap_blocked = 0;

        if (state->held & bit) {
            state->held &= ~bit;
            if (!(state->used & bit)) {
                plain_tap = 1;
                const struct binding *binding = find_binding(config, state->held,
                                                             TRIG_TAP, index);
                run_binding(binding);
                tap_blocked = binding && binding->block;
                state->used |= state->held;
            }
            state->used &= ~bit;
        }

        int was_blocked = (state->blocked_down & bit) != 0;
        int was_pending = (state->pending & bit) != 0;
        state->blocked_down &= ~bit;
        state->pending &= ~bit;

        if (was_blocked)
            return;

        if (was_pending) {
            if (plain_tap && !tap_blocked)
                sink_click(sink, ev->code);
            return;
        }

        if (state->forwarded_down & bit) {
            state->forwarded_down &= ~bit;
            forward_event(sink, ev);
        }
        return;
    }

    /* Auto-repeat or other values: forward only if the press was forwarded */
    if (state->forwarded_down & bit)
        forward_event(sink, ev);
}

static void handle_wheel(const struct config *config, struct state *state,
                         struct sink *sink, const struct input_event *ev)
{
    int vertical = ev->code == REL_WHEEL || ev->code == REL_WHEEL_HI_RES;
    int low_res = ev->code == REL_WHEEL || ev->code == REL_HWHEEL;

    if (ev->value == 0) {
        forward_event(sink, ev);
        return;
    }

    enum trigger_type type;
    if (vertical)
        type = ev->value > 0 ? TRIG_WHEEL_UP : TRIG_WHEEL_DOWN;
    else
        type = ev->value < 0 ? TRIG_WHEEL_LEFT : TRIG_WHEEL_RIGHT;

    state->used |= state->held;

    const struct binding *binding = find_binding(config, state->held, type, -1);

    /*
     * The command runs once per notch (low-resolution event). The hi-res
     * events of the same binding are blocked too, otherwise clients that use
     * them would still scroll.
     */
    if (low_res && binding) {
        long long *last = vertical ? &state->last_wheel : &state->last_hwheel;
        int delay = vertical ? config->scroll_delay_ms : config->wheel_delay_ms;
        long long now = now_ms();

        if (!(delay > 0 && *last != 0 && now - *last < delay)) {
            *last = now;
            run_binding(binding);
        }
    }

    if (!(binding && binding->block))
        forward_event(sink, ev);
}

static void handle_event(const struct config *config, struct state *state,
                         struct sink *sink, const struct input_event *ev)
{
    if (state->dropping) {
        if (ev->type == EV_SYN && ev->code == SYN_REPORT)
            state->dropping = 0;
        return;
    }

    if (ev->type == EV_SYN) {
        if (ev->code == SYN_REPORT)
            sink_flush(sink);
        else if (ev->code == SYN_DROPPED)
            handle_dropped(state, sink);
        return;
    }

    if (ev->type == EV_KEY && ev->code >= BUTTON_BASE &&
        ev->code < BUTTON_BASE + BUTTON_SLOTS) {
        handle_button(config, state, sink, ev);
        return;
    }

    if (ev->type == EV_REL &&
        (ev->code == REL_WHEEL || ev->code == REL_HWHEEL ||
         ev->code == REL_WHEEL_HI_RES || ev->code == REL_HWHEEL_HI_RES)) {
        handle_wheel(config, state, sink, ev);
        return;
    }

    /* Everything else (pointer motion, MSC_SCAN, other keys) passes through */
    forward_event(sink, ev);
}

/* ------------------------------------------------------------------ */
/*  Config reload                                                      */
/* ------------------------------------------------------------------ */

static void reload_config(const char *path, struct config *config, int *fd,
                          struct sink *sink, struct state *state, long long *next_open_at)
{
    struct config next;

    fprintf(stderr, "Config changed, reloading %s\n", path);
    if (load_config(path, &next) < 0) {
        fprintf(stderr, "Reload failed; keeping the previous configuration\n");
        return;
    }

    int same_device = strcmp(next.device_name, config->device_name) == 0;
    if (*fd >= 0 && same_device && !has_required_caps(*fd, &next)) {
        fprintf(stderr, "Reload rejected: the device lacks buttons or wheels used by the "
                        "new config; keeping the previous configuration\n");
        return;
    }

    int reopen = *fd >= 0 && (!same_device || next.needs_grab != config->needs_grab);
    *config = next;

    if (reopen) {
        close_input(fd, sink);
        reset_state(state);
        *next_open_at = 0;
    } else {
        /* Buttons that are no longer swallowed: forward their deferred press now */
        uint64_t release_now = state->pending & ~config->swallow;
        for (int i = 0; i < BUTTON_SLOTS; i++) {
            if ((release_now >> i) & 1ULL) {
                sink_emit(sink, EV_KEY, BUTTON_BASE + i, 1);
                sink->dirty = 1;
                state->forwarded_down |= 1ULL << i;
            }
        }
        sink_flush(sink);

        state->pending &= config->swallow;
        state->held &= config->tracked;
        state->used &= state->held;
    }

    fprintf(stderr, "Reloaded: %zu binding(s)%s\n", config->binding_count,
            config->needs_grab ? ", blocking enabled" : "");
}

/* ------------------------------------------------------------------ */
/*  Single-instance handling                                           */
/* ------------------------------------------------------------------ */

static int read_process_executable(pid_t pid, char *path, size_t capacity)
{
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid);
    ssize_t length = readlink(proc_path, path, capacity - 1);
    if (length < 0 || (size_t)length >= capacity - 1)
        return -1;

    path[length] = '\0';
    const char deleted_suffix[] = " (deleted)";
    size_t path_length = (size_t)length;
    size_t suffix_length = sizeof(deleted_suffix) - 1;
    if (path_length >= suffix_length &&
        strcmp(path + path_length - suffix_length, deleted_suffix) == 0)
        path[path_length - suffix_length] = '\0';
    return 0;
}

static int is_same_executable(pid_t pid, const char *self_executable)
{
    char executable[PATH_MAX];
    return read_process_executable(pid, executable, sizeof(executable)) == 0 &&
           strcmp(executable, self_executable) == 0;
}

static int stop_previous_instances(void)
{
    char self_executable[PATH_MAX];
    ssize_t self_length = readlink("/proc/self/exe", self_executable,
                                   sizeof(self_executable) - 1);
    if (self_length < 0 || (size_t)self_length >= sizeof(self_executable) - 1) {
        perror("Cannot determine executable path");
        return -1;
    }
    self_executable[self_length] = '\0';

    char lock_path[64];
    snprintf(lock_path, sizeof(lock_path), "/tmp/mbtn-startup-%lu.lock",
             (unsigned long)getuid());
    int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0) {
        perror("Cannot open startup lock");
        return -1;
    }
    while (flock(lock_fd, LOCK_EX) < 0) {
        if (errno == EINTR)
            continue;
        perror("Cannot acquire startup lock");
        close(lock_fd);
        return -1;
    }

    DIR *proc = opendir("/proc");
    if (!proc) {
        perror("Cannot open /proc");
        close(lock_fd);
        return -1;
    }

    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        char *end = NULL;
        errno = 0;
        long process_id = strtol(entry->d_name, &end, 10);
        if (errno || end == entry->d_name || *end || process_id <= 1 ||
            process_id > INT_MAX || process_id == (long)getpid())
            continue;

        pid_t pid = (pid_t)process_id;
        if (!is_same_executable(pid, self_executable))
            continue;

        if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
            perror("Cannot stop previous mbtn instance");
            result = -1;
            break;
        }

        int stopped = 0;
        for (int attempt = 0; attempt < 100; attempt++) {
            if (!is_same_executable(pid, self_executable)) {
                stopped = 1;
                break;
            }
            sleep_ms(10);
        }
        if (!stopped && is_same_executable(pid, self_executable)) {
            if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
                perror("Cannot kill previous mbtn instance");
                result = -1;
                break;
            }
            for (int attempt = 0; attempt < 100; attempt++) {
                if (!is_same_executable(pid, self_executable)) {
                    stopped = 1;
                    break;
                }
                sleep_ms(10);
            }
            if (!stopped && is_same_executable(pid, self_executable)) {
                fprintf(stderr, "Previous mbtn instance did not exit\n");
                result = -1;
                break;
            }
        }
    }

    closedir(proc);
    close(lock_fd);
    return result;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--list") == 0)
        return list_devices();

    if (argc >= 2 && strcmp(argv[1], "--monitor") == 0)
        return monitor_command(argc, argv);

    if (argc >= 2 &&
        (strcmp(argv[1], "--grant") == 0 || strcmp(argv[1], "--revoke") == 0 ||
         strcmp(argv[1], "--grant-uinput") == 0 || strcmp(argv[1], "--revoke-uinput") == 0))
        return access_command(argc, argv);

    const char *config_path = DEFAULT_CONFIG;
    int have_config_path = 0;
    int replace_existing = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replace-existing") == 0) {
            if (replace_existing) {
                print_usage(argv[0]);
                return 2;
            }
            replace_existing = 1;
        } else if (argv[i][0] != '-' && !have_config_path) {
            config_path = argv[i];
            have_config_path = 1;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    struct config config;
    if (load_config(config_path, &config) < 0) {
        if (!have_config_path)
            print_usage(argv[0]);
        return 1;
    }
    if (replace_existing && stop_previous_instances() < 0)
        return 1;

    /* Child processes (commands from the config) must not become zombies */
    signal(SIGCHLD, SIG_IGN);

    struct reloader reloader;
    reloader_init(&reloader, config_path);

    int fd = -1;
    struct sink sink = {.fd = -1, .dirty = 0, .write_failed = 0};
    struct state state;
    reset_state(&state);

    long long next_open_at = 0;
    long long reload_at = 0;
    int warned_missing = 0;
    int reported_failure = 0;

    for (;;) {
        if (fd < 0 && now_ms() >= next_open_at) {
            fd = open_input(&config, &sink, &warned_missing, &reported_failure);
            if (fd >= 0)
                reset_state(&state);
            else
                next_open_at = now_ms() + RECONNECT_DELAY_MS;
        }

        struct pollfd fds[2];
        int nfds = 0;
        int device_index = -1;
        int notify_index = -1;

        if (fd >= 0) {
            fds[nfds].fd = fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            device_index = nfds++;
        }
        if (reloader.fd >= 0) {
            fds[nfds].fd = reloader.fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            notify_index = nfds++;
        }

        long long timeout = -1;
        if (fd < 0) {
            timeout = next_open_at - now_ms();
            if (timeout < 0)
                timeout = 0;
        }
        if (reload_at) {
            long long remaining = reload_at - now_ms();
            if (remaining < 0)
                remaining = 0;
            if (timeout < 0 || remaining < timeout)
                timeout = remaining;
        }

        int ready = poll(fds, (nfds_t)nfds, (int)timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }

        if (notify_index >= 0 && (fds[notify_index].revents & POLLIN) &&
            reloader_poll(&reloader))
            reload_at = now_ms() + RELOAD_DEBOUNCE_MS;

        if (reload_at && now_ms() >= reload_at) {
            reload_at = 0;
            reload_config(config_path, &config, &fd, &sink, &state, &next_open_at);
        }

        if (fd < 0 || device_index < 0 ||
            !(fds[device_index].revents & (POLLIN | POLLERR | POLLHUP)))
            continue;

        struct input_event events[64];
        ssize_t n = read(fd, events, sizeof(events));

        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;

        if (n > 0 && (size_t)n % sizeof(struct input_event) == 0) {
            size_t count = (size_t)n / sizeof(struct input_event);
            for (size_t i = 0; i < count; i++)
                handle_event(&config, &state, &sink, &events[i]);
            continue;
        }

        fprintf(stderr,
                "Input device disconnected/read error: %s. Searching again...\n",
                n < 0 ? strerror(errno) : "EOF");

        close_input(&fd, &sink);
        reset_state(&state);
        next_open_at = now_ms() + RECONNECT_DELAY_MS;
    }
}
