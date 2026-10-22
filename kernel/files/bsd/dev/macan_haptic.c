#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/macan_audio.h>
#include <sys/macan_i2c.h>
#include <sys/macan_firmware.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
static vm_offset_t tlmm;
static uint32_t level;
static bool broken, released, leaving;
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
#define TLMM_PHYS 0x0f100000UL
#define TLMM_SIZE 0x0b0000UL
#define GPIO_CFG(n) (tlmm + 0x1000UL * (n))
#define GPIO_IO(n) (tlmm + 0x1000UL * (n) + 4)
#define CFG_PULL_UP 3
#define CFG_OUTPUT (1u << 9)
#define PIN_SDA 64
#define PIN_SCL 65
#define PIN_RESET 82
#define PIN_INT 71
static void
show_pins(void)
{
 static const struct {
  unsigned pin;
  const char *what;
 } pins[] = { { PIN_SDA, "I2C data" }, { PIN_SCL, "I2C clock" }, { PIN_RESET, "reset" }, { PIN_INT, "interrupt" } };
 for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
  uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));
  say("haptic: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what, (cfg >> 2) & 15,
      cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
 }
}
static void
reset_pin(bool high)
{
 uint32_t cfg = rd(GPIO_CFG(PIN_RESET));
 uint32_t gpio = (cfg & ~0x13ffu) | CFG_OUTPUT;
 if (cfg & (1u << 11)) {
  gpio |= 1u << 12;
 }
 wr(GPIO_IO(PIN_RESET), high ? 2 : 0);
 wr(GPIO_CFG(PIN_RESET), gpio);
 released = high;
}
static struct macan_i2c haptic_bus = {
 .sda = PIN_SDA, .scl = PIN_SCL, .half_us = 5, .drive = 0,
};
#define AW_ADDRESS 0x5a
static bool
regs_read(uint8_t reg, uint8_t *values, unsigned count)
{
 bool answered;
 macan_pins_take(&haptic_bus);
 answered = macan_start(&haptic_bus) && macan_write_byte(&haptic_bus, AW_ADDRESS << 1) &&
     macan_write_byte(&haptic_bus, reg) && macan_start(&haptic_bus) &&
     macan_write_byte(&haptic_bus, AW_ADDRESS << 1 | 1);
 for (unsigned i = 0; answered && i < count; i++) {
  answered = macan_read_byte(&haptic_bus, &values[i], i + 1 == count);
 }
 macan_stop(&haptic_bus);
 macan_pins_give_back(&haptic_bus);
 return answered;
}
static bool
regs_write(uint8_t reg, const uint8_t *values, unsigned count)
{
 bool answered;
 macan_pins_take(&haptic_bus);
 answered = macan_start(&haptic_bus) && macan_write_byte(&haptic_bus, AW_ADDRESS << 1) &&
     macan_write_byte(&haptic_bus, reg);
 for (unsigned i = 0; answered && i < count; i++) {
  answered = macan_write_byte(&haptic_bus, values[i]);
 }
 macan_stop(&haptic_bus);
 macan_pins_give_back(&haptic_bus);
 return answered;
}
#define AW_RSTCFG 0x00
#define AW_SYSST 0x01
#define AW_SYSINT 0x02
#define AW_SYSINTM 0x03
#define AW_PLAYCFG1 0x06
#define AW_PLAYCFG2 0x07
#define AW_PLAYCFG3 0x08
#define AW_PLAYCFG4 0x09
#define AW_WAVCFG1 0x0a
#define AW_WAVCFG2 0x0b
#define AW_WAVCFG9 0x12
#define AW_BASEADDRH 0x2d
#define AW_BASEADDRL 0x2e
#define AW_GLBRD5 0x3f
#define AW_RAMADDRH 0x40
#define AW_RAMADDRL 0x41
#define AW_RAMDATA 0x42
#define AW_SYSCTRL3 0x45
#define AW_TMCFG 0x5b
#define AW_TRIM_LRA 0x79
#define AW_CHIPIDH 0x57
#define AW_CHIPIDL 0x58
#define AW_CHIPID 0x9270
#define SYSINT_FAULTS 0xe6
#define HAPTIC_MAX_MS 1000
static void
give_up(const char *why)
{
 broken = true;
 say("haptic: %s: THE CHIP IS PUT INTO RESET (gpio%u low) and nothing more is done\n", why, PIN_RESET);
 if (tlmm != 0) {
  reset_pin(false);
 }
}
static bool
get(uint8_t reg, uint8_t *value)
{
 if (!regs_read(reg, value, 1)) {
  say("haptic: %#04x NOT READ\n", reg);
  return false;
 }
 return true;
}
static bool
put(uint8_t reg, uint8_t value)
{
 uint8_t after = 0;
 if (broken) {
  return false;
 }
 if (!regs_write(reg, &value, 1)) {
  say("haptic: %#04x <- %#04x NOT ACKNOWLEDGED\n", reg, value);
  give_up("a write was not acknowledged");
  return false;
 }
 if (!regs_read(reg, &after, 1)) {
  give_up("a register could not be read back");
  return false;
 }
 say("haptic: %#04x <- %#04x, reads %#04x%s\n", reg, value, after, after == value ? "" : ": NOT AS WRITTEN");
 if (after != value) {
  give_up("a register did not read back as written");
  return false;
 }
 return true;
}
static bool
healthy(const char *when)
{
 uint8_t flags = 0;
 if (!get(AW_SYSINT, &flags)) {
  give_up("the flags could not be read");
  return false;
 }
 if (flags & SYSINT_FAULTS) {
  say("haptic: %s: the chip reports %#04x:%s%s%s%s%s\n", when, flags, flags & 0x80 ? " BOOST SHORT CIRCUIT" : "",
      flags & 0x40 ? " BOOST OVER-VOLTAGE" : "", flags & 0x20 ? " UNDER-VOLTAGE" : "", flags & 0x04 ? " OVER-CURRENT" : "",
      flags & 0x02 ? " OVER-TEMPERATURE" : "");
  give_up("a fault");
  return false;
 }
 return true;
}
static const uint8_t android_regs[0x7d] = {
 [AW_PLAYCFG2] = 0x80, [AW_SYSCTRL3] = 0x12,
};
static void
show_registers(void)
{
 for (unsigned reg = 0; reg <= 0x7c; reg++) {
  uint8_t value;
  if (reg == 0x32 || reg == AW_RAMDATA) {
   continue;
  }
  if (!regs_read((uint8_t)reg, &value, 1)) {
   say("haptic: %#04x NOT READ\n", reg);
   return;
  }
  say("haptic: %#04x = %#04x\n", reg, value);
 }
}
static bool
chip_answers(void)
{
 uint8_t id[2] = { 0, 0 };
 if (!regs_read(AW_CHIPIDH, id, 2)) {
  say("haptic: nobody answers at %#x on the bus of gpio%u/%u\n", AW_ADDRESS, PIN_SDA, PIN_SCL);
  return false;
 }
 say("haptic: the chip at %#x is %#06x%s\n", AW_ADDRESS, id[0] << 8 | id[1], (id[0] << 8 | id[1]) == AW_CHIPID ? ", an AW86927" :
     ": NOT THE AW86927 EXPECTED");
 return (id[0] << 8 | id[1]) == AW_CHIPID;
}
static const uint8_t android_config[][2] = {
 { 0x03, 0x59 }, { AW_PLAYCFG1, 0x54 },
 { 0x20, 0x06 }, { 0x21, 0x08 },
 { 0x2f, 0x46 }, { 0x30, 0x00 }, { 0x31, 0x00 },
 { 0x33, 0x01 }, { 0x36, 0x0a }, { 0x39, 0x30 },
 { 0x34, 0x00 }, { 0x35, 0x00 }, { 0x37, 0x00 }, { 0x38, 0x00 }, { 0x3a, 0x00 },
 { 0x46, 0x09 }, { 0x47, 0x5a },
};
static const uint8_t android_analog[][2] = {
 { 0x70, 0x0f }, { 0x71, 0x93 }, { 0x72, 0x69 }, { 0x74, 0x89 }, { 0x75, 0x88 },
 { AW_TRIM_LRA, 0x07 },
};
static bool
unlock(bool open)
{
 uint8_t value = open ? 0x7d : 0x00;
 if (broken || !regs_write(AW_TMCFG, &value, 1)) {
  give_up("the analog registers could not be opened");
  return false;
 }
 say("haptic: %#04x <- %#04x (the analog registers %s)\n", AW_TMCFG, value, open ? "opened" : "closed");
 return true;
}
static bool
configure(void)
{
 uint8_t d2s;
 say("haptic: Ace 6T RAM configuration is written\n");
 if (!get(0x49, &d2s) || !put(0x49, (d2s & ~0x07u) | 0x06u)) {
  return false;
 }
 for (unsigned i = 0; i < sizeof(android_config) / sizeof(android_config[0]); i++) {
  if (!put(android_config[i][0], android_config[i][1])) {
   return false;
  }
 }
 if (!unlock(true)) {
  return false;
 }
 for (unsigned i = 0; i < sizeof(android_analog) / sizeof(android_analog[0]); i++) {
  if (!put(android_analog[i][0], android_analog[i][1])) {
   (void)unlock(false);
   return false;
  }
 }
 return unlock(false) && healthy("after the configuration");
}
static uint32_t states_seen;
static bool
wait_standby(unsigned ms)
{
 states_seen = 0;
 for (unsigned waited = 0; waited <= ms; waited += 2) {
  uint8_t state = 0xff;
  if (!get(AW_GLBRD5, &state)) {
   return false;
  }
  states_seen |= 1u << (state & 0x0f);
  if ((state & 0x0f) == 0) {
   return true;
  }
  wait_ms(2);
 }
 return false;
}
static bool
load_waveforms(void)
{
 uint64_t size = 0;
 const uint8_t *file = macan_image_file("fw", "aw8697_haptic_170.bin", &size);
 uint32_t sum = 0, base, length;
 uint8_t address[2], back[64];
 if (file == NULL || size != 4395) {
  say("haptic: no aw8697_haptic_170.bin in fw/ of the small image (artifacts/ace6t/hw-android/fw/ when built)\n");
  return false;
 }
 for (uint64_t i = 2; i < size; i++) {
  sum += file[i];
 }
 if ((sum & 0xffff) != 0xad78 || (file[0] << 8 | file[1]) != 0xad78 ||
     (file[2] << 8 | file[3]) != 0x0800 || (file[5] << 8 | file[6]) != 0x0839) {
  say("haptic: aw8697_haptic_170.bin does not add up (%#x, it says %#x): not loaded\n", sum & 0xffff, file[0] << 8 | file[1]);
  return false;
 }
 base = (uint32_t)(file[2] << 8 | file[3]);
 length = (uint32_t)size - 4;
 say("haptic: Android's waveforms: %u bytes from %#x on, %u waveforms\n", length, base, (unsigned)(((file[5] << 8 | file[6]) -
     base - 1) / 4));
 if (!wait_standby(250) || !put(AW_SYSCTRL3, (uint8_t)(android_regs[AW_SYSCTRL3] | 0x04))) {
  give_up("the RAM could not be opened");
  return false;
 }
 address[0] = (uint8_t)(base >> 8);
 address[1] = (uint8_t)base;
 if (!put(AW_BASEADDRH, address[0]) || !put(AW_BASEADDRL, address[1]) || !regs_write(AW_RAMADDRH, address, 2)) {
  give_up("the RAM's address could not be set");
  return false;
 }
 for (uint32_t at = 0; at < length; at += 32) {
  unsigned n = length - at < 32 ? length - at : 32;
  if (!regs_write(AW_RAMDATA, file + 4 + at, n)) {
   give_up("the RAM could not be written");
   return false;
  }
 }
 if (!regs_write(AW_RAMADDRH, address, 2)) {
  give_up("the RAM's address could not be set again");
  return false;
 }
 for (uint32_t at = 0; at < length; at += sizeof(back)) {
  unsigned n = length - at < sizeof(back) ? length - at : sizeof(back);
  if (!regs_read(AW_RAMDATA, back, n) || memcmp(back, file + 4 + at, n) != 0) {
   say("haptic: the RAM reads back differently at %#x\n", base + at);
   give_up("the waveforms did not load");
   return false;
  }
 }
 if (!put(AW_SYSCTRL3, android_regs[AW_SYSCTRL3])) {
  return false;
 }
 say("haptic: Android's waveforms are in the chip's RAM and read back as written\n");
 return healthy("after the waveforms");
}
static bool
play_effect(unsigned effect)
{
 uint64_t began = mach_absolute_time(), ended;
 bool done;
 say("haptic: waveform %u is played once, at Android's gain %#x\n", effect, android_regs[AW_PLAYCFG2]);
 if (!wait_standby(250) || !put(AW_PLAYCFG3, 0x04) || !put(AW_WAVCFG1, (uint8_t)effect) || !put(AW_WAVCFG2, 0x00) ||
     !put(AW_WAVCFG9, 0x00) || !put(AW_PLAYCFG2, android_regs[AW_PLAYCFG2]) || !healthy("before playing")) {
  return false;
 }
 {
  uint8_t go = 0x01;
  if (!regs_write(AW_PLAYCFG4, &go, 1)) {
   give_up("the start was not acknowledged");
   return false;
  }
 }
 wait_ms(2);
 done = wait_standby(HAPTIC_MAX_MS);
 absolutetime_to_nanoseconds(mach_absolute_time() - began, &ended);
 if (!done) {
  uint8_t stop = 0x02;
  say("haptic: STILL PLAYING after %u ms: stopped\n", HAPTIC_MAX_MS);
  (void)regs_write(AW_PLAYCFG4, &stop, 1);
  if (!wait_standby(50)) {
   give_up("it does not stop");
   return false;
  }
 }
 say("haptic: the waveform has played: back in standby after %llu ms (states seen meanwhile: %#x, a bit each; 0 is "
     "standby)\n", ended / NSEC_PER_MSEC, states_seen);
 return healthy("after playing");
}
#define ACTIVE_US 30000
#define SILENT_BELOW 256
#define TAP_US 60000
#define SETTLE_US 8000
#define FOLLOW_US 20000
#define TOLD_EVENTS 24
static volatile uint64_t heard_us;
static volatile uint64_t quiet_us;
static volatile uint64_t onset_us;
static volatile uint32_t heard_peak;
static uint64_t written_frames;
static unsigned taps, loops, capped;
static uint64_t
now_us(void)
{
 uint64_t ns;
 absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
 return ns / NSEC_PER_USEC;
}
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
 int16_t samples[256];
 int error = 0;
 while (uio_resid(uio) >= 2) {
  user_ssize_t bytes = uio_resid(uio);
  uint32_t peak = 0;
  if (bytes > (user_ssize_t)sizeof(samples)) {
   bytes = sizeof(samples);
  }
  bytes &= ~(user_ssize_t)1;
  if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
   break;
  }
  for (unsigned i = 0; i < bytes / 2; i++) {
   uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];
   peak = v > peak ? v : peak;
  }
  written_frames += (uint64_t)bytes / 2;
  if (peak > SILENT_BELOW) {
   uint64_t now = now_us();
   if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
    onset_us = now;
    heard_peak = 0;
   }
   heard_us = now;
   if (peak > heard_peak) {
    heard_peak = peak;
   }
  } else {
   quiet_us = now_us();
  }
 }
 return error;
}
void
macan_haptic_pretend(uint32_t peak)
{
 uint64_t now = now_us();
 if (peak <= SILENT_BELOW) {
  quiet_us = now;
  return;
 }
 if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
  onset_us = now;
  heard_peak = 0;
 }
 heard_us = now;
 if (peak > heard_peak) {
  heard_peak = peak;
 }
}
static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
 return 0;
}
static const struct cdevsw haptic_cdevsw = {
 .d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
 .d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
 .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};
static bool
start_waveform(unsigned waveform, bool forever, uint8_t gain)
{
 uint8_t go = 0x01;
 if (!put(AW_PLAYCFG3, 0x04) || !put(AW_WAVCFG1, (uint8_t)waveform) || !put(AW_WAVCFG2, 0x00) ||
     !put(AW_WAVCFG9, forever ? 0xf0 : 0x00) || !put(AW_PLAYCFG2, gain)) {
  return false;
 }
 if (!regs_write(AW_PLAYCFG4, &go, 1)) {
  give_up("the start was not acknowledged");
  return false;
 }
 return true;
}
static bool
stop_waveform(void)
{
 uint8_t stop = 0x02;
 if (!regs_write(AW_PLAYCFG4, &stop, 1) || !wait_standby(50)) {
  give_up("it does not stop");
  return false;
 }
 return true;
}
static unsigned
chip_state(void)
{
 uint8_t state = 0xff;
 return get(AW_GLBRD5, &state) ? state & 0x0f : 0xff;
}
static uint8_t
gain_for(uint32_t peak)
{
 uint32_t top = android_regs[AW_PLAYCFG2], low = top * 5 / 8, third = 32767 / 3;
 return (uint8_t)(peak >= third ? top : low + (top - low) * peak / third);
}
static void
serve(void)
{
 enum { IDLE, TAP, LOOP, REST } state = IDLE;
 uint64_t since = 0, rest_until = 0, last_look = 0, tap_onset = 0, followed = 0;
 uint8_t playing_gain = 0;
 unsigned follows = 0;
 int major = cdevsw_add(-1, &haptic_cdevsw);
 if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-haptic") == NULL) {
  say("haptic: /dev/redwood-haptic cannot be made\n");
  return;
 }
 say("haptic: THE MOTOR PLAYS WHAT IOS PLAYS ON ITS ACTUATOR (/dev/redwood-haptic) from here on, until the run ends\n");
 while (!broken) {
  uint64_t now = now_us();
  bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
  uint32_t peak = heard_peak;
  uint8_t gain = gain_for(peak);
  switch (state) {
  case IDLE:
  case TAP:
   if (active && onset_us != tap_onset && now - onset_us >= SETTLE_US) {
    uint64_t began = onset_us;
    if (state == TAP && !wait_standby(0) && !stop_waveform()) {
     break;
    }
    if (!start_waveform(1, false, gain ? gain : 1)) {
     break;
    }
    state = TAP;
    since = now;
    tap_onset = began;
    taps++;
    if (taps + loops <= TOLD_EVENTS) {
     wait_ms(4);
     say("haptic: tap %u: iOS's waveform peaks at %u of 32768: Android's waveform 1 at gain %#04x; the "
         "chip's state %u\n", taps, peak, gain ? gain : 1, chip_state());
    }
    break;
   }
   if (state == IDLE) {
    break;
   }
   if (active && quiet_us < tap_onset && now - since > TAP_US) {
    if (stop_waveform() && start_waveform(10, true, gain ? gain : 1)) {
     state = LOOP;
     since = followed = now;
     playing_gain = gain ? gain : 1;
     follows = 0;
     loops++;
     heard_peak = 0;
     if (taps + loops <= TOLD_EVENTS) {
      wait_ms(4);
      say("haptic: vibration %u: iOS goes on playing (peak %u): Android's waveform 10 over and over at "
          "gain %#04x, %u ms at most; the chip's state %u\n", loops, peak, gain ? gain : 1,
          HAPTIC_MAX_MS, chip_state());
     }
    }
   } else if (!active && now - since > TAP_US) {
    state = wait_standby(0) || stop_waveform() ? IDLE : state;
   }
   break;
  case LOOP:
   if (active && now - followed >= FOLLOW_US) {
    followed = now;
    heard_peak = 0;
    if (peak != 0 && (gain > playing_gain + 8 || gain + 8 < playing_gain)) {
     if (!put(AW_PLAYCFG2, gain)) {
      break;
     }
     playing_gain = gain;
     follows++;
    }
   }
   if (!active || now - since > HAPTIC_MAX_MS * 1000ull) {
    bool cut = now - since > HAPTIC_MAX_MS * 1000ull;
    if (cut) {
     capped++;
     rest_until = now + HAPTIC_MAX_MS * 1000ull;
    }
    if (stop_waveform()) {
     state = rest_until > now ? REST : IDLE;
     if (taps + loops <= TOLD_EVENTS + 1) {
      say("haptic: vibration %u: %s after %llu ms (its strength changed %u times, last to gain %#04x); "
          "the chip is in standby\n", loops, cut ? "CUT at the limit" : "iOS stopped, and so does the motor",
          (now - since) / 1000, follows, playing_gain);
     }
    }
   }
   break;
  case REST:
   if (now >= rest_until) {
    state = IDLE;
   }
   break;
  }
  if (state != IDLE && state != REST && now - last_look > 50000) {
   last_look = now;
   if (!healthy("while playing")) {
    break;
   }
  }
  wait_ms(4);
 }
 say("haptic: the motor is given up on (%u taps and %u vibrations played, %u cut at %u ms)\n", taps, loops, capped,
     HAPTIC_MAX_MS);
}
static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
 tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
 if (tlmm == 0) {
  goto out;
 }
 haptic_bus.tlmm = tlmm;
 say("haptic: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the reset released, registers read" :
     level == 3 ? "the chip configured and its waveforms loaded, nothing played" : level == 4 ? "one effect played" :
     "the motor for iOS");
 show_pins();
 if (level >= 2 && !((rd(GPIO_CFG(PIN_RESET)) & CFG_OUTPUT) && (rd(GPIO_IO(PIN_RESET)) & 2))) {
  say("haptic: the reset is released: gpio%u low for 5 ms, then high\n", PIN_RESET);
  reset_pin(false);
  wait_ms(5);
  reset_pin(true);
  wait_ms(8);
 }
 if (level >= 2) {
  released = true;
 }
 if (!chip_answers()) {
  say("haptic: (the reset pin %s)\n", (rd(GPIO_CFG(PIN_RESET)) & CFG_OUTPUT) && (rd(GPIO_IO(PIN_RESET)) & 2) ?
      "drives high: the chip should answer" : "does not drive high: the chip is held in reset or not powered");
  if (released) {
   give_up("the chip does not answer");
  }
 } else {
  show_registers();
  if (level >= 3 && (!configure() || !load_waveforms())) {
   give_up("configuration or waveforms unavailable");
   goto out;
  }
  if (level == 4) {
   uint32_t effect = 1;
   PE_parse_boot_argn("haptic_effect", &effect, sizeof(effect));
   if (effect < 1 || effect > 10) {
    effect = 1;
   }
   (void)play_effect(effect);
  } else if (level == 5 && !broken) {
   serve();
  }
 }
out:
 say("haptic: done\n");
 thread_terminate(current_thread());
}
void
macan_haptic_init(void)
{
 thread_t thread;
 uint32_t macan = 0;
 if (!PE_parse_boot_argn("macan", &macan, sizeof(macan)) || macan == 0) {
  return;
 }
 level = 5;
 PE_parse_boot_argn("haptic", &level, sizeof(level));
 if (level == 0) {
  return;
 }
 if (level > 5) {
  level = 5;
 }
 if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
  thread_deallocate(thread);
 }
}
void
macan_haptic_quiesce(void)
{
 if (released && tlmm != 0) {
  leaving = true;
  reset_pin(false);
  if (level == 5) {
   say("haptic: at the run's end: %u taps and %u vibrations played (%u cut at %u ms), from %llu frames iOS "
       "wrote; the chip is held in reset\n", taps, loops, capped, HAPTIC_MAX_MS, written_frames);
  }
 }
}
