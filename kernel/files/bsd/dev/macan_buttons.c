#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/macan_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
static int say(const char *format, ...) __printflike(1, 2);
static int
say(const char *format, ...)
{
 char line[400];
 va_list list;
 va_start(list, format);
 vsnprintf(line, sizeof(line), format, list);
 va_end(list);
 for (const char *c = line; *c != 0; c++) {
  loader_log_putc(*c);
 }
 return printf("%s", line);
}
#define printf say
static inline uint32_t
rd(vm_offset_t address)
{
 return *(volatile uint32_t *)address;
}
static inline void
wr(vm_offset_t address, uint32_t value)
{
 *(volatile uint32_t *)address = value;
}
#define ARB_CORE_PHYS 0x0c440000UL
#define ARB_CORE_SIZE 0x1100UL
#define ARB_CNFG_PHYS 0x0c40a000UL
#define ARB_CNFG_SIZE 0x1000UL
#define ARB_OBSERVER_PHYS 0x0e600000UL
#define ARB_OBSERVER_SIZE 0x10000UL
#define ARB_SYSTEM 0
#define ARB_VERSION 0x000
#define ARB_VERSION_5 0x50000000u
#define ARB_VERSION_7 0x70000000u
#define ARB_FEATURES 0x004
#define ARB_CHANNEL(n) (0x900 + 4 * (n))
#define ARB_CHANNEL_INTERRUPTS (1u << 24)
#define ARB_OWNER(n) (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS 512
#define OBSERVER_COMMAND 0x00
#define OBSERVER_STATUS 0x08
#define OBSERVER_DATA0 0x18
#define OBSERVER_DATA1 0x1c
#define STATUS_DONE 0x1
#define STATUS_FAILURE 0x2
#define STATUS_DENIED 0x4
#define STATUS_DROPPED 0x8
#define COMMAND_READ (1u << 27)
#define READ_TIMEOUT_US 2000
static vm_offset_t arb_core, arb_cnfg, arb_observer, arb_map;
#define MACAN_ARB_CORE_PHYS 0x0c400000UL
#define MACAN_ARB_CORE_SIZE 0x4000UL
#define MACAN_ARB_MAP_PHYS 0x0c403000UL
#define MACAN_ARB_MAP_SIZE 0x8000UL
#define MACAN_ARB_OWNER_PHYS 0x0c42a000UL
#define MACAN_ARB_OWNER_SIZE 0x8000UL
#define MACAN_ARB_OBSERVER_PHYS 0x0c4c0000UL
#define MACAN_ARB_OBSERVER_SIZE 0x40000UL
#define MACAN_ARB_VERSION_8 0x80000000u
#define MACAN_ARB_MAX_CHANNELS (MACAN_ARB_MAP_SIZE / 4)
static bool macan_arb;
static LCK_GRP_DECLARE(spmi_lck_grp, "redwood-spmi");
static LCK_SPIN_DECLARE(spmi_lock, &spmi_lck_grp);
void
redwood_spmi_lock(void)
{
 lck_spin_lock(&spmi_lock);
}
void
redwood_spmi_unlock(void)
{
 lck_spin_unlock(&spmi_lock);
}
struct peripheral {
 const char * name;
 uint16_t id;
 uint16_t channel;
 bool found;
};
static struct peripheral pon = { .name = "the PMK8350's power-on block", .id = 0x013 };
static struct peripheral pin = { .name = "GPIO6 of the PM7325", .id = 0x18d };
static struct {
 uint64_t reads, failed, total_ns, longest_ns, last_ns;
} bus;
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
 vm_offset_t channel = macan_arb ? arb_observer + 0x20UL * p->channel :
     arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
 uint64_t start = mach_absolute_time(), now, interval, taken;
 uint32_t status, data[2];
 const char *problem = NULL;
 nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
 redwood_spmi_lock();
 wr(channel + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
 do {
  status = rd(channel + OBSERVER_STATUS);
  now = mach_absolute_time();
 } while (!(status & STATUS_DONE) && now - start < interval);
 if (!(status & STATUS_DONE)) {
  problem = "no answer from the arbiter";
 } else if (status & STATUS_DENIED) {
  problem = "the arbiter refuses";
 } else if (status & STATUS_FAILURE) {
  problem = "the transfer failed (nobody at that address?)";
 } else if (status & STATUS_DROPPED) {
  problem = "the transfer was dropped";
 } else {
  data[0] = rd(channel + OBSERVER_DATA0);
  data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
  for (uint32_t i = 0; i < count; i++) {
   bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
  }
 }
 redwood_spmi_unlock();
 absolutetime_to_nanoseconds(now - start, &taken);
 bus.reads++;
 bus.total_ns += taken;
 bus.last_ns = taken;
 if (taken > bus.longest_ns) {
  bus.longest_ns = taken;
 }
 if (problem != NULL) {
  bus.failed++;
 }
 return problem;
}
static void
find_channel(struct peripheral *p, uint32_t channels)
{
 for (uint32_t n = 0; n < channels; n++) {
  uint32_t map = macan_arb ? rd(arb_map + 4 * n) : rd(arb_core + ARB_CHANNEL(n)), owner;
  uint32_t id = macan_arb ? (map & 0x1fff) : ((map >> 8) & 0xfff);
  if (map == 0 || id != p->id) {
   continue;
  }
  owner = rd(arb_cnfg + (macan_arb ? 4 * n : ARB_OWNER(n))) & 7;
  printf("buttons: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
      (map & ARB_CHANNEL_INTERRUPTS) ? ", which also gets its interrupts" : "");
  if (!p->found || owner == ARB_SYSTEM) {
   p->found = true;
   p->channel = (uint16_t)n;
  }
 }
 if (!p->found) {
  printf("buttons: no channel of the arbiter is %s (slave %u, peripheral %#x)\n", p->name, p->id >> 8, p->id & 0xff);
 }
}
#define REG_TYPE 0x04
#define REG_SUBTYPE 0x05
#define REG_RT_STATUS 0x10
#define REG_INT_SET_TYPE 0x11
#define REG_INT_POLARITY_HIGH 0x12
#define REG_INT_POLARITY_LOW 0x13
#define REG_INT_ENABLED 0x15
#define REG_INT_LATCHED 0x18
#define REG_INT_PENDING 0x19
#define PON_TYPE 0x01
#define PON_SUBTYPE_HLOS 0x09
#define PON_KPDPWR 0x80
#define PON_RESIN 0x40
#define GPIO_TYPE 0x10
#define GPIO_SUBTYPE_LV 0x10
#define GPIO_SUBTYPE_MV_VIN3 0x13
#define GPIO_SUBTYPE_MV_VIN3_CLK 0x15
#define GPIO_LEVEL 0x01
#define GPIO_REG_MODE 0x40
#define GPIO_REG_SUPPLY 0x41
#define GPIO_REG_PULL 0x42
#define GPIO_REG_ENABLE 0x46
#define POLL_MS 16
#define MINUTE_TICKS (60 * 1000 / POLL_MS)
static struct button {
 const char * name;
 uint32_t record;
 bool usable;
 bool live;
 bool last;
 bool down;
 uint64_t since;
 uint32_t presses;
} power = { .name = "power", .record = REDWOOD_BUTTON_LOCK },
    volume_up = { .name = "volume up", .record = REDWOOD_BUTTON_VOLUME_UP },
    volume_down = { .name = "volume down", .record = REDWOOD_BUTTON_VOLUME_DOWN };
static struct button *resin_key = &volume_down, *pin_key = &volume_up;
static void
now_text(char *text, size_t size, uint64_t now)
{
 clock_sec_t seconds;
 clock_usec_t micro;
 uint64_t up;
 absolutetime_to_nanoseconds(now, &up);
 clock_get_calendar_microtime(&seconds, &micro);
 snprintf(text, size, "%llu.%03llu s (%02u:%02u:%02u.%03u UTC)", up / NSEC_PER_SEC, up / NSEC_PER_MSEC % 1000,
     (unsigned)(seconds / 3600 % 24), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60), (unsigned)(micro / 1000));
}
static void
button_look(struct button *b, bool pressed)
{
 uint64_t now = mach_absolute_time(), held;
 bool steady = pressed == b->last;
 char when[64];
 b->last = pressed;
 if (!b->usable || !steady) {
  return;
 }
 if (!b->live) {
  if (!pressed) {
   b->live = true;
   printf("buttons: %s is released now: believed from here on\n", b->name);
  }
  return;
 }
 if (pressed == b->down) {
  return;
 }
 b->down = pressed;
 now_text(when, sizeof(when), now);
 if (pressed) {
  b->since = now;
  b->presses++;
  printf("buttons: %s PRESSED at %s\n", b->name, when);
 } else {
  absolutetime_to_nanoseconds(now - b->since, &held);
  printf("buttons: %s released at %s, after %llu ms\n", b->name, when, held / NSEC_PER_MSEC);
 }
 redwood_touch_post(pressed ? REDWOOD_BUTTON_DOWN : REDWOOD_BUTTON_UP, b->record, 0, 0);
}
static void
buttons_poll(void)
{
 static unsigned said;
 const char *problem;
 uint8_t status;
 if (power.usable || resin_key->usable) {
  problem = spmi_read(&pon, REG_RT_STATUS, &status, 1);
  if (problem == NULL) {
   button_look(&power, status & PON_KPDPWR);
   button_look(resin_key, status & PON_RESIN);
  } else if (said < 8) {
   said++;
   printf("buttons: reading %s: %s\n", pon.name, problem);
  }
 }
 if (pin_key->usable) {
  problem = spmi_read(&pin, REG_RT_STATUS, &status, 1);
  if (problem == NULL) {
   button_look(pin_key, !(status & GPIO_LEVEL));
  } else if (said < 8) {
   said++;
   printf("buttons: reading %s: %s\n", pin.name, problem);
  }
 }
}
static bool
identify(const struct peripheral *p, uint8_t type, uint8_t subtype_from, uint8_t subtype_to)
{
 const char *problem;
 uint8_t is[2];
 if (!p->found) {
  return false;
 }
 printf("buttons: next: read the type of %s through channel %u (2 registers, 0x%02x04)\n", p->name, p->channel, p->id & 0xff);
 problem = spmi_read(p, REG_TYPE, is, 2);
 if (problem != NULL) {
  printf("buttons: %s: %s\n", p->name, problem);
  return false;
 }
 printf("buttons: %s says type %#x, subtype %#x (the read took %llu us)\n", p->name, is[0], is[1], bus.last_ns / 1000);
 if (is[0] != type || is[1] < subtype_from || is[1] > subtype_to) {
  printf("buttons: ... which is not what this driver knows (type %#x, subtype %#x to %#x): left alone\n", type,
      subtype_from, subtype_to);
  return false;
 }
 return true;
}
static void
pon_inputs(void)
{
 const char *problem;
 uint8_t r[8], more[2];
 printf("buttons: next: read the status and the interrupt setup of %s (registers 0x1310 to 0x1319)\n", pon.name);
 problem = spmi_read(&pon, REG_RT_STATUS, r, 8);
 if (problem == NULL) {
  problem = spmi_read(&pon, REG_INT_LATCHED, more, 2);
 }
 if (problem != NULL) {
  printf("buttons: %s: %s\n", pon.name, problem);
  return;
 }
 printf("buttons: power-on block: status %#x (power %s, %s %s); interrupts: edge %#x, high %#x, low %#x, "
     "enabled %#x, latched %#x, pending %#x\n", r[0], (r[0] & PON_KPDPWR) ? "HELD" : "released", resin_key->name,
     (r[0] & PON_RESIN) ? "HELD" : "released", r[REG_INT_SET_TYPE - REG_RT_STATUS], r[REG_INT_POLARITY_HIGH - REG_RT_STATUS],
     r[REG_INT_POLARITY_LOW - REG_RT_STATUS], r[REG_INT_ENABLED - REG_RT_STATUS], more[0], more[1]);
 power.usable = resin_key->usable = true;
 power.last = r[0] & PON_KPDPWR;
 resin_key->last = r[0] & PON_RESIN;
}
static void
pin_input(void)
{
 const char *problem;
 uint8_t c[8], status = 0;
 unsigned mode, pull;
 printf("buttons: next: read the setup and the level of %s (registers 0x8d40 to 0x8d47, 0x8d10)\n", pin.name);
 problem = spmi_read(&pin, GPIO_REG_MODE, c, 8);
 if (problem == NULL) {
  problem = spmi_read(&pin, REG_RT_STATUS, &status, 1);
 }
 if (problem != NULL) {
  printf("buttons: %s: %s\n", pin.name, problem);
  return;
 }
 mode = c[0] & 3;
 pull = c[GPIO_REG_PULL - GPIO_REG_MODE] & 7;
 printf("buttons: %s's pin: mode %u (%s), supply %u, pull %u (%s), %s; registers 0x40.. = %02x %02x %02x %02x %02x %02x "
     "%02x %02x; level %s\n", pin_key->name, mode, mode == 0 ? "input" : mode == 1 ? "output" : mode == 2 ? "input and output" : "analog",
     c[GPIO_REG_SUPPLY - GPIO_REG_MODE] & 7, pull, pull < 4 ? "up" : pull == 4 ? "down" : "none",
     (c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80) ? "enabled" : "DISABLED", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
     (status & GPIO_LEVEL) ? "high (released)" : "LOW (held)");
 if (mode != 0 || pull >= 4 || !(c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80)) {
  printf("buttons: %s's pin is not an enabled input with a pull-up. Making it one is a write to the PMIC, "
      "which this driver does not do: %s is left alone\n", pin_key->name, pin_key->name);
  return;
 }
 pin_key->usable = true;
 pin_key->last = !(status & GPIO_LEVEL);
}
static void
say_summary(void)
{
 printf("buttons: so far %u presses of power, %u of volume up, %u of volume down; now%s%s%s%s; %llu reads of the bus, "
     "%llu us each (the longest %llu), %llu failed\n", power.presses, volume_up.presses, volume_down.presses,
     power.down ? " power" : "", volume_up.down ? " volume-up" : "", volume_down.down ? " volume-down" : "",
     (power.down || volume_up.down || volume_down.down) ? " held" : " none held", bus.reads,
     bus.reads ? bus.total_ns / bus.reads / 1000 : 0, bus.longest_ns / 1000, bus.failed);
}
static void
buttons_thread(void *parameter, __unused wait_result_t result)
{
 uint32_t steps = (uint32_t)(uintptr_t)parameter, version, channels;
 bool pon_known, pin_known;
 if (macan_arb) {
  printf("buttons: step 1: map the PMIC arbiter of the Ace 6T (%#lx, %#lx, %#lx, %#lx) and read its version\n",
      MACAN_ARB_CORE_PHYS, MACAN_ARB_MAP_PHYS, MACAN_ARB_OWNER_PHYS, MACAN_ARB_OBSERVER_PHYS);
  arb_core = ml_io_map(MACAN_ARB_CORE_PHYS, MACAN_ARB_CORE_SIZE);
  arb_map = ml_io_map(MACAN_ARB_MAP_PHYS, MACAN_ARB_MAP_SIZE);
  arb_cnfg = ml_io_map(MACAN_ARB_OWNER_PHYS, MACAN_ARB_OWNER_SIZE);
  arb_observer = ml_io_map(MACAN_ARB_OBSERVER_PHYS, MACAN_ARB_OBSERVER_SIZE);
  if (!arb_core || !arb_map || !arb_cnfg || !arb_observer) {
   printf("buttons: the arbiter could not be mapped: no buttons\n");
   goto out;
  }
  version = rd(arb_core + ARB_VERSION);
  channels = rd(arb_core + ARB_FEATURES) & 0x1fff;
  printf("buttons: PMIC arbiter version %#x, %u channels\n", version, channels);
  if (version < MACAN_ARB_VERSION_8) {
   printf("buttons: the Ace 6T's arbiter is version 8; this one is not: no buttons\n");
   goto out;
  }
  if (channels == 0 || channels > MACAN_ARB_MAX_CHANNELS) {
   channels = MACAN_ARB_MAX_CHANNELS;
  }
 } else {
  printf("buttons: step 1: map the PMIC arbiter (%#lx, %#lx, %#lx) and read its version\n", ARB_CORE_PHYS, ARB_CNFG_PHYS,
      ARB_OBSERVER_PHYS);
  arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
  arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
  arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
  version = rd(arb_core + ARB_VERSION);
  channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
  printf("buttons: PMIC arbiter version %#x, %u channels\n", version, channels);
  if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
   printf("buttons: this driver knows version 5 of the arbiter only: no buttons\n");
   goto out;
  }
  if (channels == 0 || channels > ARB_MAX_CHANNELS) {
   channels = ARB_MAX_CHANNELS;
  }
 }
 if (steps < 2) {
  goto out;
 }
 printf("buttons: step 2: look through the arbiter's table for the channels of the two peripherals\n");
 find_channel(&pon, channels);
 find_channel(&pin, channels);
 if (steps < 3) {
  goto out;
 }
 printf("buttons: step 3: the first transfers on the SPMI bus\n");
 pon_known = identify(&pon, PON_TYPE, PON_SUBTYPE_HLOS, PON_SUBTYPE_HLOS);
 pin_known = identify(&pin, GPIO_TYPE, GPIO_SUBTYPE_LV, macan_arb ? GPIO_SUBTYPE_MV_VIN3_CLK : GPIO_SUBTYPE_MV_VIN3);
 if (steps < 4) {
  goto out;
 }
 printf("buttons: step 4: how the keys' inputs are set up\n");
 if (pon_known) {
  pon_inputs();
 }
 if (pin_known) {
  pin_input();
 }
 if (!power.usable && !pin_key->usable) {
  printf("buttons: nothing that can be read: no buttons\n");
  goto out;
 }
 if (power.last || volume_down.last || volume_up.last) {
  printf("buttons: a key reads as held before anything was watched:%s%s%s. It is not reported until it has been "
      "seen released\n", power.last ? " power" : "", volume_up.last ? " volume-up" : "",
      volume_down.last ? " volume-down" : "");
 }
 power.live = power.usable && !power.last;
 volume_down.live = volume_down.usable && !volume_down.last;
 volume_up.live = volume_up.usable && !volume_up.last;
 if (steps < 5) {
  goto out;
 }
 printf("buttons: step 5: watching%s%s%s, every %u ms: power is the side button\n", power.usable ? " power" : "",
     volume_up.usable ? " volume-up" : "", volume_down.usable ? " volume-down" : "", POLL_MS);
 for (unsigned tick = 1;; tick++) {
  buttons_poll();
  if (tick % MINUTE_TICKS == 0 || tick == 10 * 1000 / POLL_MS) {
   say_summary();
  }
  if (bus.failed >= 1000 && bus.failed * 2 > bus.reads) {
   printf("buttons: more than half of %llu reads have failed: giving up\n", bus.reads);
   break;
  }
  delay_for_interval(POLL_MS, NSEC_PER_MSEC);
 }
out:
 if (steps < 5) {
  printf("buttons: stopping here (buttons_steps=%u)\n", steps);
 }
 thread_terminate(current_thread());
}
static int buttons_on;
SYSCTL_DECL(_kern_redwood);
SYSCTL_INT(_kern_redwood, OID_AUTO, buttons, CTLFLAG_RD | CTLFLAG_LOCKED, &buttons_on, 0,
    "1 if the power and volume keys' driver runs");
void
redwood_buttons_init(void)
{
 uint32_t enabled = 0, steps = 5, macan = 0;
 thread_t thread;
 if (PE_parse_boot_argn("macan", &macan, sizeof(macan)) && macan != 0) {
  macan_arb = true;
  resin_key = &volume_up;
  pin_key = &volume_down;
  pon.name = "the PMK8550's power-on block";
  pin.name = "GPIO6 of the PM8550";
 }
 if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
  uint32_t touch_screen = 0;
  enabled = macan_arb ||
      (PE_parse_boot_argn("touchscreen", &touch_screen, sizeof(touch_screen)) && touch_screen != 0);
 }
 if (!enabled) {
  return;
 }
 PE_parse_boot_argn("buttons_steps", &steps, sizeof(steps));
 if (steps == 0) {
  return;
 }
 if (kernel_thread_start(buttons_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
  thread_deallocate(thread);
  buttons_on = 1;
 }
}
