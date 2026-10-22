#ifndef _SYS_REDWOOD_INPUT_H_
#define _SYS_REDWOOD_INPUT_H_
#include <stdint.h>
#define REDWOOD_TOUCH_DEVICE "/dev/redwood-touch"
#define REDWOOD_TOUCH_DOWN 1
#define REDWOOD_TOUCH_MOVE 2
#define REDWOOD_TOUCH_UP 3
#define REDWOOD_BUTTON_DOWN 4
#define REDWOOD_BUTTON_UP 5
#define REDWOOD_BUTTON_LOCK 1
#define REDWOOD_BUTTON_VOLUME_UP 2
#define REDWOOD_BUTTON_VOLUME_DOWN 3
#define REDWOOD_BUTTON_HOME 4
struct redwood_touch {
 uint64_t time;
 uint32_t type;
 uint32_t finger;
 uint32_t x, y;
};
#ifdef KERNEL
void redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void redwood_input_init(void);
void redwood_virtio_input_init(void);
void redwood_goodix_init(void);
void redwood_cpufreq_init(void);
#endif
#endif
