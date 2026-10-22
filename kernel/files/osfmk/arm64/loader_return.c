#include <mach/mach_types.h>
#include <machine/atomic.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/psci_smp.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <vm/vm_page.h>
#include <vm/vm_compressor.h>
#define LOADER_LOG_SIZE (1536 * 1024)
#define LOADER_LOG_HEAD (256 * 1024)
#define LOADER_LOG_RING (1024 * 1024)
#if defined(GENERIC_GIC_V3_IRQ)
extern void gic_v3_irq_quiesce(void);
extern void gic_v3_irq_cpu_shutdown(void);
#endif
void loader_log_putc(char c);
void loader_return(void);
extern void redwood_adreno_quiesce(void);
extern void redwood_adreno_stop(void);
extern void redwood_adreno_minute(void);
extern void redwood_wifi_stop(void);
extern void redwood_wifi_quiesce(void);
extern void macan_synaptics_quiesce(void);
extern void macan_adsp_quiesce(void);
extern void macan_adsp_stop(void);
extern void macan_haptic_quiesce(void);
extern void macan_torch_quiesce(void);
void loader_checkpoint(uint64_t n);
char loader_log[LOADER_LOG_SIZE];
uint32_t loader_log_len;
static bool loader_return_entered;
static uint64_t loader_l1[512] __attribute__((aligned(4096)));
static uint64_t loader_l2[512] __attribute__((aligned(4096)));
static uint32_t loader_log_ring_at;
static bool loader_log_wrapped;
static bool loader_log_linear;
static char loader_log_scratch[LOADER_LOG_RING];
static uint32_t loader_log_lock;
void
loader_log_putc(char c)
{
 uint64_t daif;
 bool locked = false;
 if (c == '\r') {
  return;
 }
 daif = __builtin_arm_rsr64("DAIF");
 __builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
 for (uint32_t tries = 0; !locked && tries < 10000000; tries++) {
  locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
 }
 char *at = NULL;
 if (loader_log_linear || loader_log_len < LOADER_LOG_HEAD) {
  if (loader_log_len < LOADER_LOG_SIZE) {
   at = &loader_log[loader_log_len];
   loader_log[loader_log_len++] = c;
  }
 } else {
  at = &loader_log[LOADER_LOG_HEAD + loader_log_ring_at];
  loader_log[LOADER_LOG_HEAD + loader_log_ring_at] = c;
  if (++loader_log_ring_at == LOADER_LOG_RING) {
   loader_log_ring_at = 0;
   loader_log_wrapped = true;
  }
  if (!loader_log_wrapped) {
   loader_log_len = LOADER_LOG_HEAD + loader_log_ring_at;
  }
 }
 if (at != NULL && (c == '\n' || ((uintptr_t)at & 63) == 63)) {
  __asm__ volatile ("dc cvac, %0" : : "r" (at) : "memory");
 }
 if (locked) {
  os_atomic_store(&loader_log_lock, 0, release);
 }
 __builtin_arm_wsr64("DAIF", daif);
}
static void
loader_log_linearize(void)
{
 static const char note[] = "\n\n[... the log between the first 256 KB and the last 1 MB was dropped ...]\n\n";
 uint32_t older = LOADER_LOG_RING - loader_log_ring_at, n = LOADER_LOG_HEAD;
 if (loader_log_linear) {
  return;
 }
 loader_log_linear = true;
 if (!loader_log_wrapped) {
  return;
 }
 __builtin_memcpy(loader_log_scratch, loader_log + LOADER_LOG_HEAD + loader_log_ring_at, older);
 __builtin_memcpy(loader_log_scratch + older, loader_log + LOADER_LOG_HEAD, loader_log_ring_at);
 __builtin_memcpy(loader_log + n, note, sizeof(note) - 1);
 n += sizeof(note) - 1;
 __builtin_memcpy(loader_log + n, loader_log_scratch + sizeof(note) - 1, LOADER_LOG_RING - (sizeof(note) - 1));
 loader_log_len = LOADER_LOG_HEAD + LOADER_LOG_RING;
}
extern uint64_t loader_stub_slot[4];
struct loader_live {
 uint64_t magic;
 uint64_t heartbeat;
 const char *log;
 const uint32_t *log_len;
 const uint32_t *log_ring_at;
 const bool *log_wrapped;
 uint64_t log_head, log_ring;
 const uint64_t *recording;
 volatile uint64_t stop;
 uint64_t magic2;
 const uint64_t *sound;
};
uint64_t loader_recording[3];
uint64_t loader_sound[3];
struct loader_live loader_live = {
 .magic = 0x214556494c554e58ULL, .log = loader_log, .log_len = &loader_log_len, .log_ring_at = &loader_log_ring_at,
 .log_wrapped = &loader_log_wrapped, .log_head = LOADER_LOG_HEAD, .log_ring = LOADER_LOG_RING,
 .recording = loader_recording,
 .magic2 = 0x324556494c554e58ULL, .sound = loader_sound,
};
static inline uint64_t
static_kvtophys(const void *va)
{
 return (uint64_t)va + loader_stub_slot[3];
}
static void
loader_jump(uint64_t reason, uint64_t code)
{
#if ARM_PGSHIFT == 12
 uint64_t stub = loader_stub_slot[1];
 uint64_t tcr, t0sz;
 uint32_t own_core = 0;
 if (stub == 0 || loader_return_entered) {
  return;
 }
 tcr = __builtin_arm_rsr64("TCR_EL1");
#if GENERIC_USER_PAC
 tcr = (tcr & ~0x3fULL) | 25ULL;
#endif
 t0sz = tcr & 0x3f;
 if (t0sz < 25 || t0sz > 33 || (stub >> (64 - t0sz)) != 0) {
  return;
 }
 loader_return_entered = true;
#if defined(GENERIC_GIC_V3_IRQ)
 gic_v3_irq_quiesce();
 if (PE_state.bootArgs != NULL && PE_parse_boot_argn("bootcpu", &own_core, sizeof(own_core)) && own_core != 0) {
  __builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
  gic_v3_irq_cpu_shutdown();
 }
#endif
 loader_l1[stub >> 30] = static_kvtophys(loader_l2) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID;
 loader_l2[(stub >> 21) & 0x1ff] = (stub & ~0x1fffffULL) | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK |
     ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_AP(AP_RWNA) |
     ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);
 for (uintptr_t p = (uintptr_t)loader_log & ~63UL; p < (uintptr_t)loader_log + loader_log_len; p += 64) {
  __asm__ volatile ("dc cvac, %0" : : "r" (p));
 }
 for (uintptr_t p = loader_recording[2] & ~63UL; p < loader_recording[2] + loader_recording[1]; p += 64) {
  __asm__ volatile ("dc cvac, %0" : : "r" (p));
 }
 __asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_recording));
 __asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_recording + 16));
 for (uintptr_t p = loader_sound[2] & ~63UL; p < loader_sound[2] + loader_sound[1]; p += 64) {
  __asm__ volatile ("dc cvac, %0" : : "r" (p));
 }
 __asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_sound));
 __asm__ volatile ("dc cvac, %0" : : "r" ((uintptr_t)loader_sound + 16));
 __asm__ volatile ("dsb sy");
 register uint64_t r_ttbr0 __asm__("x0") = static_kvtophys(loader_l1);
 register uint64_t r_log __asm__("x2") = static_kvtophys(loader_log);
 register uint64_t r_len __asm__("x3") = loader_log_len;
 register uint64_t r_tcr __asm__("x4") = tcr;
 register uint64_t r_stub __asm__("x5") = stub;
 register uint64_t r_reason __asm__("x6") = reason;
 register uint64_t r_code __asm__("x7") = code;
 __asm__ volatile (
                "msr DAIFSet, #0xf\n"
                "bic x4, x4, #0x80\n"
                "msr TCR_EL1, x4\n"
                "msr TTBR0_EL1, x0\n"
                "isb\n"
                "tlbi vmalle1\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, x2\n"
                "mov x1, x3\n"
                "mov x2, x6\n"
                "mov x3, x7\n"
                "br x5\n"
                : : "r" (r_ttbr0), "r" (r_log), "r" (r_len), "r" (r_tcr), "r" (r_stub), "r" (r_reason), "r" (r_code));
 __builtin_unreachable();
#else
 (void)reason; (void)code;
#endif
}
extern char *debug_buf_base, *debug_buf_ptr;
struct msgbuf {
 int msg_magic;
 int msg_size;
 int msg_bufx;
 int msg_bufr;
 char *msg_bufc;
};
extern struct msgbuf *msgbufp;
void loader_log_line(const char *line);
void
loader_log_line(const char *line)
{
 for (; *line != 0; line++) {
  if (*line != '\r') {
   loader_log_putc(*line);
  }
 }
}
static void
loader_log_puts(const char *s)
{
 for (; *s; s++) {
  loader_log_putc(*s);
 }
}
static void
loader_log_append_msgbuf(void)
{
 struct msgbuf *mb = msgbufp;
 if (mb == NULL || mb->msg_bufc == NULL || mb->msg_size <= 0 || mb->msg_bufx < 0 || mb->msg_bufx > mb->msg_size) {
  return;
 }
 loader_log_puts("\n--- kernel message buffer ---\n");
 for (int pass = 0; pass < 2; pass++) {
  int from = pass == 0 ? mb->msg_bufx : 0;
  int to = pass == 0 ? mb->msg_size : mb->msg_bufx;
  for (int i = from; i < to; i++) {
   if (mb->msg_bufc[i] != 0) {
    loader_log_putc(mb->msg_bufc[i]);
   }
  }
 }
}
void
loader_return(void)
{
#if defined(GENERIC_PSCI_SMP)
 psci_smp_stop_others();
#endif
 macan_adsp_quiesce();
 macan_haptic_quiesce();
 macan_torch_quiesce();
 macan_synaptics_quiesce();
 redwood_adreno_quiesce();
 redwood_wifi_quiesce();
 loader_log_linearize();
 if (!loader_return_entered) {
  loader_log_append_msgbuf();
 }
 if (!loader_return_entered && debug_buf_base != NULL && debug_buf_ptr > debug_buf_base &&
     (size_t)(debug_buf_ptr - debug_buf_base) < 256 * 1024) {
  loader_log_puts("\n--- panic buffer ---\n");
  for (const char *c = debug_buf_base; c < debug_buf_ptr; c++) {
   loader_log_putc(*c);
  }
 }
 loader_jump(0, 0);
}
void
loader_checkpoint(uint64_t n)
{
 if (loader_stub_slot[2] == n) {
  loader_jump(2, n);
 }
}
static void
loader_return_test(void *as_panic)
{
 if (as_panic != (void *)1) {
  panic("return_panic: the panic that was asked for, on cpu %d", cpu_number());
 }
 loader_return();
}
static void
loader_return_timer(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
 static const char banner[] = "\n--- loader_return_after expired ---\n";
 uint32_t cpu = 0, as_panic = 0;
 for (const char *c = banner; *c; c++) {
  loader_log_putc(*c);
 }
 PE_parse_boot_argn("return_panic", &as_panic, sizeof(as_panic));
 if (!as_panic) {
  redwood_adreno_stop();
  macan_adsp_stop();
  redwood_wifi_stop();
 }
 if (PE_parse_boot_argn("return_cpu", &cpu, sizeof(cpu)) && cpu <= (uint32_t)ml_get_max_cpu_number() &&
     cpu_xcall((int)cpu, loader_return_test, (void *)(uintptr_t)(as_panic ? 2 : 1)) == KERN_SUCCESS) {
  for (;;) {
   delay_for_interval(1000, NSEC_PER_MSEC);
  }
 }
 loader_return_test((void *)(uintptr_t)(as_panic ? 2 : 1));
}
__startup_func
static void
loader_return_timer_init(void)
{
 uint32_t seconds = 0;
 uint64_t deadline;
 if (!PE_parse_boot_argn("loader_return_after", &seconds, sizeof(seconds)) || seconds == 0) {
  return;
 }
 thread_call_t call = thread_call_allocate(loader_return_timer, NULL);
 clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
 thread_call_enter_delayed(call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_return_timer_init);
static thread_call_t loader_stop_call;
static void
loader_stop_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
 uint64_t deadline;
 if (loader_live.stop) {
  static const char banner[] = "\n--- the host asked for the end of the run: back to the loader ---\n";
  for (const char *c = banner; *c; c++) {
   loader_log_putc(*c);
  }
  redwood_adreno_stop();
  macan_adsp_stop();
  redwood_wifi_stop();
  loader_return();
 }
 clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
 thread_call_enter_delayed(loader_stop_call, deadline);
}
__startup_func
static void
loader_stop_poll_init(void)
{
 loader_stop_call = thread_call_allocate(loader_stop_poll, NULL);
 loader_stop_poll(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_stop_poll_init);
#define VIRT_WDT_PET 0x86000007u
#define HV_WDT_PET_SECONDS 10
static thread_call_t hv_wdt_call;
static uint32_t hv_wdt_enabled;
#define MEMORY_REPORT_BEATS 6
extern unsigned int memorystatus_available_pages, memorystatus_available_pages_critical,
    memorystatus_available_pages_critical_base;
extern uint32_t c_segment_pages_compressed;
extern _Atomic uint64_t ramfs_pages_in_use;
extern void MiniIOSurfaceUsage(uint32_t *count, uint64_t *bytes);
static void
memory_report(void)
{
#define MB(pages) ((unsigned int)(((uint64_t)(pages) * PAGE_SIZE) >> 20))
 uint32_t surfaces = 0;
 uint64_t surface_bytes = 0;
 MiniIOSurfaceUsage(&surfaces, &surface_bytes);
 printf("memory: free %u, files %u, programs %u, wired %u (%u surfaces %u, ramfs %u), compressor %u (holding %u) MB; "
     "available %u, kills below %u (the idle) and %u (anybody)\n",
     MB(vm_page_free_count), MB(vm_page_pageable_external_count), MB(vm_page_pageable_internal_count),
     MB(vm_page_wire_count), surfaces, (unsigned int)(surface_bytes >> 20),
     MB(os_atomic_load(&ramfs_pages_in_use, relaxed)),
     MB(VM_PAGE_COMPRESSOR_COUNT), MB(c_segment_pages_compressed),
     MB(memorystatus_available_pages),
     MB(memorystatus_available_pages_critical), MB(memorystatus_available_pages_critical_base));
#undef MB
 redwood_adreno_minute();
}
static void
hv_wdt_pet(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
 uint64_t deadline;
 if (hv_wdt_enabled) {
  register uint64_t x0 __asm__("x0") = VIRT_WDT_PET;
  register uint64_t x1 __asm__("x1") = 0;
  register uint64_t x2 __asm__("x2") = 0;
  register uint64_t x3 __asm__("x3") = 0;
  __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
      "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
 }
 if (++loader_live.heartbeat % MEMORY_REPORT_BEATS == 0) {
  memory_report();
 }
 clock_interval_to_deadline(HV_WDT_PET_SECONDS, NSEC_PER_SEC, &deadline);
 thread_call_enter_delayed(hv_wdt_call, deadline);
}
__startup_func
static void
hv_wdt_pet_init(void)
{
 PE_parse_boot_argn("hv_wdt_pet", &hv_wdt_enabled, sizeof(hv_wdt_enabled));
 hv_wdt_call = thread_call_allocate(hv_wdt_pet, NULL);
 hv_wdt_pet(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hv_wdt_pet_init);
