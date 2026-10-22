#include <sys/macan_firmware.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/macan_input.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
extern bool macan_gpu_kept;
extern bool macan_gpu_parameter(uint32_t which, uint64_t *value);
extern uint64_t macan_gpu_address(uint64_t size);
extern void macan_gpu_address_free(uint64_t address, uint64_t size);
extern bool macan_gpu_map(uint64_t address, uint64_t phys, bool read_only);
extern void macan_gpu_mapped(void);
extern void macan_gpu_unmap(uint64_t address, uint64_t size);
extern uint32_t macan_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
extern bool macan_gpu_done(uint32_t fence);
extern uint32_t macan_gpu_last(void);
extern void macan_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
extern void macan_gpu_report(const char *why);
extern void macan_gpu_stop(void);
extern void macan_adreno_stop_kept(void);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
static uint32_t last_step, skip_steps, pause_ms = 2200, fine, watch_seconds = 20;
static uint32_t step;
static uint32_t speed_bin;
static uint32_t newest_step;
static char last_write[96] = "nothing written";
static bool quiet;
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
static void
sleep_us(uint32_t us)
{
 delay_for_interval(us, NSEC_PER_USEC);
}
static void
sleep_ms(uint32_t ms)
{
 delay_for_interval(ms, NSEC_PER_MSEC);
}
static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
 uint32_t value;
 __asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
 return value;
}
static uint32_t __attribute__((noinline))
rd8(vm_offset_t address)
{
 uint32_t value;
 __asm__ volatile ("ldrb %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
 return value;
}
static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
 __asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}
struct block {
 const char *name;
 uint32_t phys, size;
 vm_offset_t base;
};
static void
need(struct block *b)
{
 if (b->base == 0) {
  b->base = ml_io_map(b->phys, b->size);
 }
}
static void
hold(bool single_access)
{
 if (step == newest_step && pause_ms && (!single_access || fine)) {
  sleep_ms(pause_ms);
 }
}
static void
announce(const char *what)
{
 printf("adreno: step %u: %s\n", step, what);
 hold(false);
}
static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
 uint32_t value;
 need(b);
 if (quiet) {
  return rd(b->base + offset);
 }
 if (step == newest_step) {
  printf("adreno: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
  hold(true);
 }
 value = rd(b->base + offset);
 printf("adreno: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
 return value;
}
static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
 need(b);
 if (!quiet) {
  printf("adreno: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
  snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name,
      b->phys + offset);
  hold(true);
 }
 wr(b->base + offset, value);
}
static uint32_t
set(struct block *b, const char *name, uint32_t offset, uint32_t bits)
{
 uint32_t was = get(b, name, offset);
 if ((was & bits) == bits) {
  return was;
 }
 put(b, name, offset, was | bits);
 return get(b, name, offset);
}
static bool
wait_for(struct block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
 for (uint32_t i = 0; i < ms * 10; i++) {
  if ((rd(b->base + offset) & mask) == value) {
   return true;
  }
  sleep_us(100);
 }
 return (rd(b->base + offset) & mask) == value;
}
static struct block gcc = { "gcc", 0x00100000, 0x80000 };
#define GCC_PLL_VOTE 0x52000
#define GCC_PLL_VOTE_GPU_DIV (1u << 16)
#define GCC_GPU_CFG_AHB_CBCR 0x71004
#define GCC_GPU_MEMNOC_GFX_CBCR 0x7100c
#define GCC_GPU_SNOC_DVM_GFX_CBCR 0x71018
#define GCC_DDRSS_GPU_AXI_CBCR 0x71154
#define CBCR_ENABLE (1u << 0)
#define CBCR_OFF (1u << 31)
#define CMD_DB_PHYS 0x80860000UL
#define CMD_DB_SIZE 0x20000UL
struct cmd_db_slave {
 uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
 uint32_t version;
 uint8_t magic[4];
 struct cmd_db_slave slave[8];
 uint32_t checksum, reserved;
 uint8_t data[];
};
struct cmd_db_entry {
 char id[8];
 uint32_t priority[2];
 uint32_t address;
 uint16_t length, offset;
};
static const struct cmd_db_header *cmd_db;
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned *level_count)
{
 const struct cmd_db_header *db = cmd_db;
 char id[8] = { 0 };
 if (level_count != NULL) {
  *level_count = 0;
 }
 if (db == NULL) {
  db = cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
 }
 if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
  return 0;
 }
 strlcpy(id, name, sizeof(id));
 for (unsigned s = 0; s < 8; s++) {
  const struct cmd_db_slave *slave = &db->slave[s];
  const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);
  if (slave->id == 0 || slave->count > 512) {
   continue;
  }
  for (unsigned i = 0; i < slave->count; i++) {
   const uint8_t *data = db->data + slave->data_offset + entry[i].offset;
   unsigned count = entry[i].length / 2u;
   if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
    continue;
   }
   if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
    count = 0;
   }
   for (unsigned l = 0; levels != NULL && l < count && l < 16; l++) {
    levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
    if (l > 0 && levels[l] == 0) {
     break;
    }
    *level_count = l + 1;
   }
   return entry[i].address;
  }
 }
 return 0;
}
static void
show_rpmh_resource(const char *name)
{
 uint16_t level[16];
 unsigned count = 0;
 uint32_t address = rpmh_resource(name, level, &count);
 char levels[160];
 int at = 0;
 if (address == 0) {
  printf("adreno: %u: RPMh has no \"%s\"\n", step, name);
  return;
 }
 levels[0] = 0;
 for (unsigned l = 0; l < count && at < (int)sizeof(levels) - 8; l++) {
  at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level[l]);
 }
 printf("adreno: %u: RPMh has \"%s\" at %#x; levels:%s\n", step, name, address, levels);
}
static bool
step_outside(void)
{
 announce("read the GPU's clocks in the global clock controller (registers the touch screen driver uses too)");
 get(&gcc, "pll votes", GCC_PLL_VOTE);
 get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
 get(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR);
 get(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR);
 get(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR);
 announce("look up the GPU's supply rails in the boot loader's list of RPMh resources (memory at 0x80860000)");
 show_rpmh_resource("gfx.lvl");
 show_rpmh_resource("cx.lvl");
 show_rpmh_resource("mx.lvl");
 return true;
}
static struct block qfprom = { "qfprom", 0x00780000, 0x7000 };
#define QFPROM_CORRECTED 0x4000
#define QFPROM_GPU_SPEED_BIN 0x1e9
#define QFPROM_GPU_GAMING_BIN 0x1f5
static uint32_t
get8(struct block *b, const char *name, uint32_t offset)
{
 uint32_t value;
 need(b);
 if (step == newest_step) {
  printf("adreno: %u: about to read %s %s at %#x (a byte)\n", step, b->name, name, b->phys + offset);
  hold(true);
 }
 value = rd8(b->base + offset);
 printf("adreno: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
 return value;
}
static bool
step_fuses(void)
{
 uint32_t bin, gaming;
 announce("read the GPU's fuses in the error-corrected copy (0x784000), which Linux reads");
 bin = get8(&qfprom, "speed bin, low", QFPROM_CORRECTED + QFPROM_GPU_SPEED_BIN);
 bin |= get8(&qfprom, "speed bin, high", QFPROM_CORRECTED + QFPROM_GPU_SPEED_BIN + 1) << 8;
 bin = bin >> 5 & 0xff;
 speed_bin = bin;
 gaming = get8(&qfprom, "gaming bin", QFPROM_CORRECTED + QFPROM_GPU_GAMING_BIN) >> 5 & 1;
 printf("adreno: %u: speed bin %u: an Adreno %s; gaming bin %u\n", step, bin, bin == 0xac || bin == 0xbe ? "643" : "642L",
     gaming);
 announce("read the same fuses where the phone's device tree has them (0x780000)");
 bin = get8(&qfprom, "raw speed bin, low", QFPROM_GPU_SPEED_BIN);
 bin |= get8(&qfprom, "raw speed bin, high", QFPROM_GPU_SPEED_BIN + 1) << 8;
 printf("adreno: %u: raw speed bin %u\n", step, bin >> 5 & 0xff);
 return true;
}
static struct block gpucc = { "gpucc", 0x03d90000, 0x9000 };
#define GPUCC_PLL0_MODE 0x0000
#define GPUCC_PLL1_MODE 0x0100
#define GPUCC_CXO_AON_CBCR 0x1004
#define GPUCC_GX_SW_RESET 0x1008
#define GPUCC_GX_GDSCR 0x100c
#define GPUCC_GX_GMU_CBCR 0x1064
#define GPUCC_CX_GDSCR 0x106c
#define GPUCC_AHB_CBCR 0x1078
#define GPUCC_CX_GMU_CBCR 0x1098
#define GPUCC_CXO_CBCR 0x109c
#define GPUCC_GMU_RCGR 0x1120
#define GPUCC_CB_CBCR 0x1170
#define GPUCC_HUB_AON_CBCR 0x1178
#define GPUCC_HUB_RCGR 0x117c
#define GPUCC_HUB_CX_INT_CBCR 0x1204
#define GPUCC_CX_HW_CTRL 0x1540
#define GPUCC_GX_DOMAIN_MISC 0x158c
#define GPUCC_SMMU_VOTE_CBCR 0x5000
#define GDSC_SW_COLLAPSE (1u << 0)
#define GDSC_HW_CONTROL (1u << 1)
#define GDSC_POWER_ON (1u << 31)
static bool
step_gpucc(void)
{
 announce("make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb)");
 if ((set(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR, CBCR_ENABLE) & CBCR_OFF) &&
     !wait_for(&gcc, GCC_GPU_CFG_AHB_CBCR, CBCR_OFF, 0, 10)) {
  printf("adreno: %u: it stays off (%#x): the GPU's clock controller would not answer\n", step,
      rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
  return false;
 }
 announce("read the GPU's clock controller (0x3d90000) as it is: the first access to the GPU's part of the SoC");
 get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
 get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
 get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
 get(&gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
 get(&gpucc, "gx_domain_misc", GPUCC_GX_DOMAIN_MISC);
 get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
 get(&gpucc, "pll0 multiplier", GPUCC_PLL0_MODE + 4);
 get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
 get(&gpucc, "pll1 multiplier", GPUCC_PLL1_MODE + 4);
 get(&gpucc, "gmu_clk_src command", GPUCC_GMU_RCGR);
 get(&gpucc, "gmu_clk_src source", GPUCC_GMU_RCGR + 4);
 get(&gpucc, "hub_clk_src command", GPUCC_HUB_RCGR);
 get(&gpucc, "hub_clk_src source", GPUCC_HUB_RCGR + 4);
 get(&gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR);
 get(&gpucc, "gx_gmu", GPUCC_GX_GMU_CBCR);
 get(&gpucc, "ahb", GPUCC_AHB_CBCR);
 get(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
 get(&gpucc, "cxo", GPUCC_CXO_CBCR);
 get(&gpucc, "cb", GPUCC_CB_CBCR);
 get(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR);
 get(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR);
 get(&gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR);
 return true;
}
static bool
gpucc_answers(void)
{
 need(&gcc);
 need(&gpucc);
 if (rd(gcc.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_OFF) {
  printf("adreno: %u: the GPU clock controller's registers have no clock (gcc gpu_cfg_ahb %#x): step 3\n", step,
      rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
  return false;
 }
 return true;
}
static bool
cx_is_on(void)
{
 return (rd(gpucc.base + GPUCC_CX_GDSCR) | rd(gpucc.base + GPUCC_CX_HW_CTRL)) & GDSC_POWER_ON;
}
static void stale_stop(void);
static bool
step_cx_power(void)
{
 uint32_t gdscr;
 if (!gpucc_answers()) {
  return false;
 }
 announce("switch the CX half of the GPU block on (gpu_cx_gdsc): clear the collapse bit of its power switch");
 gdscr = get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
 get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
 if (cx_is_on()) {
  printf("adreno: %u: CX is on already: an earlier kernel left the GPU block running; stopping it first\n", step);
  stale_stop();
  if (cx_is_on()) {
   printf("adreno: %u: CX stays on: going on with it as it is\n", step);
   return true;
  }
  gdscr = rd(gpucc.base + GPUCC_CX_GDSCR);
 }
 if (gdscr & GDSC_HW_CONTROL) {
  printf("adreno: %u: the switch is under hardware control: not ours to operate\n", step);
  return false;
 }
 put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
 for (int i = 0; i < 500 && !cx_is_on(); i++) {
  sleep_us(100);
 }
 get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
 get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
 if (!cx_is_on()) {
  printf("adreno: %u: CX does not come on\n", step);
  return false;
 }
 printf("adreno: %u: CX is on\n", step);
 if (rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) {
  printf("adreno: %u: THE 3D CORE HAS POWER ALREADY, left on by an earlier kernel: not usable until the phone is "
      "restarted\n", step);
  return false;
 }
 return true;
}
#define RCG_UPDATE (1u << 0)
#define RCG_SOURCE_MASK 0x371fu
#define RCG_GPLL0_DIV(twice) (6u << 8 | ((twice) - 1))
static bool
clock_on(struct block *b, const char *name, uint32_t cbcr, bool must)
{
 if (!(set(b, name, cbcr, CBCR_ENABLE) & CBCR_OFF) || wait_for(b, cbcr, CBCR_OFF, 0, 10)) {
  return true;
 }
 printf("adreno: %u: %s %s stays off (%#x)%s\n", step, b->name, name, rd(b->base + cbcr),
     must ? "" : "; its status bit is not one to rely on, going on");
 return !must;
}
static bool
clock_source(const char *name, uint32_t rcgr, uint32_t source)
{
 uint32_t was = get(&gpucc, name, rcgr + 4);
 if ((was & RCG_SOURCE_MASK) == source) {
  return true;
 }
 put(&gpucc, name, rcgr + 4, (was & ~RCG_SOURCE_MASK) | source);
 put(&gpucc, name, rcgr, rd(gpucc.base + rcgr) | RCG_UPDATE);
 if (!wait_for(&gpucc, rcgr, RCG_UPDATE, 0, 10)) {
  printf("adreno: %u: %s does not take its new source (command %#x)\n", step, name, rd(gpucc.base + rcgr));
  return false;
 }
 get(&gpucc, name, rcgr + 4);
 return true;
}
static bool measure_clocks(void);
static bool
step_clocks(void)
{
 if (!gpucc_answers()) {
  return false;
 }
 if (!cx_is_on()) {
  printf("adreno: %u: CX is off: step 4\n", step);
  return false;
 }
 announce("in the global clock controller: ask for the 300 MHz tap for the GPU; the GPU's bus and memory clocks on");
 set(&gcc, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU_DIV);
 if (!clock_on(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR, false) ||
     !clock_on(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR, true) ||
     !clock_on(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR, false)) {
  return false;
 }
 announce("in the GPU's clock controller: the GMU's clock to 200 MHz, the hub's to 150 MHz");
 if (!clock_source("gmu_clk_src", GPUCC_GMU_RCGR, RCG_GPLL0_DIV(3)) ||
     !clock_source("hub_clk_src", GPUCC_HUB_RCGR, RCG_GPLL0_DIV(4))) {
  return false;
 }
 announce("in the GPU's clock controller: the clocks of the GMU and of the SMMU on");
 if (!clock_on(&gpucc, "cb", GPUCC_CB_CBCR, false) ||
     !clock_on(&gpucc, "cxo", GPUCC_CXO_CBCR, true) ||
     !clock_on(&gpucc, "ahb", GPUCC_AHB_CBCR, false) ||
     !clock_on(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR, true) ||
     !clock_on(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR, true) ||
     !clock_on(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, true) ||
     !clock_on(&gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR, false)) {
  return false;
 }
 return measure_clocks();
}
#define GCC_DEBUG_MUX 0x62000
#define GCC_DEBUG_MUX_GPUCC 0x146
#define GCC_DEBUG_DIV 0x62004
#define GCC_DEBUG_CBCR 0x62008
#define GCC_XO_DIV4_CBCR 0x6200c
#define GCC_MEASURE_CTL 0x62038
#define MEASURE_RUN (1u << 20)
#define MEASURE_CLEAR (1u << 21)
#define GCC_MEASURE_STATUS 0x6203c
#define MEASURE_DONE (1u << 25)
#define MEASURE_BRIEF 0x1000
#define MEASURE_FULL 0x20000
#define MEASURE_FAILED 0xffffffffu
#define GPUCC_DEBUG_DIV 0x10fc
#define GPUCC_DEBUG_CBCR 0x1100
#define GPUCC_DEBUG_MUX 0x1568
#define GPUCC_DEBUG_CXO_AON 0x0b
#define GPUCC_DEBUG_GX_GFX3D 0x0d
#define GPUCC_DEBUG_GX_GMU 0x11
#define GPUCC_DEBUG_AHB 0x12
#define GPUCC_DEBUG_CX_GMU 0x1a
#define GPUCC_DEBUG_CXO 0x1b
#define GPUCC_DEBUG_HUB_CX_INT 0x1c
#define GPUCC_DEBUG_CB 0x26
#define GPUCC_DEBUG_HUB_AON 0x27
static uint32_t
count_clock(uint32_t ticks)
{
 uint32_t control = get(&gcc, "measure control", GCC_MEASURE_CTL) & ~(MEASURE_RUN | MEASURE_CLEAR | 0xfffffu);
 uint32_t status = 0;
 put(&gcc, "measure control", GCC_MEASURE_CTL, control | MEASURE_CLEAR);
 sleep_us(10);
 put(&gcc, "measure control", GCC_MEASURE_CTL, control | ticks);
 put(&gcc, "measure control", GCC_MEASURE_CTL, control | ticks | MEASURE_RUN);
 for (int i = 0; i < 2000 && !(status & MEASURE_DONE); i++) {
  sleep_us(100);
  status = rd(gcc.base + GCC_MEASURE_STATUS);
 }
 put(&gcc, "measure control", GCC_MEASURE_CTL, control | ticks);
 return status & MEASURE_DONE ? get(&gcc, "measure status", GCC_MEASURE_STATUS) & 0x1ffffff : MEASURE_FAILED;
}
static uint32_t
measure(uint32_t select)
{
 uint32_t gpu_mux = get(&gpucc, "debug mux", GPUCC_DEBUG_MUX), gpu_div = get(&gpucc, "debug divider", GPUCC_DEBUG_DIV);
 uint32_t gpu_cbcr = get(&gpucc, "debug output", GPUCC_DEBUG_CBCR), mux = get(&gcc, "debug mux", GCC_DEBUG_MUX);
 uint32_t div = get(&gcc, "debug divider", GCC_DEBUG_DIV), cbcr = get(&gcc, "debug input", GCC_DEBUG_CBCR);
 uint32_t base = get(&gcc, "crystal/4", GCC_XO_DIV4_CBCR), control = get(&gcc, "measure control", GCC_MEASURE_CTL);
 uint32_t brief, full;
 put(&gpucc, "debug mux", GPUCC_DEBUG_MUX, (gpu_mux & ~0xffu) | select);
 put(&gpucc, "debug divider", GPUCC_DEBUG_DIV, (gpu_div & ~0xfu) | 1);
 put(&gpucc, "debug output", GPUCC_DEBUG_CBCR, gpu_cbcr | CBCR_ENABLE);
 put(&gcc, "debug mux", GCC_DEBUG_MUX, (mux & ~0x3ffu) | GCC_DEBUG_MUX_GPUCC);
 put(&gcc, "debug divider", GCC_DEBUG_DIV, (div & ~0xfu) | 1);
 put(&gcc, "debug input", GCC_DEBUG_CBCR, cbcr | CBCR_ENABLE);
 put(&gcc, "crystal/4", GCC_XO_DIV4_CBCR, base | CBCR_ENABLE);
 brief = count_clock(MEASURE_BRIEF);
 full = brief == MEASURE_FAILED ? MEASURE_FAILED : count_clock(MEASURE_FULL);
 put(&gcc, "measure control", GCC_MEASURE_CTL, control);
 put(&gcc, "crystal/4", GCC_XO_DIV4_CBCR, base);
 put(&gcc, "debug input", GCC_DEBUG_CBCR, cbcr);
 put(&gcc, "debug divider", GCC_DEBUG_DIV, div);
 put(&gcc, "debug mux", GCC_DEBUG_MUX, mux);
 put(&gpucc, "debug output", GPUCC_DEBUG_CBCR, gpu_cbcr);
 put(&gpucc, "debug divider", GPUCC_DEBUG_DIV, gpu_div);
 put(&gpucc, "debug mux", GPUCC_DEBUG_MUX, gpu_mux);
 if (full == MEASURE_FAILED) {
  return MEASURE_FAILED;
 }
 if (full == brief) {
  return 0;
 }
 return (uint32_t)(((uint64_t)full * 10 + 15) * 4800000 / ((uint64_t)MEASURE_FULL * 10 + 35) * 4);
}
static uint32_t
measure_quietly(uint32_t select)
{
 uint32_t hz;
 quiet = true;
 hz = measure(select);
 quiet = false;
 return hz;
}
static bool
measure_clocks(void)
{
 static const struct {
  const char *name;
  uint32_t select;
 } clocks[] = {
  { "cx_gmu (200 MHz was asked for)", GPUCC_DEBUG_CX_GMU }, { "hub_cx_int (from 150 MHz)", GPUCC_DEBUG_HUB_CX_INT },
  { "hub_aon", GPUCC_DEBUG_HUB_AON }, { "cb", GPUCC_DEBUG_CB }, { "cxo_aon", GPUCC_DEBUG_CXO_AON },
  { "gx_gmu (should be off)", GPUCC_DEBUG_GX_GMU }, { "ahb, THE ONE IN QUESTION,", GPUCC_DEBUG_AHB },
 };
 uint32_t hz, ahb_hz = MEASURE_FAILED;
 announce("measure the GPU block's clocks with the SoC's frequency counter (debug registers at gcc 0x162000.., "
     "gpucc 0x3d910fc, 0x3d91100, 0x3d91568)");
 hz = measure(GPUCC_DEBUG_CXO);
 if (hz == MEASURE_FAILED) {
  printf("adreno: %u: the counter did not finish: it cannot be used as it is set up here\n", step);
  return false;
 }
 printf("adreno: %u: measured: cxo runs at %u Hz (the crystal: 19200000)\n", step, hz);
 if (hz < 18600000 || hz > 19800000) {
  printf("adreno: %u: the counter does not show the crystal's rate: nothing it says can be used\n", step);
  return false;
 }
 for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
  hz = measure_quietly(clocks[i].select);
  if (hz == MEASURE_FAILED) {
   printf("adreno: %u: measured: %s: the counter did not finish\n", step, clocks[i].name);
  } else {
   printf("adreno: %u: measured: %s runs at %u Hz\n", step, clocks[i].name, hz);
  }
  if (clocks[i].select == GPUCC_DEBUG_AHB) {
   ahb_hz = hz;
  }
 }
 if (ahb_hz != MEASURE_FAILED && ahb_hz >= 1000000) {
  printf("adreno: %u: THE REGISTER BUS OF THE GPU BLOCK HAS ITS CLOCK, whatever its control register says\n", step);
  return true;
 }
 printf("adreno: %u: the register bus of the GPU block has no clock; what is not 0 in gpucc 0x1000..0x15fc:\n", step);
 for (uint32_t offset = 0x1000; offset < 0x1600; offset += 16) {
  uint32_t row[4] = { rd(gpucc.base + offset), rd(gpucc.base + offset + 4), rd(gpucc.base + offset + 8),
               rd(gpucc.base + offset + 12) };
  if (row[0] | row[1] | row[2] | row[3]) {
   printf("adreno: %u: gpucc %#x: %#x %#x %#x %#x\n", step, gpucc.phys + offset, row[0], row[1], row[2],
       row[3]);
  }
 }
 return false;
}
static bool
cx_ready(void)
{
 uint32_t gmu_clock, ahb_hz;
 if (!gpucc_answers()) {
  return false;
 }
 gmu_clock = rd(gpucc.base + GPUCC_CX_GMU_CBCR);
 if (!cx_is_on() || (gmu_clock & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
  printf("adreno: %u: needs CX on (it is %s) and the GMU's clock running (cx_gmu %#x): steps 4 and 5\n", step,
      cx_is_on() ? "on" : "off", gmu_clock);
  return false;
 }
 ahb_hz = measure_quietly(GPUCC_DEBUG_AHB);
 if (ahb_hz == MEASURE_FAILED) {
  printf("adreno: %u: the frequency counter did not finish (step 5 shows whether it works)\n", step);
 } else {
  printf("adreno: %u: the GPU block's register bus clock (ahb) measures %u Hz\n", step, ahb_hz);
 }
 if (ahb_hz == MEASURE_FAILED || ahb_hz < 1000000) {
  printf("adreno: %u: without that clock a register of the GPU block would not answer: not touching it\n", step);
  return false;
 }
 return true;
}
static struct block gmu = { "gmu", 0x03d6a000, 0x34000 };
#define GMU_WORD(n) (4u * (n))
#define GMU_DTCM GMU_WORD(0x1c00)
#define GMU_DTCM_VERSIONS GMU_WORD(0x1c00 + 0xff8)
#define GMU_CM3_SYSRESET GMU_WORD(0x5000)
#define GMU_CM3_BOOT_CONFIG GMU_WORD(0x5001)
#define GMU_CM3_FW_INIT_RESULT GMU_WORD(0x501c)
#define GMU_CM3_CFG GMU_WORD(0x502d)
#define GMU_ALWAYS_ON_COUNTER_L GMU_WORD(0x5088)
#define GMU_ALWAYS_ON_COUNTER_H GMU_WORD(0x5089)
#define GMU_SPTPRAC_PWR_CLK_STATUS GMU_WORD(0x50d0)
#define GMU_RPMH_POWER_STATE GMU_WORD(0x50ec)
#define GMU_HFI_CTRL_STATUS GMU_WORD(0x5180)
#define GMU_HFI_VERSION_INFO GMU_WORD(0x5181)
#define GMU_GENERAL_7 GMU_WORD(0x51cc)
#define GMU_AO_HOST_INTERRUPT_STATUS GMU_WORD(0x9305)
#define GMU_RSCC_CONTROL_ACK GMU_WORD(0x9308)
#define GMU_AO_GPU_CX_BUSY_STATUS GMU_WORD(0x930c)
#define GMU_AHB_FENCE_STATUS GMU_WORD(0x9313)
static struct block gbif = { "gbif", 0x03d00000 + 4 * 0x3c00, 0x1000 };
#define GBIF_SCACHE_CNTL0 (4 * 0x01)
#define GBIF_HALT (4 * 0x45)
#define GBIF_HALT_ACK (4 * 0x46)
static struct block cx_misc = { "cx_misc", 0x03d9e000, 0x1000 };
#define CX_MISC_SYSTEM_CACHE_CNTL_0 (4 * 1)
#define CX_MISC_SYSTEM_CACHE_CNTL_1 (4 * 2)
static bool
step_first_words(void)
{
 uint64_t start, nanos;
 uint32_t before, hz;
 if (!cx_ready()) {
  return false;
 }
 announce("read registers of the GMU (0x3d6a000): the first words from the GPU block itself");
 get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
 get(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG);
 get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
 get(&gmu, "cm3_cfg", GMU_CM3_CFG);
 get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
 get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
 get(&gmu, "general_7", GMU_GENERAL_7);
 get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
 get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
 get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
 get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
 get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
 get(&gmu, "always_on_counter_h", GMU_ALWAYS_ON_COUNTER_H);
 get(&gmu, "always_on_counter_l", GMU_ALWAYS_ON_COUNTER_L);
 start = mach_absolute_time();
 before = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L);
 sleep_ms(10);
 hz = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L) - before;
 absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
 hz = nanos ? (uint32_t)((uint64_t)hz * NSEC_PER_SEC / nanos) : 0;
 printf("adreno: %u: the GMU's counter counts at %u Hz: %s\n", step, hz, hz > 18000000 && hz < 20500000 ?
     "the 19.2 MHz crystal: THE GPU BLOCK IS ALIVE" : "not the crystal's 19.2 MHz (firmware may start it): see the rest");
 for (uint32_t i = 0; i < 5; i++) {
  get(&gmu, "dtcm, where firmware has its versions", GMU_DTCM_VERSIONS + 4 * i);
 }
 announce("read the GPU's bus interface (0x3d0f000) and the CX odds and ends (0x3d9e000)");
 get(&gbif, "scache_cntl0", GBIF_SCACHE_CNTL0);
 get(&gbif, "halt", GBIF_HALT);
 get(&gbif, "halt_ack", GBIF_HALT_ACK);
 get(&cx_misc, "system_cache_cntl_0", CX_MISC_SYSTEM_CACHE_CNTL_0);
 get(&cx_misc, "system_cache_cntl_1", CX_MISC_SYSTEM_CACHE_CNTL_1);
 return true;
}
static struct block rscc = { "rscc", 0x03de0000, 0x2000 };
#define RSCC_RSC_STATUS0_DRV0 (4 * 0x004)
#define RSCC_PDC_SLAVE_ID_DRV0 (4 * 0x00b)
#define RSCC_SEQ_BUSY_DRV0 (4 * 0x101)
#define RSCC_SEQ_MEM_0_DRV0 (4 * 0x180)
#define RSCC_TCS0_DRV0_STATUS (4 * 0x346)
#define RSCC_ASLEEP (1u << 16)
#define RSCC_DRV_CONFIG 0x00c
#define RSCC_SLOTS 0xd00
#define RSCC_SLOT_SIZE 0x2a0
static void
rscc_show(void)
{
 get(&rscc, "drv config", RSCC_DRV_CONFIG);
 get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
 get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
 get(&rscc, "slots' interrupts enabled", RSCC_SLOTS);
 get(&rscc, "slots' interrupts pending", RSCC_SLOTS + 4);
 for (uint32_t slot = 0; slot < 4; slot++) {
  vm_offset_t at = rscc.base + RSCC_SLOTS + RSCC_SLOT_SIZE * slot;
  printf("adreno: %u: rscc slot %u: wait %#x, control %#x, idle %#x, enabled %#x; commands (number, address, data, "
      "status):\n", step, slot, rd(at + 0x10), rd(at + 0x14), rd(at + 0x18), rd(at + 0x1c));
  printf("adreno: %u:   %x %x %x %x | %x %x %x %x | %x %x %x %x | %x %x %x %x\n", step,
      rd(at + 0x30), rd(at + 0x34), rd(at + 0x38), rd(at + 0x3c), rd(at + 0x44), rd(at + 0x48), rd(at + 0x4c),
      rd(at + 0x50), rd(at + 0x58), rd(at + 0x5c), rd(at + 0x60), rd(at + 0x64), rd(at + 0x6c), rd(at + 0x70),
      rd(at + 0x74), rd(at + 0x78));
 }
}
static bool
step_rscc(void)
{
 if (!cx_ready()) {
  return false;
 }
 announce("read the GMU's RPMh channel (0x3de0000)");
 get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
 get(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0);
 get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
 get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
 get(&rscc, "tcs0_drv0_status", RSCC_TCS0_DRV0_STATUS);
 get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
 return true;
}
static struct block smmu = { "smmu", 0x03da0000, 0x20000 };
#define SMMU_SCR0 0x000
#define SMMU_IDR0 0x020
#define SMMU_IDR1 0x024
#define SMMU_IDR2 0x028
#define SMMU_IDR7 0x03c
#define SMMU_SGFSR 0x048
#define SMMU_SMR(n) (0x800 + 4 * (n))
#define SMMU_S2CR(n) (0xc00 + 4 * (n))
#define SMMU_CBAR(n) (4 * (n))
#define SMMU_CB_SCTLR 0x00
#define SMMU_CB_TTBR0 0x20
#define SMMU_CB_TCR 0x30
#define SMMU_CB_FSR 0x58
static uint32_t smmu_page, smmu_pages, smmu_contexts, smmu_matches;
static bool
smmu_layout(void)
{
 uint32_t idr0 = get(&smmu, "idr0", SMMU_IDR0), idr1 = get(&smmu, "idr1", SMMU_IDR1);
 smmu_page = idr1 >> 31 ? 0x10000 : 0x1000;
 smmu_pages = 2u << (idr1 >> 28 & 7);
 smmu_contexts = idr1 & 0xff;
 smmu_matches = idr0 & 0xff;
 printf("adreno: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", step,
     smmu_matches, smmu_contexts, smmu_pages, smmu_page);
 if (2 * smmu_pages * smmu_page != smmu.size) {
  printf("adreno: %u: that makes %#x of registers, the device tree says %#x: not reading further\n", step,
      2 * smmu_pages * smmu_page, smmu.size);
  return false;
 }
 return true;
}
static bool
step_smmu(void)
{
 if (!cx_ready()) {
  return false;
 }
 announce("read the ID registers of the GPU's SMMU (0x3da0000): global registers, which the hypervisor answers for");
 if (!smmu_layout()) {
  return false;
 }
 get(&smmu, "idr2", SMMU_IDR2);
 get(&smmu, "idr7", SMMU_IDR7);
 get(&smmu, "scr0", SMMU_SCR0);
 get(&smmu, "sgfsr", SMMU_SGFSR);
 announce("read the SMMU's stream table and the kinds of its contexts, as the boot loader and hypervisor left them");
 for (uint32_t i = 0; i < smmu_matches && i < 16; i++) {
  get(&smmu, "smr", SMMU_SMR(i));
  get(&smmu, "s2cr", SMMU_S2CR(i));
 }
 for (uint32_t i = 0; i < smmu_contexts && i < 16; i++) {
  get(&smmu, "cbar", smmu_page + SMMU_CBAR(i));
 }
 return true;
}
static bool
step_smmu_context(void)
{
 uint32_t context;
 if (!cx_ready()) {
  return false;
 }
 announce("read translation context 0 of the SMMU: registers of the hardware itself, not the hypervisor's");
 if (!smmu_layout()) {
  return false;
 }
 context = smmu_pages * smmu_page;
 get(&smmu, "cb0 sctlr", context + SMMU_CB_SCTLR);
 get(&smmu, "cb0 ttbr0, low", context + SMMU_CB_TTBR0);
 get(&smmu, "cb0 ttbr0, high", context + SMMU_CB_TTBR0 + 4);
 get(&smmu, "cb0 tcr", context + SMMU_CB_TCR);
 get(&smmu, "cb0 fsr", context + SMMU_CB_FSR);
 return true;
}
static bool
step_gmu_memory(void)
{
 static const uint32_t pattern[2] = { 0x600d0001, 0x0a110002 };
 uint32_t was[2], back[2];
 if (!cx_ready()) {
  return false;
 }
 announce("hold the GMU's processor in reset, write two words into its data memory (0x3d71000), read them back");
 if (get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT) == 0xbabeface) {
  printf("adreno: %u: firmware is running in the GMU: leaving its memory alone\n", step);
  return false;
 }
 put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
 get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
 for (int i = 0; i < 2; i++) {
  was[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
 }
 for (int i = 0; i < 2; i++) {
  put(&gmu, "dtcm", GMU_DTCM + 4 * i, pattern[i]);
 }
 for (int i = 0; i < 2; i++) {
  back[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
 }
 for (int i = 0; i < 2; i++) {
  put(&gmu, "dtcm", GMU_DTCM + 4 * i, was[i]);
 }
 printf("adreno: %u: the GMU's memory %s\n", step, back[0] == pattern[0] && back[1] == pattern[1] ?
     "TAKES WHAT IS WRITTEN: its firmware can be loaded" : "does not hold what was written");
 return back[0] == pattern[0] && back[1] == pattern[1];
}
#define PTE_TABLE 3ull
#define PTE_PAGE 3ull
#define PTE_CACHED (1ull << 2)
#define PTE_UNPRIVILEGED (1ull << 6)
#define PTE_SHARED (2ull << 8)
#define PTE_ACCESSED (1ull << 10)
#define PTE_NOT_GLOBAL (1ull << 11)
#define PTE_ADDRESS 0x0000fffffffff000ull
#define PTE_GMU (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)
#define SMMU_S2CR_FAULT (2u << 16)
#define SMMU_SMR_VALID (1u << 31)
#define SMMU_STREAM_MASK 0x400
#define SMMU_CBAR_STAGE_1 (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n) (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT 1
#define SMMU_CB_TCR2 0x10
#define SMMU_CB_TTBR1 0x28
#define SMMU_CB_MAIR0 0x38
#define SMMU_CB_MAIR1 0x3c
#define SMMU_CB_FAR 0x60
#define SMMU_CB_FSYNR0 0x68
#define SMMU_CB_TLBIASID 0x610
#define SMMU_CB_TLBSYNC 0x7f0
#define SMMU_CB_TLBSTATUS 0x7f4
#define SMMU_SCTLR_ON (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12)
#define SMMU_TCR_39_BITS (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2 (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0 0xf404ff44
#define SMMU_FSR_ALL 0xc00001fe
extern void loader_return(void);
static void
settle(void)
{
 __asm__ volatile ("dsb sy");
}
static void
to_memory(vm_offset_t start, vm_size_t size)
{
 for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
  __asm__ volatile ("dc civac, %0" : : "r" (line));
 }
 settle();
}
#define SPACE_TABLES 1024
struct gpu_space {
 const char *name;
 uint32_t context;
 uint32_t control;
 uint32_t table_count;
 uint64_t *root;
 struct {
  uint64_t phys;
  uint64_t *table;
  bool changed;
 } tables[SPACE_TABLES];
 uint64_t *last_entry, *last_table;
 uint32_t last_changed;
};
static uint64_t *
space_new_table(struct gpu_space *space)
{
 static vm_offset_t spare;
 static unsigned spare_left;
 uint64_t *table;
 if (space->table_count == SPACE_TABLES) {
  return NULL;
 }
 if (spare_left == 0) {
  if (kmem_alloc(kernel_map, &spare, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
      KERN_SUCCESS) {
   return NULL;
  }
  spare_left = PAGE_SIZE / 4096;
 }
 table = (uint64_t *)(spare + PAGE_SIZE - 4096 * spare_left--);
 space->tables[space->table_count].phys = kvtophys((vm_offset_t)table);
 space->tables[space->table_count++].table = table;
 return table;
}
static void
space_changed(struct gpu_space *space, const uint64_t *entry)
{
 if ((uintptr_t)entry - (uintptr_t)space->tables[space->last_changed].table < 4096) {
  space->tables[space->last_changed].changed = true;
  return;
 }
 for (uint32_t i = space->table_count; i-- > 0;) {
  if ((uintptr_t)entry - (uintptr_t)space->tables[i].table < 4096) {
   space->tables[i].changed = true;
   space->last_changed = i;
   return;
  }
 }
}
static void
space_commit(struct gpu_space *space)
{
 for (uint32_t i = 0; i < space->table_count; i++) {
  if (space->tables[i].changed) {
   to_memory((vm_offset_t)space->tables[i].table, 4096);
   space->tables[i].changed = false;
  }
 }
}
static uint64_t *
space_next(struct gpu_space *space, uint64_t *entry)
{
 uint64_t *table;
 if (*entry & 1) {
  for (uint32_t i = 0; i < space->table_count; i++) {
   if (space->tables[i].phys == (*entry & PTE_ADDRESS)) {
    return space->tables[i].table;
   }
  }
  return NULL;
 }
 table = space_new_table(space);
 if (table != NULL) {
  *entry = kvtophys((vm_offset_t)table) | PTE_TABLE;
  space_changed(space, entry);
 }
 return table;
}
static bool
space_map(struct gpu_space *space, uint64_t address, uint64_t phys, uint64_t attributes)
{
 uint64_t *level2, *level3;
 if (address >> 39 || (space->root == NULL && (space->root = space_new_table(space)) == NULL)) {
  return false;
 }
 level2 = space_next(space, &space->root[address >> 30 & 0x1ff]);
 if (level2 == NULL) {
  return false;
 }
 if (space->last_entry == &level2[address >> 21 & 0x1ff]) {
  level3 = space->last_table;
 } else {
  level3 = space_next(space, &level2[address >> 21 & 0x1ff]);
  space->last_entry = &level2[address >> 21 & 0x1ff];
  space->last_table = level3;
 }
 if (level3 == NULL) {
  space->last_entry = NULL;
  return false;
 }
 level3[address >> 12 & 0x1ff] = phys == 0 && attributes == 0 ? 0 : phys | attributes | PTE_PAGE;
 space_changed(space, level3);
 return true;
}
struct gpu_block {
 uint64_t address;
 uint32_t size;
 vm_offset_t memory;
};
static bool
block_new(struct gpu_space *space, struct gpu_block *block, uint64_t address, uint32_t size, uint64_t attributes)
{
 block->address = address;
 block->size = (size + 4095) & ~4095u;
 if (kmem_alloc(kernel_map, &block->memory, round_page(block->size), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
     VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
  return false;
 }
 for (uint32_t offset = 0; offset < block->size; offset += 4096) {
  if (!space_map(space, address + offset, kvtophys(block->memory + offset), attributes)) {
   return false;
  }
 }
 to_memory(block->memory, block->size);
 printf("adreno: %u: %s sees %#llx..%#llx in memory of ours (%#llx)\n", step, space->name, address,
     address + block->size - 1, kvtophys(block->memory));
 return true;
}
static vm_offset_t
block_uncached(const struct gpu_block *block)
{
 uint64_t phys = kvtophys(block->memory);
 for (uint32_t offset = 0; offset < block->size; offset += 4096) {
  if (kvtophys(block->memory + offset) != phys + offset) {
   return 0;
  }
 }
 return ml_io_map_wcomb(phys, block->size);
}
static uint32_t
smmu_context(const struct gpu_space *space)
{
 return (smmu_pages + space->context) * smmu_page;
}
static bool
space_faulted(const struct gpu_space *space)
{
 uint32_t context = smmu_context(space), status = rd(smmu.base + context + SMMU_CB_FSR);
 if ((status & SMMU_FSR_ALL) == 0) {
  return false;
 }
 printf("adreno: %u: THE SMMU REFUSED %s: status %#x, address %#llx, syndrome %#x\n", step, space->name, status,
     (uint64_t)rd(smmu.base + context + SMMU_CB_FAR + 4) << 32 | rd(smmu.base + context + SMMU_CB_FAR),
     rd(smmu.base + context + SMMU_CB_FSYNR0));
 return true;
}
static bool
space_stream(struct gpu_space *space, uint32_t match, uint32_t stream)
{
 put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
 put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);
 if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
     (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context) {
  printf("adreno: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
      stream, space->context);
  return false;
 }
 return true;
}
static bool
space_on(struct gpu_space *space, uint32_t match, uint32_t stream)
{
 uint32_t context = smmu_context(space);
 uint64_t root = space->tables[0].phys | (uint64_t)space->context << 48;
 for (uint32_t i = 0; i < space->table_count; i++) {
  to_memory((vm_offset_t)space->tables[i].table, 4096);
 }
 put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, 0);
 put(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context), SMMU_CBA2R_64_BIT);
 put(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context), SMMU_CBAR_STAGE_1);
 put(&smmu, "cb tcr2", context + SMMU_CB_TCR2, SMMU_TCR2);
 put(&smmu, "cb tcr", context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
 put(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0, (uint32_t)root);
 put(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
 put(&smmu, "cb ttbr1, low", context + SMMU_CB_TTBR1, 0);
 put(&smmu, "cb ttbr1, high", context + SMMU_CB_TTBR1 + 4, 0);
 put(&smmu, "cb mair0", context + SMMU_CB_MAIR0, SMMU_MAIR0);
 put(&smmu, "cb mair1", context + SMMU_CB_MAIR1, 0);
 put(&smmu, "cb fsr", context + SMMU_CB_FSR, SMMU_FSR_ALL);
 put(&smmu, "cb tlbiasid", context + SMMU_CB_TLBIASID, space->context);
 put(&smmu, "cb tlbsync", context + SMMU_CB_TLBSYNC, 1);
 if (!wait_for(&smmu, context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
  printf("adreno: %u: the SMMU does not finish forgetting old translations (%#x)\n", step,
      rd(smmu.base + context + SMMU_CB_TLBSTATUS));
  return false;
 }
 put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, SMMU_SCTLR_ON | space->control);
 put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
 put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);
 if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
     (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context ||
     (get(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
     !(get(&smmu, "cb sctlr", context + SMMU_CB_SCTLR) & 1) ||
     get(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0) != (uint32_t)root ||
     get(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
  printf("adreno: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
      stream, space->context);
  return false;
 }
 get(&smmu, "cb tcr", context + SMMU_CB_TCR);
 get(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context));
 get(&smmu, "cb tcr2", context + SMMU_CB_TCR2);
 get(&smmu, "cb fsr", context + SMMU_CB_FSR);
 get(&smmu, "sgfsr", SMMU_SGFSR);
 return true;
}
static void
space_off(struct gpu_space *space, uint32_t match)
{
 put(&smmu, "smr", SMMU_SMR(match), 0);
 put(&smmu, "s2cr", SMMU_S2CR(match), SMMU_S2CR_FAULT);
 put(&smmu, "cb sctlr", smmu_context(space) + SMMU_CB_SCTLR, 0);
}
#define SCM_FAST (1ull << 31)
#define SCM_64 (1ull << 30)
#define SCM_SIP (2ull << 24)
#define SCM_CALL(service,command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE SCM_CALL(6, 1)
#define SCM_PAS_INIT_IMAGE SCM_CALL(2, 1)
#define SCM_PAS_AUTH_AND_RESET SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN SCM_CALL(2, 6)
#define SCM_PAS_IS_SUPPORTED SCM_CALL(2, 7)
#define SCM_ONE_VALUE 1
#define SCM_VALUE_AND_BUFFER (2 | 2 << 6)
#define SCM_INTERRUPTED 1
#define PAS_GPU 13
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
 uint64_t session = 0;
 printf("adreno: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
     second, third);
 snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
 hold(true);
 for (int i = 0; i < 100; i++) {
  register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
  register uint64_t x1 __asm__("x1") = arguments;
  register uint64_t x2 __asm__("x2") = first;
  register uint64_t x3 __asm__("x3") = second;
  register uint64_t x4 __asm__("x4") = third;
  register uint64_t x5 __asm__("x5") = 0;
  register uint64_t x6 __asm__("x6") = session;
  register uint64_t x7 __asm__("x7") = 0;
  __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
      "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  session = x6;
  if (x0 != SCM_INTERRUPTED) {
   printf("adreno: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
   *answer = x1;
   return (int32_t)x0;
  }
 }
 printf("adreno: %u: TrustZone keeps saying the call was interrupted\n", step);
 return -1;
}
static bool
step_trustzone(void)
{
 static const struct {
  const char *name;
  uint32_t id;
 } images[] = { { "audio", 1 }, { "modem", 4 }, { "video", 9 }, { "GPU zap shader", PAS_GPU } };
 uint64_t answer = 0, how = SCM_FAST | SCM_64 | SCM_SIP;
 uint32_t fast[4] = { 0 }, slow[4] = { 0 };
 announce("ask TrustZone what Linux asks it first: is the call \"is this call available\" available");
 if (scm("whether it can be asked", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE, 0,
     0, &answer) != 0 || answer != 1) {
  how = SCM_FAST | SCM_SIP;
  if (scm("the same, the 32-bit way", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE,
      0, 0, &answer) != 0 || answer != 1) {
   printf("adreno: %u: TrustZone does not answer the way it answers Linux\n", step);
   return false;
  }
 }
 announce("ask TrustZone whether it has the calls that authenticate a firmware image");
 scm("for the call that takes an image's signature", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
     SCM_SIP | SCM_PAS_INIT_IMAGE, 0, 0, &answer);
 scm("for the call that authenticates and starts an image", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
     SCM_SIP | SCM_PAS_AUTH_AND_RESET, 0, 0, &answer);
 announce("ask TrustZone which images it \"supports\": three this phone starts through it, and the GPU's; each asked "
     "both ways, at once and (as Linux does) as a call that may be preempted");
 for (unsigned i = 0; i < 4; i++) {
  answer = 0;
  fast[i] = scm(images[i].name, how | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0, 0, &answer) == 0 && answer;
  answer = 0;
  slow[i] = scm(images[i].name, (how & ~SCM_FAST) | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0,
      0, &answer) == 0 && answer;
 }
 printf("adreno: %u: \"supported\", asked at once / as Linux asks: audio %u/%u, modem %u/%u, video %u/%u, GPU %u/%u\n",
     step, fast[0], slow[0], fast[1], slow[1], fast[2], slow[2], fast[3], slow[3]);
 printf("adreno: %u: %s\n", step, fast[3] || slow[3] ? "TrustZone lists the GPU's zap shader" :
     fast[0] || slow[0] || fast[1] || slow[1] || fast[2] || slow[2] ?
     "TrustZone lists others but not the GPU's: step 12 will show whether that matters" :
     "TrustZone lists none, not even those the phone starts: this question tells nothing here");
 return true;
}
static bool image_asked_quietly;
static const uint8_t *
image_file(const char *directory, const char *name, uint64_t *size)
{
 const uint8_t *file = macan_image_file(directory, name, size);
 if (!image_asked_quietly) {
  if (file != NULL) {
   printf("adreno: %u: %s/%s: %llu bytes in the image\n", step, directory, name, *size);
  } else {
   printf("adreno: %u: no %s/%s in the small image: put the phone's firmware into fw-gpu/ and rebuild\n",
       step, directory, name);
  }
 }
 return file;
}
static bool
zap_header(uint64_t *phys)
{
 const uint8_t *header;
 vm_offset_t page = 0;
 uint64_t size = 0;
 uint32_t segments, hashes;
 header = image_file("fw", "a660_zap.mdt", &size);
 if (header == NULL) {
  return false;
 }
 segments = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
 if (memcmp(header, "\177ELF\1", 5) != 0 || segments < 2 || size > PAGE_SIZE) {
  printf("adreno: %u: that is not such a header (%02x %02x %02x %02x, %u segments)\n", step, header[0], header[1],
      header[2], header[3], segments);
  return false;
 }
 memcpy(&hashes, header + 52 + 32 + 16, sizeof(hashes));
 printf("adreno: %u: an ELF header of %u segments, %u bytes with its table, then %llu bytes; by the table, %u of hashes\n",
     step, segments, 52 + 32 * segments, size - (52 + 32 * segments), hashes);
 if (52 + 32 * segments + (uint64_t)hashes != size) {
  printf("adreno: %u: the file is not the header followed by the hashes alone: not what TrustZone takes\n", step);
  return false;
 }
 if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
  printf("adreno: %u: no memory\n", step);
  return false;
 }
 memcpy((void *)page, header, size);
 to_memory(page, PAGE_SIZE);
 *phys = kvtophys(page);
 return true;
}
static bool
step_zap_signature(void)
{
 uint64_t header = 0, answer = ~0ull;
 int32_t result;
 announce("find the zap shader's header, fw/a660_zap.mdt, in the small image that came with the kernel");
 if (!zap_header(&header)) {
  return false;
 }
 announce("hand the header to TrustZone to check its signature (image 13); a call that may be preempted, as Linux's is");
 result = scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
     header, 0, &answer);
 if (result == 0 && answer == 0) {
  printf("adreno: %u: TRUSTZONE ACCEPTS THE ZAP SHADER'S SIGNATURE FROM THIS KERNEL\n", step);
  return true;
 }
 printf("adreno: %u: TrustZone does not take it: the call returns %d, its answer is %#llx\n", step, result, answer);
 return false;
}
#define GMU_ITCM_ADDRESS 0x00000000u
#define GMU_DTCM_ADDRESS 0x10004000u
#define GMU_TCM_SIZE 0x4000u
#define GMU_LOG_ADDRESS 0x60002000u
#define GMU_LOG_SIZE 0x4000u
#define GMU_HFI_ADDRESS 0x60006000u
#define GMU_STREAM 5
#define GMU_MATCH 1
struct gmu_firmware_block {
 uint32_t address, size, type, value;
};
#define GMU_FIRMWARE_NEEDS 1
#define GMU_FIRMWARE_NEEDS_KEPT 7
static struct gpu_space gmu_space = { .name = "the GMU", .context = 1 };
static struct gpu_block gmu_blocks[12], gmu_log, gmu_hfi;
static unsigned gmu_block_count;
static const uint8_t *gmu_firmware;
static uint64_t gmu_firmware_size;
static bool gmu_space_is_on, gmu_runs, gmu_talks, core_is_on, rsc_awake, cx_off;
static void gmu_post_mortem(void);
static bool
in_tcm(uint32_t address)
{
 return address - GMU_ITCM_ADDRESS < GMU_TCM_SIZE || address - GMU_DTCM_ADDRESS < GMU_TCM_SIZE;
}
static bool
step_gmu_space(void)
{
 if (!cx_ready()) {
  return false;
 }
 announce("find the GMU's firmware, fw/a660_gmu.bin, in the small image, and make the memory it asks for");
 gmu_firmware = image_file("fw", "a660_gmu.bin", &gmu_firmware_size);
 if (gmu_firmware == NULL) {
  return false;
 }
 for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
  struct gmu_firmware_block block;
  memcpy(&block, gmu_firmware + at, sizeof(block));
  at += sizeof(block) + block.size;
  if (at > gmu_firmware_size) {
   printf("adreno: %u: the file ends inside a block\n", step);
   return false;
  }
  if (block.size != 0 || (block.type != GMU_FIRMWARE_NEEDS && block.type != GMU_FIRMWARE_NEEDS_KEPT) ||
      in_tcm(block.address)) {
   continue;
  }
  if (gmu_block_count == sizeof(gmu_blocks) / sizeof(gmu_blocks[0]) || (block.address & 4095) ||
      block.value > 0x100000 || !block_new(&gmu_space, &gmu_blocks[gmu_block_count++], block.address, block.value,
      PTE_GMU)) {
   printf("adreno: %u: cannot make %#x bytes at %#x for the firmware\n", step, block.value, block.address);
   return false;
  }
 }
 if (!block_new(&gmu_space, &gmu_log, GMU_LOG_ADDRESS, GMU_LOG_SIZE, PTE_GMU) ||
     !block_new(&gmu_space, &gmu_hfi, GMU_HFI_ADDRESS, PAGE_SIZE, PTE_GMU)) {
  return false;
 }
 announce("give the SMMU's context 1 these tables and send the GMU's requests (stream 5) through it: the first "
     "writes to the SMMU, the global ones (cbar, cba2r, s2cr, smr) through the hypervisor");
 if (!smmu_layout()) {
  return false;
 }
 gmu_space_is_on = true;
 return space_on(&gmu_space, GMU_MATCH, GMU_STREAM);
}
#define GMU_ITCM GMU_WORD(0x0c00)
static bool
step_gmu_firmware(void)
{
 uint32_t wrong = 0;
 if (!cx_ready() || gmu_firmware == NULL) {
  return false;
 }
 announce("load the GMU's firmware: its first 16K of code and of data through the GMU's registers (0x3d6d000, "
     "0x3d71000), the rest into the memory just made");
 put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
 for (int pass = 0; pass < 2; pass++) {
  for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
   struct gmu_firmware_block block;
   const uint8_t *data = gmu_firmware + at + sizeof(block);
   memcpy(&block, gmu_firmware + at, sizeof(block));
   at += sizeof(block) + block.size;
   if (block.size == 0) {
    continue;
   }
   if (in_tcm(block.address) && in_tcm(block.address + block.size - 1)) {
    uint32_t registers = block.address < GMU_DTCM_ADDRESS ? GMU_ITCM + block.address - GMU_ITCM_ADDRESS :
        GMU_DTCM + block.address - GMU_DTCM_ADDRESS;
    for (uint32_t offset = 0; offset + 4 <= block.size; offset += 4) {
     uint32_t word;
     memcpy(&word, data + offset, sizeof(word));
     if (pass == 0) {
      wr(gmu.base + registers + offset, word);
     } else if (rd(gmu.base + registers + offset) != word) {
      wrong++;
     }
    }
    if (pass == 1) {
     printf("adreno: %u: %#x bytes at the GMU's %#x, through its registers\n", step, block.size,
         block.address);
    }
    continue;
   }
   for (unsigned i = 0; pass == 0 && i <= gmu_block_count; i++) {
    if (i == gmu_block_count) {
     printf("adreno: %u: the firmware has %#x bytes for %#x, where it asked for no memory\n", step,
         block.size, block.address);
     return false;
    }
    if (block.address >= gmu_blocks[i].address &&
        block.address + block.size <= gmu_blocks[i].address + gmu_blocks[i].size) {
     memcpy((void *)(gmu_blocks[i].memory + (block.address - gmu_blocks[i].address)), data, block.size);
     to_memory(gmu_blocks[i].memory, gmu_blocks[i].size);
     printf("adreno: %u: %#x bytes at the GMU's %#x, in memory\n", step, block.size, block.address);
     break;
    }
   }
  }
 }
 snprintf(last_write, sizeof(last_write), "step %u wrote the GMU's firmware into its code and data memory", step);
 if (wrong) {
  printf("adreno: %u: %u words of the GMU's own memory do not read back as written\n", step, wrong);
  return false;
 }
 printf("adreno: %u: THE GMU'S FIRMWARE IS LOADED; its versions, read back from the GMU: core %#x, power %#x, HFI %#x\n",
     step, rd(gmu.base + GMU_DTCM_VERSIONS), rd(gmu.base + GMU_DTCM_VERSIONS + 8), rd(gmu.base + GMU_DTCM_VERSIONS + 16));
 return true;
}
#define GMU_ICACHE_CONFIG GMU_WORD(0x4c00)
#define GMU_DCACHE_CONFIG GMU_WORD(0x4c01)
#define GMU_SYS_BUS_CONFIG GMU_WORD(0x4c0f)
#define GMU_PWR_COL_INTER_FRAME_CTRL GMU_WORD(0x50c0)
#define GMU_RPMH_CTRL GMU_WORD(0x50e8)
#define GMU_RPMH_CTRL_ALL 0xf11
#define GMU_CX_FAL_INTF GMU_WORD(0x50f0)
#define GMU_CX_FALNEXT_INTF GMU_WORD(0x50f1)
#define GMU_PWR_COL_CP_MSG GMU_WORD(0x5100)
#define GMU_PWR_COL_CP_RESP GMU_WORD(0x5101)
#define GMU_HFI_SFR_ADDR GMU_WORD(0x5182)
#define GMU_HFI_QTBL_INFO GMU_WORD(0x5184)
#define GMU_HFI_QTBL_ADDR GMU_WORD(0x5185)
#define GMU_HFI_CTRL_INIT GMU_WORD(0x5186)
#define GMU_GMU2HOST_INTR_CLR GMU_WORD(0x5191)
#define GMU_GMU2HOST_INTR_INFO GMU_WORD(0x5192)
#define GMU_HOST2GMU_INTR_SET GMU_WORD(0x5194)
#define GMU_RSCC_CONTROL_REQ GMU_WORD(0x9307)
#define GMU_AO_AHB_FENCE_CTRL GMU_WORD(0x9310)
#define GMU_AHB_FENCE_RANGE_0 GMU_WORD(0x9311)
#define GMU_CHIP 0x06035000
#define GMU_STARTED_MASK 0x1ff
#define GMU_STARTED 0x100
#define GMU_FAILED 0xe00
struct hfi_queue {
 uint32_t status, start, type, words, message_words, unused[5], read, write;
};
struct hfi_table {
 uint32_t version, words, first_queue, queue_words, queues, active_queues;
 struct hfi_queue queue[17];
};
#define HFI_QUEUES 3
#define HFI_QUEUE_WORDS 1024
#define HFI_QUEUE_AT(n) (((uint32_t)sizeof(struct hfi_table) + 15) / 16 * 16 + 4096 * (n))
static volatile struct hfi_table *hfi;
static bool
step_gmu_start(void)
{
 static const uint8_t priority[HFI_QUEUES] = { 10, 10, 40 };
 uint32_t result = 0;
 if (!cx_ready() || !gmu_space_is_on) {
  return false;
 }
 hfi = (volatile struct hfi_table *)block_uncached(&gmu_hfi);
 if (hfi == NULL) {
  printf("adreno: %u: the page for the messages is not in one piece\n", step);
  return false;
 }
 hfi->words = sizeof(struct hfi_table) / 4;
 hfi->first_queue = 6;
 hfi->queue_words = sizeof(struct hfi_queue) / 4;
 hfi->queues = hfi->active_queues = HFI_QUEUES;
 for (unsigned i = 0; i < HFI_QUEUES; i++) {
  hfi->queue[i].start = GMU_HFI_ADDRESS + HFI_QUEUE_AT(i);
  hfi->queue[i].type = i | (uint32_t)priority[i] << 8;
  hfi->queue[i].words = HFI_QUEUE_WORDS;
  hfi->queue[i].status = 1;
 }
 announce("tell the GMU where things are, by its registers: the message table, the log, which GPU this is; its "
     "caches and its use of RPMh; then let its processor out of reset");
 if (get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET) != 1) {
  printf("adreno: %u: the GMU's processor is not held in reset: not configuring it while it runs\n", step);
  return false;
 }
 rscc_show();
 if (rd(rscc.base + RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP) {
  put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 2);
  if (!wait_for(&gmu, GMU_RSCC_CONTROL_ACK, 2, 2, 100) || !wait_for(&rscc, RSCC_SEQ_BUSY_DRV0, ~0u, 0, 100)) {
   printf("adreno: %u: the GMU's RPMh channel does not wake\n", step);
  }
  get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
  put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
  get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
  get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
 }
 rsc_awake = true;
 put(&gmu, "cx_falnext_intf", GMU_CX_FALNEXT_INTF, 1);
 put(&gmu, "cx_fal_intf", GMU_CX_FAL_INTF, 1);
 put(&gmu, "general_7", GMU_GENERAL_7, 1);
 put(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT, 0);
 put(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG, 2);
 put(&gmu, "hfi_qtbl_addr", GMU_HFI_QTBL_ADDR, GMU_HFI_ADDRESS);
 put(&gmu, "hfi_qtbl_info", GMU_HFI_QTBL_INFO, 1);
 put(&gmu, "ahb_fence_range_0", GMU_AHB_FENCE_RANGE_0, 1u << 31 | (0xa << 2) << 18 | 0x8a0);
 put(&gmu, "cm3_cfg", GMU_CM3_CFG, 0x4052);
 put(&gbif, "halt", GBIF_HALT, 0);
 put(&gmu, "pwr_col_cp_resp", GMU_PWR_COL_CP_RESP, 0);
 put(&gmu, "hfi_sfr_addr", GMU_HFI_SFR_ADDR, GMU_CHIP);
 put(&gmu, "pwr_col_cp_msg", GMU_PWR_COL_CP_MSG, GMU_LOG_ADDRESS | (GMU_LOG_SIZE / 4096 - 1));
 put(&gmu, "sys_bus_config", GMU_SYS_BUS_CONFIG, 1);
 put(&gmu, "icache_config", GMU_ICACHE_CONFIG, 1);
 put(&gmu, "dcache_config", GMU_DCACHE_CONFIG, 1);
 put(&gmu, "pwr_col_inter_frame_ctrl", GMU_PWR_COL_INTER_FRAME_CTRL, 0x9c40400);
 set(&gmu, "rpmh_ctrl", GMU_RPMH_CTRL, GMU_RPMH_CTRL_ALL);
 put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, 0xffffffff);
 gmu_runs = true;
 put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 0);
 for (int i = 0; i < 5000 && (result & GMU_STARTED_MASK) != GMU_STARTED && !(result & GMU_FAILED); i++) {
  sleep_us(100);
  result = rd(gmu.base + GMU_CM3_FW_INIT_RESULT);
 }
 get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
 get(&gmu, "gmu2host_intr_info", GMU_GMU2HOST_INTR_INFO);
 get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
 get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
 if (space_faulted(&gmu_space) || (result & GMU_STARTED_MASK) != GMU_STARTED) {
  printf("adreno: %u: the GMU's firmware does not come up\n", step);
  gmu_post_mortem();
  return false;
 }
 printf("adreno: %u: THE GMU'S FIRMWARE RUNS\n", step);
 return true;
}
static struct block pdc = { "pdc", 0x0b290000, 0x10000 };
#define PDC_GPU_ENABLE (4 * 0x1140)
#define PDC_GPU_SEQ_START_ADDR (4 * 0x1148)
#define RSCC_PDC_SEQ_START_ADDR (4 * 0x008)
#define RSCC_PDC_MATCH_VALUE_LO (4 * 0x009)
#define RSCC_PDC_MATCH_VALUE_HI (4 * 0x00a)
#define RSCC_HIDDEN_TCS_CMD0_ADDR (4 * 0x00d)
#define RSCC_HIDDEN_TCS_CMD0_DATA (4 * 0x00e)
#define RSCC_OVERRIDE_START_ADDR (4 * 0x100)
static bool
step_gmu_queues(void)
{
 static const uint32_t program[5] = { 0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2, 0x0020edad };
 if (!cx_ready() || !gmu_runs) {
  return false;
 }
 announce("enable the GPU's part of the always-on sequencer (pdc, 0xb294500 and 0xb294520: a part of the SoC not "
     "touched before)");
 get(&pdc, "gpu_enable", PDC_GPU_ENABLE);
 get(&pdc, "gpu_seq_start_addr", PDC_GPU_SEQ_START_ADDR);
 put(&pdc, "gpu_seq_start_addr", PDC_GPU_SEQ_START_ADDR, 0);
 put(&pdc, "gpu_enable", PDC_GPU_ENABLE, 0x80000001);
 get(&pdc, "gpu_enable", PDC_GPU_ENABLE);
 announce("give the GMU's RPMh channel its sleep and wake program (rscc, 0x3de0000), unless it has it");
 if (get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0) == program[0] &&
     get(&rscc, "hidden_tcs_cmd2_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 4) == 0x80000021) {
  goto programmed;
 }
 put(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0, 1u << 24);
 put(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0, 1);
 put(&rscc, "hidden_tcs_cmd0_data", RSCC_HIDDEN_TCS_CMD0_DATA, 0);
 put(&rscc, "hidden_tcs_cmd0_addr", RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
 put(&rscc, "hidden_tcs_cmd1_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 2, 0);
 put(&rscc, "hidden_tcs_cmd1_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 4 * 2, 0);
 put(&rscc, "hidden_tcs_cmd2_data", RSCC_HIDDEN_TCS_CMD0_DATA + 4 * 4, 0x80000021);
 put(&rscc, "hidden_tcs_cmd2_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 4 * 4, 0);
 put(&rscc, "override_start_addr", RSCC_OVERRIDE_START_ADDR, 0);
 put(&rscc, "pdc_seq_start_addr", RSCC_PDC_SEQ_START_ADDR, 0x4520);
 put(&rscc, "pdc_match_value_lo", RSCC_PDC_MATCH_VALUE_LO, 0x4510);
 put(&rscc, "pdc_match_value_hi", RSCC_PDC_MATCH_VALUE_HI, 0x4514);
 for (unsigned i = 0; i < 5; i++) {
  put(&rscc, "seq_mem_drv0", RSCC_SEQ_MEM_0_DRV0 + 4 * i, program[i]);
 }
 get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
programmed:
 announce("ask the GMU to start its message queues");
 put(&gmu, "hfi_ctrl_init", GMU_HFI_CTRL_INIT, 1);
 if (!wait_for(&gmu, GMU_HFI_CTRL_STATUS, 1, 1, 500)) {
  get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
  get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
  space_faulted(&gmu_space);
  printf("adreno: %u: the GMU does not start its message queues\n", step);
  return false;
 }
 get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
 get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
 gmu_talks = true;
 printf("adreno: %u: THE GMU'S MESSAGE QUEUES ARE UP\n", step);
 return !space_faulted(&gmu_space);
}
#define HFI_BW_TABLE 3
#define HFI_PERF_TABLE 4
#define HFI_START 10
#define HFI_CORE_FW_START 14
#define HFI_GX_BW_PERF_VOTE 30
#define HFI_PREPARE_SLUMBER 33
#define HFI_ERROR 100
#define HFI_ANSWER 1
#define HFI_NO_INDEX 0xff
struct hfi_perf_table {
 uint32_t header, gpu_levels, gmu_levels;
 struct {
  uint32_t vote, acd, khz;
 } gpu[16];
 struct {
  uint32_t vote, khz;
 } gmu[4];
};
struct hfi_bw_table {
 uint32_t header, levels, cnoc_commands, ddr_commands, cnoc_wait, ddr_wait;
 uint32_t cnoc_address[6], cnoc_data[2][6], ddr_address[8], ddr_data[16][8];
};
static uint32_t hfi_number;
static bool post_mortem_done;
static bool
hfi_receive(unsigned which, uint32_t number, uint32_t *verdict)
{
 volatile struct hfi_queue *queue = &hfi->queue[which];
 volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(which));
 bool answered = false;
 while (queue->read != queue->write) {
  uint32_t message[8] = { 0 }, at = queue->read % HFI_QUEUE_WORDS, words = data[at] >> 8 & 0xff;
  for (uint32_t i = 0; i < words; i++) {
   if (i < 8) {
    message[i] = data[at];
   }
   at = (at + 1) % HFI_QUEUE_WORDS;
  }
  queue->read = (at + 3) / 4 * 4 % HFI_QUEUE_WORDS;
  if ((message[0] >> 16 & 0xf) == HFI_ANSWER && message[1] >> 20 == number) {
   *verdict = message[2];
   answered = true;
  } else {
   message[7] = 0;
   if ((message[0] & 0xff) == HFI_ERROR) {
    printf("adreno: %u: THE GMU REPORTS AN ERROR: %u, %u, \"%s\"\n", step, message[1] >> 16,
        message[1] & 0xffff, (const char *)&message[2]);
   } else {
    printf("adreno: %u: the GMU says (queue %u, %u words): %#x %#x %#x %#x %#x %#x\n", step, which, words,
        message[0], message[1], message[2], message[3], message[4], message[5]);
   }
  }
  if (words == 0) {
   break;
  }
 }
 return answered;
}
static uint32_t
gmu_data(uint32_t address)
{
 for (unsigned i = 0; i < gmu_block_count; i++) {
  if (address - (uint32_t)gmu_blocks[i].address < gmu_blocks[i].size - 3) {
   vm_offset_t at = gmu_blocks[i].memory + (address - (uint32_t)gmu_blocks[i].address);
   to_memory(at, 4);
   return *(volatile uint32_t *)at;
  }
 }
 return 0xdeadbeef;
}
#define GMU_STATE_ADDRESS 0x108454
static void
gmu_post_mortem(void)
{
 uint32_t next = rd(gmu.base + GMU_PWR_COL_CP_RESP) & 0xffff, entries = GMU_LOG_SIZE / 16;
 const volatile uint32_t *log = (const volatile uint32_t *)gmu_log.memory;
 to_memory(gmu_log.memory, GMU_LOG_SIZE);
 printf("adreno: %u: the GMU's log (event, time, two values), %u events in, the last 40:\n", step, next);
 for (uint32_t i = next > 40 ? next - 40 : 0; i < next && i + 4 <= entries; i += 4) {
  printf("adreno: %u:   %x %x %x %x | %x %x %x %x | %x %x %x %x | %x %x %x %x\n", step,
      log[4 * i], log[4 * i + 1], log[4 * i + 2], log[4 * i + 3],
      log[4 * i + 4], log[4 * i + 5], log[4 * i + 6], log[4 * i + 7],
      log[4 * i + 8], log[4 * i + 9], log[4 * i + 10], log[4 * i + 11],
      log[4 * i + 12], log[4 * i + 13], log[4 * i + 14], log[4 * i + 15]);
 }
 for (uint32_t i = 0; i < 24; i += 8) {
  printf("adreno: %u: the GMU's state +%#x: %x %x %x %x %x %x %x %x\n", step, 4 * i,
      gmu_data(GMU_STATE_ADDRESS + 4 * i), gmu_data(GMU_STATE_ADDRESS + 4 * i + 4),
      gmu_data(GMU_STATE_ADDRESS + 4 * i + 8), gmu_data(GMU_STATE_ADDRESS + 4 * i + 12),
      gmu_data(GMU_STATE_ADDRESS + 4 * i + 16), gmu_data(GMU_STATE_ADDRESS + 4 * i + 20),
      gmu_data(GMU_STATE_ADDRESS + 4 * i + 24), gmu_data(GMU_STATE_ADDRESS + 4 * i + 28));
 }
 rscc_show();
 get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
 get(&gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
 get(&gpucc, "gx_domain_misc", GPUCC_GX_DOMAIN_MISC);
 get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
 get(&gpucc, "pll0 multiplier", GPUCC_PLL0_MODE + 4);
 get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
 get(&gpucc, "pll1 multiplier", GPUCC_PLL1_MODE + 4);
 get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
 get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
 get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
 get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
}
static bool
hfi_send(const char *what, uint32_t id, uint32_t *message, uint32_t words)
{
 volatile struct hfi_queue *queue = &hfi->queue[0];
 volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(0));
 uint32_t at = queue->write % HFI_QUEUE_WORDS, number = ++hfi_number & 0xfff, verdict = 0, waiting = 0;
 bool answered = false;
 message[0] = id | words << 8 | number << 20;
 printf("adreno: %u: about to send the GMU %s: message %u, %u words, number %u\n", step, what, id, words, number);
 snprintf(last_write, sizeof(last_write), "step %u sent the GMU message %u (%s)", step, id, what);
 hold(true);
 for (uint32_t i = 0; i < (words + 3) / 4 * 4; i++) {
  data[at] = i < words ? message[i] : 0xfafafafa;
  at = (at + 1) % HFI_QUEUE_WORDS;
 }
 queue->write = at;
 settle();
 wr(gmu.base + GMU_HOST2GMU_INTR_SET, 1);
 for (int i = 0; i < 10000 && !answered; i++) {
  sleep_us(100);
  waiting = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
  if (waiting & 1) {
   wr(gmu.base + GMU_GMU2HOST_INTR_CLR, 1);
   answered = hfi_receive(1, number, &verdict);
  }
  if (rd(gmu.base + GMU_CM3_FW_INIT_RESULT) & GMU_FAILED) {
   break;
  }
 }
 hfi_receive(2, ~0u, &waiting);
 if (!answered || verdict != 0) {
  printf("adreno: %u: the GMU %s (%#x) to %s; fw_init_result %#x, gmu2host_intr_info %#x\n", step,
      answered ? "says no" : "does not answer", verdict, what, rd(gmu.base + GMU_CM3_FW_INIT_RESULT),
      rd(gmu.base + GMU_GMU2HOST_INTR_INFO));
  space_faulted(&gmu_space);
  if (!post_mortem_done) {
   post_mortem_done = true;
   gmu_post_mortem();
  }
  return false;
 }
 printf("adreno: %u: the GMU accepts %s\n", step, what);
 return true;
}
static bool
rail_vote(uint32_t level, const uint16_t *rail, unsigned rail_levels, const uint16_t *mx, unsigned mx_levels, uint32_t *vote)
{
 unsigned at, with = 0;
 for (at = 0; at < rail_levels && rail[at] < level; at++) {
 }
 if (at == rail_levels || mx_levels == 0) {
  return false;
 }
 while (with + 1 < mx_levels && mx[with] < rail[at]) {
  with++;
 }
 *vote = (uint32_t)rail[at] << 16 | with << 8 | at;
 return true;
}
static struct block gpu = { "gpu", 0x03d00000, 0x40000 };
#define GPU_WORD(n) (4u * (n))
#define RBBM_INT_0_STATUS GPU_WORD(0x0201)
#define RBBM_STATUS GPU_WORD(0x0210)
#define RBBM_CLOCK_CNTL GPU_WORD(0x00ae)
#define CP_RB_BASE GPU_WORD(0x0800)
#define GMU_CORE_REQUEST (1u << 30)
#define GMU_CORE_GRANTED (1u << 31)
static uint32_t core_khz;
static bool
step_core_power(void)
{
 static const struct {
  uint32_t khz, level;
 } speeds[] = { { 0, 0 }, { 315000, 64 }, { 450000, 128 }, { 550000, 192 } };
 static struct hfi_perf_table perf;
 static struct hfi_bw_table bw;
 uint32_t core_start[2] = { 0, 0 }, start[1] = { 0 }, vote[4] = { 0, 1, 3, HFI_NO_INDEX }, granted = 0, speed = 0;
 uint16_t gfx[16], cx[16], mx[16];
 unsigned gfx_levels, cx_levels, mx_levels;
 if (!cx_ready() || !gmu_talks) {
  return false;
 }
 if (!rpmh_resource("gfx.lvl", gfx, &gfx_levels) || !rpmh_resource("cx.lvl", cx, &cx_levels) ||
     !rpmh_resource("mx.lvl", mx, &mx_levels)) {
  printf("adreno: %u: RPMh's list lacks one of the rails\n", step);
  return false;
 }
 perf.gpu_levels = sizeof(speeds) / sizeof(speeds[0]);
 for (unsigned i = 0; i < perf.gpu_levels; i++) {
  perf.gpu[i].khz = speeds[i].khz;
  perf.gpu[i].acd = 0xffffffff;
  if (!rail_vote(speeds[i].level, gfx, gfx_levels, mx, mx_levels, &perf.gpu[i].vote)) {
   printf("adreno: %u: RPMh has no level %u for the 3D core's rail\n", step, speeds[i].level);
   return false;
  }
 }
 perf.gmu_levels = 2;
 perf.gmu[1].khz = 200000;
 if (!rail_vote(0, cx, cx_levels, mx, mx_levels, &perf.gmu[0].vote) ||
     !rail_vote(48, cx, cx_levels, mx, mx_levels, &perf.gmu[1].vote)) {
  return false;
 }
 printf("adreno: %u: votes: 3D core %#x %#x %#x %#x, GMU %#x %#x\n", step, perf.gpu[0].vote, perf.gpu[1].vote,
     perf.gpu[2].vote, perf.gpu[3].vote, perf.gmu[0].vote, perf.gmu[1].vote);
 bw.levels = 1;
 bw.ddr_commands = 3;
 bw.ddr_wait = 7;
 bw.ddr_address[0] = rpmh_resource("SH0", NULL, NULL);
 bw.ddr_address[1] = rpmh_resource("MC0", NULL, NULL);
 bw.ddr_address[2] = rpmh_resource("ACV", NULL, NULL);
 bw.ddr_data[0][0] = bw.ddr_data[0][1] = bw.ddr_data[0][2] = 0x40000000;
 bw.cnoc_commands = 1;
 bw.cnoc_wait = 1;
 bw.cnoc_address[0] = rpmh_resource("CN0", NULL, NULL);
 bw.cnoc_data[0][0] = 0x40000000;
 bw.cnoc_data[1][0] = 0x60000001;
 printf("adreno: %u: RPMh's bus resources: SH0 %#x, MC0 %#x, ACV %#x, CN0 %#x (Linux has 0x50004, 0x50000, 0x50088, "
     "0x5006c)\n", step, bw.ddr_address[0], bw.ddr_address[1], bw.ddr_address[2], bw.cnoc_address[0]);
 if (bw.ddr_address[0] == 0 || bw.ddr_address[1] == 0 || bw.ddr_address[2] == 0 || bw.cnoc_address[0] == 0) {
  return false;
 }
 if (PE_parse_boot_argn("adreno_speed", &speed, sizeof(speed)) && speed >= 1 && speed < perf.gpu_levels) {
  vote[2] = speed;
 }
 core_khz = speeds[vote[2]].khz;
 printf("adreno: %u: the speed wanted: %u MHz\n", step, core_khz / 1000);
 announce("send the GMU its tables (speeds and supply levels, bus votes), \"start\", and the speed wanted; "
     "from here on it casts votes in RPMh by itself");
 if (!hfi_send("the table of speeds", HFI_PERF_TABLE, &perf.header, sizeof(perf) / 4) ||
     !hfi_send("the table of bus votes", HFI_BW_TABLE, &bw.header, sizeof(bw) / 4) ||
     !hfi_send("\"start the core firmware\"", HFI_CORE_FW_START, core_start, 2) ||
     !hfi_send("the speed wanted", HFI_GX_BW_PERF_VOTE, vote, 4)) {
  return false;
 }
 announce("ask the GMU to power the 3D core and keep it powered");
 get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
 get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
 put(&gmu, "host2gmu_intr_set", GMU_HOST2GMU_INTR_SET, GMU_CORE_REQUEST);
 for (int i = 0; i < 5000 && !(granted & GMU_CORE_GRANTED); i++) {
  sleep_us(100);
  granted = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
 }
 core_is_on = true;
 put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, GMU_CORE_GRANTED);
 get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
 get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
 get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
 if (!(granted & GMU_CORE_GRANTED) || (rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS) & 0xc0) != 0) {
  printf("adreno: %u: the GMU does not power the 3D core (gmu2host_intr_info %#x, fw_init_result %#x)\n", step,
      granted, rd(gmu.base + GMU_CM3_FW_INIT_RESULT));
  space_faulted(&gmu_space);
  return false;
 }
 if (!hfi_send("\"start\"", HFI_START, start, 1)) {
  return false;
 }
 printf("adreno: %u: measured: the 3D core's clock runs at %u Hz (%u MHz was asked for), the GMU's in it at %u Hz\n", step,
     measure_quietly(GPUCC_DEBUG_GX_GFX3D), core_khz / 1000, measure_quietly(GPUCC_DEBUG_GX_GMU));
 announce("read registers of the 3D core itself (0x3d00000): the first time it has power");
 get(&gpu, "rbbm_status", RBBM_STATUS);
 get(&gpu, "rbbm_int_0_status", RBBM_INT_0_STATUS);
 get(&gpu, "rbbm_clock_cntl", RBBM_CLOCK_CNTL);
 get(&gpu, "cp_rb_base", CP_RB_BASE);
 printf("adreno: %u: THE 3D CORE HAS POWER AND ANSWERS\n", step);
 return !space_faulted(&gmu_space);
}
#define CORE_STREAM 0
#define CORE_MATCH 0
#define PTE_CORE (PTE_GMU | PTE_UNPRIVILEGED)
#define PTE_READ_ONLY (1ull << 7)
#define CORE_RING_ADDRESS 0x01000000u
#define CORE_RING_SIZE 0x8000u
#define CORE_SQE_ADDRESS 0x01100000u
#define CORE_SCRATCH_ADDRESS 0x01200000u
#define CORE_TARGET_ADDRESS 0x10000000u
#define CORE_NOWHERE 0x7f000000u
#define GBIF_QSB_SIDE0 (4 * 0x03)
#define RBBM_GBIF_CLIENT_QOS_CNTL GPU_WORD(0x0011)
#define RBBM_GBIF_HALT GPU_WORD(0x0016)
#define RBBM_GBIF_HALT_ACK GPU_WORD(0x0017)
#define RBBM_INTERFACE_HANG_INT_CNTL GPU_WORD(0x001f)
#define RBBM_PERFCTR_CNTL GPU_WORD(0x0500)
#define RBBM_PERFCTR_GPU_BUSY_MASKED GPU_WORD(0x050b)
#define CP_RB_CNTL GPU_WORD(0x0802)
#define CP_RB_RPTR GPU_WORD(0x0806)
#define CP_RB_WPTR GPU_WORD(0x0807)
#define CP_SQE_CNTL GPU_WORD(0x0808)
#define CP_HW_FAULT GPU_WORD(0x0821)
#define CP_INTERRUPT_STATUS GPU_WORD(0x0823)
#define CP_PROTECT_STATUS GPU_WORD(0x0824)
#define CP_SQE_INSTR_BASE GPU_WORD(0x0830)
#define CP_CHICKEN_DBG GPU_WORD(0x0841)
#define CP_ADDR_MODE_CNTL GPU_WORD(0x0842)
#define CP_APRIV_CNTL GPU_WORD(0x0844)
#define CP_ROQ_THRESHOLDS_1 GPU_WORD(0x08c1)
#define CP_ROQ_THRESHOLDS_2 GPU_WORD(0x08c2)
#define CP_MEM_POOL_SIZE GPU_WORD(0x08c3)
#define CP_AHB_CNTL GPU_WORD(0x098d)
#define CP_LPAC_PROG_FIFO_SIZE GPU_WORD(0x0b34)
#define UCHE_WRITE_RANGE_MAX GPU_WORD(0x0e05)
#define UCHE_WRITE_THRU_BASE GPU_WORD(0x0e07)
#define UCHE_TRAP_BASE GPU_WORD(0x0e09)
#define UCHE_CACHE_WAYS GPU_WORD(0x0e17)
#define UCHE_FILTER_CNTL GPU_WORD(0x0e18)
#define UCHE_CLIENT_PF GPU_WORD(0x0e19)
#define RB_DBG_ECO_CNTL GPU_WORD(0x8e04)
#define PC_DBG_ECO_CNTL GPU_WORD(0x9e00)
#define RBBM_SECVID_TSB_TRUSTED_BASE GPU_WORD(0xf800)
#define RBBM_SECVID_TSB_TRUSTED_SIZE GPU_WORD(0xf802)
#define RBBM_SECVID_TSB_CNTL GPU_WORD(0xf803)
#define RBBM_SECVID_TSB_ADDR_MODE_CNTL GPU_WORD(0xf810)
#define CP_RB_CNTL_VALUE (12u | 2u << 8 | 1u << 27)
#define CP_NOP 0x10
#define CP_WAIT_FOR_IDLE 0x26
#define CP_BLIT 0x2c
#define CP_MEM_WRITE 0x3d
#define CP_EVENT_WRITE 0x46
#define CP_ME_INIT 0x48
#define CP_SET_MARKER 0x65
#define CP_SET_SECURE_MODE 0x66
#define SMMU_SCTLR_SERVE_AFTER_FAULT (1u << 8)
static struct gpu_space core_space = { .name = "the 3D core", .context = 0, .control = SMMU_SCTLR_SERVE_AFTER_FAULT };
static struct gpu_space second_space = {
 .name = "the 3D core's second stream", .context = 2, .control = SMMU_SCTLR_SERVE_AFTER_FAULT,
};
#define SECOND_MATCH 2
static bool second_space_is_on, second_stream_is_on;
static struct gpu_block core_ring, core_sqe, core_scratch, core_target;
static bool core_space_is_on, core_runs, zap_is_loaded;
#define RING_WORDS (CORE_RING_SIZE / 4)
static uint32_t ring_at;
static uint32_t ring_sent;
static uint32_t
odd_parity(uint32_t v)
{
 return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}
static void
ring(uint32_t word)
{
 ((uint32_t *)core_ring.memory)[ring_at] = word;
 ring_at = (ring_at + 1) % RING_WORDS;
}
static void
ring_command(uint32_t command, uint32_t words)
{
 ring(7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23);
}
static void
ring_register(uint32_t number, uint32_t value)
{
 ring(4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27);
 ring(value);
}
static void
ring_send(void)
{
 if (ring_at >= ring_sent) {
  to_memory(core_ring.memory + 4 * ring_sent, 4 * (ring_at - ring_sent));
 } else {
  to_memory(core_ring.memory + 4 * ring_sent, 4 * (RING_WORDS - ring_sent));
  to_memory(core_ring.memory, 4 * ring_at);
 }
 ring_sent = ring_at;
 wr(gpu.base + CP_RB_WPTR, ring_at);
}
static bool
ring_run(const char *what)
{
 uint32_t taken = ~0u, status = ~0u;
 printf("adreno: %u: about to have the 3D core %s: the ring up to word %u\n", step, what, ring_at);
 snprintf(last_write, sizeof(last_write), "step %u had the 3D core %s", step, what);
 hold(true);
 ring_send();
 for (int i = 0; i < 20000 && (taken != ring_at || (status & ~1u)); i++) {
  sleep_us(100);
  taken = rd(gpu.base + CP_RB_RPTR);
  status = rd(gpu.base + RBBM_STATUS);
 }
 if (taken == ring_at && !(status & ~1u) && rd(gpu.base + CP_HW_FAULT) == 0 && !space_faulted(&core_space)) {
  printf("adreno: %u: the 3D core has done it (ring read up to %u, status %#x)\n", step, taken, status);
  return true;
 }
 printf("adreno: %u: the 3D core does not get through: ring read up to %u of %u, status %#x, interrupts %#x, "
     "command processor: fault %#x, interrupts %#x, protection %#x\n", step, taken, ring_at, status,
     rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS),
     rd(gpu.base + CP_PROTECT_STATUS));
 space_faulted(&core_space);
 return false;
}
static void
put64(struct block *b, const char *name, uint32_t offset, uint64_t value)
{
 put(b, name, offset, (uint32_t)value);
 put(b, name, offset + 4, (uint32_t)(value >> 32));
}
static bool
step_core_space(void)
{
 const uint8_t *microcode;
 uint64_t size = 0;
 uint32_t stream1 = 1;
 if (!cx_ready() || !core_is_on) {
  return false;
 }
 PE_parse_boot_argn("adreno_stream1", &stream1, sizeof(stream1));
 announce("make the 3D core's memory: its ring, its microcode (fw/a660_sqe.fw), a page to try writing to");
 microcode = image_file("fw", "a660_sqe.fw", &size);
 if (microcode == NULL || size < 8 || size > 0x40000 ||
     !block_new(&core_space, &core_ring, CORE_RING_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
     !block_new(&core_space, &core_sqe, CORE_SQE_ADDRESS, (uint32_t)size - 4, PTE_CORE | PTE_READ_ONLY) ||
     !block_new(&core_space, &core_scratch, CORE_SCRATCH_ADDRESS, 4096, PTE_CORE)) {
  return false;
 }
 memcpy((void *)core_sqe.memory, microcode + 4, size - 4);
 to_memory(core_sqe.memory, core_sqe.size);
 announce("give the SMMU's context 0 these tables and send the 3D core's requests (stream 0) through it");
 core_space_is_on = true;
 if (!space_on(&core_space, CORE_MATCH, CORE_STREAM)) {
  return false;
 }
 if (stream1 == 2) {
  if ((second_space.root = space_new_table(&second_space)) == NULL) {
   return false;
  }
  second_space_is_on = true;
  if (!space_on(&second_space, SECOND_MATCH, 1)) {
   return false;
  }
 } else if (stream1 == 1) {
  second_stream_is_on = true;
  if (!space_stream(&core_space, SECOND_MATCH, 1)) {
   return false;
  }
 }
 get(&gpu, "rbbm_status", RBBM_STATUS);
 return !space_faulted(&core_space);
}
#define ZAP_MEMORY 0x8b71a000ull
#define ZAP_MEMORY_SIZE 0x2000u
#define SCM_PAS_MEM_SETUP SCM_CALL(2, 2)
#define SCM_THREE_VALUES 3
static bool
zap_unload(void)
{
 uint64_t answer = ~0ull;
 return scm("to give up the zap shader", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_GPU, 0, 0, &answer) == 0 &&
        answer == 0;
}
static bool
step_zap_load(void)
{
 const uint8_t *segment;
 volatile uint8_t *memory;
 uint64_t size = 0, header = 0, answer = ~0ull;
 if (!cx_ready() || !core_is_on) {
  return false;
 }
 announce("ask TrustZone to give up a zap shader that a kernel before this one may have left");
 printf("adreno: %u: %s\n", step, zap_unload() ? "TrustZone has given up what it had (it says so with none, too)" :
     "TRUSTZONE DOES NOT GIVE IT UP: the next answers tell whether it keeps one");
 segment = image_file("fw", "a660_zap.b02", &size);
 if (segment == NULL || size == 0 || size > 0x1000) {
  return false;
 }
 announce("hand the zap shader's header to TrustZone, tell it where the image goes, put the image there, and ask "
     "TrustZone to authenticate and run it");
 if (!zap_header(&header)) {
  return false;
 }
 if (scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
     header, 0, &answer) != 0 || answer != 0) {
  printf("adreno: %u: TrustZone does not take the signature (%#llx): TAKEN TO MEAN THAT IT KEEPS A ZAP SHADER FROM "
      "BEFORE, which is used as it is\n", step, answer);
  zap_is_loaded = true;
  return true;
 }
 if (scm("where the image will be", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_GPU, ZAP_MEMORY, 0x1000,
     &answer) != 0 || answer != 0) {
  printf("adreno: %u: TrustZone does not take the place (%#llx): TAKEN TO MEAN THAT IT KEEPS A ZAP SHADER FROM "
      "BEFORE, which is used as it is\n", step, answer);
  zap_is_loaded = true;
  return true;
 }
 memory = (volatile uint8_t *)ml_io_map_wcomb(ZAP_MEMORY, ZAP_MEMORY_SIZE);
 printf("adreno: %u: the reserved memory begins %02x %02x %02x %02x, the zap shader %02x %02x %02x %02x\n", step,
     memory[0], memory[1], memory[2], memory[3], segment[0], segment[1], segment[2], segment[3]);
 printf("adreno: %u: about to write %llu bytes at %#llx\n", step, size, ZAP_MEMORY);
 snprintf(last_write, sizeof(last_write), "step %u wrote the zap shader into memory at %#llx", step, ZAP_MEMORY);
 hold(true);
 for (uint64_t i = 0; i < 0x1000; i++) {
  memory[i] = i < size ? segment[i] : 0;
 }
 settle();
 if (scm("to authenticate the zap shader and run it", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_GPU, 0,
     0, &answer) != 0 || answer != 0) {
  printf("adreno: %u: TrustZone does not authenticate the zap shader (%#llx)\n", step, answer);
  return false;
 }
 printf("adreno: %u: TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER\n", step);
 zap_is_loaded = true;
 return true;
}
static bool
step_core_start(void)
{
 static const uint32_t address_modes[] = { 0x0842, 0x0c01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0x0e00, 0xae01,
                                    0xb601, 0xf810 };
 uint32_t blind = 0;
 if (!cx_ready() || !core_is_on || !core_space_is_on) {
  return false;
 }
 announce("write the 3D core's registers as Linux does before it starts the command processor: the first writes "
     "to the core itself");
 put(&gbif, "halt", GBIF_HALT, 0);
 put(&gpu, "rbbm_gbif_halt", RBBM_GBIF_HALT, 0);
 put(&gpu, "rbbm_secvid_tsb_cntl", RBBM_SECVID_TSB_CNTL, 0);
 put64(&gpu, "rbbm_secvid_tsb_trusted_base", RBBM_SECVID_TSB_TRUSTED_BASE, 0);
 put(&gpu, "rbbm_secvid_tsb_trusted_size", RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
 for (unsigned i = 0; i < sizeof(address_modes) / sizeof(address_modes[0]); i++) {
  put(&gpu, "address mode: 64 bits", GPU_WORD(address_modes[i]), 1);
 }
 for (unsigned i = 0; i < 4; i++) {
  put(&gbif, "qsb_side", GBIF_QSB_SIDE0 + 4 * i, 0x00071620);
 }
 put(&gpu, "rbbm_gbif_client_qos_cntl", RBBM_GBIF_CLIENT_QOS_CNTL, 3);
 put(&gpu, "rbbm_perfctr_gpu_busy_masked", RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
 put64(&gpu, "uche_write_range_max", UCHE_WRITE_RANGE_MAX, 0x0001ffffffffffc0ull);
 put64(&gpu, "uche_trap_base", UCHE_TRAP_BASE, 0x0001fffffffff000ull);
 put64(&gpu, "uche_write_thru_base", UCHE_WRITE_THRU_BASE, 0x0001fffffffff000ull);
 put(&gpu, "uche_filter_cntl", UCHE_FILTER_CNTL, 0x804);
 put(&gpu, "uche_cache_ways", UCHE_CACHE_WAYS, 0x4);
 put(&gpu, "cp_roq_thresholds_2", CP_ROQ_THRESHOLDS_2, 0x02000140);
 put(&gpu, "cp_roq_thresholds_1", CP_ROQ_THRESHOLDS_1, 0x8040362c);
 put(&gpu, "cp_lpac_prog_fifo_size", CP_LPAC_PROG_FIFO_SIZE, 0x20);
 put(&gpu, "cp_mem_pool_size", CP_MEM_POOL_SIZE, 128);
 put(&gpu, "pc_dbg_eco_cntl", PC_DBG_ECO_CNTL, 0x00200200);
 put(&gpu, "cp_ahb_cntl", CP_AHB_CNTL, 1);
 put(&gpu, "rb_nc_mode_cntl", GPU_WORD(0x8e08), 1u << 11 | 1u << 4 | 1u << 1);
 put(&gpu, "tpl1_nc_mode_cntl", GPU_WORD(0xb604), 1u << 1);
 put(&gpu, "sp_nc_mode_cntl", GPU_WORD(0xae02), 2u << 4 | 1u << 1);
 put(&gpu, "uche_mode_cntl", GPU_WORD(0x0e01), 1u << 21);
 put(&gpu, "rbbm_nc_mode_cntl", GPU_WORD(0x0534), 1);
 put(&gpu, "rbbm_perfctr_cntl", RBBM_PERFCTR_CNTL, 1);
 put(&gpu, "rbbm_interface_hang_int_cntl", RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0x1fffff);
 put(&gpu, "uche_client_pf", UCHE_CLIENT_PF, 1u << 7 | 1);
 put(&gpu, "cp_chicken_dbg", CP_CHICKEN_DBG, 1);
 put(&gpu, "rbbm_gbif_client_qos_cntl", RBBM_GBIF_CLIENT_QOS_CNTL, 0);
 put(&gpu, "cp_apriv_cntl", CP_APRIV_CNTL, 1u << 6 | 1u << 5 | 1u << 3 | 1u << 2 | 1u << 1);
 put(&gpu, "rb_dbg_eco_cntl", RB_DBG_ECO_CNTL, 0x04100000);
 PE_parse_boot_argn("adreno_blind", &blind, sizeof(blind));
 put64(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE, blind ? CORE_NOWHERE : CORE_SQE_ADDRESS);
 put64(&gpu, "cp_rb_base", CP_RB_BASE, CORE_RING_ADDRESS);
 put(&gpu, "cp_rb_cntl", CP_RB_CNTL, CP_RB_CNTL_VALUE);
 get(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE);
 get(&gpu, "cp_rb_base", CP_RB_BASE);
 get(&gpu, "cp_rb_cntl", CP_RB_CNTL);
 get(&gpu, "cp_apriv_cntl", CP_APRIV_CNTL);
 get(&gpu, "rbbm_status", RBBM_STATUS);
 announce(blind ? "start the 3D core's command processor with its microcode nowhere" :
     "start the 3D core's command processor");
 core_runs = true;
 put(&gpu, "cp_sqe_cntl", CP_SQE_CNTL, 1);
 sleep_ms(20);
 printf("adreno: %u: alive after it; status %#x, interrupts %#x, command processor: fault %#x, interrupts %#x, "
     "protection %#x, ring read up to %u\n", step, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
     rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS),
     rd(gpu.base + CP_RB_RPTR));
 get(&smmu, "sgfsr", SMMU_SGFSR);
 get(&smmu, "sgfsynr0", SMMU_SGFSR + 8);
 get(&smmu, "sgfsynr1", SMMU_SGFSR + 12);
 if (blind) {
  return space_faulted(&core_space);
 }
 if (second_space_is_on) {
  sleep_ms(200);
  return space_faulted(&second_space) | !space_faulted(&core_space);
 }
 if (space_faulted(&core_space)) {
  return false;
 }
 announce("give the 3D core its first command, \"initialize\"");
 ring_command(CP_ME_INIT, 8);
 ring(0x0000002f);
 ring(0x00000003);
 ring(0x20000000);
 ring(0);
 ring(0);
 ring(0);
 ring(0);
 ring(0);
 if (!ring_run("initialize its command processor")) {
  return false;
 }
 printf("adreno: %u: THE 3D CORE TAKES COMMANDS\n", step);
 return true;
}
static bool
step_core_commands(void)
{
 volatile uint32_t *scratch = (volatile uint32_t *)core_scratch.memory;
 if (!cx_ready() || !core_runs) {
  return false;
 }
 announce("command the 3D core out of secure mode (it asks the zap shader), then to write a word into a page of ours");
 ring_command(CP_SET_SECURE_MODE, 1);
 ring(0);
 if (!ring_run("leave secure mode")) {
  return false;
 }
 ring_command(CP_MEM_WRITE, 3);
 ring(CORE_SCRATCH_ADDRESS);
 ring(0);
 ring(0x600dc0de);
 if (!ring_run("write a word to memory")) {
  return false;
 }
 to_memory(core_scratch.memory, 64);
 printf("adreno: %u: the page reads %#x: %s\n", step, scratch[0], scratch[0] == 0x600dc0de ?
     "THE 3D CORE WRITES OUR MEMORY ON COMMAND" : "not what it was told to write");
 return scratch[0] == 0x600dc0de;
}
#define RB_CCU_CNTL 0x8e07
#define GRAS_2D_BLIT_CNTL 0x8400
#define GRAS_2D_DST_TL 0x8405
#define GRAS_2D_DST_BR 0x8406
#define RB_2D_BLIT_CNTL 0x8c00
#define RB_2D_PIXEL_CNTL 0x8c01
#define RB_2D_DST_INFO 0x8c17
#define RB_2D_DST 0x8c18
#define RB_2D_DST_PITCH 0x8c1a
#define RB_2D_SRC_SOLID_C0 0x8c2c
#define SP_2D_DST_FORMAT 0xacc0
#define FORMAT_8888 0x30
#define BLIT_SOLID_8888 (1u << 7 | FORMAT_8888 << 8 | 0xfu << 20 | 0x10u << 24)
#define DST_BGRA_LINEAR (FORMAT_8888 | 1u << 10)
#define EVENT_FLUSH_COLOR 0x1d
#define EVENT_CACHE_FLUSH 0x04
#define TARGET_WIDTH 1080
#define TARGET_HEIGHT 2400
#define TARGET_ROW 4352
extern void MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes);
static void
ring_fill(uint32_t top, uint32_t bottom, uint32_t red, uint32_t green, uint32_t blue)
{
 ring_register(GRAS_2D_DST_TL, top << 16);
 ring_register(GRAS_2D_DST_BR, (TARGET_WIDTH - 1) | (bottom - 1) << 16);
 ring_register(RB_2D_SRC_SOLID_C0, red);
 ring_register(RB_2D_SRC_SOLID_C0 + 1, green);
 ring_register(RB_2D_SRC_SOLID_C0 + 2, blue);
 ring_register(RB_2D_SRC_SOLID_C0 + 3, 255);
 ring_register(RB_2D_BLIT_CNTL, BLIT_SOLID_8888);
 ring_register(GRAS_2D_BLIT_CNTL, BLIT_SOLID_8888);
 ring_register(SP_2D_DST_FORMAT, 1 | FORMAT_8888 << 3 | 0xfu << 12);
 ring_register(RB_2D_PIXEL_CNTL, 0);
 ring_register(RB_2D_DST_INFO, DST_BGRA_LINEAR);
 ring_register(RB_2D_DST, CORE_TARGET_ADDRESS);
 ring_register(RB_2D_DST + 1, 0);
 ring_register(RB_2D_DST_PITCH, TARGET_ROW >> 6);
 ring_command(CP_WAIT_FOR_IDLE, 0);
 ring_command(CP_BLIT, 1);
 ring(3);
}
static bool
step_frame(void)
{
 const volatile uint8_t *pixel;
 if (!cx_ready() || !core_runs) {
  return false;
 }
 announce("make a picture's worth of memory for the 3D core (10 MB) and have it fill three bands of colour");
 if (!block_new(&core_space, &core_target, CORE_TARGET_ADDRESS, TARGET_ROW * TARGET_HEIGHT, PTE_CORE)) {
  return false;
 }
 for (uint32_t i = 0; i < core_space.table_count; i++) {
  to_memory((vm_offset_t)core_space.tables[i].table, 4096);
 }
 ring_command(CP_SET_MARKER, 1);
 ring(12);
 ring_register(RB_CCU_CNTL, 0x10000004);
 ring_fill(0, 800, 255, 128, 0);
 ring_fill(800, 1600, 255, 255, 255);
 ring_fill(1600, TARGET_HEIGHT, 0, 128, 255);
 ring_command(CP_EVENT_WRITE, 4);
 ring(EVENT_FLUSH_COLOR);
 ring(CORE_SCRATCH_ADDRESS + 32);
 ring(0);
 ring(0x0c0c0c0c);
 ring_command(CP_EVENT_WRITE, 4);
 ring(EVENT_CACHE_FLUSH);
 ring(CORE_SCRATCH_ADDRESS + 16);
 ring(0);
 ring(0x0d0e0d0e);
 ring_command(CP_WAIT_FOR_IDLE, 0);
 if (!ring_run("fill a picture")) {
  return false;
 }
 to_memory(core_scratch.memory, 64);
 printf("adreno: %u: the words it writes when its caches are in memory: %#x, %#x (told: 0xc0c0c0c, 0xd0e0d0e)\n", step,
     ((volatile uint32_t *)core_scratch.memory)[8], ((volatile uint32_t *)core_scratch.memory)[4]);
 to_memory(core_target.memory, core_target.size);
 pixel = (const volatile uint8_t *)core_target.memory;
 printf("adreno: %u: the picture's first pixel is %02x %02x %02x %02x (blue green red alpha; told: 00 80 ff ff), one in "
     "the middle %02x %02x %02x %02x, one in the last row %02x %02x %02x %02x\n", step, pixel[0], pixel[1], pixel[2],
     pixel[3], pixel[1200 * TARGET_ROW + 2160], pixel[1200 * TARGET_ROW + 2161], pixel[1200 * TARGET_ROW + 2162],
     pixel[1200 * TARGET_ROW + 2163], pixel[2399 * TARGET_ROW + 4316], pixel[2399 * TARGET_ROW + 4317],
     pixel[2399 * TARGET_ROW + 4318], pixel[2399 * TARGET_ROW + 4319]);
 if ((pixel[0] | pixel[1] | pixel[2]) == 0 || pixel[1200 * TARGET_ROW + 2160] == 0 || (pixel[2399 * TARGET_ROW + 4316] |
     pixel[2399 * TARGET_ROW + 4317] | pixel[2399 * TARGET_ROW + 4318]) == 0) {
  printf("adreno: %u: the picture is not what the 3D core was told to draw\n", step);
  return false;
 }
 MiniIOMobileFramebufferShow((const uint8_t *)core_target.memory, TARGET_WIDTH, TARGET_HEIGHT, TARGET_ROW);
 printf("adreno: %u: A PICTURE DRAWN BY THE GPU IS ON THE PANEL: orange, white, blue from the top\n", step);
 return true;
}
static void
gx_off(void)
{
 uint32_t gx = get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
 if (gx & GDSC_POWER_ON) {
  put(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR, gx | GDSC_SW_COLLAPSE);
  wait_for(&gpucc, GPUCC_GX_GDSCR, GDSC_POWER_ON, 0, 100);
  get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
 }
}
static void
stale_stop(void)
{
 need(&gmu);
 need(&rscc);
 put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
 gx_off();
 if (rd(rscc.base + RSCC_SEQ_MEM_0_DRV0) == 0xeaaae5a0 && !(rd(rscc.base + RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP)) {
  put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 1);
  wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, RSCC_ASLEEP, RSCC_ASLEEP, 100);
  get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
  put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
 }
 if (smmu_layout()) {
  for (uint32_t match = 0; match < 3; match++) {
   put(&smmu, "smr", SMMU_SMR(match), 0);
   put(&smmu, "s2cr", SMMU_S2CR(match), SMMU_S2CR_FAULT);
   put(&smmu, "cb sctlr", (smmu_pages + match) * smmu_page + SMMU_CB_SCTLR, 0);
  }
 }
 put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, rd(gpucc.base + GPUCC_CX_GDSCR) | GDSC_SW_COLLAPSE);
 for (int i = 0; i < 1000 && cx_is_on(); i++) {
  sleep_us(100);
 }
 get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
 get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
}
static void
core_bus_halt(bool for_good)
{
 if (for_good) {
  wr(gpu.base + RBBM_GBIF_HALT, 1);
  for (int i = 0; i < 100000 && !(rd(gpu.base + RBBM_GBIF_HALT_ACK) & 1); i++) {
  }
 }
 wr(gbif.base + GBIF_HALT, 1);
 for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 1); i++) {
 }
 wr(gbif.base + GBIF_HALT, 2);
 for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 2); i++) {
 }
 if (!for_good) {
  wr(gbif.base + GBIF_HALT, 0);
 }
}
static bool core_stuck;
static void
gpu_block_stop(void)
{
 uint32_t slumber[3] = { 0, 0, 0 };
 bool power_off = false;
 step = 0;
 newest_step = ~0u;
 if (core_runs && core_is_on) {
  printf("adreno: %u: the 3D core before it is stopped: status %#x, interrupts %#x, ring read up to %u of %u; the GMU "
      "sees it busy: %#x\n", step, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
      rd(gpu.base + CP_RB_RPTR), ring_at, rd(gmu.base + GMU_AO_GPU_CX_BUSY_STATUS));
  core_bus_halt(core_stuck);
  printf("adreno: %u: the GPU block's bus has been halted%s (rbbm_gbif_halt_ack %#x, gbif halt_ack %#x)\n", step,
      core_stuck ? ", the 3D core's side too, and stays so" : " and let go again", rd(gpu.base + RBBM_GBIF_HALT_ACK),
      rd(gbif.base + GBIF_HALT_ACK));
 }
 if (core_is_on) {
  put(&gmu, "host2gmu_intr_set", GMU_HOST2GMU_INTR_SET, 1u << 31);
  core_is_on = false;
 }
 if (gmu_talks) {
  hfi_send("\"slumber\"", HFI_PREPARE_SLUMBER, slumber, 3);
  if (!wait_for(&gmu, GMU_AO_GPU_CX_BUSY_STATUS, 1u << 23, 0, 100)) {
   get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
  }
  get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
  get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
  gmu_talks = false;
 }
 if (gmu_runs) {
  put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
  gmu_runs = false;
  gx_off();
 }
 if (zap_is_loaded) {
  printf("adreno: %u: the zap shader: %s\n", step, zap_unload() ? "TrustZone has given it up" :
      "TRUSTZONE DOES NOT GIVE IT UP; the next kernel will find it there");
  zap_is_loaded = false;
 }
 if (rsc_awake && rd(rscc.base + RSCC_SEQ_MEM_0_DRV0) == 0xeaaae5a0) {
  put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 1);
  if (!wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, RSCC_ASLEEP, RSCC_ASLEEP, 100)) {
   printf("adreno: %u: the GMU's RPMh channel does not go to sleep\n", step);
  }
  get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
  put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
 }
 rsc_awake = false;
 if (gmu_space_is_on) {
  space_faulted(&gmu_space);
  space_off(&gmu_space, GMU_MATCH);
  gmu_space_is_on = false;
  power_off = true;
 }
 if (second_space_is_on) {
  space_faulted(&second_space);
  space_off(&second_space, SECOND_MATCH);
  second_space_is_on = false;
 }
 if (second_stream_is_on) {
  put(&smmu, "smr", SMMU_SMR(SECOND_MATCH), 0);
  put(&smmu, "s2cr", SMMU_S2CR(SECOND_MATCH), SMMU_S2CR_FAULT);
  second_stream_is_on = false;
 }
 if (core_space_is_on) {
  space_faulted(&core_space);
  space_off(&core_space, CORE_MATCH);
  core_space_is_on = core_runs = false;
 }
 if (power_off && gpucc_answers()) {
  put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, rd(gpucc.base + GPUCC_CX_GDSCR) | GDSC_SW_COLLAPSE);
  for (int i = 0; i < 1000 && cx_is_on(); i++) {
   sleep_us(100);
  }
  get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
  get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
  cx_off = !cx_is_on();
 }
}
void
redwood_adreno_quiesce(void)
{
 if (macan_gpu_kept) {
  macan_adreno_stop_kept();
  return;
 }
 if (cx_off) {
  return;
 }
 if (gmu_runs) {
  wr(gmu.base + GMU_CM3_SYSRESET, 1);
  gmu_runs = false;
 }
 if (core_runs) {
  core_bus_halt(true);
  core_runs = false;
 }
 if (gmu_space_is_on) {
  wr(smmu.base + smmu_context(&gmu_space) + SMMU_CB_SCTLR, 0);
  gmu_space_is_on = false;
 }
 if (core_space_is_on) {
  wr(smmu.base + smmu_context(&core_space) + SMMU_CB_SCTLR, 0);
  core_space_is_on = false;
 }
 if (second_space_is_on) {
  wr(smmu.base + smmu_context(&second_space) + SMMU_CB_SCTLR, 0);
  second_space_is_on = false;
 }
}
#define GPU_CHIP_ID 0x06030500ull
#define GPU_ADDRESSES_START 0x20000000ull
#define GPU_ADDRESSES_SIZE 0xe0000000ull
#define GPU_FENCE_AT 0x40
#define GPU_TIMES_AT 0x400
#define GPU_TIMES 64
#define CP_ALWAYS_ON_COUNTER 0x0980
#define CP_REG_TO_MEM 0x3e
#define CP_SCRATCH_REG2 0x0885
#define CP_IB1_BASE GPU_WORD(0x0928)
#define CP_IB1_REM_SIZE GPU_WORD(0x092a)
#define CP_IB2_BASE GPU_WORD(0x092b)
#define CP_IB2_REM_SIZE GPU_WORD(0x092d)
#define CP_INDIRECT_BUFFER 0x3f
#define EVENT_INVALIDATE_DEPTH 0x18
#define EVENT_INVALIDATE_COLOR 0x19
static bool gpu_kept;
static uint32_t gpu_fence;
static uint32_t gpu_timed;
static uint64_t gpu_ticks;
static uint32_t gpu_timed_count;
static uint32_t gpu_timed_by[7];
static uint32_t gpu_exact = 1;
uint32_t gpu_log;
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
static struct {
 uint64_t start, size;
} gpu_free[1024];
static unsigned gpu_free_count;
extern void RedwoodGPUStart(void);
extern void RedwoodGPUStop(void);
static void gpu_block_stop(void);
void
redwood_gpu_stop(void)
{
 if (macan_gpu_kept) {
  macan_gpu_stop();
  return;
 }
 if (gpu_kept) {
  uint32_t waited = 0;
  gpu_kept = false;
  quiet = false;
  while (!redwood_gpu_done(gpu_fence) && waited < 5000) {
   sleep_us(100);
   waited++;
  }
  core_stuck = !redwood_gpu_done(gpu_fence);
  printf("adreno: the run is about to end: the GPU is given back (%s after %u us)\n",
      core_stuck ? "STILL AT WORK" : "it had finished what it was given", 100 * waited);
  gpu_block_stop();
 }
}
void
redwood_adreno_stop(void)
{
 uint32_t dirty = 0;
 if (macan_gpu_kept) {
  macan_adreno_stop_kept();
  return;
 }
 if (gpu_kept && !(PE_parse_boot_argn("adreno_dirty", &dirty, sizeof(dirty)) && dirty)) {
  RedwoodGPUStop();
 }
}
bool
redwood_gpu_ready(void)
{
 if (macan_gpu_kept) {
  return true;
 }
 return gpu_kept;
}
bool
redwood_gpu_parameter(uint32_t which, uint64_t *value)
{
 if (macan_gpu_kept) {
  return macan_gpu_parameter(which, value);
 }
 switch (which) {
 case 0x01:
 case 0x06:
 case 0x08:
 case 0x09:
 case 0x0a:
 case 0x11:
  *value = 0;
  return true;
 case 0x02:
  *value = 512 * 1024;
  return true;
 case 0x03:
  *value = GPU_CHIP_ID | (uint64_t)speed_bin << 32;
  return true;
 case 0x04:
  *value = core_khz * 1000ull;
  return true;
 case 0x05:
  *value = mach_absolute_time();
  return true;
 case 0x07:
  *value = 1;
  return true;
 case 0x10:
  *value = 14;
  return true;
 case 0x12:
  *value = 6;
  return true;
 case 0x13:
  *value = 1;
  return true;
 default:
  return false;
 }
}
uint64_t
redwood_gpu_address(uint64_t size)
{
 if (macan_gpu_kept) {
  return macan_gpu_address(size);
 }
 size = (size + 0xffff) & ~0xffffull;
 for (unsigned i = 0; i < gpu_free_count; i++) {
  if (gpu_free[i].size >= size) {
   uint64_t address = gpu_free[i].start;
   gpu_free[i].start += size;
   gpu_free[i].size -= size;
   if (gpu_free[i].size == 0) {
    memmove(&gpu_free[i], &gpu_free[i + 1], (--gpu_free_count - i) * sizeof(gpu_free[0]));
   }
   return address;
  }
 }
 return 0;
}
void
redwood_gpu_address_free(uint64_t address, uint64_t size)
{
 if (macan_gpu_kept) {
  macan_gpu_address_free(address, size);
  return;
 }
 unsigned at;
 size = (size + 0xffff) & ~0xffffull;
 for (at = 0; at < gpu_free_count && gpu_free[at].start < address; at++) {
 }
 if (at > 0 && gpu_free[at - 1].start + gpu_free[at - 1].size == address) {
  gpu_free[at - 1].size += size;
  if (at < gpu_free_count && address + size == gpu_free[at].start) {
   gpu_free[at - 1].size += gpu_free[at].size;
   memmove(&gpu_free[at], &gpu_free[at + 1], (--gpu_free_count - at) * sizeof(gpu_free[0]));
  }
 } else if (at < gpu_free_count && address + size == gpu_free[at].start) {
  gpu_free[at].start = address;
  gpu_free[at].size += size;
 } else if (gpu_free_count < sizeof(gpu_free) / sizeof(gpu_free[0])) {
  memmove(&gpu_free[at + 1], &gpu_free[at], (gpu_free_count++ - at) * sizeof(gpu_free[0]));
  gpu_free[at].start = address;
  gpu_free[at].size = size;
 }
}
bool
redwood_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
 if (macan_gpu_kept) {
  return macan_gpu_map(address, phys, read_only);
 }
 return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}
void
redwood_gpu_mapped(void)
{
 if (macan_gpu_kept) {
  macan_gpu_mapped();
  return;
 }
 space_commit(&core_space);
}
void
redwood_gpu_unmap(uint64_t address, uint64_t size)
{
 if (macan_gpu_kept) {
  macan_gpu_unmap(address, size);
  return;
 }
 uint32_t context = smmu_context(&core_space);
 for (uint64_t offset = 0; offset < size; offset += 4096) {
  space_map(&core_space, address + offset, 0, 0);
 }
 space_commit(&core_space);
 wr(smmu.base + context + SMMU_CB_TLBIASID, core_space.context);
 wr(smmu.base + context + SMMU_CB_TLBSYNC, 1);
 for (int i = 0; i < 1000 && (rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1); i++) {
 }
}
void
redwood_gpu_to_memory(void *start, uint64_t size)
{
 to_memory((vm_offset_t)start, size);
}
bool
redwood_gpu_done(uint32_t fence)
{
 if (macan_gpu_kept) {
  return macan_gpu_done(fence);
 }
 volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);
 to_memory((vm_offset_t)word, 4);
 return (int32_t)(*word - fence) >= 0;
}
uint32_t
redwood_gpu_last(void)
{
 if (macan_gpu_kept) {
  return macan_gpu_last();
 }
 return gpu_fence;
}
static void
ring_time(uint32_t fence, uint32_t end)
{
 ring_command(CP_REG_TO_MEM, 3);
 ring(CP_ALWAYS_ON_COUNTER | 2u << 18 | 1u << 30);
 ring(CORE_SCRATCH_ADDRESS + GPU_TIMES_AT + 16 * (fence % GPU_TIMES) + 8 * end);
 ring(0);
}
static void
gpu_times(void)
{
 volatile uint64_t *times = (volatile uint64_t *)(core_scratch.memory + GPU_TIMES_AT);
 uint32_t done;
 to_memory(core_scratch.memory, 4096);
 done = *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);
 if (done - gpu_timed > GPU_TIMES) {
  gpu_timed = done - GPU_TIMES;
 }
 while (gpu_timed != done) {
  uint32_t slot = ++gpu_timed % GPU_TIMES;
  if (gpu_timed != 0 && times[2 * slot + 1] > times[2 * slot]) {
   static const uint32_t under[6] = { 1920, 4800, 9600, 19200, 38400, 76800 };
   uint64_t ticks = times[2 * slot + 1] - times[2 * slot];
   unsigned by = 0;
   if (gpu_log) {
    redwood_tl("tl: q %u took %llu\n", gpu_timed, ticks * 10 / 192);
   }
   while (by < 6 && ticks >= under[by]) {
    by++;
   }
   gpu_timed_by[by]++;
   gpu_ticks += ticks;
   gpu_timed_count++;
  }
 }
}
void
redwood_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
 if (macan_gpu_kept) {
  macan_gpu_worked(microseconds, submissions, by);
  return;
 }
 gpu_times();
 *microseconds = gpu_ticks * 10 / 192;
 *submissions = gpu_timed_count;
 memcpy(by, gpu_timed_by, sizeof(gpu_timed_by));
 bzero(gpu_timed_by, sizeof(gpu_timed_by));
 gpu_ticks = 0;
 gpu_timed_count = 0;
}
uint32_t
redwood_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
 uint32_t wanted = 26 + 4 * count, room = 0;
 if (macan_gpu_kept) {
  return macan_gpu_submit(addresses, words, count);
 }
 for (int i = 0; i < 20000; i++) {
  room = (rd(gpu.base + CP_RB_RPTR) + RING_WORDS - ring_at - 1) % RING_WORDS;
  if (room >= wanted) {
   break;
  }
  sleep_us(100);
 }
 if (room < wanted) {
  return 0;
 }
 gpu_times();
 if (++gpu_fence == 0) {
  gpu_fence = 1;
 }
 ring_time(gpu_fence, 0);
 ring_command(CP_EVENT_WRITE, 1);
 ring(EVENT_INVALIDATE_DEPTH);
 ring_command(CP_EVENT_WRITE, 1);
 ring(EVENT_INVALIDATE_COLOR);
 for (uint32_t i = 0; i < count; i++) {
  ring_command(CP_INDIRECT_BUFFER, 3);
  ring((uint32_t)addresses[i]);
  ring((uint32_t)(addresses[i] >> 32));
  ring(words[i]);
 }
 if (gpu_exact) {
  ring_command(CP_WAIT_FOR_IDLE, 0);
 }
 ring_time(gpu_fence, 1);
 ring_register(CP_SCRATCH_REG2, gpu_fence);
 ring_command(CP_EVENT_WRITE, 4);
 ring(EVENT_CACHE_FLUSH);
 ring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);
 ring(0);
 ring(gpu_fence);
 ring_send();
 return gpu_fence;
}
void
redwood_gpu_report(const char *why)
{
 uint32_t context = smmu_context(&core_space);
 if (macan_gpu_kept) {
  macan_gpu_report(why);
  return;
 }
 printf("adreno: gpu: %s: submission %u is the last, the core has done %u; ring: read up to %u, written up to %u; status "
     "%#x, interrupts %#x; command processor: fault %#x, interrupts %#x, protection %#x\n", why, gpu_fence,
     *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT), rd(gpu.base + CP_RB_RPTR), ring_at,
     rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT),
     rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS));
 printf("adreno: gpu:   in the first command buffer at %#llx, %u words to go; in the second at %#llx, %u to go\n",
     (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE), rd(gpu.base + CP_IB1_REM_SIZE),
     (uint64_t)rd(gpu.base + CP_IB2_BASE + 4) << 32 | rd(gpu.base + CP_IB2_BASE), rd(gpu.base + CP_IB2_REM_SIZE));
 step = 0;
 if (space_faulted(&core_space)) {
  wr(smmu.base + context + SMMU_CB_FSR, SMMU_FSR_ALL);
 }
}
static bool(*const steps[])(void) = {
 step_outside, step_fuses, step_gpucc, step_cx_power, step_clocks, step_first_words, step_rscc, step_smmu,
 step_smmu_context, step_gmu_memory, step_trustzone, step_zap_signature, step_gmu_space, step_gmu_firmware,
 step_gmu_start, step_gmu_queues, step_core_power, step_core_space, step_zap_load, step_core_start, step_core_commands,
 step_frame,
};
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))
static struct block tsens = { "tsens1", 0x0c265000, 0x200 };
#define TSENS_STATUS(sensor) (0xa0 + 4 * (sensor))
#define TSENS_VALID (1u << 21)
void
redwood_adreno_minute(void)
{
 int32_t tenths[2];
 if (!gpu_kept) {
  return;
 }
 need(&tsens);
 for (unsigned i = 0; i < 2; i++) {
  tenths[i] = -2730;
  for (unsigned again = 0; again < 16 && tenths[i] == -2730; again++) {
   uint32_t status = rd(tsens.base + TSENS_STATUS(1 + i));
   if (status & TSENS_VALID) {
    tenths[i] = (int32_t)(status << 20) >> 20;
   }
  }
 }
 printf("adreno: the GPU's sensors read %d.%d and %d.%d degrees; its clock is set to %u MHz\n", tenths[0] / 10,
     tenths[0] % 10 < 0 ? -(tenths[0] % 10) : tenths[0] % 10, tenths[1] / 10,
     tenths[1] % 10 < 0 ? -(tenths[1] % 10) : tenths[1] % 10, core_khz / 1000);
}
static struct block dispcc = { "dispcc", 0x0af00000, 0x2000 };
static struct block disp_intf = { "intf1", 0x0ae36000, 0x300 };
#define GCC_DISP_AHB_CBCR 0x27004
#define DISPCC_MDSS_CORE_GDSCR 0x1004
#define DISPCC_MDSS_PCLK0_CBCR 0x1010
#define DISPCC_MDSS_MDP_CBCR 0x1014
#define DISPCC_MDSS_AHB_CBCR 0x1050
#define INTF_TIMING_ENGINE_EN 0x000
#define INTF_HSYNC_CTL 0x008
#define INTF_VSYNC_PERIOD 0x00c
#define INTF_DISPLAY_V_START 0x01c
#define INTF_DISPLAY_V_END 0x024
#define INTF_DISPLAY_HCTL 0x03c
#define INTF_FRAME_LINE_COUNT_EN 0x0a8
#define INTF_FRAME_COUNT 0x0ac
#define INTF_LINE_COUNT 0x0b0
static void
display_probe(void)
{
 uint32_t saved_fine = fine, line, frame, shown, counting, frames[2], lines[2], bus;
 uint64_t times[2];
 step = newest_step = 0;
 fine = 1;
 announce("the display: has its clock controller its bus clock (gcc 0x127004; GCC answers always)");
 if (get(&gcc, "disp_ahb_cbcr", GCC_DISP_AHB_CBCR) & CBCR_OFF) {
  announce("turn the bus clock of the display's clock controller on (gcc 0x127004 bit 0: THE ONE WRITE)");
  put(&gcc, "disp_ahb_cbcr", GCC_DISP_AHB_CBCR, rd(gcc.base + GCC_DISP_AHB_CBCR) | CBCR_ENABLE);
  sleep_ms(10);
 }
 if (!(get(&gcc, "disp_ahb_cbcr", GCC_DISP_AHB_CBCR) & CBCR_ENABLE)) {
  printf("adreno: 0: the display's clock controller has no bus clock, and does not get one: nothing more is read\n");
  goto out;
 }
 announce("the display block's power switch and clocks, in its clock controller (0xaf00000: NOT READ BEFORE)");
 get(&dispcc, "mdss_mdp_cbcr", DISPCC_MDSS_MDP_CBCR);
 get(&dispcc, "mdss_pclk0_cbcr", DISPCC_MDSS_PCLK0_CBCR);
 if (!(get(&dispcc, "mdss_core_gdscr", DISPCC_MDSS_CORE_GDSCR) & GDSC_POWER_ON)) {
  printf("adreno: 0: the display block is without power: nothing more is read\n");
  goto out;
 }
 bus = get(&dispcc, "mdss_ahb_cbcr", DISPCC_MDSS_AHB_CBCR);
 if (bus & CBCR_OFF) {
  announce("turn the bus clock of the display block's registers on (dispcc 0xaf01050 bit 0: THE ONE WRITE THERE)");
  put(&dispcc, "mdss_ahb_cbcr", DISPCC_MDSS_AHB_CBCR, bus | CBCR_ENABLE);
  sleep_ms(10);
  bus = get(&dispcc, "mdss_ahb_cbcr", DISPCC_MDSS_AHB_CBCR);
 }
 if (bus & CBCR_OFF) {
  printf("adreno: 0: the bus clock of the display block's registers does not run (%#x): nothing more is read\n", bus);
  goto out;
 }
 announce("the timing of the interface the panel hangs on (0xae36000: NOT READ BEFORE)");
 get(&disp_intf, "timing_engine_en", INTF_TIMING_ENGINE_EN);
 line = get(&disp_intf, "hsync_ctl", INTF_HSYNC_CTL) >> 16;
 frame = get(&disp_intf, "vsync_period", INTF_VSYNC_PERIOD);
 shown = get(&disp_intf, "display_v_end", INTF_DISPLAY_V_END) - get(&disp_intf, "display_v_start", INTF_DISPLAY_V_START);
 get(&disp_intf, "display_hctl", INTF_DISPLAY_HCTL);
 counting = get(&disp_intf, "frame_line_count_en", INTF_FRAME_LINE_COUNT_EN);
 frames[0] = get(&disp_intf, "frame_count", INTF_FRAME_COUNT);
 lines[0] = get(&disp_intf, "line_count", INTF_LINE_COUNT);
 quiet = true;
 for (int i = 0; i < 2; i++) {
  frames[i] = rd(disp_intf.base + INTF_FRAME_COUNT);
  lines[i] = rd(disp_intf.base + INTF_LINE_COUNT);
  absolutetime_to_nanoseconds(mach_absolute_time(), &times[i]);
  if (rd(disp_intf.base + INTF_FRAME_COUNT) != frames[i]) {
   i--;
   continue;
  }
  if (i == 0) {
   sleep_ms(1000);
  }
 }
 quiet = false;
 if (line != 0) {
  printf("adreno: 0: THE DISPLAY: %u lines a frame, of which %u shown, %u clocks a line (the device tree's timing "
      "for 120 frames a second has 2432 lines; for 60, about twice that)\n", frame / line, (shown + 1) / line, line);
 }
 printf("adreno: 0: the interface's counters (frame_line_count_en %#x): frame %u line %u, then frame %u line %u %llu us "
     "later\n", counting, frames[0], lines[0], frames[1], lines[1], (times[1] - times[0]) / 1000);
 if (line != 0 && frame >= line && frames[1] != frames[0] && times[1] > times[0]) {
  uint64_t per_frame = frame / line;
  uint64_t gone = (uint64_t)(frames[1] - frames[0]) * per_frame + lines[1] - lines[0];
  uint64_t rate = gone * 1000000000ull / per_frame * 1000 / (times[1] - times[0]);
  printf("adreno: 0: THE PANEL IS SENT %llu.%03llu FRAMES A SECOND (%llu lines in that time)\n", rate / 1000, rate % 1000,
      gone);
 }
 for (int i = 0; i < 8; i++) {
  uint64_t nanos;
  absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
  printf("adreno: 0: the scan: at %llu us line %u of frame %u\n", nanos / 1000 % 1000000, rd(disp_intf.base + INTF_LINE_COUNT),
      rd(disp_intf.base + INTF_FRAME_COUNT));
  sleep_us(1300);
 }
out:
 fine = saved_fine;
 newest_step = 0;
}
#define REDWOOD_SOC_BASE 0x00400000ul
extern vm_offset_t pe_arm_get_soc_base_phys(void);
static bool
start_by_itself(void)
{
 static const char *const files[] = { "a660_gmu.bin", "a660_sqe.fw", "a660_zap.mdt", "a660_zap.b02" };
 const char *missing = NULL;
 uint64_t size;
 if (pe_arm_get_soc_base_phys() != REDWOOD_SOC_BASE) {
  printf("adreno: THE GPU IS NOT STARTED: gpu=1 is for the phone, and this is not it\n");
  return false;
 }
 image_asked_quietly = true;
 for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]) && missing == NULL; i++) {
  if (image_file("fw", files[i], &size) == NULL) {
   missing = files[i];
  }
 }
 image_asked_quietly = false;
 if (missing != NULL) {
  printf("adreno: THE GPU IS NOT STARTED: the small image has no fw/%s (the phone's /vendor/firmware/%s goes into "
      "fw-gpu/); the system draws with its software renderer\n", missing, missing);
  return false;
 }
 return true;
}
void
redwood_adreno_init(void)
{
 static const char *const what[] = {
  "", "the clocks outside", "the fuses", "the clock controller", "the CX half's power", "the clocks", "the first words",
  "the GMU's RPMh sequencer", "the SMMU", "an SMMU context", "the GMU's memory", "TrustZone", "the zap shader's header",
  "the GMU's address space", "the GMU's firmware", "the GMU's start", "the GMU's message queues", "the 3D core's power",
  "the 3D core's address space", "the zap shader", "the 3D core's start", "the 3D core's first commands", "a frame"
 };
 uint32_t only = 0, done = 0, failed = 0, keep = 0;
 bool by_itself = false;
 if (PE_parse_boot_argn("display_probe", &only, sizeof(only)) && only) {
  display_probe();
 }
 if (!PE_parse_boot_argn("adreno", &last_step, sizeof(last_step))) {
  if (!(PE_parse_boot_argn("gpu", &only, sizeof(only)) && only) || !start_by_itself()) {
   return;
  }
  by_itself = true;
  last_step = 21;
  skip_steps = 1u << 10 | 1u << 11 | 1u << 12;
  pause_ms = 0;
  watch_seconds = 0;
  keep = 1;
 }
 PE_parse_boot_argn("adreno_skip", &skip_steps, sizeof(skip_steps));
 if (PE_parse_boot_argn("adreno_only", &only, sizeof(only)) && only > 0 && only <= STEPS) {
  last_step = only;
  skip_steps = ~(1u << only);
 }
 PE_parse_boot_argn("adreno_pause", &pause_ms, sizeof(pause_ms));
 PE_parse_boot_argn("adreno_fine", &fine, sizeof(fine));
 if (last_step == 0) {
  return;
 }
 if (last_step > STEPS) {
  last_step = STEPS;
 }
 PE_parse_boot_argn("adreno_watch", &watch_seconds, sizeof(watch_seconds));
 for (newest_step = last_step; newest_step > 0 && (skip_steps & 1u << newest_step); newest_step--) {
 }
 printf("adreno: steps up to %u, without those in mask %#x; step %u is the new one and waits %u ms %s\n", last_step,
     skip_steps, newest_step, pause_ms, fine ? "before every access" : "after each announcement");
 for (step = 1; step <= last_step; step++) {
  if (skip_steps & 1u << step) {
   continue;
  }
  if (!steps[step - 1]()) {
   printf("adreno: step %u: FAILED; the steps after it are not attempted\n", step);
   failed = step;
   break;
  }
  printf("adreno: step %u: done\n", step);
  done = step;
 }
 PE_parse_boot_argn("adreno_keep", &keep, sizeof(keep));
 if (keep) {
  if (done >= 21 && core_runs) {
   gpu_free[0].start = GPU_ADDRESSES_START;
   gpu_free[0].size = GPU_ADDRESSES_SIZE;
   gpu_free_count = 1;
   bzero((void *)core_scratch.memory, 4096);
   to_memory(core_scratch.memory, 4096);
   PE_parse_boot_argn("gpu_exact", &gpu_exact, sizeof(gpu_exact));
   PE_parse_boot_argn("gpu_log", &gpu_log, sizeof(gpu_log));
   gpu_kept = true;
   quiet = true;
   printf("adreno: THE GPU STAYS ON for the system to draw with: the service RedwoodGPU\n");
   RedwoodGPUStart();
   return;
  }
  if (!by_itself) {
   printf("adreno: not kept: the steps did not get as far as a 3D core that takes commands\n");
  }
 }
 if (by_itself) {
  printf("adreno: THE GPU IS NOT STARTED: step %u (%s) failed; the system draws with its software renderer\n",
      failed != 0 ? failed : 21, what[failed != 0 && failed <= STEPS ? failed : 21]);
 }
 for (uint32_t second = 1; second <= watch_seconds; second++) {
  sleep_ms(1000);
  printf("adreno: alive %u s after step %u; %s\n", second, done, last_write);
 }
 gpu_block_stop();
 if (PE_parse_boot_argn("adreno_return", &only, sizeof(only)) && only) {
  printf("adreno: over; back to the loader (adreno_return)\n");
  loader_return();
 }
 printf("adreno: over; the system goes on starting\n");
}
