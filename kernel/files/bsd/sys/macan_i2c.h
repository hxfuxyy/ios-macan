#ifndef _SYS_MACAN_I2C_H_
#define _SYS_MACAN_I2C_H_
#include <mach/mach_types.h>
#include <stdbool.h>
#include <stdint.h>
struct macan_i2c {
 vm_offset_t tlmm;
 unsigned sda, scl, half_us, drive;
 uint32_t saved_cfg[2], saved_io[2];
};
void macan_pins_take(struct macan_i2c *bus);
void macan_pins_give_back(struct macan_i2c *bus);
bool macan_start(struct macan_i2c *bus);
void macan_stop(struct macan_i2c *bus);
bool macan_write_byte(struct macan_i2c *bus, uint8_t byte);
bool macan_read_byte(struct macan_i2c *bus, uint8_t *byte, bool last);
#endif
