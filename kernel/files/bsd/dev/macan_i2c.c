#include <kern/clock.h>
#include <libkern/libkern.h>
#include <sys/macan_i2c.h>
static uint32_t
macan_pin_rd(struct macan_i2c *bus, unsigned pin, unsigned offset)
{
 return *(volatile uint32_t *)(bus->tlmm + pin * 0x1000UL + offset);
}
static void
macan_pin_wr(struct macan_i2c *bus, unsigned pin, unsigned offset, uint32_t value)
{
 *(volatile uint32_t *)(bus->tlmm + pin * 0x1000UL + offset) = value;
 __asm__ volatile ("dsb sy");
}
static void
macan_half_bit(struct macan_i2c *bus)
{
 uint64_t until, interval;
 nanoseconds_to_absolutetime(bus->half_us * NSEC_PER_USEC, &interval);
 until = mach_absolute_time() + interval;
 while (mach_absolute_time() < until) {
 }
}
static void
macan_line(struct macan_i2c *bus, unsigned pin, int high)
{
 uint32_t cfg = macan_pin_rd(bus, pin, 0);
 if (high) {
  macan_pin_wr(bus, pin, 0, cfg & ~(1u << 9));
 } else {
  macan_pin_wr(bus, pin, 4, 0);
  macan_pin_wr(bus, pin, 0, cfg | (1u << 9));
 }
}
static int
macan_line_level(struct macan_i2c *bus, unsigned pin)
{
 return macan_pin_rd(bus, pin, 4) & 1;
}
static bool
macan_scl_high(struct macan_i2c *bus)
{
 macan_line(bus, bus->scl, 1);
 for (unsigned i = 0; i < 500; i++) {
  if (macan_line_level(bus, bus->scl)) {
   return true;
  }
  macan_half_bit(bus);
 }
 return false;
}
void
macan_pins_take(struct macan_i2c *bus)
{
 unsigned pins[2] = { bus->sda, bus->scl };
 for (unsigned i = 0; i < 2; i++) {
  uint32_t cfg;
  bus->saved_cfg[i] = macan_pin_rd(bus, pins[i], 0);
  bus->saved_io[i] = macan_pin_rd(bus, pins[i], 4);
  cfg = bus->saved_cfg[i] & ~0x13ffu;
  if (bus->saved_cfg[i] & (1u << 11)) {
   cfg |= 1u << 12;
  }
  cfg |= (bus->drive << 6) | 3u;
  macan_pin_wr(bus, pins[i], 4, 0);
  macan_pin_wr(bus, pins[i], 0, cfg);
 }
}
void
macan_pins_give_back(struct macan_i2c *bus)
{
 unsigned pins[2] = { bus->sda, bus->scl };
 for (unsigned i = 0; i < 2; i++) {
  macan_pin_wr(bus, pins[i], 4, bus->saved_io[i]);
  macan_pin_wr(bus, pins[i], 0, bus->saved_cfg[i]);
 }
}
bool
macan_start(struct macan_i2c *bus)
{
 macan_line(bus, bus->sda, 1);
 if (!macan_scl_high(bus)) {
  return false;
 }
 macan_half_bit(bus);
 for (unsigned i = 0; i < 9 && !macan_line_level(bus, bus->sda); i++) {
  macan_line(bus, bus->scl, 0);
  macan_half_bit(bus);
  if (!macan_scl_high(bus)) {
   return false;
  }
  macan_half_bit(bus);
 }
 if (!macan_line_level(bus, bus->sda)) {
  return false;
 }
 macan_line(bus, bus->sda, 0);
 macan_half_bit(bus);
 macan_line(bus, bus->scl, 0);
 macan_half_bit(bus);
 return true;
}
void
macan_stop(struct macan_i2c *bus)
{
 macan_line(bus, bus->sda, 0);
 macan_half_bit(bus);
 (void)macan_scl_high(bus);
 macan_half_bit(bus);
 macan_line(bus, bus->sda, 1);
 macan_half_bit(bus);
}
bool
macan_write_byte(struct macan_i2c *bus, uint8_t byte)
{
 bool ack;
 for (int bit = 7; bit >= 0; bit--) {
  macan_line(bus, bus->sda, (byte >> bit) & 1);
  macan_half_bit(bus);
  if (!macan_scl_high(bus)) {
   return false;
  }
  macan_half_bit(bus);
  macan_line(bus, bus->scl, 0);
 }
 macan_line(bus, bus->sda, 1);
 macan_half_bit(bus);
 if (!macan_scl_high(bus)) {
  return false;
 }
 ack = !macan_line_level(bus, bus->sda);
 macan_half_bit(bus);
 macan_line(bus, bus->scl, 0);
 return ack;
}
bool
macan_read_byte(struct macan_i2c *bus, uint8_t *byte, bool last)
{
 uint8_t value = 0;
 macan_line(bus, bus->sda, 1);
 for (int bit = 7; bit >= 0; bit--) {
  macan_half_bit(bus);
  if (!macan_scl_high(bus)) {
   return false;
  }
  value = (uint8_t)((value << 1) | macan_line_level(bus, bus->sda));
  macan_half_bit(bus);
  macan_line(bus, bus->scl, 0);
 }
 macan_line(bus, bus->sda, last ? 1 : 0);
 macan_half_bit(bus);
 if (!macan_scl_high(bus)) {
  return false;
 }
 macan_half_bit(bus);
 macan_line(bus, bus->scl, 0);
 macan_line(bus, bus->sda, 1);
 *byte = value;
 return true;
}
