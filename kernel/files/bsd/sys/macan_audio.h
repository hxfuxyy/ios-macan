#ifndef _SYS_MACAN_AUDIO_H_
#define _SYS_MACAN_AUDIO_H_
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
void macan_haptic_init(void);
void macan_haptic_quiesce(void);
void macan_haptic_pretend(uint32_t peak);
void macan_torch_init(void);
void macan_torch_quiesce(void);
bool redwood_audio_recording_fed(void);
void redwood_audio_record_fed(const int16_t *frames, uint32_t count);
bool redwood_audio_recording_mic(void);
#endif
#endif
