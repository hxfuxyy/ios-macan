#ifndef _ARM64_PSCI_SMP_H_
#define _ARM64_PSCI_SMP_H_
#include <mach/mach_types.h>
#include <sys/cdefs.h>
__BEGIN_DECLS
#define PSCI_SMP_CPU_ASKED 1
#define PSCI_SMP_CPU_IN_KERNEL 2
#define PSCI_SMP_CPU_RUNNING 3
#define PSCI_SMP_CPU_OFF 4
extern void psci_smp_init(void);
extern kern_return_t psci_smp_start_cpu(int cpu);
extern void psci_smp_cpu_reached(uint32_t stage);
extern void psci_smp_cpu_running(void);
extern void psci_smp_cpu_off(void) __dead2;
extern void psci_smp_stop_others(void);
extern void psci_smp_stop_here(void);
extern void gic_v3_irq_signal(uint32_t phys_id);
extern void gic_v3_irq_signal_stop(uint32_t phys_id);
extern void gic_v3_irq_quiesce(void);
extern void gic_v3_irq_cpu_shutdown(void);
__END_DECLS
#endif
