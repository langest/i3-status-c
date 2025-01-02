#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#include <alsa/asoundlib.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#if defined(DEBUG) && DEBUG > 0
#define DEBUG_PRINT(fmt, ...) fprintf(stderr, "[DEBUG] %s:%d:%s(): " fmt, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define DEBUG_ERROR(fmt) fprintf(stderr, "[ERROR] %s:%d:%s(): " fmt, __FILE__, __LINE__, __func__)
#else
#define DEBUG_PRINT(fmt, ...)
#define DEBUG_ERROR(fmt)
#endif

volatile sig_atomic_t force_update = 0;

int read_file(const char* path, float* out) {
    if (!path || !out) return -1;

    FILE* file = fopen(path, "r");
    if (!file) {
        DEBUG_ERROR("Failed to open file\n");
        return -1;
    }

    int err = fscanf(file, "%f", out);
    if (err != 1) {
        DEBUG_ERROR("Failed to scan file\n");
        fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        DEBUG_ERROR("Failed to close file\n");
        return -1;
    }

    return 0;
}

void get_battery_status(int battery_index, char* out, const size_t size) {
    if (!out || size < 16) {
        if (out && size > 0) out[0] = '\0';
        return;
    }

    char info_file[64];
    int ret = snprintf(info_file, sizeof(info_file), "/sys/class/power_supply/BAT%d/capacity", battery_index);
    if (ret < 0 || ret >= sizeof(info_file)) {
        snprintf(out, size, "err");
        return;
    }

    // Read capacity
    FILE* file = fopen(info_file, "r");
    if (!file) {
        snprintf(out, size, "missing");
        return;
    }

    int capacity;
    if (fscanf(file, "%d", &capacity) != 1) {
        fclose(file);
        snprintf(out, size, "err");
        return;
    }
    fclose(file);

    if (capacity < 0) {
        snprintf(out, size, "???%%");
        return;
    }

    // Read status
    ret = snprintf(info_file, sizeof(info_file), "/sys/class/power_supply/BAT%d/status", battery_index);
    if (ret < 0 || ret >= sizeof(info_file)) {
        snprintf(out, size, "%d%%", capacity);
        return;
    }

    file = fopen(info_file, "r");
    if (!file) {
        snprintf(out, size, "%d%%", capacity);
        return;
    }

    char status[16] = {0};
    if (fscanf(file, "%15s", status) != 1) {
        fclose(file);
        snprintf(out, size, "%d%%", capacity);
        return;
    }
    fclose(file);

    if (status[0] == 'C') {
        snprintf(out, size, "⌁⏶%d%%", capacity);
    } else if (status[0] == 'D') {
        snprintf(out, size, "⌁⏷%d%%", capacity);
    } else {
        snprintf(out, size, "%d%%", capacity);
    }
}

void get_audio_volume(long* outvol) {
    if (!outvol) return;
    *outvol = -1;

    static const char* mix_name = "Master";
    static const char* card = "default";
    static const int mix_index = 0;

    snd_mixer_t* handle;
    if (snd_mixer_open(&handle, 0) < 0) {
        DEBUG_ERROR("Failed to open mixer\n");
        return;
    }

    if (snd_mixer_attach(handle, card) < 0) {
        DEBUG_ERROR("Failed to attach handle\n");
        snd_mixer_close(handle);
        return;
    }

    if (snd_mixer_selem_register(handle, NULL, NULL) < 0) {
        DEBUG_ERROR("Failed to register handle\n");
        snd_mixer_close(handle);
        return;
    }

    if (snd_mixer_load(handle) < 0) {
        DEBUG_ERROR("Failed to load handle\n");
        snd_mixer_close(handle);
        return;
    }

    snd_mixer_selem_id_t* sid;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, mix_index);
    snd_mixer_selem_id_set_name(sid, mix_name);

    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);
    if (!elem) {
        DEBUG_ERROR("Failed to find elem\n");
        snd_mixer_close(handle);
        return;
    }

    long minv, maxv;
    snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv);

    if (snd_mixer_selem_get_playback_volume(elem, 0, outvol) < 0) {
        DEBUG_ERROR("Failed to get playback volume\n");
        snd_mixer_close(handle);
        return;
    }

    // Calculate percentage and round to nearest 5
    *outvol -= minv;
    long range = maxv - minv;
    if (range <= 0) range = 1;  // Prevent division by zero
    *outvol = (*outvol * 100) / range;
    *outvol = ((*outvol + 2) / 5) * 5;

    snd_mixer_close(handle);
}

int get_time(char* out, const size_t size) {
    if (!out || size < 32) return -1;

    time_t t = time(NULL);
    struct tm* local_time = localtime(&t);
    if (!local_time) {
        DEBUG_ERROR("Failed to get local_time\n");
        out[0] = '\0';
        return -1;
    }

    static const char* weekdays = "日\0月\0火\0水\0木\0金\0土\0";
    char temp[64];
    int len = strftime(temp, sizeof(temp), "W%V %%s %d %b %H:%M", local_time);
    if (len <= 0) {
        DEBUG_ERROR("Failed to format time\n");
        out[0] = '\0';
        return -1;
    }

    snprintf(out, size, temp, &weekdays[local_time->tm_wday * 4]);
    return local_time->tm_sec;
}

void get_keyboard_layout(Display* display, char* out, const size_t size) {
    if (!display || !out || size < 3) {
        if (out && size > 0) out[0] = '\0';
        return;
    }

    XkbDescRec* kbd_desc = XkbAllocKeyboard();
    if (!kbd_desc) {
        snprintf(out, size, "??");
        return;
    }

    if (XkbGetNames(display, XkbSymbolsNameMask, kbd_desc) != Success) {
        XkbFreeKeyboard(kbd_desc, XkbSymbolsNameMask, True);
        snprintf(out, size, "??");
        return;
    }

    char* layout_string = XGetAtomName(display, kbd_desc->names->symbols);
    if (!layout_string) {
        XkbFreeKeyboard(kbd_desc, XkbSymbolsNameMask, True);
        snprintf(out, size, "??");
        return;
    }

    const char* layout = "??";
    if (strstr(layout_string, "us")) layout = "US";
    else if (strstr(layout_string, "se")) layout = "SE";

    snprintf(out, size, "%s", layout);
    XFree(layout_string);
    XkbFreeKeyboard(kbd_desc, XkbSymbolsNameMask, True);
}

void catch_signal(int signo, siginfo_t* sinfo, void *context) {
    (void)sinfo;
    (void)context;

    if (signo == SIGUSR1) {
        force_update = 1;
    }
}

void print_status_line(const char* battery0, const char* battery1,
                      const char* kb, long volume, const char* time) {
    printf("🔋%s, 🔋%s | ⌨️%s | 🔊%ld%% | %s\n",
           battery0,
           battery1,
           kb,
           volume,
           time);
}

int main(void) {
    struct sigaction action = {0};
    action.sa_sigaction = &catch_signal;
    action.sa_flags = SA_SIGINFO;

    if (sigaction(SIGUSR1, &action, NULL) < 0) {
        DEBUG_ERROR("Failed to register SIGUSR1 handler\n");
        return 2;
    }

    Display* display = XOpenDisplay(NULL);
    if (!display) {
        DEBUG_ERROR("Failed to open display\n");
        return 1;
    }

    char battery0[16] = {0};
    char battery1[16] = {0};
    char kb[4] = {0};
    char time_str[32] = {0};
    long volume = -1;

    while (1) {
        get_battery_status(0, battery0, sizeof(battery0));
        get_battery_status(1, battery1, sizeof(battery1));
        get_keyboard_layout(display, kb, sizeof(kb));
        get_audio_volume(&volume);

        int sleep_duration = get_time(time_str, sizeof(time_str));
        if (sleep_duration < 0) sleep_duration = 60;

        print_status_line(battery0, battery1, kb, volume, time_str);

        if (force_update) {
            force_update = 0;
            sleep(1);
        } else {
            sleep(sleep_duration);
        }
    }

    XCloseDisplay(display);
    return 0;
}