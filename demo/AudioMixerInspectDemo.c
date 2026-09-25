#include <alsa/asoundlib.h>

#include <stdio.h>

static void print_volume(const char *label,
                         snd_mixer_elem_t *element,
                         int (*getRange)(snd_mixer_elem_t *, long *, long *),
                         int (*getValue)(snd_mixer_elem_t *, snd_mixer_selem_channel_id_t, long *)) {
    long minimum = 0;
    long maximum = 0;
    long value = 0;

    getRange(element, &minimum, &maximum);
    if (getValue(element, SND_MIXER_SCHN_FRONT_LEFT, &value) >= 0) {
        printf("  %s: %ld (range %ld..%ld)\n", label, value, minimum, maximum);
    }
}

int main(void) {
    snd_mixer_t *mixer = NULL;
    snd_mixer_elem_t *element;
    int result;

    result = snd_mixer_open(&mixer, 0);
    if (result < 0) {
        fprintf(stderr, "AudioMixerInspectDemo: snd_mixer_open 失败: %s\n", snd_strerror(result));
        return 1;
    }
    /* 与 AudioPlayback 一样优先走板端 ALSA 配置过的 default 路由。 */
    result = snd_mixer_attach(mixer, "default");
    if (result >= 0) {
        result = snd_mixer_selem_register(mixer, NULL, NULL);
    }
    if (result >= 0) {
        result = snd_mixer_load(mixer);
    }
    if (result < 0) {
        fprintf(stderr, "AudioMixerInspectDemo: 无法加载 default mixer: %s\n", snd_strerror(result));
        snd_mixer_close(mixer);
        return 1;
    }

    printf("AudioMixerInspectDemo: card0 的 active simple controls\n");
    for (element = snd_mixer_first_elem(mixer); element != NULL; element = snd_mixer_elem_next(element)) {
        if (!snd_mixer_selem_is_active(element)) {
            continue;
        }
        printf("%s\n", snd_mixer_selem_get_name(element));
        if (snd_mixer_selem_has_capture_volume(element)) {
            print_volume("capture volume",
                         element,
                         snd_mixer_selem_get_capture_volume_range,
                         snd_mixer_selem_get_capture_volume);
        }
        if (snd_mixer_selem_has_playback_volume(element)) {
            print_volume("playback volume",
                         element,
                         snd_mixer_selem_get_playback_volume_range,
                         snd_mixer_selem_get_playback_volume);
        }
        if (snd_mixer_selem_has_capture_switch(element)) {
            int enabled = 0;
            if (snd_mixer_selem_get_capture_switch(element, SND_MIXER_SCHN_FRONT_LEFT, &enabled) >= 0) {
                printf("  capture switch: %s\n", enabled ? "on" : "off");
            }
        }
        if (snd_mixer_selem_has_playback_switch(element)) {
            int enabled = 0;
            if (snd_mixer_selem_get_playback_switch(element, SND_MIXER_SCHN_FRONT_LEFT, &enabled) >= 0) {
                printf("  playback switch: %s\n", enabled ? "on" : "off");
            }
        }
    }
    snd_mixer_close(mixer);
    return 0;
}
