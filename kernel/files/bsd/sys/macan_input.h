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
void redwood_spmi_lock(void);
void redwood_spmi_unlock(void);
int redwood_battery_temperature(void);
void redwood_buttons_init(void);
void redwood_ufs_init(void);
void redwood_ufs_report(const char *when);
bool redwood_ufs_dev(dev_t dev);
void redwood_battery_init(void);
void redwood_net_init(void);
void redwood_cpufreq_init(void);
void redwood_adreno_init(void);
void redwood_adreno_quiesce(void);
void redwood_adreno_stop(void);
void redwood_adreno_minute(void);
void redwood_wifi_init(void);
void redwood_adsp_init(void);
void redwood_wifi_stop(void);
int redwood_wifi_say(const char *format, ...) __printflike(1, 2);
int redwood_wifi_tell(const char *format, ...) __printflike(1, 2);
uint32_t redwood_wifi_option(const char *name);
const uint8_t *redwood_wifi_file(const char *name, uint64_t *size);
bool redwood_wlfw_exchange(uint32_t step);
bool redwood_wlfw_registers(uint64_t *address, uint32_t *size, bool *mission);
void redwood_wlfw_stop(uint32_t step);
void redwood_wifi_quiesce(void);
int redwood_lx_start(void);
void redwood_lx_stop(void);
int redwood_lx_prepare(void);
void redwood_lx_net_early(void);
int redwood_wifi_driver_calibrated(void);
bool redwood_wifi_dsp_is_up(void);
const uint8_t *redwood_wifi_driver_file(const char *name, uint64_t *size);
bool redwood_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address);
bool redwood_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi);
bool redwood_wifi_radio_address(uint8_t *address);
bool redwood_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);
bool redwood_gpu_ready(void);
bool redwood_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t redwood_gpu_address(uint64_t size);
void redwood_gpu_address_free(uint64_t address, uint64_t size);
bool redwood_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void redwood_gpu_mapped(void);
void redwood_gpu_unmap(uint64_t address, uint64_t size);
void redwood_gpu_to_memory(void *start, uint64_t size);
uint32_t redwood_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool redwood_gpu_done(uint32_t fence);
uint32_t redwood_gpu_last(void);
void redwood_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void redwood_gpu_report(const char *why);
void redwood_gpu_stop(void);
#endif
struct redwood_battery_state {
 uint32_t size;
 int32_t percent_hundredths;
 int32_t voltage_uv;
 int32_t current_ua;
 int32_t temperature_hundredths;
 int32_t present;
 int32_t external;
 int32_t status;
 int32_t charge_uah, full_uah, design_uah, cycles;
 int32_t usb_type;
 int32_t usb_voltage_uv, usb_current_ua;
};
void redwood_battery_report(const struct redwood_battery_state *state);
#endif
