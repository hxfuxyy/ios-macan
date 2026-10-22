#include <stdarg.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/macan_input.h>
#include <sys/macan_i2c.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);
#define LOADER_BATTERY_MAGIC 0x2154544142574452ULL
#define LOADER_BATTERY_VERSION 1
#define LOADER_BATTERY_VALID (1u << 0)
#define LOADER_BATTERY_PRESENT (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID (1u << 3)
#define LOADER_BATTERY_CHARGING (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED (1u << 5)
#define LOADER_PAGE_SIZE 0x4000UL
struct loader_battery {
 uint64_t magic;
 uint32_t version, size;
 volatile uint32_t sequence;
 uint32_t flags;
 int32_t soc_percent, voltage_mv, current_ma, temperature_c;
 uint32_t charger_action, polls, failures, reserved;
};
static volatile struct loader_battery *shared_battery;
static int battery_soc_percent = -1, battery_voltage_mv = -1, battery_current_ma;
static int battery_temperature_c = -273, battery_present, battery_charger_present;
int
redwood_battery_temperature(void)
{
 return battery_temperature_c;
}
static int battery_charging, battery_soc_estimated, battery_charger_action = -1;
static uint64_t battery_updates, battery_failures;
SYSCTL_NODE(_kern, OID_AUTO, redwood, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "redwood phone bits");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current reported by Qualcomm firmware, in milliamps");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature reported by Qualcomm firmware, in Celsius");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if Qualcomm firmware reports a battery");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if Qualcomm firmware reports an attached charger");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while an attached charger is charging a non-full battery");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "last Qualcomm charger action, for diagnostics");
static bool
battery_snapshot(struct loader_battery *out)
{
 uint32_t before, after;
 for (unsigned retry = 0; retry < 4; retry++) {
  before = shared_battery->sequence;
  if (before & 1) continue;
  __asm__ volatile ("dmb ishld" ::: "memory");
  memcpy(out, (const void *)(uintptr_t)shared_battery, sizeof(*out));
  __asm__ volatile ("dmb ishld" ::: "memory");
  after = shared_battery->sequence;
  if (before == after && !(after & 1)) return true;
 }
 return false;
}
static void
battery_poll(void)
{
 struct loader_battery sample;
 if (!battery_snapshot(&sample) || sample.magic != LOADER_BATTERY_MAGIC ||
     sample.version != LOADER_BATTERY_VERSION || sample.size < sizeof(sample) ||
     !(sample.flags & LOADER_BATTERY_VALID) || sample.soc_percent < 0 || sample.soc_percent > 100) {
  battery_failures++;
  return;
 }
 battery_soc_percent = sample.soc_percent;
 battery_voltage_mv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv : -1;
 battery_current_ma = sample.current_ma;
 battery_temperature_c = sample.temperature_c;
 battery_present = (sample.flags & LOADER_BATTERY_PRESENT) != 0;
 battery_charger_present = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) != 0;
 battery_charging = (sample.flags & LOADER_BATTERY_CHARGING) != 0;
 battery_soc_estimated = (sample.flags & LOADER_BATTERY_SOC_ESTIMATED) != 0;
 battery_charger_action = (int)sample.charger_action;
 battery_updates++;
}
static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
 for (unsigned tick = 0;; tick++) {
  battery_poll();
  if (tick % 12 == 0) {
   printf("battery: SOC=%d%%%s, V=%d mV, I=%d mA, T=%d C, battery=%d charger=%d charging=%d "
       "action=%d (%llu reads, %llu failed)\n", battery_soc_percent,
       battery_soc_estimated ? " (voltage estimate)" : "", battery_voltage_mv,
       battery_current_ma, battery_temperature_c, battery_present, battery_charger_present,
       battery_charging, battery_charger_action, battery_updates, battery_failures);
  }
  delay_for_interval(5000, NSEC_PER_MSEC);
 }
}
#define MACAN_TLMM_PHYS 0x0f100000UL
#define MACAN_TLMM_SIZE 0x0b0000UL
#define MACAN_GAUGE_SDA 68
#define MACAN_GAUGE_SCL 69
#define MACAN_GAUGE_ADDRESS 0x55
#define MACAN_GAUGE_TEMPERATURE 0x06
#define MACAN_GAUGE_VOLTAGE 0x08
#define MACAN_GAUGE_CURRENT 0x14
#define MACAN_GAUGE_SOC 0x2c
#define MACAN_GAUGE_REMAINING 0x10
#define MACAN_GAUGE_FULL 0x12
#define MACAN_GAUGE_CYCLES 0x2a
#define MACAN_GAUGE_DESIGN 0x3c
#define MACAN_GAUGE_PERIOD_S 15
#define MACAN_HALF_BIT_US 10
extern void loader_log_putc(char);
static vm_offset_t macan_tlmm;
static void
macan_say(const char *format, ...)
{
 char line[200];
 va_list args;
 va_start(args, format);
 vsnprintf(line, sizeof(line), format, args);
 va_end(args);
 for (char *c = line; *c; c++) {
  loader_log_putc(*c);
 }
 printf("%s", line);
}
static struct macan_i2c gauge_bus = {
 .sda = MACAN_GAUGE_SDA, .scl = MACAN_GAUGE_SCL, .half_us = MACAN_HALF_BIT_US, .drive = 1,
};
static int battery_full_uah, battery_design_uah, battery_cycles;
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
extern void RedwoodBatteryReport(const void *state, unsigned size);
void
redwood_battery_report(const struct redwood_battery_state *b)
{
 int percent = (b->percent_hundredths + 50) / 100;
 battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
 battery_voltage_mv = b->voltage_uv / 1000;
 battery_current_ma = b->current_ua / 1000;
 battery_temperature_c = (b->temperature_hundredths + 50) / 100;
 battery_present = b->present != 0;
 battery_charger_present = b->external != 0;
 battery_charging = b->status == 1;
 battery_soc_estimated = 0;
 battery_charger_action = b->status;
 if (b->full_uah > 0) {
  battery_full_uah = b->full_uah;
 }
 if (b->design_uah > 0) {
  battery_design_uah = b->design_uah;
 }
 if (b->cycles > 0) {
  battery_cycles = b->cycles;
 }
 battery_updates++;
 RedwoodBatteryReport(b, sizeof(*b));
}
static bool
macan_gauge_word_once(uint8_t reg, uint16_t *word)
{
 uint8_t low = 0, high = 0;
 boolean_t interrupts = ml_set_interrupts_enabled(FALSE);
 bool ok = macan_start(&gauge_bus) &&
     macan_write_byte(&gauge_bus, MACAN_GAUGE_ADDRESS << 1) &&
     macan_write_byte(&gauge_bus, reg) &&
     macan_start(&gauge_bus) &&
     macan_write_byte(&gauge_bus, (MACAN_GAUGE_ADDRESS << 1) | 1) &&
     macan_read_byte(&gauge_bus, &low, false) &&
     macan_read_byte(&gauge_bus, &high, true);
 macan_stop(&gauge_bus);
 (void)ml_set_interrupts_enabled(interrupts);
 delay_for_interval(200, NSEC_PER_USEC);
 if (ok) {
  *word = (uint16_t)(low | (high << 8));
 }
 return ok;
}
static bool
macan_gauge_word(uint8_t reg, uint16_t *word)
{
 for (unsigned attempt = 0; attempt < 3; attempt++) {
  if (macan_gauge_word_once(reg, word)) {
   return true;
  }
  delay_for_interval(2, NSEC_PER_MSEC);
 }
 return false;
}
static void
macan_gauge_thread(__unused void *parameter, __unused wait_result_t result)
{
 unsigned said = 0;
 for (;;) {
  uint16_t soc = 0, mv = 0, raw_ma = 0, deci_kelvin = 0, remaining = 0, full = 0, cycles = 0, design = 0;
  bool ok, have_current, have_temperature, have_remaining, have_full, have_cycles, have_design;
  macan_pins_take(&gauge_bus);
  ok = macan_gauge_word(MACAN_GAUGE_SOC, &soc) && macan_gauge_word(MACAN_GAUGE_VOLTAGE, &mv);
  have_current = ok && macan_gauge_word(MACAN_GAUGE_CURRENT, &raw_ma);
  have_temperature = ok && macan_gauge_word(MACAN_GAUGE_TEMPERATURE, &deci_kelvin);
  have_remaining = ok && macan_gauge_word(MACAN_GAUGE_REMAINING, &remaining);
  have_full = ok && macan_gauge_word(MACAN_GAUGE_FULL, &full);
  have_cycles = ok && macan_gauge_word(MACAN_GAUGE_CYCLES, &cycles);
  have_design = ok && macan_gauge_word(MACAN_GAUGE_DESIGN, &design);
  macan_pins_give_back(&gauge_bus);
  if (ok && soc <= 100 && mv >= 2500 && mv <= 5000) {
   int ma = have_current ? (int16_t)raw_ma : battery_current_ma;
   struct redwood_battery_state state = {
    .size = sizeof(state),
    .percent_hundredths = soc * 100,
    .voltage_uv = mv * 1000,
    .current_ua = ma * 1000,
    .temperature_hundredths = have_temperature ? ((int)deci_kelvin - 2732) * 10 :
        battery_temperature_c * 100,
    .present = 1,
    .external = ma > 0 || (soc >= 100 && ma >= 0),
    .status = soc >= 100 && ma >= 0 ? 4 : ma > 0 ? 1 : 2,
    .charge_uah = have_remaining && remaining < 20000 ? remaining * 1000 : 0,
    .full_uah = have_full && full > 0 && full < 20000 ? full * 1000 : 0,
    .design_uah = have_design && design > 0 && design < 20000 ? design * 1000 : 0,
    .cycles = have_cycles && cycles < 10000 ? cycles : 0,
   };
   redwood_battery_report(&state);
   if (said < 4 || battery_updates % 15 == 0) {
    said++;
    macan_say("battery: gauge %u%%, %u mV, %d mA%s, %d C%s, %u of %u mAh (design %u), %u cycles\n", soc, mv, ma,
        have_current ? "" : " (not read)", battery_temperature_c, have_temperature ? "" : " (not read)",
        remaining, full, design, cycles);
   }
  } else {
   battery_failures++;
   if (said < 8) {
    said++;
    macan_say("battery: gauge read %s (soc %u, %u mV)\n", ok ? "out of range" : "failed", soc, mv);
   }
  }
  delay_for_interval(MACAN_GAUGE_PERIOD_S, NSEC_PER_SEC);
 }
}
static bool
macan_battery_start(void)
{
 uint32_t macan = 0;
 thread_t thread;
 if (!PE_parse_boot_argn("macan", &macan, sizeof(macan)) || macan == 0) {
  return false;
 }
 gauge_bus.tlmm = macan_tlmm = ml_io_map(MACAN_TLMM_PHYS, MACAN_TLMM_SIZE);
 if (!macan_tlmm) {
  printf("battery: cannot map the TLMM for the Ace 6T gauge\n");
  return true;
 }
 macan_say("battery: Ace 6T gauge on I2C 0x%x (gpio%u/gpio%u), every %u seconds\n", MACAN_GAUGE_ADDRESS,
     MACAN_GAUGE_SDA, MACAN_GAUGE_SCL, MACAN_GAUGE_PERIOD_S);
 if (kernel_thread_start(macan_gauge_thread, NULL, &thread) == KERN_SUCCESS) {
  thread_deallocate(thread);
 }
 return true;
}
void
redwood_battery_init(void)
{
 uint32_t enabled = 0;
 uint64_t shared_pa = 0;
 vm_offset_t page;
 thread_t thread;
 static bool started;
 if (started) {
  return;
 }
 started = true;
 if (macan_battery_start()) {
  return;
 }
 if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) return;
 if (!PE_parse_boot_argn("battery_shared_pa", &shared_pa, sizeof(shared_pa)) || shared_pa == 0) {
  printf("battery: xnuloader did not provide battery_shared_pa\n");
  return;
 }
 page = ml_io_map((vm_offset_t)(shared_pa & ~(LOADER_PAGE_SIZE - 1)), LOADER_PAGE_SIZE);
 shared_battery = (volatile struct loader_battery *)(page + (shared_pa & (LOADER_PAGE_SIZE - 1)));
 if (shared_battery->magic != LOADER_BATTERY_MAGIC || shared_battery->version != LOADER_BATTERY_VERSION ||
     shared_battery->size < sizeof(struct loader_battery)) {
  printf("battery: bad xnuloader cache at %#llx (magic %#llx version %u size %u)\n", shared_pa,
      shared_battery->magic, shared_battery->version, shared_battery->size);
  return;
 }
 printf("battery: Qualcomm UEFI cache at pa %#llx; polling every 5 seconds\n", shared_pa);
 if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
