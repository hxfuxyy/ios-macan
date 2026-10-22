#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/packfs/packfs.h>
#include <pexpert/pexpert.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void loader_return(void);
extern void macan_minidump_klog(void);
extern uint64_t kvtophys(vm_offset_t va);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);
void macan_adreno_init(void);
static uint32_t last_step, pause_ms = 1500, step, newest_step;
static char last_write[96] = "nothing written";
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
hold(void)
{
 if (step == newest_step && pause_ms) {
  sleep_ms(pause_ms);
 }
}
static void
announce(const char *what)
{
 printf("macan-gpu: step %u: %s\n", step, what);
 hold();
}
static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
 uint32_t value;
 need(b);
 value = rd(b->base + offset);
 printf("macan-gpu: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
 return value;
}
static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
 need(b);
 printf("macan-gpu: %u: write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
 snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name,
     b->phys + offset);
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
static uint32_t
clear(struct block *b, const char *name, uint32_t offset, uint32_t bits)
{
 uint32_t was = get(b, name, offset);
 if ((was & bits) == 0) {
  return was;
 }
 put(b, name, offset, was & ~bits);
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
static struct block gcc = { "gcc", 0x00100000, 0x1f4200 };
#define GCC_PLL_VOTE 0x52000
#define GCC_PLL_VOTE_GPU (1u << 15 | 1u << 16)
#define GCC_GPU_CFG_AHB_CBCR 0x71004
#define GCC_GPU_GEMNOC_GFX_CBCR 0x71010
#define GCC_GPU_SMMU_VOTE_CBCR 0x7d000
#define CBCR_ENABLE (1u << 0)
#define CBCR_OFF (1u << 31)
static struct block gpucc = { "gpucc", 0x03d90000, 0x9800 };
#define GPUCC_PLL0_MODE 0x0000
#define GPUCC_ALWAYS_ON_CXO_AON 0x9004
#define GPUCC_ALWAYS_ON_SLEEP 0x9008
#define GPUCC_ALWAYS_ON_CB 0x90cc
#define GPUCC_ALWAYS_ON_RSCC_XO 0x93a4
#define GPUCC_ALWAYS_ON_RSCC_HUB 0x93e8
#define GPUCC_GX_GMU_CBCR 0x9060
#define GPUCC_CX_GDSCR 0x9080
#define GPUCC_CX_CFG_GDSCR 0x9084
#define GPUCC_CX_GDS_HW_CTRL 0x9094
#define GPUCC_AHB_CBCR 0x90bc
#define GPUCC_CX_GMU_CBCR 0x90d4
#define GPUCC_CXO_CBCR 0x90e4
#define GPUCC_HUB_CX_INT_CBCR 0x90e8
#define GPUCC_MEMNOC_GFX_CBCR 0x90f0
#define GPUCC_GMU_RCGR 0x9318
#define GPUCC_HUB_AON_CBCR 0x93ec
#define GPUCC_HUB_RCGR 0x93f0
#define GPUCC_HUB_DIV 0x9430
#define GPUCC_SMMU_VOTE_CBCR 0x7000
#define GDSC_SW_COLLAPSE (1u << 0)
#define GDSC_POWER_ON (1u << 31)
#define RCG_UPDATE (1u << 0)
#define RCG_ROOT_OFF (1u << 31)
static struct block gmu = { "gmu", 0x03d37000, 0x68000 };
static struct block cx_misc = { "cx_misc", 0x03d9e000, 0x2000 };
#define GPU_BASE 0x03d00000u
#define GMU_REG(words) ((uint32_t)(words) * 4u + GPU_BASE - 0x03d37000u)
#define GMUCX_GFX_PWR_CLK_STATUS GMU_REG(0x1f7e8)
#define GMUCX_RPMH_POWER_STATE GMU_REG(0x1f7e9)
#define GMUCX_CM3_SYSRESET GMU_REG(0x1f800)
#define GMUCX_CM3_FW_INIT_RESULT GMU_REG(0x1f81c)
#define GMUCX_AO_COUNTER_LO GMU_REG(0x1f840)
#define GMUCX_AO_COUNTER_HI GMU_REG(0x1f841)
#define GMUAO_GPU_CX_BUSY_STATUS GMU_REG(0x23b0c)
#define GMUAO_AHB_FENCE_CTRL GMU_REG(0x23b10)
#define CX_MISC_AO_COUNTER_LO (0x27880u * 4u + GPU_BASE - 0x03d9e000u)
#define CX_MISC_SLICE_ENABLE_FINAL (0x27887u * 4u + GPU_BASE - 0x03d9e000u)
#define SCM_64 (1ull << 30)
#define SCM_SIP (2ull << 24)
#define SCM_CALL(service,command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN SCM_CALL(2, 6)
#define SCM_ONE_VALUE 1
#define SCM_THREE_VALUES 3
#define SCM_VALUE_AND_BUFFER (2 | 2 << 6)
#define SCM_INTERRUPTED 1
static int32_t scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer);
#define CMD_DB_PHYS 0x81c60000UL
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
static void
show_rpmh_resource(const struct cmd_db_header *db, const char *name)
{
 char id[8] = { 0 };
 strlcpy(id, name, sizeof(id));
 for (unsigned s = 0; s < 8; s++) {
  const struct cmd_db_slave *slave = &db->slave[s];
  const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);
  if (slave->id == 0 || slave->count > 512 || slave->header_offset > CMD_DB_SIZE - 0x1000) {
   continue;
  }
  for (unsigned i = 0; i < slave->count; i++) {
   const uint8_t *data = db->data + slave->data_offset + entry[i].offset;
   char levels[160];
   int at = 0;
   if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
    continue;
   }
   levels[0] = 0;
   if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length <= CMD_DB_SIZE - 0x1000) {
    for (unsigned l = 0; l < entry[i].length / 2u && l < 16 && at < (int)sizeof(levels) - 8; l++) {
     at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", data[2 * l] | data[2 * l + 1] << 8);
    }
   }
   printf("macan-gpu: %u: RPMh has \"%s\" at %#x; levels:%s\n", step, name, entry[i].address, levels);
   return;
  }
 }
 printf("macan-gpu: %u: RPMh has no \"%s\"\n", step, name);
}
static bool
step_outside(void)
{
 const struct cmd_db_header *db;
 announce("read the GPU's clocks in the global clock controller");
 get(&gcc, "pll votes", GCC_PLL_VOTE);
 get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
 get(&gcc, "gpu_gemnoc_gfx", GCC_GPU_GEMNOC_GFX_CBCR);
 get(&gcc, "gpu_smmu_vote", GCC_GPU_SMMU_VOTE_CBCR);
 announce("look up the GPU's rails in the command DB (memory at 0x81c60000)");
 db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
 if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
  printf("macan-gpu: %u: no command DB there (magic %02x %02x %02x %02x)\n", step, db->magic[0], db->magic[1],
      db->magic[2], db->magic[3]);
  return true;
 }
 show_rpmh_resource(db, "gfx.lvl");
 show_rpmh_resource(db, "cx.lvl");
 show_rpmh_resource(db, "mx.lvl");
 show_rpmh_resource(db, "gmxc.lvl");
 show_rpmh_resource(db, "mxc.lvl");
 return true;
}
static void
show_gpucc(void)
{
 get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
 get(&gpucc, "cx_cfg_gdscr", GPUCC_CX_CFG_GDSCR);
 get(&gpucc, "cx_gds_hw_ctrl", GPUCC_CX_GDS_HW_CTRL);
 get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
 get(&gpucc, "pll0 l", GPUCC_PLL0_MODE + 4);
 get(&gpucc, "gmu_clk_src command", GPUCC_GMU_RCGR);
 get(&gpucc, "gmu_clk_src config", GPUCC_GMU_RCGR + 4);
 get(&gpucc, "hub_clk_src command", GPUCC_HUB_RCGR);
 get(&gpucc, "hub_clk_src config", GPUCC_HUB_RCGR + 4);
 get(&gpucc, "hub_div", GPUCC_HUB_DIV);
 get(&gpucc, "ahb", GPUCC_AHB_CBCR);
 get(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
 get(&gpucc, "cxo", GPUCC_CXO_CBCR);
 get(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR);
 get(&gpucc, "memnoc_gfx", GPUCC_MEMNOC_GFX_CBCR);
 get(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR);
 get(&gpucc, "gx_gmu", GPUCC_GX_GMU_CBCR);
 get(&gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR);
 get(&gpucc, "cxo_aon", GPUCC_ALWAYS_ON_CXO_AON);
 get(&gpucc, "sleep", GPUCC_ALWAYS_ON_SLEEP);
 get(&gpucc, "cb", GPUCC_ALWAYS_ON_CB);
 get(&gpucc, "rscc_xo_aon", GPUCC_ALWAYS_ON_RSCC_XO);
 get(&gpucc, "rscc_hub_aon", GPUCC_ALWAYS_ON_RSCC_HUB);
}
static bool
gpucc_answers(void)
{
 need(&gcc);
 if (rd(gcc.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_OFF) {
  printf("macan-gpu: %u: the GPU clock controller's registers have no clock (gcc gpu_cfg_ahb %#x)\n", step,
      rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
  return false;
 }
 return true;
}
static bool
step_gpucc(void)
{
 announce("make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb)");
 if ((set(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR, CBCR_ENABLE) & CBCR_OFF) &&
     !wait_for(&gcc, GCC_GPU_CFG_AHB_CBCR, CBCR_OFF, 0, 10)) {
  printf("macan-gpu: %u: it stays off: the GPU's clock controller would not answer\n", step);
  return false;
 }
 announce("read the GPU's clock controller (0x3d90000) as UEFI left it");
 show_gpucc();
 return true;
}
static bool cx_switched_on, clocks_switched_on;
static bool
cx_is_on(void)
{
 return (rd(gpucc.base + GPUCC_CX_GDSCR) | rd(gpucc.base + GPUCC_CX_GDS_HW_CTRL)) & GDSC_POWER_ON;
}
static bool
step_cx_power(void)
{
 if (!gpucc_answers()) {
  return false;
 }
 need(&gpucc);
 if (cx_is_on()) {
  printf("macan-gpu: %u: CX is on already (gdscr %#x, hw_ctrl %#x)\n", step, rd(gpucc.base + GPUCC_CX_GDSCR),
      rd(gpucc.base + GPUCC_CX_GDS_HW_CTRL));
  return true;
 }
 announce("switch the CX half of the GPU block on (clear the collapse bit of gpu_cc_cx_gdsc)");
 clear(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, GDSC_SW_COLLAPSE);
 cx_switched_on = true;
 for (unsigned i = 0; i < 500 && !cx_is_on(); i++) {
  sleep_us(100);
 }
 get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
 get(&gpucc, "cx_gds_hw_ctrl", GPUCC_CX_GDS_HW_CTRL);
 if (!cx_is_on()) {
  printf("macan-gpu: %u: CX did not come on in 50 ms\n", step);
  return false;
 }
 printf("macan-gpu: %u: CX is on\n", step);
 return true;
}
static void
rcg_set(const char *name, uint32_t rcgr, uint32_t config)
{
 put(&gpucc, name, rcgr + 4, config);
 put(&gpucc, name, rcgr, rd(gpucc.base + rcgr) | RCG_UPDATE);
 if (!wait_for(&gpucc, rcgr, RCG_UPDATE, 0, 5)) {
  printf("macan-gpu: %u: %s: the update is not taken (%#x)\n", step, name, rd(gpucc.base + rcgr));
 }
 get(&gpucc, name, rcgr);
}
static bool
clock_on(struct block *b, const char *name, uint32_t cbcr, bool wait)
{
 uint32_t value = set(b, name, cbcr, CBCR_ENABLE);
 if (!wait || !(value & CBCR_OFF)) {
  return true;
 }
 if (wait_for(b, cbcr, CBCR_OFF, 0, 10)) {
  return true;
 }
 printf("macan-gpu: %u: %s stays off (%#x)\n", step, name, rd(b->base + cbcr));
 return false;
}
static bool
step_clocks(void)
{
 bool ok = true;
 if (!gpucc_answers() || (need(&gpucc), !cx_is_on())) {
  printf("macan-gpu: %u: CX is not on: step 3 first\n", step);
  return false;
 }
 announce("vote GPLL0 (600 MHz and its 300 MHz tap) to the GPU clock controller; the GPU's bus clock");
 set(&gcc, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU);
 ok &= clock_on(&gcc, "gpu_gemnoc_gfx", GCC_GPU_GEMNOC_GFX_CBCR, true);
 clocks_switched_on = true;
 announce("the clocks Linux keeps on for good (cxo_aon, sleep, cb, rscc_xo_aon, rscc_hub_aon)");
 clock_on(&gpucc, "cxo_aon", GPUCC_ALWAYS_ON_CXO_AON, false);
 clock_on(&gpucc, "sleep", GPUCC_ALWAYS_ON_SLEEP, false);
 clock_on(&gpucc, "cb", GPUCC_ALWAYS_ON_CB, false);
 clock_on(&gpucc, "rscc_xo_aon", GPUCC_ALWAYS_ON_RSCC_XO, false);
 clock_on(&gpucc, "rscc_hub_aon", GPUCC_ALWAYS_ON_RSCC_HUB, false);
 announce("clock sources: the GMU at the crystal (19.2 MHz), the hub at 200 MHz (GPLL0 / 3)");
 rcg_set("gmu_clk_src", GPUCC_GMU_RCGR, 0 << 8 | 1);
 rcg_set("hub_clk_src", GPUCC_HUB_RCGR, 5 << 8 | (2 * 3 - 1));
 announce("the branches of the GMU's node: ahb, cx_gmu, cxo, hub_cx_int, memnoc_gfx, hub_aon, smmu vote");
 ok &= clock_on(&gpucc, "ahb", GPUCC_AHB_CBCR, false);
 ok &= clock_on(&gpucc, "cxo", GPUCC_CXO_CBCR, true);
 ok &= clock_on(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR, true);
 ok &= clock_on(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR, true);
 ok &= clock_on(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, true);
 ok &= clock_on(&gpucc, "memnoc_gfx", GPUCC_MEMNOC_GFX_CBCR, true);
 clock_on(&gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR, true);
 return ok;
}
static bool
step_first_words(void)
{
 uint32_t a, b, c, d;
 if (!gpucc_answers() || (need(&gpucc), !cx_is_on()) || (rd(gpucc.base + GPUCC_CX_GMU_CBCR) & CBCR_OFF) ||
     (rd(gpucc.base + GPUCC_AHB_CBCR) & CBCR_OFF)) {
  printf("macan-gpu: %u: CX or the GMU's clocks are off: steps 3 and 4 first\n", step);
  return false;
 }
 announce("read the always-on counter of CX_MISC twice, a millisecond apart");
 a = get(&cx_misc, "ao_counter_lo", CX_MISC_AO_COUNTER_LO);
 sleep_ms(1);
 b = get(&cx_misc, "ao_counter_lo", CX_MISC_AO_COUNTER_LO);
 printf("macan-gpu: %u: CX_MISC counter %s (%u ticks)\n", step, a != b ? "counts" : "STANDS", b - a);
 announce("read the GMU: its always-on counter twice, and what its registers say about power and firmware");
 c = get(&gmu, "ao_counter_lo", GMUCX_AO_COUNTER_LO);
 sleep_ms(1);
 d = get(&gmu, "ao_counter_lo", GMUCX_AO_COUNTER_LO);
 get(&gmu, "ao_counter_hi", GMUCX_AO_COUNTER_HI);
 printf("macan-gpu: %u: GMU counter %s (%u ticks)\n", step, c != d ? "counts" : "STANDS", d - c);
 get(&gmu, "cm3_sysreset", GMUCX_CM3_SYSRESET);
 get(&gmu, "cm3_fw_init_result", GMUCX_CM3_FW_INIT_RESULT);
 get(&gmu, "rpmh_power_state", GMUCX_RPMH_POWER_STATE);
 get(&gmu, "gfx_pwr_clk_status", GMUCX_GFX_PWR_CLK_STATUS);
 get(&gmu, "gpu_cx_busy_status", GMUAO_GPU_CX_BUSY_STATUS);
 get(&gmu, "ahb_fence_ctrl", GMUAO_AHB_FENCE_CTRL);
 return true;
}
static bool
cx_ready(void)
{
 if (!gpucc_answers()) {
  return false;
 }
 need(&gpucc);
 if (!cx_is_on() || (rd(gpucc.base + GPUCC_CX_GMU_CBCR) & CBCR_OFF) || (rd(gpucc.base + GPUCC_AHB_CBCR) & CBCR_OFF)) {
  printf("macan-gpu: %u: needs CX on and the GMU's and ahb clocks running (cx_gmu %#x, ahb %#x): steps 3 and 4\n",
      step, rd(gpucc.base + GPUCC_CX_GMU_CBCR), rd(gpucc.base + GPUCC_AHB_CBCR));
  return false;
 }
 return true;
}
static struct block smmu = { "smmu", 0x03da0000, 0x40000 };
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
 printf("macan-gpu: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n",
     step, smmu_matches, smmu_contexts, smmu_pages, smmu_page);
 if (2 * smmu_pages * smmu_page > smmu.size) {
  printf("macan-gpu: %u: that makes %#x of registers, the device tree says %#x: not reading further\n", step,
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
 announce("read the ID registers of the GPU's SMMU (0x3da0000)");
 if (!smmu_layout()) {
  return false;
 }
 get(&smmu, "idr2", SMMU_IDR2);
 get(&smmu, "idr7", SMMU_IDR7);
 get(&smmu, "scr0", SMMU_SCR0);
 get(&smmu, "sgfsr", SMMU_SGFSR);
 announce("read the SMMU's stream table and the kinds of its contexts");
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
 announce("read translation context 0 of the SMMU");
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
static struct block rscc = { "rscc", 0x03d50000, 0x10000 };
#define RSCC_SEQ_BUSY_DRV0 (4 * 0x101)
#define RSCC_TCS0_DRV0_STATUS (4 * 0x34a)
#define RSCC_TCS1_DRV0_STATUS (4 * 0x3f2)
#define RSCC_TCS2_DRV0_STATUS (4 * 0x49a)
#define RSCC_TCS3_DRV0_STATUS (4 * 0x542)
#define GMUAO_RSCC_CONTROL_REQ GMU_REG(0x23b07)
#define GMUAO_RSCC_CONTROL_ACK GMU_REG(0x23b08)
static bool
step_rscc(void)
{
 if (!cx_ready()) {
  return false;
 }
 announce("read the GMU's RPMh channel (0x3d50000)");
 get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
 get(&rscc, "tcs0_drv0_status", RSCC_TCS0_DRV0_STATUS);
 get(&rscc, "tcs1_drv0_status", RSCC_TCS1_DRV0_STATUS);
 get(&rscc, "tcs2_drv0_status", RSCC_TCS2_DRV0_STATUS);
 get(&rscc, "tcs3_drv0_status", RSCC_TCS3_DRV0_STATUS);
 get(&gmu, "rscc_control_req", GMUAO_RSCC_CONTROL_REQ);
 get(&gmu, "rscc_control_ack", GMUAO_RSCC_CONTROL_ACK);
 return true;
}
#define GMU_ITCM GMU_REG(0x1b400)
#define GMU_DTCM GMU_REG(0x1c400)
static bool
step_gmu_memory(void)
{
 static const uint32_t pattern[2] = { 0x600d0001, 0x0a110002 };
 uint32_t was[2], back[2];
 if (!cx_ready()) {
  return false;
 }
 announce("hold the GMU's processor in reset, write two words into its data memory (0x3d71000), read them back");
 if (get(&gmu, "cm3_fw_init_result", GMUCX_CM3_FW_INIT_RESULT) & (1u << 8)) {
  printf("macan-gpu: %u: firmware is running in the GMU: leaving its memory alone\n", step);
  return false;
 }
 if (!(get(&gmu, "cm3_sysreset", GMUCX_CM3_SYSRESET) & 1)) {
  put(&gmu, "cm3_sysreset", GMUCX_CM3_SYSRESET, 1);
 }
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
 get(&gmu, "itcm", GMU_ITCM);
 printf("macan-gpu: %u: the GMU's memory %s\n", step, back[0] == pattern[0] && back[1] == pattern[1] ?
     "TAKES WHAT IS WRITTEN: its firmware can be loaded" : "does not hold what was written");
 return back[0] == pattern[0] && back[1] == pattern[1];
}
#define PTE_TABLE 3ull
#define PTE_PAGE 3ull
#define PTE_SHARED (2ull << 8)
#define PTE_ACCESSED (1ull << 10)
#define PTE_NOT_GLOBAL (1ull << 11)
#define PTE_ADDRESS 0x0000fffffffff000ull
#define PTE_GMU (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)
#define PTE_UNPRIVILEGED (1ull << 6)
#define PTE_CORE (PTE_GMU | PTE_UNPRIVILEGED)
#define PTE_READ_ONLY (1ull << 7)
#define SMMU_S2CR_FAULT (2u << 16)
#define SMMU_SMR_VALID (1u << 31)
#define SMMU_STREAM_MASK 0x0
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
#define SPACE_TABLES 2048
struct gpu_space {
 const char *name;
 uint32_t context;
 uint32_t table_count;
 uint32_t last;
 uint64_t *root;
 struct {
  uint64_t phys;
  uint64_t *table;
 } tables[SPACE_TABLES];
};
static uint64_t *
space_new_table(struct gpu_space *space)
{
 vm_offset_t page;
 if (space->table_count == SPACE_TABLES ||
     kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
  return NULL;
 }
 space->tables[space->table_count].phys = kvtophys(page);
 space->tables[space->table_count++].table = (uint64_t *)page;
 return (uint64_t *)page;
}
static uint64_t *
space_next(struct gpu_space *space, uint64_t *entry)
{
 uint64_t *table;
 if (*entry & 1) {
  if (space->last < space->table_count && space->tables[space->last].phys == (*entry & PTE_ADDRESS)) {
   return space->tables[space->last].table;
  }
  for (uint32_t i = 0; i < space->table_count; i++) {
   if (space->tables[i].phys == (*entry & PTE_ADDRESS)) {
    space->last = i;
    return space->tables[i].table;
   }
  }
  return NULL;
 }
 table = space_new_table(space);
 if (table != NULL) {
  to_memory((vm_offset_t)table, 4096);
  *entry = kvtophys((vm_offset_t)table) | PTE_TABLE;
  __asm__ volatile ("dc civac, %0" : : "r" (entry) : "memory");
  space->last = space->table_count - 1;
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
 level3 = level2 == NULL ? NULL : space_next(space, &level2[address >> 21 & 0x1ff]);
 if (level3 == NULL) {
  return false;
 }
 level3[address >> 12 & 0x1ff] = phys == 0 && attributes == 0 ? 0 : phys | attributes | PTE_PAGE;
 __asm__ volatile ("dc civac, %0" : : "r" (&level3[address >> 12 & 0x1ff]) : "memory");
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
 printf("macan-gpu: %u: %s sees %#llx..%#llx in memory of ours (%#llx)\n", step, space->name, address,
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
 printf("macan-gpu: %u: THE SMMU REFUSED %s: status %#x, address %#llx, syndrome %#x\n", step, space->name, status,
     (uint64_t)rd(smmu.base + context + SMMU_CB_FAR + 4) << 32 | rd(smmu.base + context + SMMU_CB_FAR),
     rd(smmu.base + context + SMMU_CB_FSYNR0));
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
  printf("macan-gpu: %u: the SMMU does not finish forgetting old translations (%#x)\n", step,
      rd(smmu.base + context + SMMU_CB_TLBSTATUS));
  return false;
 }
 put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
 put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
 put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);
 if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
     (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context ||
     (get(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
     !(get(&smmu, "cb sctlr", context + SMMU_CB_SCTLR) & 1) ||
     get(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0) != (uint32_t)root ||
     get(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
  printf("macan-gpu: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
      stream, space->context);
  return false;
 }
 get(&smmu, "cb fsr", context + SMMU_CB_FSR);
 get(&smmu, "sgfsr", SMMU_SGFSR);
 return true;
}
static void
space_off(struct gpu_space *space, uint32_t match)
{
 wr(smmu.base + SMMU_SMR(match), 0);
 wr(smmu.base + SMMU_S2CR(match), SMMU_S2CR_FAULT);
 wr(smmu.base + smmu_context(space) + SMMU_CB_SCTLR, 0);
}
static const struct packfs_dnode *
image_entry(const uint8_t *image, const struct packfs_super *super, const struct packfs_dnode *directory, const char *name)
{
 const struct packfs_dnode *nodes = (const struct packfs_dnode *)(image + super->ps_nodes_off);
 size_t length = strlen(name);
 if (directory == NULL || !S_ISDIR(directory->pd_mode) || directory->pd_data + directory->pd_size > super->ps_node_count) {
  return NULL;
 }
 for (uint64_t i = 0; i < directory->pd_size; i++) {
  const struct packfs_dnode *node = &nodes[directory->pd_data + i];
  if (node->pd_name_len == length && (uint64_t)node->pd_name + length <= super->ps_strings_size &&
      memcmp(image + super->ps_strings_off + node->pd_name, name, length) == 0) {
   return node;
  }
 }
 return NULL;
}
static const uint8_t *
image_file(const char *directory, const char *name, uint64_t *size)
{
 for (int disk = 1; disk >= 0; disk--) {
  const struct packfs_super *super;
  const struct packfs_dnode *file;
  vm_offset_t base = 0;
  uint64_t length = 0;
  dev_t dev = mdevlookup(disk);
  if (dev == (dev_t)-1 || mdevgetkernelrange(dev, &base, &length) != 0 || length < PACKFS_SUPER_SIZE) {
   continue;
  }
  super = (const struct packfs_super *)base;
  if (memcmp(super->ps_magic, PACKFS_MAGIC, sizeof(super->ps_magic)) != 0 || super->ps_image_size > length ||
      super->ps_node_count == 0 ||
      super->ps_nodes_off + (uint64_t)super->ps_node_count * sizeof(struct packfs_dnode) > super->ps_image_size ||
      super->ps_strings_off + super->ps_strings_size > super->ps_image_size) {
   continue;
  }
  file = (const struct packfs_dnode *)(base + super->ps_nodes_off);
  file = image_entry((const uint8_t *)base, super, file, directory);
  file = image_entry((const uint8_t *)base, super, file, name);
  if (file != NULL && S_ISREG(file->pd_mode) && !(file->pd_flags & PACKFS_F_CHUNKED) &&
      file->pd_data + file->pd_size <= super->ps_image_size) {
   printf("macan-gpu: %u: %s/%s: %llu bytes in the image of /dev/md%d\n", step, directory, name, file->pd_size,
       disk);
   *size = file->pd_size;
   return (const uint8_t *)base + file->pd_data;
  }
 }
 printf("macan-gpu: %u: no %s/%s in the small image (uncompressed)\n", step, directory, name);
 return NULL;
}
#define GMU_ITCM_ADDRESS 0x00000000u
#define GMU_DTCM_ADDRESS 0x10004000u
#define GMU_TCM_SIZE 0x4000u
#define GMU_LOG_ADDRESS 0x60004000u
#define GMU_LOG_SIZE 0x10000u
#define GMU_HFI_ADDRESS 0x60014000u
#define GMU_HFI_SIZE 0x4000u
#define GMU_STREAM 5
#define GMU_MATCH 1
struct gmu_firmware_block {
 uint32_t address, size, type, value;
};
#define GMU_FIRMWARE_NEEDS 1
#define GMU_FIRMWARE_NEEDS_KEPT 7
static struct gpu_space gmu_space = { .name = "the GMU", .context = 1 };
static struct gpu_block gmu_blocks[16], gmu_log, gmu_hfi;
static unsigned gmu_block_count;
static const uint8_t *gmu_firmware;
static uint64_t gmu_firmware_size;
static bool gmu_space_is_on, gmu_runs, gmu_firmware_loaded;
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
 announce("find the GMU's firmware, fw/gen80900_gmu.bin, in the small image, and make the memory it asks for");
 gmu_firmware = image_file("fw", "gen80900_gmu.bin", &gmu_firmware_size);
 if (gmu_firmware == NULL) {
  return false;
 }
 for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
  struct gmu_firmware_block block;
  memcpy(&block, gmu_firmware + at, sizeof(block));
  at += sizeof(block) + block.size;
  if (at > gmu_firmware_size) {
   printf("macan-gpu: %u: the file ends inside a block\n", step);
   return false;
  }
  if (block.size != 0 || (block.type != GMU_FIRMWARE_NEEDS && block.type != GMU_FIRMWARE_NEEDS_KEPT) ||
      in_tcm(block.address)) {
   continue;
  }
  if (gmu_block_count == sizeof(gmu_blocks) / sizeof(gmu_blocks[0]) || (block.address & 4095) ||
      block.value > 0x400000 || !block_new(&gmu_space, &gmu_blocks[gmu_block_count++], block.address, block.value,
      PTE_GMU)) {
   printf("macan-gpu: %u: cannot make %#x bytes at %#x for the firmware\n", step, block.value, block.address);
   return false;
  }
 }
 if (!block_new(&gmu_space, &gmu_log, GMU_LOG_ADDRESS, GMU_LOG_SIZE, PTE_GMU) ||
     !block_new(&gmu_space, &gmu_hfi, GMU_HFI_ADDRESS, GMU_HFI_SIZE, PTE_GMU)) {
  return false;
 }
 announce("give the SMMU's context 1 these tables and send the GMU's requests (stream 5) through it");
 if (!smmu_layout()) {
  return false;
 }
 gmu_space_is_on = true;
 return space_on(&gmu_space, GMU_MATCH, GMU_STREAM);
}
#define GMU_DTCM_VERSIONS (GMU_DTCM + 4 * 0xff8)
static bool
step_gmu_firmware(void)
{
 uint32_t wrong = 0, core_version = 0;
 if (!cx_ready() || gmu_firmware == NULL) {
  return false;
 }
 announce("ask TrustZone to program the GPU's access-controlled registers (gpu_init_regs 0xb), read the slices on");
 {
  uint64_t answer = ~0ull;
  int32_t result = scm("to program the GPU's protected registers", SCM_CALL(0x28, 1), SCM_ONE_VALUE, 0xb, 0, 0, &answer);
  printf("macan-gpu: %u: gpu_init_regs: %d (%#llx)\n", step, result, answer);
  get(&cx_misc, "slice_enable_final", CX_MISC_SLICE_ENABLE_FINAL);
 }
 announce("load the GMU's firmware: code and data memory through its registers, the rest into our memory");
 put(&gmu, "cm3_sysreset", GMUCX_CM3_SYSRESET, 1);
 for (int pass = 0; pass < 2; pass++) {
  for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
   struct gmu_firmware_block block;
   const uint8_t *data = gmu_firmware + at + sizeof(block);
   memcpy(&block, gmu_firmware + at, sizeof(block));
   at += sizeof(block) + block.size;
   if (block.size == 0 || block.type == 2 || block.type == 3) {
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
     printf("macan-gpu: %u: %#x bytes at the GMU's %#x, through its registers\n", step, block.size,
         block.address);
    }
    continue;
   }
   for (unsigned i = 0; pass == 0 && i <= gmu_block_count; i++) {
    if (i == gmu_block_count) {
     printf("macan-gpu: %u: the firmware has %#x bytes for %#x, where it asked for no memory\n", step,
         block.size, block.address);
     return false;
    }
    if (block.address >= gmu_blocks[i].address &&
        block.address + block.size <= gmu_blocks[i].address + gmu_blocks[i].size) {
     memcpy((void *)(gmu_blocks[i].memory + (block.address - gmu_blocks[i].address)), data, block.size);
     to_memory(gmu_blocks[i].memory, gmu_blocks[i].size);
     printf("macan-gpu: %u: %#x bytes at the GMU's %#x, in memory\n", step, block.size, block.address);
     break;
    }
   }
  }
 }
 snprintf(last_write, sizeof(last_write), "step %u wrote the GMU's firmware", step);
 if (wrong) {
  printf("macan-gpu: %u: %u words of the GMU's own memory do not read back as written\n", step, wrong);
  return false;
 }
 for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
  struct gmu_firmware_block block;
  memcpy(&block, gmu_firmware + at, sizeof(block));
  at += sizeof(block) + block.size;
  if (block.size == 0 && block.type == 2) {
   core_version = block.value;
  }
 }
 if (core_version == 0 || rd(gmu.base + GMU_DTCM_VERSIONS) != core_version) {
  printf("macan-gpu: %u: the GMU's data memory says core version %#x, the file %#x: not starting it\n", step,
      rd(gmu.base + GMU_DTCM_VERSIONS), core_version);
  gmu_firmware_loaded = false;
  return false;
 }
 gmu_firmware_loaded = true;
 printf("macan-gpu: %u: THE GMU'S FIRMWARE IS LOADED; versions in its data memory: core %#x, power %#x, HFI %#x\n",
     step, rd(gmu.base + GMU_DTCM_VERSIONS), rd(gmu.base + GMU_DTCM_VERSIONS + 8), rd(gmu.base + GMU_DTCM_VERSIONS + 16));
 return true;
}
#define GMUCX_ICACHE_CONFIG GMU_REG(0x1f400)
#define GMUCX_DCACHE_CONFIG GMU_REG(0x1f401)
#define GMUCX_SYS_BUS_CONFIG GMU_REG(0x1f40f)
#define GMUCX_MRC_GBIF_QOS_CTRL GMU_REG(0x1f50b)
#define GMUCX_CX_FAL_INTF GMU_REG(0x1f7ec)
#define GMUCX_CX_FALNEXT_INTF GMU_REG(0x1f7ed)
#define GMUCX_CM3_BOOT_CONFIG GMU_REG(0x1f801)
#define GMUCX_CM3_CFG GMU_REG(0x1f82d)
#define GMUCX_HFI_CTRL_STATUS GMU_REG(0x1f980)
#define GMUCX_HFI_VERSION_INFO GMU_REG(0x1f981)
#define GMUCX_HFI_QTBL_INFO GMU_REG(0x1f984)
#define GMUCX_HFI_QTBL_ADDR GMU_REG(0x1f985)
#define GMUCX_HFI_CTRL_INIT GMU_REG(0x1f986)
#define GMUCX_GMU2HOST_INTR_CLR GMU_REG(0x1f991)
#define GMUCX_GMU2HOST_INTR_INFO GMU_REG(0x1f992)
#define GMUCX_GENERAL_8 GMU_REG(0x1f9c8)
#define GMUCX_GENERAL_9 GMU_REG(0x1f9c9)
#define GMUCX_GENERAL_10 GMU_REG(0x1f9ca)
#define GMUAO_AHB_FENCE_RANGE_0 GMU_REG(0x23b11)
#define GMUAO_AO_HOST_INTERRUPT_STATUS GMU_REG(0x23b05)
#define GMU_CHIP 0x08090000u
#define GMU_STARTED_MASK 0x1ffu
#define GMU_STARTED 0x100u
#define CP_CONTEXT_SWITCH_CNTL 0x8c0u
static struct block gbif = { "gbif", 0x03d00000 + 4 * 0x3c00, 0x1000 };
#define GBIF_CX_CONFIG (4 * 0x00)
#define GBIF_QSB_SIDE(n) (4 * (0x03 + (n)))
#define GBIF_HALT (4 * 0x45)
#define GBIF_HALT_ACK (4 * 0x46)
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
static void
gmu_post_mortem(void)
{
 get(&gmu, "cm3_fw_init_result", GMUCX_CM3_FW_INIT_RESULT);
 get(&gmu, "gmu2host_intr_info", GMUCX_GMU2HOST_INTR_INFO);
 get(&gmu, "ao_host_interrupt_status", GMUAO_AO_HOST_INTERRUPT_STATUS);
 get(&gmu, "hfi_ctrl_status", GMUCX_HFI_CTRL_STATUS);
 get(&gmu, "hfi_version_info", GMUCX_HFI_VERSION_INFO);
 space_faulted(&gmu_space);
 to_memory(gmu_log.memory, 256);
 for (unsigned i = 0; i < 8; i++) {
  const uint32_t *w = (const uint32_t *)gmu_log.memory + 8 * i;
  printf("macan-gpu: %u: log %3u: %08x %08x %08x %08x %08x %08x %08x %08x\n", step, 32 * i, w[0], w[1], w[2], w[3],
      w[4], w[5], w[6], w[7]);
 }
}
static bool
step_gmu_start(void)
{
 static const uint8_t priority[HFI_QUEUES] = { 10, 10, 40 };
 uint32_t result = 0;
 if (!cx_ready() || !gmu_space_is_on || !gmu_firmware_loaded) {
  printf("macan-gpu: %u: needs steps 10 and 11 (the GMU's memory and firmware) in this run\n", step);
  return false;
 }
 hfi = (volatile struct hfi_table *)block_uncached(&gmu_hfi);
 if (hfi == NULL) {
  printf("macan-gpu: %u: the page for the messages is not in one piece\n", step);
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
 announce("tell the GMU where things are and how to run, by its registers; then let its processor out of reset");
 if (get(&gmu, "cm3_sysreset", GMUCX_CM3_SYSRESET) != 1) {
  printf("macan-gpu: %u: the GMU's processor is not held in reset: not configuring it while it runs\n", step);
  return false;
 }
 put(&gmu, "rpmh_power_state", GMUCX_RPMH_POWER_STATE, 0xf);
 put(&gmu, "cx_falnext_intf", GMUCX_CX_FALNEXT_INTF, 1);
 put(&gmu, "cx_fal_intf", GMUCX_CX_FAL_INTF, 1);
 put(&gmu, "cm3_fw_init_result", GMUCX_CM3_FW_INIT_RESULT, 0);
 put(&gmu, "cm3_boot_config", GMUCX_CM3_BOOT_CONFIG, 2);
 put(&gmu, "hfi_qtbl_addr", GMUCX_HFI_QTBL_ADDR, GMU_HFI_ADDRESS);
 put(&gmu, "hfi_qtbl_info", GMUCX_HFI_QTBL_INFO, 1);
 put(&gmu, "ahb_fence_range_0", GMUAO_AHB_FENCE_RANGE_0, 1u << 31 | 0x32u << 18 | CP_CONTEXT_SWITCH_CNTL);
 put(&gmu, "cm3_cfg", GMUCX_CM3_CFG, 0x4052);
 for (unsigned i = 0; i < 4; i++) {
  put(&gbif, "qsb_side", GBIF_QSB_SIDE(i), 0x00071e20);
 }
 put(&gbif, "cx_config", GBIF_CX_CONFIG, 0x20023000);
 put(&gmu, "mrc_gbif_qos_ctrl", GMUCX_MRC_GBIF_QOS_CTRL, 0x33);
 put(&gbif, "halt", GBIF_HALT, 1u << 3);
 put(&gmu, "general_9 (log write index)", GMUCX_GENERAL_9, 0);
 put(&gmu, "general_10 (chip)", GMUCX_GENERAL_10, GMU_CHIP);
 put(&gmu, "general_8 (log)", GMUCX_GENERAL_8, GMU_LOG_ADDRESS | (GMU_LOG_SIZE / 4096 - 1));
 put(&gmu, "sys_bus_config", GMUCX_SYS_BUS_CONFIG, 1);
 put(&gmu, "icache_config", GMUCX_ICACHE_CONFIG, 1);
 put(&gmu, "dcache_config", GMUCX_DCACHE_CONFIG, 1);
 put(&gmu, "gmu2host_intr_clr", GMUCX_GMU2HOST_INTR_CLR, 0xffffffff);
 gmu_runs = true;
 put(&gmu, "cm3_sysreset", GMUCX_CM3_SYSRESET, 0);
 settle();
 for (int i = 0; i < 2000 && (result & GMU_STARTED_MASK) != GMU_STARTED; i++) {
  sleep_us(100);
  result = rd(gmu.base + GMUCX_CM3_FW_INIT_RESULT);
 }
 if (space_faulted(&gmu_space) || (result & GMU_STARTED_MASK) != GMU_STARTED) {
  printf("macan-gpu: %u: the GMU's firmware does not come up (fw_init_result %#x)\n", step, result);
  gmu_post_mortem();
  return false;
 }
 gmu_post_mortem();
 printf("macan-gpu: %u: THE GMU'S FIRMWARE RUNS (fw_init_result %#x)\n", step, result);
 return true;
}
#define HFI_BW_TABLE 3
#define HFI_CORE_FW_START 14
#define HFI_GX_BW_PERF_VOTE 30
#define HFI_TABLE 15
#define HFI_ERROR 100
#define HFI_ANSWER 1
#define HFI_TABLE_GPU_PERF 1
#define GMUCX_HOST2GMU_INTR_SET GMU_REG(0x1f994)
#define GMU_OOB_GPU_SET (1u << 30)
#define GMU_OOB_GPU_CHECK (1u << 31)
#define GMU_OOB_GPU_CLEAR (1u << 31)
static uint32_t hfi_number;
static bool gmu_talks, gpu_oob_held;
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
    printf("macan-gpu: %u: THE GMU REPORTS AN ERROR: %u, %u, \"%s\"\n", step, message[1] >> 16,
        message[1] & 0xffff, (const char *)&message[2]);
   } else {
    printf("macan-gpu: %u: the GMU says (queue %u, %u words): %#x %#x %#x %#x %#x %#x\n", step, which,
        words, message[0], message[1], message[2], message[3], message[4], message[5]);
   }
  }
  if (words == 0) {
   break;
  }
 }
 return answered;
}
static bool
hfi_send(const char *what, uint32_t id, uint32_t *message, uint32_t words)
{
 volatile struct hfi_queue *queue = &hfi->queue[0];
 volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(0));
 uint32_t at = queue->write % HFI_QUEUE_WORDS, number = ++hfi_number & 0xfff, verdict = 0, waiting = 0;
 bool answered = false;
 message[0] = id | words << 8 | number << 20;
 printf("macan-gpu: %u: send the GMU %s: message %u, %u words, number %u\n", step, what, id, words, number);
 snprintf(last_write, sizeof(last_write), "step %u sent the GMU message %u (%s)", step, id, what);
 for (uint32_t i = 0; i < (words + 3) / 4 * 4; i++) {
  data[at] = i < words ? message[i] : 0xfafafafa;
  at = (at + 1) % HFI_QUEUE_WORDS;
 }
 queue->write = at;
 settle();
 wr(gmu.base + GMUCX_HOST2GMU_INTR_SET, 1);
 for (int i = 0; i < 10000 && !answered; i++) {
  sleep_us(100);
  waiting = rd(gmu.base + GMUCX_GMU2HOST_INTR_INFO);
  if (waiting & 1) {
   wr(gmu.base + GMUCX_GMU2HOST_INTR_CLR, 1);
   answered = hfi_receive(1, number, &verdict);
  }
 }
 hfi_receive(2, ~0u, &waiting);
 if (!answered || verdict != 0) {
  printf("macan-gpu: %u: the GMU %s (%#x) to %s; fw_init_result %#x, gmu2host_intr_info %#x\n", step,
      answered ? "says no" : "does not answer", verdict, what, rd(gmu.base + GMUCX_CM3_FW_INIT_RESULT),
      rd(gmu.base + GMUCX_GMU2HOST_INTR_INFO));
  space_faulted(&gmu_space);
  return false;
 }
 printf("macan-gpu: %u: the GMU accepts %s\n", step, what);
 return true;
}
static bool
rail_vote(uint32_t level, const uint16_t *rail, unsigned rail_levels, const uint16_t *mx, unsigned mx_levels,
    uint32_t *vote, uint32_t *mx_at)
{
 unsigned at, with;
 for (at = 0; at < rail_levels && rail[at] < level; at++) {
 }
 if (at == rail_levels || mx_levels == 0) {
  return false;
 }
 for (with = 0; with + 1 < mx_levels && mx[with] < rail[at]; with++) {
 }
 *vote = (uint32_t)rail[at] << 16 | with << 8 | at;
 *mx_at = with;
 return true;
}
static unsigned
rpmh_levels(const char *name, uint16_t *levels)
{
 const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
 char id[8] = { 0 };
 strlcpy(id, name, sizeof(id));
 for (unsigned s = 0; s < 8; s++) {
  const struct cmd_db_slave *slave = &db->slave[s];
  const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);
  if (slave->id == 0 || slave->count > 512 || slave->header_offset > CMD_DB_SIZE - 0x1000) {
   continue;
  }
  for (unsigned i = 0; i < slave->count; i++) {
   const uint8_t *data = db->data + slave->data_offset + entry[i].offset;
   unsigned count = 0;
   if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
    continue;
   }
   for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
    levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
    if (l > 0 && levels[l] == 0) {
     break;
    }
    count = l + 1;
   }
   return count;
  }
 }
 return 0;
}
static bool
step_hfi(void)
{
 static const struct {
  uint32_t khz, level;
 } speeds[] = { { 195000, 52 }, { 301000, 56 }, { 403000, 64 } };
 static const struct {
  uint32_t khz, level;
 } gmu_speeds[] = { { 475000, 0x38 } };
 static uint32_t table[200], bw[160];
 uint32_t core_start[2] = { 0, 0 }, words, vote, mx_at;
 uint16_t gfx[16], cx[16], mx[16];
 unsigned gfx_levels, cx_levels, mx_levels, n = sizeof(speeds) / sizeof(speeds[0]) + 1;
 if (!cx_ready() || !gmu_runs || hfi == NULL) {
  printf("macan-gpu: %u: needs a running GMU (steps 10 to 12 in this run)\n", step);
  return false;
 }
 gfx_levels = rpmh_levels("gfx.lvl", gfx);
 cx_levels = rpmh_levels("cx.lvl", cx);
 mx_levels = rpmh_levels("mx.lvl", mx);
 if (!gfx_levels || !cx_levels || !mx_levels) {
  printf("macan-gpu: %u: the command DB lacks a rail\n", step);
  return false;
 }
 announce("send the GMU the table of the 3D core's speeds and its own (H2F_MSG_TABLE, HFI_TABLE_GPU_PERF)");
 table[1] = 0;
 table[2] = HFI_TABLE_GPU_PERF;
 words = 3;
 table[words++] = n;
 table[words++] = 3;
 table[words++] = 0;
 table[words++] = 0xffffffff;
 table[words++] = 0;
 for (unsigned i = 0; i + 1 < n; i++) {
  if (!rail_vote(speeds[i].level, gfx, gfx_levels, mx, mx_levels, &vote, &mx_at)) {
   printf("macan-gpu: %u: no gfx.lvl level for %u\n", step, speeds[i].level);
   return false;
  }
  table[words++] = vote;
  table[words++] = 0x3ffffu << 14 | (mx_at & 0x3f) << 8 | 0xff;
  table[words++] = speeds[i].khz;
  printf("macan-gpu: %u: %u kHz: vote %#x\n", step, speeds[i].khz, vote);
 }
 table[words++] = 2;
 table[words++] = 2;
 table[words++] = 0;
 table[words++] = 0;
 if (!rail_vote(gmu_speeds[0].level, cx, cx_levels, mx, mx_levels, &vote, &mx_at)) {
  printf("macan-gpu: %u: no cx.lvl level for the GMU\n", step);
  return false;
 }
 table[words++] = vote;
 table[words++] = gmu_speeds[0].khz;
 if (!hfi_send("the table of speeds", HFI_TABLE, table, words)) {
  return false;
 }
 announce("send the GMU a bandwidth table with one level, off, and no commands (as Linux's 7c3 table)");
 bzero(bw, sizeof(bw));
 bw[1] = 1;
 if (!hfi_send("the bandwidth table", HFI_BW_TABLE, bw, 160)) {
  return false;
 }
 announce("tell the GMU to start (H2F_MSG_CORE_FW_START)");
 if (!hfi_send("core start", HFI_CORE_FW_START, core_start, 2)) {
  return false;
 }
 announce("tell the GMU the speed wanted now (H2F_MSG_GX_BW_PERF_VOTE: level 1, 195 MHz)");
 {
  uint32_t perf_vote[4] = { 0, 1 , 1, 0xff };
  if (!hfi_send("the speed wanted now", HFI_GX_BW_PERF_VOTE, perf_vote, 4)) {
   return false;
  }
 }
 gmu_talks = true;
 printf("macan-gpu: %u: THE GMU TAKES OUR MESSAGES\n", step);
 return true;
}
static struct block gpu = { "gpu", 0x03d00000, 0x40000 };
#define GPU_RBBM_STATUS (4 * 0x012)
static bool
step_gx(void)
{
 uint32_t info = 0, status;
 if (!cx_ready() || !gmu_talks) {
  printf("macan-gpu: %u: needs step 13 in this run\n", step);
  return false;
 }
 announce("ask the GMU to power the 3D core (oob_gpu: host2gmu bit 30, answer gmu2host bit 31)");
 get(&gmu, "gfx_pwr_clk_status", GMUCX_GFX_PWR_CLK_STATUS);
 put(&gmu, "host2gmu_intr_set", GMUCX_HOST2GMU_INTR_SET, GMU_OOB_GPU_SET);
 gpu_oob_held = true;
 for (int i = 0; i < 50000 && !(info & GMU_OOB_GPU_CHECK); i++) {
  sleep_us(100);
  info = rd(gmu.base + GMUCX_GMU2HOST_INTR_INFO);
  if (i % 5000 == 4999) {
   printf("macan-gpu: %u: after %u ms: gmu2host %#x, gfx_pwr_clk_status %#x, rpmh_power_state %#x\n", step,
       (i + 1) / 10, info, rd(gmu.base + GMUCX_GFX_PWR_CLK_STATUS), rd(gmu.base + GMUCX_RPMH_POWER_STATE));
  }
 }
 get(&gmu, "gmu2host_intr_info", GMUCX_GMU2HOST_INTR_INFO);
 status = get(&gmu, "gfx_pwr_clk_status", GMUCX_GFX_PWR_CLK_STATUS);
 get(&gmu, "rpmh_power_state", GMUCX_RPMH_POWER_STATE);
 hfi_receive(2, ~0u, &info);
 if (!(rd(gmu.base + GMUCX_GMU2HOST_INTR_INFO) & GMU_OOB_GPU_CHECK) || (status & 3)) {
  printf("macan-gpu: %u: the GMU does not power the 3D core (gfx_pwr_clk_status %#x)\n", step, status);
  gmu_post_mortem();
  return false;
 }
 wr(gmu.base + GMUCX_GMU2HOST_INTR_CLR, GMU_OOB_GPU_CHECK);
 announce("THE 3D CORE HAS POWER AND CLOCK: read its status register (rbbm_status)");
 get(&gpu, "rbbm_status", GPU_RBBM_STATUS);
 printf("macan-gpu: %u: THE 3D CORE IS UP\n", step);
 return true;
}
#define GPU_REG(words) ((uint32_t)(words) * 4u)
#define CP_RB_BASE_LO_GC GPU_REG(0x800)
#define CP_RB_BASE_HI_GC GPU_REG(0x801)
#define CP_RB_CNTL_GC GPU_REG(0x802)
#define CP_RB_RPTR_ADDR_LO_BR GPU_REG(0x804)
#define CP_RB_RPTR_ADDR_HI_BR GPU_REG(0x805)
#define CP_RB_RPTR_BR GPU_REG(0x806)
#define CP_RB_WPTR_GC GPU_REG(0x807)
#define CP_RB_RPTR_ADDR_LO_BV GPU_REG(0x808)
#define CP_RB_RPTR_ADDR_HI_BV GPU_REG(0x809)
#define CP_SQE_CNTL GPU_REG(0x815)
#define CP_SQE_INSTR_BASE_LO GPU_REG(0x816)
#define CP_SQE_INSTR_BASE_HI GPU_REG(0x817)
#define CP_APERTURE_CNTL_HOST GPU_REG(0x81c)
#define CP_APRIV_CNTL_PIPE GPU_REG(0x84d)
#define CP_HW_FAULT_STATUS_PIPE GPU_REG(0x8b3)
#define CP_INTERRUPT_STATUS_GLOBAL GPU_REG(0x8b5)
#define CP_INTERRUPT_STATUS_PIPE GPU_REG(0x8b7)
#define CP_SQE_STATUS_PIPE GPU_REG(0x8bb)
#define RBBM_INT_0_STATUS GPU_REG(0x06a)
#define RBBM_SECVID_TSB_CNTL GPU_REG(0xf803)
#define RBBM_SECVID_TSB_BASE_LO GPU_REG(0xf800)
#define RBBM_SECVID_TSB_BASE_HI GPU_REG(0xf801)
#define RBBM_SECVID_TSB_SIZE GPU_REG(0xf802)
#define GMUAO_AHB_FENCE_STATUS GMU_REG(0x23b13)
#define PIPE_NONE 0
#define PIPE_BR 1
#define PIPE_BV 2
#define APRIV_DEFAULT 0xfu
#define APRIV_BR_DEFAULT (APRIV_DEFAULT | 1u << 6 | 1u << 5)
#define SECURE_BASE 0xf0000000ull
#define SECURE_SIZE 0xeffff000u
#define CORE_RING_ADDRESS 0x00100000ull
#define CORE_RING_SIZE 0x8000u
#define CORE_RING_WORDS (CORE_RING_SIZE / 4)
#define CORE_SQE_ADDRESS 0x00200000ull
#define CORE_SCRATCH_ADDRESS 0x00400000ull
#define CORE_PWRUP_ADDRESS 0x00500000ull
#define CORE_STREAM 0
#define CORE_MATCH 0
#define LPAC_STREAM 1
#define LPAC_MATCH 2
#define CP_THREAD_CONTROL 0x17
#define CP_ME_INIT 0x48
static struct gpu_space core_space = { .name = "the 3D core", .context = 0 };
static struct gpu_block core_ring, core_sqe, core_scratch, core_pwrup;
static bool core_space_is_on, lpac_stream_is_on, core_started;
static uint32_t ring_at, ring_sent;
static uint32_t
odd_parity(uint32_t value)
{
 value ^= value >> 16;
 value ^= value >> 8;
 value ^= value >> 4;
 return (~0x6996u >> (value & 0xf)) & 1;
}
static void
ring(uint32_t word)
{
 ((volatile uint32_t *)core_ring.memory)[ring_at] = word;
 ring_at = (ring_at + 1) % CORE_RING_WORDS;
}
static void
ring_packet(uint32_t opcode, uint32_t words)
{
 ring(7u << 28 | (words & 0x3fff) | odd_parity(words) << 15 | (opcode & 0x7f) << 16 | odd_parity(opcode) << 23);
}
static void
aperture(uint32_t pipe)
{
 wr(gpu.base + CP_APERTURE_CNTL_HOST, pipe << 12);
 settle();
}
static uint32_t
core_rptr(void)
{
 uint32_t value;
 to_memory(core_scratch.memory, 64);
 value = *(volatile uint32_t *)core_scratch.memory;
 return value;
}
static bool
step_core_space(void)
{
 const uint8_t *microcode;
 uint64_t size = 0;
 if (!cx_ready() || !gpu_oob_held) {
  printf("macan-gpu: %u: needs step 14 in this run\n", step);
  return false;
 }
 announce("make the 3D core's memory: its ring, its microcode (fw/gen80900_sqe.fw), a page for how far it has read, "
     "and an empty power-up register list");
 microcode = image_file("fw", "gen80900_sqe.fw", &size);
 if (microcode == NULL || size < 8 || size > 0x80000 ||
     !block_new(&core_space, &core_ring, CORE_RING_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
     !block_new(&core_space, &core_sqe, CORE_SQE_ADDRESS, (uint32_t)size - 4, PTE_CORE | PTE_READ_ONLY) ||
     !block_new(&core_space, &core_scratch, CORE_SCRATCH_ADDRESS, 4096, PTE_CORE) ||
     !block_new(&core_space, &core_pwrup, CORE_PWRUP_ADDRESS, 4096, PTE_CORE)) {
  return false;
 }
 memcpy((void *)core_sqe.memory, microcode + 4, size - 4);
 to_memory(core_sqe.memory, core_sqe.size);
 memset((void *)core_ring.memory, 0xaa, CORE_RING_SIZE);
 to_memory(core_ring.memory, CORE_RING_SIZE);
 printf("macan-gpu: %u: microcode version word %#x, %llu bytes\n", step, *(const uint32_t *)(microcode + 4), size - 4);
 announce("give the SMMU's context 0 these tables and send the 3D core's requests (stream 0) through it");
 core_space_is_on = true;
 if (!space_on(&core_space, CORE_MATCH, CORE_STREAM)) {
  return false;
 }
 announce("send stream 1 (the LPAC pipe's) through the same context 0");
 lpac_stream_is_on = true;
 if (!space_on(&core_space, LPAC_MATCH, LPAC_STREAM)) {
  return false;
 }
 get(&gpu, "rbbm_status", GPU_RBBM_STATUS);
 return !space_faulted(&core_space);
}
struct gen8_register {
 uint32_t offset, value, pipes;
};
static const struct gen8_register gen8_9_0_nonctxt[] = {
 { 0x00008, 0x22122212, 0x1 },
 { 0x00010, 0x00000030, 0x1 },
 { 0x00011, 0x00000030, 0x1 },
 { 0x08e01, 0x00000020, 0x6 },
 { 0x0002f, 0x0fffffff, 0x1 },
 { 0x0007d, 0x70809060, 0x1 },
 { 0x0007e, 0x30000000, 0x1 },
 { 0x0009d, 0x00000040, 0x1 },
 { 0x0ae03, 0x04000000, 0x1 },
 { 0x0ae05, 0x00000008, 0x1 },
 { 0x0ae09, 0x00c21800, 0x1 },
 { 0x00440, 0x00000001, 0x1 },
 { 0x00500, 0x00000001, 0x1 },
 { 0x005ec, 0x00000001, 0x1 },
 { 0x00814, 0x00000101, 0x1 },
 { 0x00e01, 0x80080000, 0x1 },
 { 0x00e10, 0x00000020, 0x1 },
 { 0x00e12, 0x010240e0, 0x1 },
 { 0x00f01, 0x00001000, 0x1 },
 { 0x00f02, 0x00000800, 0x1 },
 { 0x08600, 0x00200000, 0x6 },
 { 0x08702, 0x00000800, 0x6 },
 { 0x08e07, 0x00000068, 0x2 },
 { 0x08e06, 0x00002200, 0x2 },
 { 0x08e04, 0x00020000, 0x6 },
 { 0x08e09, 0x02600000, 0x2 },
 { 0x08f01, 0x00000007, 0x2 },
 { 0x08f02, 0x00004000, 0x2 },
 { 0x09741, 0x00000001, 0x6 },
 { 0x09e0a, 0x00000001, 0x6 },
 { 0x09e0d, 0x10010000, 0x6 },
 { 0x09e22, 0x00500000, 0x6 },
 { 0x09e23, 0x00500050, 0x6 },
 { 0x09e50, 0x00000003, 0x6 },
 { 0x09e64, 0x00000002, 0x6 },
 { 0x09f20, 0x00000200, 0x6 },
 { 0x0a600, 0x00008000, 0x6 },
 { 0x0a639, 0x00500050, 0x6 },
 { 0x0a63a, 0x00600060, 0x6 },
 { 0x0a63b, 0x00200020, 0x6 },
 { 0x0a63c, 0x00000020, 0x6 },
 { 0x0ae0a, 0x00300000, 0x1 },
 { 0x0ae0f, 0x0000003f, 0x1 },
 { 0x0ae6c, 0x00004000, 0x1 },
 { 0x0ae73, 0x00020000, 0x1 },
 { 0x0ae74, 0x01000000, 0x1 },
 { 0x0ae6b, 0x00000080, 0x1 },
 { 0x0ae6d, 0x0001ff00, 0x1 },
 { 0x0b600, 0x10100000, 0x1 },
 { 0x0b602, 0x04000720, 0x1 },
 { 0x0ae76, 0x00000080, 0x1 },
};
static const struct gen8_register gen8_bicubic[] = {
 { 0x0b607, 0x3fe05ff4, 0x1 },
 { 0x0b608, 0x3fa0ebee, 0x1 },
 { 0x0b609, 0x3f5193ed, 0x1 },
 { 0x0b60a, 0x3f0243f0, 0x1 },
 { 0x0b60b, 0x00000000, 0x1 },
 { 0x0b60c, 0x3fd093e8, 0x1 },
 { 0x0b60d, 0x3f4133dc, 0x1 },
 { 0x0b60e, 0x3ea1dfdb, 0x1 },
 { 0x0b60f, 0x3e0283e0, 0x1 },
 { 0x0b610, 0x0000ac2b, 0x1 },
 { 0x0b611, 0x0000f01d, 0x1 },
 { 0x0b612, 0x00114412, 0x1 },
 { 0x0b613, 0x0021980a, 0x1 },
 { 0x0b614, 0x0051ec05, 0x1 },
 { 0x0b615, 0x0000380e, 0x1 },
 { 0x0b616, 0x3ff09001, 0x1 },
 { 0x0b617, 0x3fc10bfa, 0x1 },
 { 0x0b618, 0x3f9193f7, 0x1 },
 { 0x0b619, 0x3f7227f7, 0x1 },
};
#define RBBM_PERFCTR_GPU_BUSY_MASKED GPU_REG(0x0019e)
#define UCHE_CCHE_GC_GMEM_RANGE_MIN_LO GPU_REG(0x00f08)
#define UCHE_CCHE_GC_GMEM_RANGE_MIN_HI GPU_REG(0x00f09)
#define SP_HLSQ_GC_GMEM_RANGE_MIN_LO GPU_REG(0x0ae10)
#define SP_HLSQ_GC_GMEM_RANGE_MIN_HI GPU_REG(0x0ae11)
#define UCHE_CCHE_LPAC_GMEM_RANGE_MIN_LO GPU_REG(0x00f0a)
#define UCHE_CCHE_LPAC_GMEM_RANGE_MIN_HI GPU_REG(0x00f0b)
#define SP_HLSQ_LPAC_GMEM_RANGE_MIN_LO GPU_REG(0x0ae12)
#define SP_HLSQ_LPAC_GMEM_RANGE_MIN_HI GPU_REG(0x0ae13)
#define UCHE_TRAP_BASE_LO GPU_REG(0x00e08)
#define UCHE_TRAP_BASE_HI GPU_REG(0x00e09)
#define UCHE_WRITE_THRU_BASE_LO GPU_REG(0x00e06)
#define UCHE_WRITE_THRU_BASE_HI GPU_REG(0x00e07)
#define UCHE_CCHE_TRAP_BASE_LO GPU_REG(0x00f06)
#define UCHE_CCHE_TRAP_BASE_HI GPU_REG(0x00f07)
#define UCHE_CCHE_WRITE_THRU_BASE_LO GPU_REG(0x00f04)
#define UCHE_CCHE_WRITE_THRU_BASE_HI GPU_REG(0x00f05)
#define GRAS_NC_MODE_CNTL GPU_REG(0x08700)
#define RB_CCU_NC_MODE_CNTL GPU_REG(0x08e08)
#define RB_CMP_NC_MODE_CNTL GPU_REG(0x08f00)
#define SP_NC_MODE_CNTL GPU_REG(0x0ae02)
#define TPL1_NC_MODE_CNTL GPU_REG(0x0b604)
#define UCHE_CLIENT_PF GPU_REG(0x00e11)
#define RBBM_INTERFACE_HANG_INT_CNTL GPU_REG(0x0002f)
#define RBBM_SLICE_INTERFACE_HANG_INT_CNTL GPU_REG(0x0058f)
#define CP_INTERRUPT_STATUS_MASK_PIPE GPU_REG(0x008b8)
#define CP_HW_FAULT_STATUS_MASK_PIPE GPU_REG(0x008b4)
#define CP_INTERRUPT_STATUS_MASK_GLOBAL GPU_REG(0x008b6)
#define RBBM_SECVID_TRUST_CNTL GPU_REG(0x0f400)
#define RB_GC_GMEM_PROTECT GPU_REG(0x08e09)
#define UCHE_GMEM_BASE 0x4000000u
#define UCHE_TRAP_BASE 0x1fffffffff000ull
#define NC_HBB 3u
#define NC_HBB_LO (NC_HBB & 3)
#define NC_HBB_HI ((NC_HBB >> 2) & 1)
#define NC_MAL 0u
#define NC_YUVNOTCOMPTOFC 1u
#define NC_MODE2 5u
#define HANG_DETECT_CYCLES 0xcfffffu
static void
gen8_registers(const struct gen8_register *table, unsigned count)
{
 for (uint32_t pipe = PIPE_NONE; pipe <= PIPE_BV; pipe++) {
  aperture(pipe);
  for (unsigned i = 0; i < count; i++) {
   if (table[i].pipes & 1u << pipe) {
    wr(gpu.base + GPU_REG(table[i].offset), table[i].value);
   }
  }
 }
 aperture(PIPE_NONE);
}
static void
gen8_start_rest(void)
{
 announce("set up the 3D core the rest of the way gen8_start does: GMEM, the caches' bases, the bank bits and "
     "compression mode, the bicubic table, the non-context registers of each pipe, hang detection");
 put(&gpu, "perfctr_gpu_busy_masked", RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
 put(&gpu, "uche_cche_gc_gmem_range_min_lo", UCHE_CCHE_GC_GMEM_RANGE_MIN_LO, UCHE_GMEM_BASE);
 put(&gpu, "uche_cche_gc_gmem_range_min_hi", UCHE_CCHE_GC_GMEM_RANGE_MIN_HI, 0);
 put(&gpu, "sp_hlsq_gc_gmem_range_min_lo", SP_HLSQ_GC_GMEM_RANGE_MIN_LO, UCHE_GMEM_BASE);
 put(&gpu, "sp_hlsq_gc_gmem_range_min_hi", SP_HLSQ_GC_GMEM_RANGE_MIN_HI, 0);
 put(&gpu, "uche_cche_lpac_gmem_range_min_lo", UCHE_CCHE_LPAC_GMEM_RANGE_MIN_LO, UCHE_GMEM_BASE);
 put(&gpu, "uche_cche_lpac_gmem_range_min_hi", UCHE_CCHE_LPAC_GMEM_RANGE_MIN_HI, 0);
 put(&gpu, "sp_hlsq_lpac_gmem_range_min_lo", SP_HLSQ_LPAC_GMEM_RANGE_MIN_LO, UCHE_GMEM_BASE);
 put(&gpu, "sp_hlsq_lpac_gmem_range_min_hi", SP_HLSQ_LPAC_GMEM_RANGE_MIN_HI, 0);
 put(&gpu, "uche_trap_base_lo", UCHE_TRAP_BASE_LO, (uint32_t)UCHE_TRAP_BASE);
 put(&gpu, "uche_trap_base_hi", UCHE_TRAP_BASE_HI, (uint32_t)(UCHE_TRAP_BASE >> 32));
 put(&gpu, "uche_write_thru_base_lo", UCHE_WRITE_THRU_BASE_LO, (uint32_t)UCHE_TRAP_BASE);
 put(&gpu, "uche_write_thru_base_hi", UCHE_WRITE_THRU_BASE_HI, (uint32_t)(UCHE_TRAP_BASE >> 32));
 put(&gpu, "uche_cche_trap_base_lo", UCHE_CCHE_TRAP_BASE_LO, (uint32_t)UCHE_TRAP_BASE);
 put(&gpu, "uche_cche_trap_base_hi", UCHE_CCHE_TRAP_BASE_HI, (uint32_t)(UCHE_TRAP_BASE >> 32));
 put(&gpu, "uche_cche_write_thru_base_lo", UCHE_CCHE_WRITE_THRU_BASE_LO, (uint32_t)UCHE_TRAP_BASE);
 put(&gpu, "uche_cche_write_thru_base_hi", UCHE_CCHE_WRITE_THRU_BASE_HI, (uint32_t)(UCHE_TRAP_BASE >> 32));
 aperture(PIPE_BV);
 put(&gpu, "gras_nc_mode_cntl (bv)", GRAS_NC_MODE_CNTL, NC_HBB << 5);
 aperture(PIPE_BR);
 put(&gpu, "gras_nc_mode_cntl (br)", GRAS_NC_MODE_CNTL, NC_HBB << 5);
 put(&gpu, "rb_ccu_nc_mode_cntl (br)", RB_CCU_NC_MODE_CNTL, NC_YUVNOTCOMPTOFC << 6 | NC_HBB_HI << 3 | NC_HBB_LO << 1);
 put(&gpu, "rb_cmp_nc_mode_cntl (br)", RB_CMP_NC_MODE_CNTL, NC_MODE2 << 15 | NC_YUVNOTCOMPTOFC << 6 | NC_MAL);
 aperture(PIPE_NONE);
 put(&gpu, "sp_nc_mode_cntl", SP_NC_MODE_CNTL, NC_HBB_HI << 10 | 2u << 4 | NC_MAL << 3 | NC_HBB_LO << 1);
 put(&gpu, "tpl1_nc_mode_cntl", TPL1_NC_MODE_CNTL, NC_HBB_HI << 4 | NC_MAL << 3 | NC_HBB_LO << 1);
 gen8_registers(gen8_bicubic, sizeof(gen8_bicubic) / sizeof(gen8_bicubic[0]));
 put(&gpu, "uche_client_pf", UCHE_CLIENT_PF, 1u << 7 | 1);
 gen8_registers(gen8_9_0_nonctxt, sizeof(gen8_9_0_nonctxt) / sizeof(gen8_9_0_nonctxt[0]));
 printf("macan-gpu: %u: %u non-context and %u bicubic registers written\n", step,
     (unsigned)(sizeof(gen8_9_0_nonctxt) / sizeof(gen8_9_0_nonctxt[0])),
     (unsigned)(sizeof(gen8_bicubic) / sizeof(gen8_bicubic[0])));
 put(&gpu, "interface_hang_int_cntl", RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | HANG_DETECT_CYCLES);
 put(&gpu, "slice_interface_hang_int_cntl", RBBM_SLICE_INTERFACE_HANG_INT_CNTL, 1u << 30);
}
static bool
step_core_start(void)
{
 uint32_t taken = ~0u, status = ~0u, fence;
 if (!core_space_is_on) {
  printf("macan-gpu: %u: needs step 15 in this run\n", step);
  return false;
 }
 announce("open the register fence, give each pipe of the command processor its privileges, and the secure window");
 put(&gmu, "ahb_fence_ctrl (allow)", GMUAO_AHB_FENCE_CTRL, 0);
 gen8_start_rest();
 for (uint32_t pipe = PIPE_BR; pipe <= PIPE_BV; pipe++) {
  aperture(pipe);
  put(&gpu, pipe == PIPE_BR ? "cp_apriv_cntl (br)" : "cp_apriv_cntl (bv)", CP_APRIV_CNTL_PIPE,
      pipe == PIPE_BR ? APRIV_BR_DEFAULT : APRIV_DEFAULT);
 }
 aperture(PIPE_NONE);
 put(&gpu, "secvid_tsb_cntl", RBBM_SECVID_TSB_CNTL, 0);
 put(&gpu, "secvid_tsb_base_lo", RBBM_SECVID_TSB_BASE_LO, (uint32_t)SECURE_BASE);
 put(&gpu, "secvid_tsb_base_hi", RBBM_SECVID_TSB_BASE_HI, (uint32_t)(SECURE_BASE >> 32));
 put(&gpu, "secvid_tsb_size", RBBM_SECVID_TSB_SIZE, SECURE_SIZE);
 announce("tell the command processor where its ring and microcode are, and start it (cp_sqe_cntl 1)");
 put(&gpu, "rptr_addr_lo (br)", CP_RB_RPTR_ADDR_LO_BR, (uint32_t)core_scratch.address);
 put(&gpu, "rptr_addr_hi (br)", CP_RB_RPTR_ADDR_HI_BR, (uint32_t)(core_scratch.address >> 32));
 put(&gpu, "rptr_addr_lo (bv)", CP_RB_RPTR_ADDR_LO_BV, (uint32_t)(core_scratch.address + 4));
 put(&gpu, "rptr_addr_hi (bv)", CP_RB_RPTR_ADDR_HI_BV, (uint32_t)((core_scratch.address + 4) >> 32));
 put(&gpu, "rb_cntl", CP_RB_CNTL_GC, 12u | 2u << 8);
 put(&gpu, "rb_base_lo", CP_RB_BASE_LO_GC, (uint32_t)core_ring.address);
 put(&gpu, "rb_base_hi", CP_RB_BASE_HI_GC, (uint32_t)(core_ring.address >> 32));
 put(&gpu, "sqe_instr_base_lo", CP_SQE_INSTR_BASE_LO, (uint32_t)core_sqe.address);
 put(&gpu, "sqe_instr_base_hi", CP_SQE_INSTR_BASE_HI, (uint32_t)(core_sqe.address >> 32));
 put(&gpu, "sqe_cntl", CP_SQE_CNTL, 1);
 core_started = true;
 sleep_ms(1);
 if (space_faulted(&core_space)) {
  return false;
 }
 announce("give the command processor its first packets: CP_THREAD_CONTROL, CP_ME_INIT (gen8_cp_init_cmds)");
 ring_at = ring_sent = 0;
 ring_packet(CP_THREAD_CONTROL, 1);
 ring(1u << 27);
 ring_packet(CP_ME_INIT, 7);
 ring(1u << 0 | 1u << 1 | 1u << 3 | 1u << 6 | 1u << 8);
 ring(0x00000003);
 ring(0x20000000);
 ring(0x00000002);
 ring((uint32_t)core_pwrup.address);
 ring((uint32_t)(core_pwrup.address >> 32));
 ring(1u << 31);
 to_memory(core_ring.memory, 4 * ring_at);
 ring_sent = ring_at;
 put(&gpu, "rb_wptr", CP_RB_WPTR_GC, ring_at);
 fence = get(&gmu, "ahb_fence_status", GMUAO_AHB_FENCE_STATUS);
 if (fence & 3) {
  printf("macan-gpu: %u: the fence dropped the write of wptr (%#x): written again\n", step, fence);
  put(&gpu, "rb_wptr", CP_RB_WPTR_GC, ring_at);
 }
 for (int i = 0; i < 20000 && (taken != ring_at || (status & ~1u)); i++) {
  sleep_us(100);
  taken = core_rptr();
  status = rd(gpu.base + GPU_RBBM_STATUS);
 }
 aperture(PIPE_BR);
 printf("macan-gpu: %u: ring read up to %u of %u (register %u), rbbm_status %#x, rbbm_int %#x, cp global %#x, "
     "br: interrupts %#x, hw fault %#x, sqe status %#x\n", step, taken, ring_at, rd(gpu.base + CP_RB_RPTR_BR), status,
     rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_INTERRUPT_STATUS_GLOBAL), rd(gpu.base + CP_INTERRUPT_STATUS_PIPE),
     rd(gpu.base + CP_HW_FAULT_STATUS_PIPE), rd(gpu.base + CP_SQE_STATUS_PIPE));
 aperture(PIPE_NONE);
 if (space_faulted(&core_space) || taken != ring_at) {
  printf("macan-gpu: %u: the command processor did not get through its first packets\n", step);
  return false;
 }
 printf("macan-gpu: %u: THE COMMAND PROCESSOR RUNS: it read CP_ME_INIT and is idle\n", step);
 return true;
}
#define ZAP_MEMORY 0x9959a000ull
#define ZAP_MEMORY_SIZE 0x2000u
#define PAS_GPU 13
#define ELF_SEGMENT_TYPE(flags) (((flags) >> 24) & 7)
#define ELF_SEGMENT_HASH 2
#define ELF_RELOCATABLE (1u << 27)
static bool zap_loaded;
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
 uint64_t session = 0;
 function |= SCM_64 | SCM_SIP;
 printf("macan-gpu: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function,
     first, second, third);
 snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
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
   printf("macan-gpu: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
   *answer = x1;
   return (int32_t)x0;
  }
 }
 printf("macan-gpu: %u: TrustZone keeps saying the call was interrupted\n", step);
 return -1;
}
static bool
zap_unload(void)
{
 uint64_t answer = ~0ull;
 return scm("to give up the zap shader", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_GPU, 0, 0, &answer) == 0 && answer == 0;
}
static uint32_t
le32(const uint8_t *p)
{
 return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool
step_zap_load(void)
{
 const uint8_t *image, *table, *code = NULL, *hashes = NULL;
 uint32_t segments, header_size = 0, code_size = 0, code_at = 0, hashes_size = 0, lowest = ~0u, highest = 0;
 uint64_t size = 0, answer = ~0ull;
 vm_offset_t page = 0, memory;
 int32_t result;
 uint32_t want = 1;
 if (!core_started) {
  printf("macan-gpu: %u: needs step 16 in this run\n", step);
  return false;
 }
 if (PE_parse_boot_argn("macan_gpu_zap", &want, sizeof(want)) && want == 0) {
  printf("macan-gpu: %u: macan_gpu_zap=0: no zap shader in this run (step 18 then tries without it)\n", step);
  return true;
 }
 announce("ask TrustZone to give up a zap shader a kernel before this one may have left");
 printf("macan-gpu: %u: %s\n", step, zap_unload() ? "TrustZone has given up what it had (it says so with none, too)" :
     "TrustZone does not give it up");
 image = image_file("fw", "gen80900_zap.mbn", &size);
 if (image == NULL || size < 52 || memcmp(image, "\177ELF\1", 5) != 0) {
  return false;
 }
 segments = image[44] | image[45] << 8;
 if (segments < 2 || segments > 8 || 52 + 32 * (uint64_t)segments > size) {
  printf("macan-gpu: %u: not the image expected (%u segments)\n", step, segments);
  return false;
 }
 table = image + 52;
 for (uint32_t i = 0; i < segments; i++) {
  const uint8_t *e = table + 32 * i;
  uint32_t type = le32(e), offset = le32(e + 4), paddr = le32(e + 12), filesz = le32(e + 16), memsz = le32(e + 20);
  uint32_t flags = le32(e + 24);
  if ((uint64_t)offset + filesz > size) {
   printf("macan-gpu: %u: segment %u goes past the file\n", step, i);
   return false;
  }
  if (i == 0) {
   header_size = filesz;
  } else if (ELF_SEGMENT_TYPE(flags) == ELF_SEGMENT_HASH) {
   hashes = image + offset;
   hashes_size = filesz;
  } else if (type == 1 && filesz != 0) {
   if (code != NULL || !(flags & ELF_RELOCATABLE)) {
    printf("macan-gpu: %u: more than one loadable segment, or not relocatable: not handled\n", step);
    return false;
   }
   code = image + offset;
   code_size = filesz;
   code_at = paddr;
   lowest = paddr < lowest ? paddr : lowest;
   highest = paddr + memsz > highest ? paddr + memsz : highest;
  }
 }
 if (header_size == 0 || hashes == NULL || code == NULL || header_size + hashes_size > PAGE_SIZE ||
     ((highest - lowest + 4095) & ~4095u) > ZAP_MEMORY_SIZE) {
  printf("macan-gpu: %u: header %u, hashes %u, code %u bytes at %#x: not what TrustZone takes here\n", step,
      header_size, hashes_size, code_size, code_at);
  return false;
 }
 printf("macan-gpu: %u: zap shader: header %u bytes, hashes %u, code %u at %#x\n", step, header_size, hashes_size,
     code_size, code_at);
 announce("hand TrustZone the header and hashes (image 13), tell it where the image goes, put the code there, "
     "and ask it to authenticate and start it");
 if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
  return false;
 }
 memcpy((void *)page, image, header_size);
 memcpy((void *)(page + header_size), hashes, hashes_size);
 to_memory(page, PAGE_SIZE);
 result = scm("to take the zap shader's signature", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU, kvtophys(page), 0,
     &answer);
 if (result != 0 || answer != 0) {
  printf("macan-gpu: %u: TRUSTZONE DOES NOT TAKE THE SIGNATURE (%d, %#llx)\n", step, result, answer);
  return false;
 }
 result = scm("where the zap shader goes", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_GPU, ZAP_MEMORY,
     (highest - lowest + 4095) & ~4095u, &answer);
 if (result != 0 || answer != 0) {
  printf("macan-gpu: %u: TrustZone does not take the place (%d, %#llx)\n", step, result, answer);
  return false;
 }
 memory = ml_io_map_wcomb(ZAP_MEMORY, ZAP_MEMORY_SIZE);
 if (!memory) {
  return false;
 }
 for (uint32_t i = 0; i < code_size; i++) {
  ((volatile uint8_t *)memory)[code_at - lowest + i] = code[i];
 }
 settle();
 result = scm("to authenticate the zap shader and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_GPU, 0, 0,
     &answer);
 if (result != 0 || answer != 0) {
  printf("macan-gpu: %u: TRUSTZONE DOES NOT START THE ZAP SHADER (%d, %#llx)\n", step, result, answer);
  return false;
 }
 zap_loaded = true;
 printf("macan-gpu: %u: THE ZAP SHADER IS LOADED\n", step);
 return true;
}
#define CP_SET_SECURE_MODE 0x66
#define CP_MEM_WRITE 0x3d
#define CORE_PROOF_OFFSET 0x100
#define CORE_PROOF_VALUE 0xc0ffee29u
static bool
step_core_commands(void)
{
 uint32_t taken = ~0u, status = ~0u, proof = 0;
 uint32_t start = ring_at;
 uint32_t fence;
 if (!core_started) {
  printf("macan-gpu: %u: needs step 16 in this run\n", step);
  return false;
 }
 printf("macan-gpu: %u: before: rptr register %u, memory %u, wptr %u, gfx_pwr_clk_status %#x, rpmh_power_state %#x, "
     "ahb_fence_status %#x, rbbm_status %#x\n", step, (aperture(PIPE_BR), rd(gpu.base + CP_RB_RPTR_BR)), core_rptr(),
     rd(gpu.base + CP_RB_WPTR_GC), rd(gmu.base + GMUCX_GFX_PWR_CLK_STATUS), rd(gmu.base + GMUCX_RPMH_POWER_STATE),
     rd(gmu.base + GMUAO_AHB_FENCE_STATUS), rd(gpu.base + GPU_RBBM_STATUS));
 aperture(PIPE_NONE);
 announce(zap_loaded ? "command the 3D core out of secure mode (CP_SET_SECURE_MODE 0), and to write a word into a page "
     "of ours (CP_MEM_WRITE)" : "command the 3D core to write a word into a page of ours (CP_MEM_WRITE), no zap shader");
 *(volatile uint32_t *)(core_scratch.memory + CORE_PROOF_OFFSET) = 0;
 to_memory(core_scratch.memory, 4096);
 if (zap_loaded) {
  ring_packet(CP_SET_SECURE_MODE, 1);
  ring(0);
 }
 ring_packet(CP_MEM_WRITE, 3);
 ring((uint32_t)(core_scratch.address + CORE_PROOF_OFFSET));
 ring((uint32_t)((core_scratch.address + CORE_PROOF_OFFSET) >> 32));
 ring(CORE_PROOF_VALUE);
 to_memory(core_ring.memory, CORE_RING_SIZE);
 ring_sent = ring_at;
 put(&gpu, "rb_wptr", CP_RB_WPTR_GC, ring_at);
 fence = get(&gmu, "ahb_fence_status", GMUAO_AHB_FENCE_STATUS);
 get(&gpu, "rb_wptr (read back)", CP_RB_WPTR_GC);
 if (fence & 3) {
  printf("macan-gpu: %u: the fence dropped the write of wptr (%#x): written again\n", step, fence);
  put(&gpu, "rb_wptr", CP_RB_WPTR_GC, ring_at);
 }
 for (int i = 0; i < 20000 && (taken != ring_at || proof != CORE_PROOF_VALUE); i++) {
  sleep_us(100);
  taken = core_rptr();
  to_memory(core_scratch.memory + CORE_PROOF_OFFSET, 4);
  proof = *(volatile uint32_t *)(core_scratch.memory + CORE_PROOF_OFFSET);
  status = rd(gpu.base + GPU_RBBM_STATUS);
 }
 aperture(PIPE_BR);
 printf("macan-gpu: %u: ring read from %u up to %u of %u (register %u, wptr %u), the word %#x, rbbm_status %#x, "
     "rbbm_int %#x, cp global %#x, br: interrupts %#x, hw fault %#x, sqe status %#x; gfx_pwr_clk_status %#x\n", step,
     start, taken, ring_at, rd(gpu.base + CP_RB_RPTR_BR), rd(gpu.base + CP_RB_WPTR_GC), proof, status,
     rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_INTERRUPT_STATUS_GLOBAL), rd(gpu.base + CP_INTERRUPT_STATUS_PIPE),
     rd(gpu.base + CP_HW_FAULT_STATUS_PIPE), rd(gpu.base + CP_SQE_STATUS_PIPE), rd(gmu.base + GMUCX_GFX_PWR_CLK_STATUS));
 aperture(PIPE_NONE);
 if (space_faulted(&core_space) || proof != CORE_PROOF_VALUE) {
  printf("macan-gpu: %u: the word did not arrive\n", step);
  return false;
 }
 if (zap_loaded) {
  aperture(PIPE_BR);
  put(&gpu, "rb_gc_gmem_protect (br)", RB_GC_GMEM_PROTECT, 0x02600000);
  aperture(PIPE_NONE);
 }
 printf("macan-gpu: %u: THE 3D CORE WROTE OUR WORD: it is out of secure mode and does what it is told\n", step);
 return true;
}
#define CP_WAIT_FOR_IDLE 0x26
#define CP_EVENT_WRITE7 0x46
#define CP_SET_MARKER 0x65
#define CP_BLIT 0x2c
#define CP_CONTEXT_REG_BUNCH 0x5c
#define CP_NON_CONTEXT_REG_BUNCH 0x5d
#define EV_CCU_INVALIDATE_DEPTH 0x18
#define EV_CCU_INVALIDATE_COLOR 0x19
#define EV_CCU_CLEAN_DEPTH 0x20
#define EV_CCU_CLEAN_COLOR 0x21
#define EV_CACHE_CLEAN 0x31
#define EV_DEBUG_LABEL 0x3f
#define RM6_BLIT2DSCALE 12
#define BLIT_OP_SCALE 3
#define R_RB_CCU_CACHE_CNTL 0x88e5
#define R_VPC_ATTR_BUF_SIZE 0x9314
#define R_VPC_ATTR_BUF_BASE 0x9315
#define R_VPC_POS_BUF_SIZE 0x9316
#define R_VPC_POS_BUF_BASE 0x9317
#define R_VPC_BV_POS_BUF_SIZE 0x9318
#define R_VPC_BV_POS_BUF_BASE 0x9319
#define R_PC_ATTR_BUF_SIZE 0x9b16
#define R_PC_POS_BUF_SIZE 0x9b17
#define R_PC_BV_POS_BUF_SIZE 0x9b18
#define R_GRAS_A2D_BLT_CNTL 0x8500
#define R_GRAS_A2D_DEST_TL 0x8505
#define R_GRAS_A2D_DEST_BR 0x8506
#define R_RB_A2D_BLT_CNTL 0x8c00
#define R_RB_A2D_PIXEL_CNTL 0x8c01
#define R_RB_A2D_DEST_INFO 0x8c17
#define R_RB_A2D_DEST_BASE 0x8c18
#define R_RB_A2D_DEST_PITCH 0x8c1a
#define R_RB_A2D_CLEAR_COLOR 0x8c2c
#define R_SP_A2D_OUTPUT_INFO 0xa9bf
#define R_TPL1_A2D_BLT_CNTL 0xb2d2
#define FMT6_8_8_8_8_UNORM 0x30
#define A2D_BLT_SOLID_8888 (1u << 7 | FMT6_8_8_8_8_UNORM << 8 | 0xfu << 20)
#define A2D_OUTPUT_8888 (FMT6_8_8_8_8_UNORM << 3 | 0xfu << 12)
#define A2D_DEST_BGRA_LINEAR (FMT6_8_8_8_8_UNORM | 1u << 10)
#define TPL1_A2D_TEX_2D (1u << 29)
#define CCU_CACHE_SYSMEM (0x108000u >> 12 | 0u << 14 | (0x48000u >> 12) << 16 | 3u << 30)
#define CORE_TARGET_ADDRESS 0x01000000ull
#define TARGET_WIDTH 1280
#define TARGET_HEIGHT 2800
#define TARGET_ROW (TARGET_WIDTH * 4)
#define FRAME_DONE_OFFSET 0x200
#define FRAME_DONE_VALUE 0x600df00du
extern void MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes);
static struct gpu_block core_target;
static void
ring_event(uint32_t event)
{
 ring_packet(CP_EVENT_WRITE7, 1);
 ring(event);
}
static void
ring_fill(uint32_t top, uint32_t bottom, uint32_t red, uint32_t green, uint32_t blue)
{
 static const uint32_t count = 15;
 ring_packet(CP_NON_CONTEXT_REG_BUNCH, 2 + 2 * count);
 ring(1);
 ring(0);
 ring(R_GRAS_A2D_DEST_TL); ring(top << 16);
 ring(R_GRAS_A2D_DEST_BR); ring((TARGET_WIDTH - 1) | (bottom - 1) << 16);
 ring(R_RB_A2D_CLEAR_COLOR); ring(red);
 ring(R_RB_A2D_CLEAR_COLOR + 1); ring(green);
 ring(R_RB_A2D_CLEAR_COLOR + 2); ring(blue);
 ring(R_RB_A2D_CLEAR_COLOR + 3); ring(255);
 ring(R_RB_A2D_BLT_CNTL); ring(A2D_BLT_SOLID_8888);
 ring(R_GRAS_A2D_BLT_CNTL); ring(A2D_BLT_SOLID_8888);
 ring(R_TPL1_A2D_BLT_CNTL); ring(TPL1_A2D_TEX_2D);
 ring(R_SP_A2D_OUTPUT_INFO); ring(A2D_OUTPUT_8888);
 ring(R_RB_A2D_PIXEL_CNTL); ring(0);
 ring(R_RB_A2D_DEST_INFO); ring(A2D_DEST_BGRA_LINEAR);
 ring(R_RB_A2D_DEST_BASE); ring((uint32_t)CORE_TARGET_ADDRESS);
 ring(R_RB_A2D_DEST_BASE + 1); ring((uint32_t)(CORE_TARGET_ADDRESS >> 32));
 ring(R_RB_A2D_DEST_PITCH); ring(TARGET_ROW >> 6);
 ring_event(EV_DEBUG_LABEL);
 ring_packet(CP_BLIT, 1);
 ring(BLIT_OP_SCALE);
}
static bool
step_frame(void)
{
 const volatile uint8_t *pixel;
 uint32_t taken = ~0u, done = 0, start = ring_at;
 if (!zap_loaded || !core_started) {
  printf("macan-gpu: %u: needs steps 16 to 18 in this run\n", step);
  return false;
 }
 announce("make a picture's worth of memory in the 3D core's space and have its 2D engine fill three bands of colour");
 if (!block_new(&core_space, &core_target, CORE_TARGET_ADDRESS, TARGET_ROW * TARGET_HEIGHT, PTE_CORE)) {
  return false;
 }
 for (uint32_t i = 0; i < core_space.table_count; i++) {
  to_memory((vm_offset_t)core_space.tables[i].table, 4096);
 }
 *(volatile uint32_t *)(core_scratch.memory + FRAME_DONE_OFFSET) = 0;
 to_memory(core_scratch.memory, 4096);
 ring_packet(CP_THREAD_CONTROL, 1);
 ring(1u | 1u << 27);
 ring_event(EV_CCU_CLEAN_COLOR);
 ring_event(EV_CCU_INVALIDATE_COLOR);
 ring_event(EV_CCU_CLEAN_DEPTH);
 ring_event(EV_CCU_INVALIDATE_DEPTH);
 ring_packet(CP_SET_MARKER, 1);
 ring(RM6_BLIT2DSCALE);
 ring_packet(CP_CONTEXT_REG_BUNCH, 2 * 10);
 ring(R_RB_CCU_CACHE_CNTL); ring(CCU_CACHE_SYSMEM);
 ring(R_VPC_ATTR_BUF_SIZE); ring(131072);
 ring(R_VPC_ATTR_BUF_BASE); ring(0x1c8000);
 ring(R_PC_ATTR_BUF_SIZE); ring(131072);
 ring(R_VPC_POS_BUF_SIZE); ring(65536);
 ring(R_VPC_POS_BUF_BASE); ring(0x188000);
 ring(R_PC_POS_BUF_SIZE); ring(65536);
 ring(R_VPC_BV_POS_BUF_BASE); ring(0x248000);
 ring(R_VPC_BV_POS_BUF_SIZE); ring(24576);
 ring(R_PC_BV_POS_BUF_SIZE); ring(24576);
 ring_fill(0, 930, 255, 128, 0);
 ring_fill(930, 1860, 255, 255, 255);
 ring_fill(1860, TARGET_HEIGHT, 0, 128, 255);
 ring_event(EV_CCU_CLEAN_COLOR);
 ring_event(EV_CCU_CLEAN_DEPTH);
 ring_event(EV_CACHE_CLEAN);
 ring_packet(CP_WAIT_FOR_IDLE, 0);
 ring_packet(CP_MEM_WRITE, 3);
 ring((uint32_t)(core_scratch.address + FRAME_DONE_OFFSET));
 ring((uint32_t)((core_scratch.address + FRAME_DONE_OFFSET) >> 32));
 ring(FRAME_DONE_VALUE);
 to_memory(core_ring.memory, CORE_RING_SIZE);
 ring_sent = ring_at;
 put(&gpu, "rb_wptr", CP_RB_WPTR_GC, ring_at);
 for (int i = 0; i < 30000 && done != FRAME_DONE_VALUE; i++) {
  sleep_us(100);
  taken = core_rptr();
  to_memory(core_scratch.memory + FRAME_DONE_OFFSET, 4);
  done = *(volatile uint32_t *)(core_scratch.memory + FRAME_DONE_OFFSET);
 }
 aperture(PIPE_BR);
 printf("macan-gpu: %u: ring read from %u up to %u of %u (register %u), done word %#x, rbbm_status %#x, rbbm_int %#x, "
     "cp global %#x, br: interrupts %#x, hw fault %#x, sqe status %#x\n", step, start, taken, ring_at,
     rd(gpu.base + CP_RB_RPTR_BR), done, rd(gpu.base + GPU_RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
     rd(gpu.base + CP_INTERRUPT_STATUS_GLOBAL), rd(gpu.base + CP_INTERRUPT_STATUS_PIPE),
     rd(gpu.base + CP_HW_FAULT_STATUS_PIPE), rd(gpu.base + CP_SQE_STATUS_PIPE));
 aperture(PIPE_NONE);
 if (space_faulted(&core_space) || done != FRAME_DONE_VALUE) {
  printf("macan-gpu: %u: the 3D core did not get through the picture\n", step);
  return false;
 }
 to_memory(core_target.memory, core_target.size);
 pixel = (const volatile uint8_t *)core_target.memory;
 printf("macan-gpu: %u: the picture's first pixel %02x %02x %02x %02x (blue green red alpha; told 00 80 ff ff), "
     "one in the middle %02x %02x %02x %02x (ff ff ff ff), one in the last row %02x %02x %02x %02x (ff 80 00 ff)\n", step,
     pixel[0], pixel[1], pixel[2], pixel[3], pixel[1400 * TARGET_ROW + 2560], pixel[1400 * TARGET_ROW + 2561],
     pixel[1400 * TARGET_ROW + 2562], pixel[1400 * TARGET_ROW + 2563], pixel[2799 * TARGET_ROW + 5116],
     pixel[2799 * TARGET_ROW + 5117], pixel[2799 * TARGET_ROW + 5118], pixel[2799 * TARGET_ROW + 5119]);
 if ((pixel[0] | pixel[1] | pixel[2]) == 0 || pixel[1400 * TARGET_ROW + 2560] == 0 ||
     (pixel[2799 * TARGET_ROW + 5116] | pixel[2799 * TARGET_ROW + 5117]) == 0) {
  printf("macan-gpu: %u: the picture is not what the 3D core was told to draw\n", step);
  return false;
 }
 MiniIOMobileFramebufferShow((const uint8_t *)core_target.memory, TARGET_WIDTH, TARGET_HEIGHT, TARGET_ROW);
 printf("macan-gpu: %u: A PICTURE DRAWN BY THE ADRENO 829 IS ON THE PANEL: orange, white, blue from the top\n", step);
 sleep_ms(5000);
 return true;
}
static bool(*const steps[])(void) = {
 step_outside, step_gpucc, step_cx_power, step_clocks, step_first_words,
 step_smmu, step_smmu_context, step_rscc, step_gmu_memory,
 step_gmu_space, step_gmu_firmware, step_gmu_start, step_hfi, step_gx,
 step_core_space, step_core_start, step_zap_load, step_core_commands, step_frame,
};
#define STEPS (sizeof(steps) / sizeof(steps[0]))
static void
gpu_block_stop(void)
{
 step = 0;
 if (core_started) {
  wr(gpu.base + CP_SQE_CNTL, 0);
  sleep_ms(1);
  printf("macan-gpu: the command processor is halted again\n");
 }
 if (zap_loaded) {
  printf("macan-gpu: %s\n", zap_unload() ? "the zap shader is given back to TrustZone" :
      "TrustZone did not take the zap shader back");
 }
 if (lpac_stream_is_on) {
  wr(smmu.base + SMMU_SMR(LPAC_MATCH), 0);
  wr(smmu.base + SMMU_S2CR(LPAC_MATCH), SMMU_S2CR_FAULT);
 }
 if (core_space_is_on) {
  space_off(&core_space, CORE_MATCH);
  printf("macan-gpu: the 3D core's stream goes nowhere again\n");
 }
 if (gpu_oob_held) {
  wr(gmu.base + GMUCX_HOST2GMU_INTR_SET, GMU_OOB_GPU_CLEAR);
  sleep_ms(5);
  printf("macan-gpu: oob_gpu cleared: gfx_pwr_clk_status %#x\n", rd(gmu.base + GMUCX_GFX_PWR_CLK_STATUS));
 }
 if (gmu_runs) {
  wr(gmu.base + GMUCX_CM3_SYSRESET, 1);
  sleep_ms(1);
  printf("macan-gpu: the GMU's processor is held in reset again\n");
 }
 if (gmu_space_is_on) {
  space_off(&gmu_space, GMU_MATCH);
  printf("macan-gpu: the GMU's stream goes nowhere again\n");
 }
 if (clocks_switched_on) {
  static const uint32_t branches[] = {
   GPUCC_SMMU_VOTE_CBCR, GPUCC_MEMNOC_GFX_CBCR, GPUCC_CX_GMU_CBCR, GPUCC_HUB_AON_CBCR,
   GPUCC_HUB_CX_INT_CBCR, GPUCC_CXO_CBCR, GPUCC_AHB_CBCR,
  };
  for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
   wr(gpucc.base + branches[i], rd(gpucc.base + branches[i]) & ~CBCR_ENABLE);
  }
  wr(gcc.base + GCC_GPU_GEMNOC_GFX_CBCR, rd(gcc.base + GCC_GPU_GEMNOC_GFX_CBCR) & ~CBCR_ENABLE);
  printf("macan-gpu: the GPU block's clocks are off again\n");
 }
 if (cx_switched_on) {
  wr(gpucc.base + GPUCC_CX_GDSCR, rd(gpucc.base + GPUCC_CX_GDSCR) | GDSC_SW_COLLAPSE);
  sleep_ms(1);
  printf("macan-gpu: CX collapse asked for again: gdscr %#x\n", rd(gpucc.base + GPUCC_CX_GDSCR));
 }
}
#define CP_INDIRECT_BUFFER 0x3f
#define EV_CACHE_INVALIDATE7 0x33
#define GPU_ADDRESSES_START 0x10000000ull
#define GPU_ADDRESSES_SIZE 0xc0000000ull
#define GPU_FENCE_AT 0x300
#define GPU_CHIP_ID 0x44030a20u
bool macan_gpu_kept;
static struct {
 uint64_t start, size;
} gpu_free[1024];
static unsigned gpu_free_count;
static uint32_t gpu_fence, gpu_submissions;
extern void RedwoodGPUStart(void);
extern void RedwoodGPUStop(void);
bool macan_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t macan_gpu_address(uint64_t size);
void macan_gpu_address_free(uint64_t address, uint64_t size);
bool macan_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void macan_gpu_mapped(void);
void macan_gpu_unmap(uint64_t address, uint64_t size);
uint32_t macan_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool macan_gpu_done(uint32_t fence);
uint32_t macan_gpu_last(void);
void macan_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void macan_gpu_report(const char *why);
void macan_gpu_stop(void);
void macan_adreno_stop_kept(void);
bool
macan_gpu_parameter(uint32_t which, uint64_t *value)
{
 switch (which) {
 case 0x01:
 case 0x08:
 case 0x09:
 case 0x0a:
 case 0x11:
  *value = 0;
  return true;
 case 0x02:
  *value = 2432 * 1024;
  return true;
 case 0x03:
  *value = GPU_CHIP_ID;
  return true;
 case 0x04:
  *value = 403000000ull;
  return true;
 case 0x05:
  *value = mach_absolute_time();
  return true;
 case 0x06:
  *value = UCHE_GMEM_BASE;
  return true;
 case 0x07:
  *value = 1;
  return true;
 case 0x10:
  *value = 16;
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
macan_gpu_address(uint64_t size)
{
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
macan_gpu_address_free(uint64_t address, uint64_t size)
{
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
macan_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
 return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}
static void
core_space_commit(void)
{
 uint32_t context = smmu_context(&core_space);
 settle();
 wr(smmu.base + context + SMMU_CB_TLBIASID, core_space.context);
 wr(smmu.base + context + SMMU_CB_TLBSYNC, 1);
 for (int i = 0; i < 1000 && (rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1); i++) {
 }
}
void
macan_gpu_mapped(void)
{
 core_space_commit();
}
void
macan_gpu_unmap(uint64_t address, uint64_t size)
{
 for (uint64_t offset = 0; offset < size; offset += 4096) {
  space_map(&core_space, address + offset, 0, 0);
 }
 core_space_commit();
}
bool
macan_gpu_done(uint32_t fence)
{
 volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);
 to_memory((vm_offset_t)word, 4);
 return (int32_t)(*word - fence) >= 0;
}
uint32_t
macan_gpu_last(void)
{
 return gpu_fence;
}
void
macan_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
 *microseconds = 0;
 *submissions = gpu_submissions;
 bzero(by, 7 * sizeof(uint32_t));
 gpu_submissions = 0;
}
uint32_t
macan_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
 uint32_t wanted = 24 + 4 * count, room = 0;
 for (int i = 0; i < 20000; i++) {
  room = (core_rptr() + CORE_RING_WORDS - ring_at - 1) % CORE_RING_WORDS;
  if (room >= wanted) {
   break;
  }
  sleep_us(100);
 }
 if (room < wanted) {
  return 0;
 }
 if (++gpu_fence == 0) {
  gpu_fence = 1;
 }
 ring_packet(CP_THREAD_CONTROL, 1);
 ring(1u | 1u << 27);
 ring_event(EV_CCU_INVALIDATE_DEPTH);
 ring_event(EV_CCU_INVALIDATE_COLOR);
 ring_event(EV_CACHE_INVALIDATE7);
 for (uint32_t i = 0; i < count; i++) {
  ring_packet(CP_INDIRECT_BUFFER, 3);
  ring((uint32_t)addresses[i]);
  ring((uint32_t)(addresses[i] >> 32));
  ring(words[i]);
 }
 ring_packet(CP_THREAD_CONTROL, 1);
 ring(1u | 1u << 27);
 ring_packet(CP_EVENT_WRITE7, 4);
 ring(EV_CACHE_CLEAN | 0u << 20 | 0u << 24 | 1u << 27);
 ring((uint32_t)(core_scratch.address + GPU_FENCE_AT));
 ring((uint32_t)((core_scratch.address + GPU_FENCE_AT) >> 32));
 ring(gpu_fence);
 to_memory(core_ring.memory, CORE_RING_SIZE);
 ring_sent = ring_at;
 wr(gpu.base + CP_RB_WPTR_GC, ring_at);
 gpu_submissions++;
 return gpu_fence;
}
void
macan_gpu_report(const char *why)
{
 aperture(PIPE_BR);
 printf("macan-gpu: %s: submission %u, done %u, ring read %u of %u, rbbm_status %#x, rbbm_int %#x, cp global %#x, "
     "br: interrupts %#x, hw fault %#x, sqe status %#x\n", why, gpu_fence,
     *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT), core_rptr(), ring_at, rd(gpu.base + GPU_RBBM_STATUS),
     rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_INTERRUPT_STATUS_GLOBAL), rd(gpu.base + CP_INTERRUPT_STATUS_PIPE),
     rd(gpu.base + CP_HW_FAULT_STATUS_PIPE), rd(gpu.base + CP_SQE_STATUS_PIPE));
 aperture(PIPE_NONE);
 space_faulted(&core_space);
}
void
macan_gpu_stop(void)
{
 macan_gpu_kept = false;
}
void
macan_adreno_stop_kept(void)
{
 if (macan_gpu_kept) {
  RedwoodGPUStop();
 }
 macan_gpu_kept = false;
 if (core_started || gmu_runs) {
  gpu_block_stop();
 }
}
void
macan_adreno_init(void)
{
 uint32_t done = 0, back = 0;
 if (!PE_parse_boot_argn("macan_gpu", &last_step, sizeof(last_step)) || last_step == 0) {
  return;
 }
 if (last_step > STEPS) {
  last_step = STEPS;
 }
 PE_parse_boot_argn("macan_gpu_pause", &pause_ms, sizeof(pause_ms));
 macan_minidump_klog();
 newest_step = last_step;
 printf("macan-gpu: steps 1 to %u; step %u is the new one and waits %u ms after each announcement\n", last_step,
     newest_step, pause_ms);
 for (step = 1; step <= last_step; step++) {
  if (!steps[step - 1]()) {
   printf("macan-gpu: step %u: FAILED; the steps after it are not attempted\n", step);
   break;
  }
  printf("macan-gpu: step %u: done\n", step);
  done = step;
 }
 {
  uint32_t keep = 0;
  if (PE_parse_boot_argn("macan_gpu_keep", &keep, sizeof(keep)) && keep && done >= 18 && zap_loaded && core_started) {
   gpu_free[0].start = GPU_ADDRESSES_START;
   gpu_free[0].size = GPU_ADDRESSES_SIZE;
   gpu_free_count = 1;
   macan_gpu_kept = true;
   printf("macan-gpu: THE GPU STAYS ON for the system to draw with: the service RedwoodGPU (msm's interface)\n");
   RedwoodGPUStart();
   return;
  }
 }
 for (uint32_t second = 1; second <= 3; second++) {
  sleep_ms(1000);
  printf("macan-gpu: alive %u s after step %u; %s\n", second, done, last_write);
 }
 gpu_block_stop();
 if (PE_parse_boot_argn("macan_gpu_return", &back, sizeof(back)) && back) {
  printf("macan-gpu: over; back to the loader\n");
  loader_return();
 }
 printf("macan-gpu: over; the system goes on starting\n");
}
