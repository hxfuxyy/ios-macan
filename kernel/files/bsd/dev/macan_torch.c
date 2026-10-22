#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/macan_audio.h>
#include <sys/macan_input.h>
#include <sys/systm.h>
#include <sys/uio.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
static uint32_t level;
static bool broken, leaving;
static int say(const char *format, ...) __printflike(1, 2);
static int
say(const char *format, ...)
{
 char line[300];
 va_list list;
 int length;
 va_start(list, format);
 length = vsnprintf(line, sizeof(line), format, list);
 va_end(list);
 for (const char *c = line; *c != 0; c++) {
  loader_log_putc(*c);
 }
 if (!leaving && ml_get_interrupts_enabled()) {
  printf("%s", line);
 }
 return length;
}
#define NOTES 24
static char notes[NOTES][200];
static unsigned noted;
static void note(const char *format, ...) __printflike(1, 2);
static void
note(const char *format, ...)
{
 char *slot = notes[noted++ % NOTES];
 uint64_t ns;
 va_list list;
 int n;
 absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
 n = snprintf(slot, sizeof(notes[0]), "%llu.%03llu s: ", ns / NSEC_PER_SEC, ns / NSEC_PER_MSEC % 1000);
 va_start(list, format);
 vsnprintf(slot + n, sizeof(notes[0]) - (size_t)n, format, list);
 va_end(list);
 say("torch: %s\n", slot);
}
static inline uint32_t
rd(vm_offset_t address)
{
 return *(volatile uint32_t *)address;
}
static inline void
wr(vm_offset_t address, uint32_t value)
{
 *(volatile uint32_t *)address = value;
 __asm__ volatile ("dsb sy");
}
static void
spin_us(uint32_t us)
{
 uint64_t until;
 nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
 until += mach_absolute_time();
 while (mach_absolute_time() < until) {
 }
}
static void
wait_ms(uint32_t ms)
{
 if (leaving) {
  spin_us(ms * 1000);
 } else {
  delay_for_interval(ms, NSEC_PER_MSEC);
 }
}
#define ARB_CORE_PHYS 0x0c400000UL
#define ARB_CORE_SIZE 0x3000UL
#define ARB_MAP_PHYS 0x0c403000UL
#define ARB_MAP_SIZE 0x8000UL
#define ARB_CNFG_PHYS 0x0c42a000UL
#define ARB_CNFG_SIZE 0x8000UL
#define ARB_OBSERVER_PHYS 0x0c4c0000UL
#define ARB_OBSERVER_SIZE 0x40000UL
#define ARB_CHNLS_PHYS 0x0c900000UL
#define ARB_SYSTEM 0
#define ARB_VERSION 0x000
#define ARB_VERSION_8 0x80000000u
#define ARB_CHANNEL(n) (4 * (n))
#define ARB_OWNER(n) (4 * (n))
#define ARB_MAX_CHANNELS (ARB_MAP_SIZE / 4)
#define CHANNEL_COMMAND 0x00
#define CHANNEL_STATUS 0x08
#define CHANNEL_WDATA0 0x10
#define OBSERVER_DATA0 0x18
#define OBSERVER_DATA1 0x1c
#define STATUS_DONE 0x1
#define STATUS_FAILURE 0x2
#define STATUS_DENIED 0x4
#define STATUS_DROPPED 0x8
#define COMMAND_WRITE (0u << 27)
#define COMMAND_READ (1u << 27)
#define TRANSFER_TIMEOUT_US 2000
#define DROPPED "the transfer was dropped"
struct peripheral {
 const char * name;
 uint16_t id;
 uint16_t channel;
 bool found, writable;
 vm_offset_t write;
};
static struct peripheral flash = { .name = "the PM8550's flash LED module", .id = 0x1ee };
static struct peripheral alarm = { .name = "the PM8550's temperature alarm", .id = 0x10a };
static vm_offset_t arb_core, arb_cnfg, arb_observer, arb_map;
static uint8_t alarm_subtype;
static void
lock(void)
{
 if (!leaving) {
  redwood_spmi_lock();
 }
}
static void
unlock(void)
{
 if (!leaving) {
  redwood_spmi_unlock();
 }
}
static const char *
wait_done(vm_offset_t channel)
{
 uint64_t start = mach_absolute_time(), interval;
 uint32_t status;
 nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
 do {
  status = rd(channel + CHANNEL_STATUS);
 } while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
 return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
        (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}
static const char *spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count);
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
 const char *problem = NULL;
 for (int tries = 0; tries < 3; tries++) {
  if ((problem = spmi_read_once(p, reg, bytes, count)) == NULL || strcmp(problem, DROPPED) != 0) {
   break;
  }
 }
 return problem;
}
static const char *
spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
 vm_offset_t channel = arb_observer + 0x20UL * p->channel;
 const char *problem;
 uint32_t data[2];
 if (!p->found) {
  return "no channel";
 }
 lock();
 wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
 problem = wait_done(channel);
 data[0] = rd(channel + OBSERVER_DATA0);
 data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
 unlock();
 if (problem != NULL) {
  return problem;
 }
 for (uint32_t i = 0; i < count; i++) {
  bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
 }
 return NULL;
}
#define FLASH_REVISION1 0x00
#define FLASH_TYPE 0x04
#define FLASH_SUBTYPE 0x05
#define FLASH_STATUS1 0x06
#define FLASH_STATUS2 0x07
#define STATUS2_OTST1 0x20
#define STATUS2_OTST2 0x10
#define STATUS2_VPH_LOW 0x01
#define FLASH_INT_RT_STS 0x10
#define RT_STS_FAULT 0x01
#define FLASH_SAFETY_TIMER0 0x3e
#define FLASH_ITARGET0 0x42
#define FLASH_MODULE_EN 0x46
#define FLASH_IRESOLUTION 0x49
#define FLASH_STROBE_CTRL0 0x4a
#define FLASH_EN_LED_CTRL 0x4e
#define TORCH_CHANNELS 0x09
#define TORCH_SHORT 0x41
#define FLASH_OTST2_CFG1 0x78
#define FLASH_OTST1_CFG1 0x7a
#define ALARM_SUBTYPE 0x05
#define ALARM_STATUS 0x08
#define ANDROID_TORCH_MA 100
#define IRES_UA 5000
#define ANDROID_ITARGET 19
#define HOTTEST_BATTERY_C 45
static const uint8_t writable_regs[] = {
 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
 0x46, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e,
};
static bool shutting_down;
static void
give_up(const char *why)
{
 if (!broken) {
  broken = true;
  note("%s: no more writing to the flash module for the rest of the run", why);
 }
}
static bool
put(uint8_t reg, uint8_t value)
{
 const char *problem;
 uint8_t back = 0xff;
 bool allowed = false;
 for (size_t i = 0; i < sizeof(writable_regs); i++) {
  allowed |= writable_regs[i] == reg;
 }
 if ((broken && !(shutting_down && value == 0 &&
     (reg == FLASH_EN_LED_CTRL || reg == FLASH_MODULE_EN))) || !allowed || !flash.writable || flash.write == 0 || flash.id != 0x1ee) {
  give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
  return false;
 }
 if ((reg >= FLASH_ITARGET0 && reg < FLASH_ITARGET0 + 4 && (value & 0x7f) > ANDROID_ITARGET) ||
     (reg >= FLASH_SAFETY_TIMER0 && reg < FLASH_SAFETY_TIMER0 + 4 && value != 0) ||
     (reg >= FLASH_STROBE_CTRL0 && reg < FLASH_STROBE_CTRL0 + 4 && (value & 0x7f) != 1) ||
     (reg == FLASH_IRESOLUTION && (value & 0x0f) != 0x0f) ||
     (reg == FLASH_EN_LED_CTRL && (value & 0x0f) != 0 && (value & 0x0f) != TORCH_CHANNELS)) {
  give_up("a value outside the torch-only limits");
  return false;
 }
 if (((rd(arb_map + ARB_CHANNEL(flash.channel)) & 0x1fff) != flash.id ||
     (rd(arb_cnfg + ARB_OWNER(flash.channel)) & 7) != ARB_SYSTEM)) {
  give_up("the arbiter's channel is no longer the flash module's");
  return false;
 }
 for (int tries = 0; tries < 3; tries++) {
  lock();
  wr(flash.write + CHANNEL_WDATA0, value);
  wr(flash.write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
  problem = wait_done(flash.write);
  unlock();
  if (problem == NULL || strcmp(problem, DROPPED) != 0) {
   break;
  }
 }
 if (problem != NULL || (problem = spmi_read(&flash, reg, &back, 1)) != NULL) {
  say("torch: %#04x <- %#04x: %s\n", reg, value, problem);
  give_up("a write that failed");
  return false;
 }
 if (back != value) {
  say("torch: %#04x <- %#04x, but it reads %#04x\n", reg, value, back);
  give_up("a register that did not read back as written");
  return false;
 }
 return true;
}
static bool
get(const struct peripheral *p, uint8_t reg, uint8_t *value)
{
 const char *problem = spmi_read(p, reg, value, 1);
 if (problem != NULL) {
  say("torch: reading %#04x of %s: %s\n", reg, p->name, problem);
  return false;
 }
 return true;
}
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
 uint8_t now;
 if (!get(&flash, reg, &now)) {
  give_up("a read that failed");
  return false;
 }
 return (!shutting_down && (now & mask) == (value & mask)) || put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}
static void
find_channel(struct peripheral *p, uint32_t channels)
{
 for (uint32_t n = 0; n < channels; n++) {
  uint32_t map = rd(arb_map + ARB_CHANNEL(n)), owner;
  if (map == 0 || (map & 0x1fff) != p->id) {
   continue;
  }
  owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
  say("torch: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
      owner == ARB_SYSTEM ? ", which is us" : "");
  if (!p->found || owner == ARB_SYSTEM) {
   p->found = true;
   p->channel = (uint16_t)n;
   p->writable = owner == ARB_SYSTEM;
  }
 }
 if (!p->found) {
  say("torch: no channel of the arbiter is %s\n", p->name);
 }
}
static void
show_registers(void)
{
 uint8_t r[8];
 char line[200];
 for (unsigned base = 0; base < 0x100; base += 0x20) {
  int n = snprintf(line, sizeof(line), "torch: %#04x:", base);
  for (unsigned i = 0; i < 0x20; i += 8) {
   if (spmi_read(&flash, (uint8_t)(base + i), r, 8) != NULL) {
    n += snprintf(line + n, sizeof(line) - (size_t)n, " (unreadable)");
    continue;
   }
   for (unsigned j = 0; j < 8; j++) {
    n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", r[j]);
   }
  }
  say("%s\n", line);
 }
}
static const char *
trouble(void)
{
 static char why[180];
 uint8_t rt = 0, status1 = 0, status2 = 0, status3 = 0, alarm_status = 0, stage;
 if (!get(&flash, FLASH_INT_RT_STS, &rt) || !get(&flash, FLASH_STATUS1, &status1) ||
     !get(&flash, FLASH_STATUS2, &status2) || !get(&flash, 0x09, &status3) || (alarm.found && !get(&alarm, ALARM_STATUS, &alarm_status))) {
  return "a read of the flags that failed";
 }
 stage = alarm_subtype == 0x08 ? alarm_status & 0x03 : (alarm_status >> 4) & 0x07;
 if ((status1 & TORCH_SHORT) || (status2 & 0x7f) || (status3 & TORCH_SHORT) || stage != 0) {
  uint8_t more[8] = { 0 }, latched[2] = { 0 };
  (void)spmi_read(&flash, 0x06, more, 8);
  (void)spmi_read(&flash, 0x18, latched, 2);
  snprintf(why, sizeof(why), "the module says fault %#x, channels %#x, temperature and supply %#x (0x06-0x0d: %02x %02x %02x "
      "%02x %02x %02x %02x %02x, latched %#x); the PMIC's temperature stage %u", rt, status1, status2, more[0], more[1],
      more[2], more[3], more[4], more[5], more[6], more[7], latched[0], stage);
  return why;
 }
 if (redwood_battery_temperature() >= HOTTEST_BATTERY_C) {
  snprintf(why, sizeof(why), "the battery is at %d C", redwood_battery_temperature());
  return why;
 }
 return NULL;
}
static bool lit, touched;
static bool
dark(void)
{
 bool channels, module;
 shutting_down = true;
 channels = put(FLASH_EN_LED_CTRL, 0x00);
 module = put(FLASH_MODULE_EN, 0x00);
 shutting_down = false;
 lit = !(channels || module);
 return channels && module;
}
static bool
light(uint32_t permille)
{
 uint32_t ua = ANDROID_TORCH_MA * permille, code;
 const char *why = trouble();
 if (why != NULL) {
  note("not lit: %s", why);
  (void)dark();
  give_up("trouble before the light");
  return false;
 }
 ua = ua < IRES_UA ? IRES_UA : ua;
 code = ua / IRES_UA - 1;
 code = code > ANDROID_ITARGET ? ANDROID_ITARGET : code;
 touched = true;
 for (unsigned ch = 0; ch < 4; ch++) {
  if (!put_bits(FLASH_STROBE_CTRL0 + ch, 0x7f, 0x01)) {
   goto failed;
  }
 }
 if (!put_bits(FLASH_EN_LED_CTRL, 0x0f, 0) || !put_bits(FLASH_MODULE_EN, 0x80, 0)) {
  goto failed;
 }
 for (unsigned ch = 0; ch < 4; ch++) {
  if (!put_bits(FLASH_ITARGET0 + ch, 0x7f, (uint8_t)code)) {
   goto failed;
  }
 }
 if (!put_bits(FLASH_IRESOLUTION, 0x0f, 0x0f)) {
  goto failed;
 }
 for (unsigned ch = 0; ch < 4; ch++) {
  if (!put(FLASH_SAFETY_TIMER0 + ch, 0x00)) {
   goto failed;
  }
 }
 if (!put_bits(FLASH_MODULE_EN, 0x80, 0x80) || !put_bits(FLASH_EN_LED_CTRL, 0x0f, TORCH_CHANNELS)) {
  goto failed;
 }
 lit = true;
 return true;
failed:
 (void)dark();
 give_up("torch setup failed");
 return false;
}
static volatile uint32_t wanted;
static volatile uint32_t asked;
static uint32_t max_secs = 30;
static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
 uint32_t value;
 int error;
 while (uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
  if ((error = uiomove((caddr_t)&value, sizeof(value), uio)) != 0) {
   return error;
  }
  wanted = value > 1000 ? 1000 : value;
  asked++;
 }
 return 0;
}
static uint32_t shown;
static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
 uint32_t value = shown;
 return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}
static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
 return 0;
}
static const struct cdevsw torch_cdevsw = {
 .d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
 .d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
 .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};
static uint64_t
now_ms(void)
{
 uint64_t ns;
 absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
 return ns / NSEC_PER_MSEC;
}
static void
serve(void)
{
 uint32_t applied = 0, times = 0, cut = 0;
 uint64_t since = 0, last_look = 0;
 bool waiting_for_off = false;
 int major = cdevsw_add(-1, &torch_cdevsw);
 if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-torch") == NULL) {
  say("torch: /dev/redwood-torch cannot be made\n");
  return;
 }
 say("torch: THE LIGHT IS IOS'S (/dev/redwood-torch) from here on, until the run ends: at most %u mA per channel, at most %u s "
     "at a time\n", ANDROID_TORCH_MA, max_secs);
 while (!broken) {
  uint32_t want = wanted;
  uint64_t now = now_ms();
  if (want == 0) {
   waiting_for_off = false;
  }
  if (waiting_for_off) {
   want = 0;
  }
  if (want != applied) {
   if (want == 0 ? dark() : light(want)) {
    if (applied == 0) {
     since = now;
     times++;
    }
    if (want == 0) {
     note("off (iOS asked for 0)");
    } else {
     note("ON (iOS asked for %u of 1000; the battery %d C)", wanted, redwood_battery_temperature());
    }
    applied = want;
    shown = want;
   } else {
    break;
   }
  }
  if (applied != 0 && now - since >= max_secs * 1000ull) {
   cut++;
   note("on for %u s, the limit: off until iOS asks again", max_secs);
   waiting_for_off = true;
   if (!dark()) {
    break;
   }
   applied = 0;
   shown = 0;
   continue;
  }
  if (applied != 0 && now - last_look >= 100) {
   const char *why;
   last_look = now;
   if ((why = trouble()) != NULL) {
    note("OFF: %s", why);
    (void)dark();
    give_up("trouble while lit");
    break;
   }
  }
  wait_ms(20);
 }
 if (touched) {
  (void)dark();
 }
 shown = 0;
 say("torch: the light is given up on (%u times lit, %u cut at %u s)\n", times, cut, max_secs);
}
static void
torch_thread(__unused void *parameter, __unused wait_result_t result)
{
 uint32_t version, channels;
 uint8_t id[2] = { 0, 0 }, revision = 0, alarm_status = 0;
 say("torch: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the light on once" :
     "the light for iOS");
 arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
 arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
 arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
 arb_map = ml_io_map(ARB_MAP_PHYS, ARB_MAP_SIZE);
 if (!arb_core || !arb_cnfg || !arb_observer || !arb_map) {
  goto out;
 }
 version = rd(arb_core + ARB_VERSION);
 channels = rd(arb_core + 0x004) & 0x1fff;
 if (version < ARB_VERSION_8 || version >= 0x90000000u) {
  say("torch: PMIC arbiter version %#x, not 8: nothing done\n", version);
  goto out;
 }
 if (channels == 0 || channels > ARB_MAX_CHANNELS) {
  say("torch: invalid bus 0 APID count %u: nothing done\n", channels);
  goto out;
 }
 find_channel(&flash, channels);
 find_channel(&alarm, channels);
 if (!flash.found || spmi_read(&flash, FLASH_TYPE, id, 2) != NULL || spmi_read(&flash, FLASH_REVISION1, &revision, 1) != NULL) {
  say("torch: the flash module cannot be read\n");
  goto out;
 }
 say("torch: the module says type %#x, subtype %#x, revision %#x%s\n", id[0], id[1], revision,
     id[0] == 0x18 && id[1] == 0x07 ? ": a flash LED module of four channels, as Android's driver expects" : ": NOT WHAT WAS EXPECTED");
 show_registers();
 if (alarm.found && get(&alarm, ALARM_SUBTYPE, &alarm_subtype) && get(&alarm, ALARM_STATUS, &alarm_status)) {
  uint8_t stage = alarm_subtype == 0x08 ? alarm_status & 0x03 : (alarm_status >> 4) & 0x07;
  say("torch: the PMIC's temperature alarm (subtype %#x) says %#x: stage %u%s\n", alarm_subtype, alarm_status, stage,
      stage == 0 ? " (all is well)" : "");
 } else {
  alarm.found = false;
 }
 if (id[0] != 0x18 || id[1] != 0x07 || level < 2) {
  goto out;
 }
 if (!flash.writable) {
  say("torch: the channel is not ours to write through: nothing written\n");
  goto out;
 }
 flash.write = ml_io_map(ARB_CHNLS_PHYS + 0x200UL * flash.channel, 0x200);
 if (flash.write == 0 || !alarm.found || (alarm_subtype != 0x08 && alarm_subtype != 0x09)) {
  say("torch: write mapping or PMIC temperature monitor unavailable: nothing written\n");
  goto out;
 }
 uint8_t initial_module, initial_channels;
 if (!get(&flash, FLASH_MODULE_EN, &initial_module) || !get(&flash, FLASH_EN_LED_CTRL, &initial_channels) ||
     (initial_module & 0x80) || (initial_channels & 0x0f)) {
  give_up("unexpected initial illumination");
  goto out;
 }
 PE_parse_boot_argn("torch_max_s", &max_secs, sizeof(max_secs));
 max_secs = max_secs < 1 ? 1 : max_secs > 120 ? 120 : max_secs;
 if (level == 2) {
  uint32_t secs = 2;
  uint64_t until;
  const char *why = NULL;
  PE_parse_boot_argn("torch_secs", &secs, sizeof(secs));
  secs = secs < 1 ? 1 : secs > max_secs ? max_secs : secs;
  say("torch: the light ON at %u mA per channel (ITARGET %u, 5 mA steps) for %u s\n", ANDROID_TORCH_MA, ANDROID_ITARGET,
      secs);
  if (light(1000)) {
   uint8_t rt = 0, s1 = 0, s2 = 0;
   wait_ms(50);
   (void)get(&flash, FLASH_INT_RT_STS, &rt);
   (void)get(&flash, FLASH_STATUS1, &s1);
   (void)get(&flash, FLASH_STATUS2, &s2);
   note("lit at %u mA per channel; 50 ms on: interrupt status %#x (%s), channels %#x, temperature and supply %#x",
       ANDROID_TORCH_MA, rt, (rt & 0x10) ? "ramped up" : "NOT ramped up", s1, s2);
   show_registers();
   for (until = now_ms() + secs * 1000ull; now_ms() < until && (why = trouble()) == NULL;) {
    wait_ms(100);
   }
   if (why != NULL) {
    note("OFF early: %s", why);
   }
   if (dark()) {
    note("off again after %u s", secs);
   }
   show_registers();
  }
 } else {
  serve();
 }
out:
 say("torch: done\n");
 thread_terminate(current_thread());
}
void
macan_torch_init(void)
{
 thread_t thread;
 uint32_t macan = 0;
 if (!PE_parse_boot_argn("macan", &macan, sizeof(macan)) || macan == 0) {
  return;
 }
 level = 3;
 PE_parse_boot_argn("torch", &level, sizeof(level));
 if (level == 0) {
  return;
 }
 level = level > 3 ? 3 : level;
 if (kernel_thread_start(torch_thread, NULL, &thread) == KERN_SUCCESS) {
  thread_deallocate(thread);
 }
}
void
macan_torch_quiesce(void)
{
 uint8_t module = 0xff, channels = 0xff, target = 0xff;
 if (level < 2 || flash.write == 0) {
  return;
 }
 leaving = true;
 if (touched) {
  broken = false;
  say("torch: at the run's end: the light %s\n", dark() ? "switched off" : "COULD NOT BE SWITCHED OFF");
 }
 say("torch: what happened in this run (%u events):\n", noted);
 for (unsigned i = noted > NOTES ? noted - NOTES : 0; i < noted; i++) {
  say("torch:   %s\n", notes[i % NOTES]);
 }
 (void)spmi_read(&flash, FLASH_MODULE_EN, &module, 1);
 (void)spmi_read(&flash, FLASH_EN_LED_CTRL, &channels, 1);
 (void)spmi_read(&flash, FLASH_ITARGET0, &target, 1);
 say("torch: at the run's end the module reads: enable %#04x, channels %#04x, target %#04x%s\n", module, channels, target,
     module == 0 && (channels & 0x0f) == 0 ? " (dark)" : " (NOT DARK)");
}
