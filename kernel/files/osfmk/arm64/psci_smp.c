#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <arm64/psci_smp.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <vm/pmap.h>
#if defined(GENERIC_PSCI_SMP)
#define PSCI_CPU_OFF 0x84000002u
#define PSCI_CPU_ON 0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u
#define PSCI_AFFINITY_OFF 1
#define PSCI_SMP_START_SECONDS 5
#define PSCI_SMP_STOP_SECONDS 3
extern void loader_return(void);
extern void loader_log_putc(char c);
extern unsigned long gVirtBase, gPhysBase;
uint32_t psci_smp_hvc;
struct psci_smp_start_fault {
 uint64_t valid, mpidr, esr, elr, far, lr, sp, unused;
} psci_smp_start_fault __attribute__((aligned(64)));
_Static_assert(sizeof(struct psci_smp_start_fault) == 64, "one cache line");
static uint32_t psci_smp_cpu_stage[MAX_CPUS];
static uint32_t psci_smp_started;
static int psci_smp_stop_owner = -1;
static bool psci_smp_boot_wanted;
static bool psci_smp_boot_busy;
static bool psci_smp_stopping;
static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
 register uint64_t x0 __asm__("x0") = function;
 register uint64_t x1 __asm__("x1") = arg1;
 register uint64_t x2 __asm__("x2") = arg2;
 register uint64_t x3 __asm__("x3") = arg3;
 if (psci_smp_hvc) {
  __asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
 } else {
  __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
 }
 return (int64_t)x0;
}
static const char *
psci_error(int64_t status)
{
 static const char *const names[] = {
  "success", "not supported", "invalid parameters", "denied", "already on", "on pending",
  "internal failure", "not present", "disabled", "invalid address",
 };
 return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "?";
}
void
psci_smp_init(void)
{
 DTEntry chosen;
 void const *prop;
 unsigned int size;
 if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
     SecureDTGetProperty(chosen, "psci-method", &prop, &size) == kSuccess &&
     size >= 3 && strncmp((const char *)prop, "hvc", 3) == 0) {
  psci_smp_hvc = 1;
 }
}
void
psci_smp_cpu_reached(uint32_t stage)
{
 os_atomic_store(&psci_smp_cpu_stage[cpu_number()], stage, release);
}
void
psci_smp_cpu_running(void)
{
 psci_smp_cpu_reached(PSCI_SMP_CPU_RUNNING);
}
static void
psci_smp_clean_segment(const char *name)
{
 unsigned long size = 0;
 vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);
 if (start != 0 && size != 0) {
  CleanPoC_DcacheRegion(start, size);
 }
}
kern_return_t
psci_smp_start_cpu(int cpu)
{
 cpu_data_t *cdp = cpu_datap(cpu);
 uint32_t phys_id = cdp->cpu_phys_id, stage;
 uint64_t entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
 uint64_t now, deadline;
 int64_t status;
 psci_smp_start_fault.valid = 0;
 psci_smp_clean_segment("__TEXT");
 psci_smp_clean_segment("__DATA_CONST");
 psci_smp_clean_segment("__TEXT_EXEC");
 psci_smp_clean_segment("__DATA");
 psci_smp_clean_segment("__BOOTDATA");
 for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
  if (cpu_datap(other) != NULL) {
   CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
  }
 }
 __builtin_arm_dsb(DSB_SY);
 os_atomic_store(&psci_smp_cpu_stage[cpu], PSCI_SMP_CPU_ASKED, release);
 status = psci_call(PSCI_CPU_ON, phys_id, entry, 0);
 if (status != 0) {
  os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
  printf("psci: cpu %d (MPIDR 0x%x) was not started: the firmware answers %lld (%s)\n",
      cpu, phys_id, status, psci_error(status));
  return KERN_FAILURE;
 }
 os_atomic_or(&psci_smp_started, 1u << cpu, relaxed);
 clock_interval_to_deadline(PSCI_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
 do {
  stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
  if (stage >= PSCI_SMP_CPU_RUNNING) {
   return KERN_SUCCESS;
  }
  FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
  if (psci_smp_start_fault.valid != 0) {
   break;
  }
  delay_for_interval(1, NSEC_PER_MSEC);
  now = mach_absolute_time();
 } while (now < deadline);
 if (psci_smp_start_fault.valid != 0) {
  struct psci_smp_start_fault *f = &psci_smp_start_fault;
  uint64_t to_virtual = gVirtBase - gPhysBase;
  printf("psci: cpu %d (MPIDR 0x%llx) took an exception on its way in and has switched itself off: "
      "ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx SP 0x%llx (ELR and LR as kernel addresses: 0x%llx 0x%llx)\n",
      cpu, f->mpidr, f->esr, f->elr, f->far, f->lr, f->sp, f->elr + to_virtual, f->lr + to_virtual);
  os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);
 } else {
  printf("psci: cpu %d (MPIDR 0x%x) was started %d seconds ago and has not joined: %s\n",
      cpu, phys_id, PSCI_SMP_START_SECONDS,
      stage >= PSCI_SMP_CPU_IN_KERNEL ? "it got as far as arm_init_cpu()" : "no sign of it since the firmware took the request");
 }
 return KERN_FAILURE;
}
void
psci_smp_cpu_off(void)
{
 __builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
 __builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
 gic_v3_irq_cpu_shutdown();
 os_atomic_store(&psci_smp_cpu_stage[cpu_number()], PSCI_SMP_CPU_OFF, release);
 __builtin_arm_dsb(DSB_SY);
 (void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
 for (;;) {
  __builtin_arm_wfi();
 }
}
static void
psci_smp_note(const char *what, int cpu)
{
 static const char prefix[] = "\npsci: cpu ";
 for (const char *c = prefix; *c; c++) {
  loader_log_putc(*c);
 }
 loader_log_putc((char)('0' + cpu));
 for (const char *c = what; *c; c++) {
  loader_log_putc(*c);
 }
}
static void
psci_smp_stop_rest(void)
{
 int me = cpu_number(), max_cpu = ml_get_max_cpu_number();
 os_atomic_store(&psci_smp_stopping, true, release);
 __builtin_arm_dsb(DSB_SY);
 for (int cpu = 0; cpu <= max_cpu; cpu++) {
  cpu_data_t *cdp = cpu_datap(cpu);
  if (cpu != me && cdp != NULL && (cpu == master_cpu || (psci_smp_started & (1u << cpu)))) {
   gic_v3_irq_signal_stop(cdp->cpu_phys_id);
  }
 }
 __builtin_arm_sev();
}
void
psci_smp_stop_others(void)
{
 int me, max_cpu;
 uint64_t now, deadline;
 if (psci_smp_started == 0) {
  return;
 }
 __builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
 me = cpu_number();
 max_cpu = ml_get_max_cpu_number();
 if (!os_atomic_cmpxchg(&psci_smp_stop_owner, -1, me, acq_rel) && psci_smp_stop_owner != me && me != master_cpu) {
  psci_smp_cpu_off();
 }
 if (me != master_cpu) {
  psci_smp_note(" ends the system: the return to the boot loader is the boot processor's to make\n", me);
  os_atomic_store(&psci_smp_boot_wanted, true, release);
  __builtin_arm_dsb(DSB_SY);
  gic_v3_irq_signal_stop(cpu_datap(master_cpu)->cpu_phys_id);
  __builtin_arm_sev();
  now = mach_absolute_time();
  nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
  deadline += now;
  while (!os_atomic_load(&psci_smp_stopping, acquire) && now < deadline) {
   now = mach_absolute_time();
  }
  if (!os_atomic_load(&psci_smp_stopping, acquire)) {
   psci_smp_note(" cannot return to the boot loader and the boot processor does not answer: all cores stop here\n", me);
   psci_smp_stop_rest();
  }
  psci_smp_cpu_off();
 }
 psci_smp_stop_rest();
 now = mach_absolute_time();
 nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
 deadline += now;
 for (int cpu = 0; cpu <= max_cpu; cpu++) {
  cpu_data_t *cdp = cpu_datap(cpu);
  if (cpu == me || cdp == NULL || !(psci_smp_started & (1u << cpu))) {
   continue;
  }
  while (now < deadline) {
   if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) == PSCI_SMP_CPU_OFF) {
    int64_t state = psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0);
    if (state == PSCI_AFFINITY_OFF || state < 0) {
     break;
    }
   }
   now = mach_absolute_time();
  }
  if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) != PSCI_SMP_CPU_OFF &&
      psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0) != PSCI_AFFINITY_OFF) {
   psci_smp_note(" did not stop: it is left running\n", cpu);
  }
 }
}
void
psci_smp_stop_here(void)
{
 if (psci_smp_started == 0) {
  return;
 }
 if (cpu_number() != master_cpu) {
  if (os_atomic_load(&psci_smp_stopping, acquire)) {
   psci_smp_cpu_off();
  }
  return;
 }
 if (os_atomic_load(&psci_smp_boot_wanted, acquire) && !psci_smp_boot_busy) {
  psci_smp_boot_busy = true;
  loader_return();
  for (;;) {
   __builtin_arm_wfi();
  }
 }
}
#endif
