#include <mach/mach_types.h>
#include <kern/debug.h>
#include <kern/simple_lock.h>
#include <os/atomic_private.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_cpu.h>
#include <arm/machine_routines.h>
#include <arm/rtclock.h>
#include <arm64/psci_smp.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#if defined(GENERIC_GIC_V3_IRQ)
#define GICD_CTLR 0x0000
#define GICD_CTLR_ENABLE_G1NS (1u << 1)
#define GICD_CTLR_ARE_NS (1u << 4)
#define GICD_CTLR_RWP (1u << 31)
#define GICD_IGROUPR(n) (0x0080 + 4 * (n))
#define GICD_ISENABLER(n) (0x0100 + 4 * (n))
#define GICD_ICENABLER(n) (0x0180 + 4 * (n))
#define GICD_ICPENDR(n) (0x0280 + 4 * (n))
#define GICD_ICACTIVER(n) (0x0380 + 4 * (n))
#define GICD_IPRIORITYR(n) (0x0400 + (n))
#define GICD_ICFGR(n) (0x0c00 + 4 * (n))
#define GICD_IGRPMODR(n) (0x0d00 + 4 * (n))
#define GICD_IROUTER(n) (0x6000 + 8 * (n))
#define GICR_FRAME_SIZE 0x20000
#define GICR_TYPER 0x0008
#define GICR_TYPER_LAST (1u << 4)
#define GICR_WAKER 0x0014
#define GICR_WAKER_PROCSLEEP (1u << 1)
#define GICR_WAKER_CHILDASLEEP (1u << 2)
#define GICR_SGI_BASE 0x10000
#define GICR_IGROUPR0 (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0 (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0 (GICR_SGI_BASE + 0x0180)
#define GICR_ICPENDR0 (GICR_SGI_BASE + 0x0280)
#define GICR_ICACTIVER0 (GICR_SGI_BASE + 0x0380)
#define GICR_IPRIORITYR(n) (GICR_SGI_BASE + 0x0400 + (n))
#define GICR_IGRPMODR0 (GICR_SGI_BASE + 0x0d00)
#define GIC_SGI_SIGNAL 0
#define GIC_SGI_STOP 1
#define GIC_INTID_VTIMER 27
#define GIC_INTID_SPI_MIN 32
#define GIC_INTID_SPURIOUS_MIN 1020
#define GIC_SPI_MOST 48
#define GIC_PRIORITY_DEFAULT 0xa0
static vm_offset_t gicd_base, gicr_base;
static vm_size_t gicr_size;
static uint64_t gicr_stride = GICR_FRAME_SIZE;
static vm_offset_t gicr_cpu_base[MAX_CPUS];
#define GIC_IAR_TAKEN (1ULL << 63)
static uint64_t gic_active_iar[MAX_CPUS];
uint64_t gic_unhandled_count;
uint32_t gic_last_unhandled;
struct gic_spi {
 uint32_t intid;
 void (*handler)(void *context, uint32_t intid);
 void *context;
 uint32_t busy;
 uint64_t count;
};
static struct gic_spi gic_spi[GIC_SPI_MOST];
static decl_simple_lock_data(, gic_spi_lock);
void gic_v3_irq_cpu_init(void);
void gic_v3_irq_quiesce(void);
kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
void gic_v3_spi_mask(uint32_t intid);
void gic_v3_spi_unmask(uint32_t intid);
void gic_v3_spi_detach(uint32_t intid);
uint64_t gic_v3_spi_count(uint32_t intid);
void gic_v3_irq_cpu_shutdown(void);
void gic_v3_irq_signal(uint32_t phys_id);
void gic_v3_irq_signal_stop(uint32_t phys_id);
static inline uint32_t
gic_read32(vm_offset_t addr)
{
 return *(volatile uint32_t *)addr;
}
static inline void
gic_write32(vm_offset_t addr, uint32_t value)
{
 *(volatile uint32_t *)addr = value;
}
static void
gic_mask(uint32_t intid)
{
 if (intid < 32) {
  gic_write32(gicr_cpu_base[cpu_number()] + GICR_ICENABLER0, 1u << intid);
 } else {
  gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
 }
}
static void
gic_send_sgi(uint32_t phys_id, uint32_t sgi)
{
 uint64_t aff0 = phys_id & 0xff, aff1 = (phys_id >> 8) & 0xff;
 uint64_t aff2 = (phys_id >> 16) & 0xff;
 __builtin_arm_wsr64("ICC_SGI1R_EL1", (aff2 << 32) | ((uint64_t)sgi << 24) | (aff1 << 16) | (1ULL << (aff0 & 15)));
 __builtin_arm_isb(ISB_SY);
}
void
gic_v3_irq_signal(uint32_t phys_id)
{
 gic_send_sgi(phys_id, GIC_SGI_SIGNAL);
}
void
gic_v3_irq_signal_stop(uint32_t phys_id)
{
 gic_send_sgi(phys_id, GIC_SGI_STOP);
}
static bool
gic_spi_handle(uint32_t intid)
{
 for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
  struct gic_spi *spi = &gic_spi[i];
  if (os_atomic_load(&spi->intid, acquire) == intid) {
   os_atomic_inc(&spi->busy, acquire);
   if (os_atomic_load(&spi->intid, relaxed) == intid) {
    spi->count++;
    spi->handler(spi->context, intid);
   }
   os_atomic_dec(&spi->busy, release);
   return true;
  }
 }
 return false;
}
static void
gic_distributor_settle(void)
{
 while (gic_read32(gicd_base + GICD_CTLR) & GICD_CTLR_RWP) {
 }
}
void
gic_v3_spi_mask(uint32_t intid)
{
 if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
  gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
  gic_distributor_settle();
 }
}
void
gic_v3_spi_unmask(uint32_t intid)
{
 if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
  gic_write32(gicd_base + GICD_ISENABLER(intid / 32), 1u << (intid % 32));
 }
}
kern_return_t
gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
 uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
 struct gic_spi *slot = NULL;
 uint32_t config;
 if (gicd_base == 0 || intid < GIC_INTID_SPI_MIN || intid >= GIC_INTID_SPURIOUS_MIN || handler == NULL) {
  return KERN_INVALID_ARGUMENT;
 }
 simple_lock(&gic_spi_lock, LCK_GRP_NULL);
 for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
  if (gic_spi[i].intid == intid) {
   simple_unlock(&gic_spi_lock);
   return KERN_NAME_EXISTS;
  }
  if (slot == NULL && gic_spi[i].intid == 0 && gic_spi[i].busy == 0) {
   slot = &gic_spi[i];
  }
 }
 if (slot == NULL) {
  simple_unlock(&gic_spi_lock);
  return KERN_RESOURCE_SHORTAGE;
 }
 slot->handler = handler;
 slot->context = context;
 slot->count = 0;
 os_atomic_store(&slot->intid, intid, release);
 simple_unlock(&gic_spi_lock);
 gic_v3_spi_mask(intid);
 gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
 gic_write32(gicd_base + GICD_ICACTIVER(intid / 32), 1u << (intid % 32));
 gic_write32(gicd_base + GICD_IGROUPR(intid / 32), gic_read32(gicd_base + GICD_IGROUPR(intid / 32)) | 1u << (intid % 32));
 gic_write32(gicd_base + GICD_IGRPMODR(intid / 32), gic_read32(gicd_base + GICD_IGRPMODR(intid / 32)) & ~(1u << (intid % 32)));
 *(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(intid)) = GIC_PRIORITY_DEFAULT;
 config = gic_read32(gicd_base + GICD_ICFGR(intid / 16)) & ~(3u << (intid % 16 * 2));
 gic_write32(gicd_base + GICD_ICFGR(intid / 16), config | (edge ? 2u : 0u) << (intid % 16 * 2));
 *(volatile uint64_t *)(gicd_base + GICD_IROUTER(intid)) = (mpidr & 0xff00000000ull) | (mpidr & 0xffffff);
 gic_distributor_settle();
 gic_v3_spi_unmask(intid);
 return KERN_SUCCESS;
}
void
gic_v3_spi_detach(uint32_t intid)
{
 gic_v3_spi_mask(intid);
 if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
  gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
 }
 for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
  if (gic_spi[i].intid == intid) {
   os_atomic_store(&gic_spi[i].intid, 0, release);
   while (os_atomic_load(&gic_spi[i].busy, acquire) != 0) {
   }
  }
 }
}
uint64_t
gic_v3_spi_count(uint32_t intid)
{
 for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
  if (gic_spi[i].intid == intid) {
   return gic_spi[i].count;
  }
 }
 return 0;
}
static void
gic_irq_handler(__unused void *target, __unused void *refCon, __unused void *nub, __unused int source)
{
 int cpu = cpu_number();
 for (;;) {
  uint64_t iar = __builtin_arm_rsr64("ICC_IAR1_EL1");
  uint32_t intid = (uint32_t)(iar & 0xffffff);
  if (intid >= GIC_INTID_SPURIOUS_MIN) {
   break;
  }
  gic_active_iar[cpu] = iar | GIC_IAR_TAKEN;
  if (intid == GIC_INTID_VTIMER) {
   getCpuDatap()->cpu_decrementer = -1;
   rtclock_intr(TRUE);
  } else if (intid == GIC_SGI_SIGNAL) {
   cpu_signal_handler();
  } else if (intid == GIC_SGI_STOP) {
   psci_smp_stop_here();
  } else if (intid >= GIC_INTID_SPI_MIN && gic_spi_handle(intid)) {
  } else {
   gic_mask(intid);
   gic_unhandled_count++;
   gic_last_unhandled = intid;
  }
  __builtin_arm_wsr64("ICC_EOIR1_EL1", iar);
  __builtin_arm_isb(ISB_SY);
  gic_active_iar[cpu] = 0;
 }
}
void
gic_v3_irq_quiesce(void)
{
 int cpu;
 if (gicd_base == 0) {
  return;
 }
 cpu = cpu_number();
 for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
  uint32_t intid = gic_spi[i].intid;
  if (intid != 0) {
   gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
  }
 }
 if (gic_active_iar[cpu] != 0) {
  __builtin_arm_wsr64("ICC_EOIR1_EL1", gic_active_iar[cpu] & ~GIC_IAR_TAKEN);
  __builtin_arm_isb(ISB_SY);
  gic_active_iar[cpu] = 0;
 }
}
void
gic_v3_irq_cpu_shutdown(void)
{
 vm_offset_t gicr;
 if (gicd_base == 0) {
  return;
 }
 gic_v3_irq_quiesce();
 gicr = gicr_cpu_base[cpu_number()];
 if (gicr != 0) {
  gic_write32(gicr + GICR_ICENABLER0, 0xffffffffu);
  gic_write32(gicr + GICR_ICPENDR0, 0xffffffffu);
  gic_write32(gicr + GICR_ICACTIVER0, 0xffffffffu);
 }
 __builtin_arm_wsr64("ICC_IGRPEN1_EL1", 0);
 __builtin_arm_isb(ISB_SY);
}
static vm_offset_t
gic_find_redistributor(void)
{
 uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
 uint32_t affinity = (uint32_t)(((mpidr >> 32) & 0xff) << 24 | (mpidr & 0xffffff));
 for (vm_offset_t frame = gicr_base; frame + GICR_FRAME_SIZE <= gicr_base + gicr_size; frame += gicr_stride) {
  uint64_t typer = *(volatile uint64_t *)(frame + GICR_TYPER);
  if ((uint32_t)(typer >> 32) == affinity) {
   return frame;
  }
  if (typer & GICR_TYPER_LAST) {
   break;
  }
 }
 panic("gic: no redistributor for MPIDR 0x%llx", mpidr);
}
void
gic_v3_irq_cpu_init(void)
{
 int cpu = cpu_number();
 vm_offset_t gicr;
 uint32_t mine = 1u << GIC_INTID_VTIMER;
 if (gicd_base == 0) {
  DTEntry entry;
  void const *prop;
  unsigned int prop_size;
  if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
      SecureDTGetProperty(entry, "reg", &prop, &prop_size) != kSuccess ||
      prop_size < 4 * sizeof(uint64_t)) {
   panic("gic: no usable /arm-io/gic node in the device tree");
  }
  uint64_t const *reg = prop;
  vm_offset_t soc_base = pe_arm_get_soc_base_phys();
  simple_lock_init(&gic_spi_lock, 0);
  gicd_base = ml_io_map(soc_base + reg[0], reg[1]);
  gicr_base = ml_io_map(soc_base + reg[2], reg[3]);
  gicr_size = reg[3];
        if (SecureDTGetProperty(entry, "redistributor-stride", &prop, &prop_size) == kSuccess) {
            if (prop_size != sizeof(uint64_t)) panic("gic: invalid redistributor-stride size");
            gicr_stride = *(const uint64_t *)prop;
            if (gicr_stride < GICR_FRAME_SIZE || gicr_stride > gicr_size ||
                (gicr_stride & (gicr_stride - 1))) panic("gic: invalid redistributor-stride");
        }
  if (gicd_base == 0 || gicr_base == 0) {
   panic("gic: cannot map the distributor/redistributor");
  }
 } else if (gicr_cpu_base[cpu] != 0) {
  return;
 }
 __builtin_arm_wsr64("ICC_SRE_EL1", __builtin_arm_rsr64("ICC_SRE_EL1") | 1);
 __builtin_arm_isb(ISB_SY);
 gicr = gicr_cpu_base[cpu] = gic_find_redistributor();
 uint32_t waker = gic_read32(gicr + GICR_WAKER);
 if (waker & (GICR_WAKER_PROCSLEEP | GICR_WAKER_CHILDASLEEP)) {
  gic_write32(gicr + GICR_WAKER, waker & ~GICR_WAKER_PROCSLEEP);
  while (gic_read32(gicr + GICR_WAKER) & GICR_WAKER_CHILDASLEEP) {
  }
 }
 if (ml_get_cpu_count() > 1) {
  uint32_t sgis = (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);
  gic_write32(gicr + GICR_ICACTIVER0, sgis);
  gic_write32(gicr + GICR_ICPENDR0, sgis);
  *(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_SGI_SIGNAL)) = GIC_PRIORITY_DEFAULT;
  *(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_SGI_STOP)) = GIC_PRIORITY_DEFAULT;
  mine |= sgis;
 }
 gic_write32(gicr + GICR_IGROUPR0, gic_read32(gicr + GICR_IGROUPR0) | mine);
 gic_write32(gicr + GICR_IGRPMODR0, gic_read32(gicr + GICR_IGRPMODR0) & ~mine);
 *(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_INTID_VTIMER)) = GIC_PRIORITY_DEFAULT;
 gic_write32(gicr + GICR_ISENABLER0, mine);
 uint32_t ctlr = gic_read32(gicd_base + GICD_CTLR);
 if ((ctlr & (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) != (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) {
  gic_write32(gicd_base + GICD_CTLR, ctlr | GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS);
  while (gic_read32(gicd_base + GICD_CTLR) & GICD_CTLR_RWP) {
  }
 }
 __builtin_arm_wsr64("ICC_AP1R0_EL1", 0);
 __builtin_arm_wsr64("ICC_PMR_EL1", 0xff);
 __builtin_arm_wsr64("ICC_BPR1_EL1", 0);
 __builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
 __builtin_arm_isb(ISB_SY);
 ml_install_interrupt_handler(NULL, 0, NULL, gic_irq_handler, NULL);
 kprintf("gic: v3, distributor %p, cpu %d's redistributor %p, virtual timer on PPI %d\n",
     (void *)gicd_base, cpu, (void *)gicr, GIC_INTID_VTIMER);
}
#endif
