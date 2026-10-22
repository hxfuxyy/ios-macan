#ifndef _SYS_REDWOOD_AUDIO_H_
#define _SYS_REDWOOD_AUDIO_H_
#include <stdint.h>
#define REDWOOD_AUDIO_DEVICE "/dev/redwood-audio"
#define REDWOOD_AUDIO_RATE 48000
#define REDWOOD_AUDIO_CHANNELS 2
#define REDWOOD_AUDIO_FRAME 4
#include <sys/ioccom.h>
struct redwood_audio_volume {
 uint32_t below;
 uint32_t muted;
};
#define REDWOOD_AUDIO_SET_VOLUME _IOW('r', 1, struct redwood_audio_volume)
#ifdef KERNEL
#include <stdbool.h>
typedef uint32_t (*redwood_audio_sink_t)(const int16_t *frames, uint32_t count);
void redwood_audio_set_sink(const char *name, redwood_audio_sink_t sink);
bool redwood_audio_volume(uint32_t *below, bool *muted, uint32_t *generation);
void redwood_audio_init(void);
void redwood_virtio_sound_init(void);
void redwood_speaker_init(void);
bool redwood_speaker_prepare(const uint8_t *acf, uint64_t size);
bool redwood_speaker_start(void);
bool redwood_speaker_volume(uint32_t below, uint32_t step_ms);
bool redwood_speaker_healthy(void);
void redwood_speaker_stop(void);
void redwood_speaker_quiesce(void);
struct redwood_apr_answer {
 uint32_t opcode, token, length;
 uint32_t words[64];
};
typedef bool (*redwood_apr_events_t)(uint8_t service, uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload,
    uint32_t length);
bool redwood_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t wait_ms, struct redwood_apr_answer *answer);
void redwood_adsp_apr_events(redwood_apr_events_t handler);
const uint8_t *redwood_adsp_fw_file(const char *name, uint64_t *size);
void redwood_adsp_fw_free(const uint8_t *data, uint64_t size);
void redwood_sound_dsp_up(void);
void redwood_sound_dsp_down(void);
void redwood_sound_quiesce(void);
#endif
#endif
