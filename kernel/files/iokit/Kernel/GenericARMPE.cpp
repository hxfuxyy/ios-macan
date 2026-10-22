extern "C" {
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <kern/cpu_number.h>
}
#include <machine/machine_routines.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCPU.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include "MiniDrivers.h"
extern "C" {
kern_return_t processor_start(processor_t processor);
kern_return_t processor_exit(processor_t processor);
extern uint32_t processor_avail_count;
void psci_smp_init(void);
kern_return_t psci_smp_start_cpu(int cpu);
void psci_smp_cpu_running(void);
void psci_smp_cpu_off(void) __dead2;
void gic_v3_irq_signal(uint32_t phys_id);
}
class GenericARMCPU : public IOCPU
{
 OSDeclareDefaultStructors(GenericARMCPU);
 uint32_t physID;
public:
 virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
 virtual void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
 virtual void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
 virtual kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
 virtual void haltCPU(void) APPLE_KEXT_OVERRIDE;
 virtual void signalCPU(IOCPU * target) APPLE_KEXT_OVERRIDE;
 virtual const OSSymbol * getCPUName(void) APPLE_KEXT_OVERRIDE;
 static int cpuNumberOfNub(IOService * nub);
 void run(void);
};
OSDefineMetaClassAndStructors(GenericARMCPU, IOCPU);
int
GenericARMCPU::cpuNumberOfNub(IOService * nub)
{
 OSData * reg = OSDynamicCast(OSData, nub->getProperty("reg"));
 if (nub->getProperty("state") == NULL || reg == NULL || reg->getLength() < sizeof(uint32_t)) {
  return -1;
 }
 return ml_get_cpu_number(*(const uint32_t *)reg->getBytesNoCopy());
}
bool
GenericARMCPU::start(IOService * provider)
{
 ml_processor_info_t info;
 perfmon_interrupt_handler_func pmi_handler = NULL;
 int cpu = cpuNumberOfNub(provider);
 if (cpu < 0 || !IOCPU::start(provider)) {
  return false;
 }
 const ml_topology_cpu_t * topology = &ml_get_topology_info()->cpus[cpu];
 physID = topology->phys_id;
 setCPUNumber((UInt32)cpu);
 bzero(&info, sizeof(info));
 info.cpu_id = (cpu_id_t)this;
 info.log_id = getCPUNumber();
 info.phys_id = physID;
 info.cluster_id = topology->cluster_id;
 info.cluster_type = topology->cluster_type;
 if (ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi_handler) != KERN_SUCCESS) {
  IOLog("GenericARMCPU: ml_processor_register failed\n");
  return false;
 }
 setCPUState(kIOCPUStateUninitalized);
 registerService();
 IOLog("GenericARMCPU: cpu %u (phys id 0x%x) registered\n", (unsigned)getCPUNumber(), physID);
 return true;
}
void
GenericARMCPU::run(void)
{
 processor_start(machProcessor);
}
void
GenericARMCPU::initCPU(bool boot)
{
 setCPUState(kIOCPUStateRunning);
 if (ml_get_cpu_count() > 1) {
  psci_smp_cpu_running();
  gic_v3_irq_signal(physID);
 }
}
void
GenericARMCPU::quiesceCPU(void)
{
 if (getCPUNumber() != (UInt32)master_cpu) {
  setCPUState(kIOCPUStateStopped);
  psci_smp_cpu_off();
 }
}
kern_return_t
GenericARMCPU::startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr)
{
 return psci_smp_start_cpu((int)getCPUNumber());
}
void
GenericARMCPU::haltCPU(void)
{
 if (getCPUNumber() != (UInt32)master_cpu) {
  processor_exit(machProcessor);
 }
}
void
GenericARMCPU::signalCPU(IOCPU * target)
{
 gic_v3_irq_signal(((GenericARMCPU *)target)->physID);
}
const OSSymbol *
GenericARMCPU::getCPUName(void)
{
 char name[32];
 snprintf(name, sizeof(name), "Primary%u", (unsigned)getCPUNumber());
 return OSSymbol::withCString(name);
}
class GenericARMPE : public IODTPlatformExpert
{
 OSDeclareDefaultStructors(GenericARMPE);
public:
 virtual IOService * probe(IOService * provider, SInt32 * score) APPLE_KEXT_OVERRIDE;
 virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
 virtual const char * deleteList(void) APPLE_KEXT_OVERRIDE;
 virtual const char * excludeList(void) APPLE_KEXT_OVERRIDE;
 virtual bool getMachineName(char * name, int maxLength) APPLE_KEXT_OVERRIDE;
 virtual long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
 virtual void setGMTTimeOfDay(long secs) APPLE_KEXT_OVERRIDE;
};
#define super IODTPlatformExpert
OSDefineMetaClassAndStructors(GenericARMPE, IODTPlatformExpert);
IOService *
GenericARMPE::probe(IOService * provider, SInt32 * score)
{
 if (!IOPlatformExpert::probe(provider, score)) {
  return NULL;
 }
 return this;
}
bool
GenericARMPE::start(IOService * provider)
{
 if (!super::start(provider)) {
  return false;
 }
 IOLog("GenericARMPE: platform \"%s\"\n", provider->getName());
 static GenericARMCPU * cpus[MAX_CPUS];
 unsigned int count = 0;
 OSIterator * iter = getChildIterator(gIOServicePlane);
 if (iter != NULL) {
  OSObject * obj;
  while ((obj = iter->getNextObject()) != NULL) {
   IOService * nub = OSDynamicCast(IOService, obj);
   int number = nub ? GenericARMCPU::cpuNumberOfNub(nub) : -1;
   if (number < 0 || number >= MAX_CPUS || cpus[number] != NULL) {
    continue;
   }
   GenericARMCPU * cpu = new GenericARMCPU;
   if (cpu != NULL && cpu->init() && cpu->attach(nub)) {
    if (cpu->start(nub)) {
     cpus[number] = cpu;
     count++;
     continue;
    }
    cpu->detach(nub);
   }
   if (cpu != NULL) {
    cpu->release();
   }
  }
  iter->release();
 }
 if (cpus[master_cpu] != NULL) {
  ml_set_max_cpus(count);
  if (count > 1) {
   psci_smp_init();
  }
  uint32_t later = 28;
  static thread_call_t later_call;
  PE_parse_boot_argn("small_cores_after", &later, sizeof(later));
  cpus[master_cpu]->run();
  for (unsigned int number = 0; number < MAX_CPUS; number++) {
   if (cpus[number] != NULL && number != (unsigned int)master_cpu) {
    if (later != 0 && ml_get_topology_info()->cpus[number].l2_access_penalty != 0) {
     continue;
    }
    cpus[number]->run();
   }
  }
  if (later != 0) {
   uint64_t deadline;
   later_call = thread_call_allocate([](thread_call_param_t, thread_call_param_t) {
    for (unsigned int number = 0; number < MAX_CPUS; number++) {
     if (cpus[number] != NULL && ml_get_topology_info()->cpus[number].l2_access_penalty != 0) {
      cpus[number]->run();
     }
    }
    IOLog("GenericARMPE: the slower cores started too: %u processors running\n", processor_avail_count);
   }, NULL);
   clock_interval_to_deadline(later, NSEC_PER_SEC, &deadline);
   thread_call_enter_delayed(later_call, deadline);
  }
  if (count > 1) {
   IOLog("GenericARMPE: %u of %u processors are running\n", processor_avail_count, count);
  }
 }
 registerService();
 MiniIOSurfaceStart(this);
 MiniIOMobileFramebufferStart(this);
 MiniAppleKeyStoreStart();
 MiniIONetworkStackStart();
 return true;
}
static long gBootEpoch;
static bool gBootEpochKnown;
static void
findBootEpoch(void)
{
 uint64_t epoch = 0;
 if (gBootEpochKnown) {
  return;
 }
 gBootEpochKnown = true;
 if (!PE_parse_boot_argn("epoch", &epoch, sizeof(epoch)) || epoch == 0) {
  IORegistryEntry * chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
  OSData * data = chosen ? OSDynamicCast(OSData, chosen->getProperty("boot-epoch")) : NULL;
  if (data != NULL && data->getLength() == sizeof(epoch)) {
   memcpy(&epoch, data->getBytesNoCopy(), sizeof(epoch));
  }
  OSSafeReleaseNULL(chosen);
 }
 if (epoch != 0) {
  clock_sec_t secs;
  clock_usec_t usecs;
  clock_get_system_microtime(&secs, &usecs);
  gBootEpoch = (long) epoch - (long) secs;
 }
}
long
GenericARMPE::getGMTTimeOfDay(void)
{
 clock_sec_t secs;
 clock_usec_t usecs;
 findBootEpoch();
 if (gBootEpoch == 0) {
  return 0;
 }
 clock_get_system_microtime(&secs, &usecs);
 return gBootEpoch + (long) secs;
}
void
GenericARMPE::setGMTTimeOfDay(long secs)
{
 clock_sec_t uptime;
 clock_usec_t usecs;
 clock_get_system_microtime(&uptime, &usecs);
 gBootEpoch = secs - (long) uptime;
 gBootEpochKnown = true;
}
const char *
GenericARMPE::deleteList(void)
{
 return "('packages', 'psuedo-usb', 'psuedo-hid', 'multiboot', 'rtas')";
}
const char *
GenericARMPE::excludeList(void)
{
 return "('chosen', 'memory', 'options', 'aliases', 'defaults', 'pram', 'cpus')";
}
bool
GenericARMPE::getMachineName(char * name, int maxLength)
{
 uint32_t real = 1;
 PE_parse_boot_argn("machine_name", &real, sizeof(real));
 if (!real || !IODTPlatformExpert::getMachineName(name, maxLength) || name[0] == 0) {
  strlcpy(name, "arm64", maxLength);
 }
 return true;
}
