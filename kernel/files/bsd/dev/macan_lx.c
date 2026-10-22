#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/macan_input.h>
#include <sys/macan_lx.h>
#include <sys/macan_net.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);
#define say redwood_wifi_say
static LCK_GRP_DECLARE(lx_lock_group, "redwood-lx");
static LCK_MTX_DECLARE(lx_lock, &lx_lock_group);
void *
lx_os_alloc(unsigned long size)
{
 return kalloc_data(size, Z_WAITOK | Z_ZERO);
}
void
lx_os_free(void *memory, unsigned long size)
{
 kfree_data(memory, size);
}
void *
lx_os_radio_memory(unsigned long size, unsigned long long *address)
{
 vm_offset_t cached = 0, uncached;
 uint64_t physical;
 size = round_page(size);
 if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
     KERN_SUCCESS) {
  say("lx: no %lu bytes of memory in one piece\n", size);
  return NULL;
 }
 physical = kvtophys(cached);
 bzero((void *)cached, size);
 for (vm_offset_t line = cached; line < cached + size; line += 64) {
  __asm__ volatile ("dc civac, %0" : : "r" (line));
 }
 __asm__ volatile ("dsb sy");
 uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
 if (uncached == 0 || !redwood_wifi_radio_map(physical, size, address)) {
  return NULL;
 }
 return (void *)uncached;
}
void *
lx_os_map_registers(unsigned long long physical, unsigned long size)
{
 unsigned long long page = trunc_page(physical);
 vm_offset_t mapped = ml_io_map_unmappable(page, round_page(physical + size) - page, VM_WIMG_IO);
 return mapped == 0 ? NULL : (void *)(mapped + (physical - page));
}
void
lx_os_print(const char *text)
{
 say("%s", text);
}
void
lx_os_tell(const char *text)
{
 redwood_wifi_tell("%s", text);
}
void
lx_os_panic(const char *text)
{
 panic("%s", text);
}
unsigned long long
lx_os_nanoseconds(void)
{
 uint64_t nanoseconds;
 absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
 return nanoseconds;
}
void *
lx_os_thread_self(void)
{
 return current_thread();
}
struct lx_thread {
 void (*function)(void *);
 void *argument;
};
static void
lx_thread_runs(void *parameter, __unused wait_result_t result)
{
 struct lx_thread start = *(struct lx_thread *)parameter;
 kfree_type(struct lx_thread, parameter);
 start.function(start.argument);
 thread_terminate(current_thread());
}
int
lx_os_thread(const char *name, void (*function)(void *), void *argument)
{
 struct lx_thread *start = kalloc_type(struct lx_thread, Z_WAITOK | Z_ZERO);
 thread_t thread;
 if (start == NULL) {
  return ENOMEM;
 }
 start->function = function;
 start->argument = argument;
 if (kernel_thread_start(lx_thread_runs, start, &thread) != KERN_SUCCESS) {
  kfree_type(struct lx_thread, start);
  return EAGAIN;
 }
 thread_set_thread_name(thread, name);
 thread_deallocate(thread);
 return 0;
}
void
lx_os_lock(void)
{
 lck_mtx_lock(&lx_lock);
}
void
lx_os_unlock(void)
{
 lck_mtx_unlock(&lx_lock);
}
int
lx_os_sleep(const void *address, unsigned long long nanoseconds)
{
 uint64_t deadline;
 if (nanoseconds == 0) {
  return lck_mtx_sleep(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT) == THREAD_TIMED_OUT;
 }
 nanoseconds_to_absolutetime(nanoseconds, &deadline);
 deadline += mach_absolute_time();
 return lck_mtx_sleep_deadline(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT, deadline) == THREAD_TIMED_OUT;
}
void
lx_os_wakeup(const void *address)
{
 thread_wakeup((event_t)address);
}
void
lx_os_random(void *buffer, unsigned long size)
{
 read_random(buffer, (u_int)size);
}
unsigned
lx_os_option(const char *name)
{
 return redwood_wifi_option(name);
}
int
lx_os_firmware(const char *name, const void **data, unsigned long *size)
{
 uint64_t length = 0;
 const uint8_t *file = redwood_wifi_driver_file(name, &length);
 if (file == NULL) {
  return ENOENT;
 }
 *data = file;
 *size = (unsigned long)length;
 return 0;
}
int
lx_os_rproc_boot(void)
{
 return redwood_wifi_dsp_is_up() ? 0 : ENXIO;
}
int
lx_os_radio_calibrated(void)
{
 return redwood_wifi_driver_calibrated();
}
int
lx_os_radio_address(unsigned char *address)
{
 return redwood_wifi_radio_address(address);
}
int
lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length)
{
 return redwood_wifi_network(name, name_length, passphrase, passphrase_length);
}
#define LX_QUEUE 256
struct lx_frame {
 uint16_t length;
 uint8_t bytes[REDWOOD_NET_FRAME_MAX];
};
static bool lx_link_attached;
static LCK_MTX_DECLARE(lx_queue_lock, &lx_lock_group);
static struct lx_frame *lx_queue;
static uint32_t lx_queue_in, lx_queue_out;
static uint64_t lx_queue_dropped;
static void
lx_queue_runs(__unused void *parameter, __unused wait_result_t result)
{
 for (;;) {
  struct lx_frame *frame;
  lck_mtx_lock(&lx_queue_lock);
  while (lx_queue_in == lx_queue_out) {
   lck_mtx_sleep(&lx_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_queue_in, THREAD_UNINT);
  }
  frame = &lx_queue[lx_queue_out % LX_QUEUE];
  lck_mtx_unlock(&lx_queue_lock);
  lx_transmit(frame->bytes, frame->length);
  lck_mtx_lock(&lx_queue_lock);
  lx_queue_out++;
  lck_mtx_unlock(&lx_queue_lock);
 }
}
static bool
lx_link_transmit(mbuf_t packet, size_t length)
{
 struct lx_frame *frame;
 lck_mtx_lock(&lx_queue_lock);
 if (length > REDWOOD_NET_FRAME_MAX || lx_queue_in - lx_queue_out == LX_QUEUE) {
  lx_queue_dropped++;
  lck_mtx_unlock(&lx_queue_lock);
  return false;
 }
 frame = &lx_queue[lx_queue_in % LX_QUEUE];
 if (mbuf_copydata(packet, 0, length, frame->bytes) != 0) {
  lck_mtx_unlock(&lx_queue_lock);
  return false;
 }
 frame->length = (uint16_t)length;
 lx_queue_in++;
 lck_mtx_unlock(&lx_queue_lock);
 thread_wakeup((event_t)&lx_queue_in);
 return true;
}
static struct redwood_net_link lx_link = {
 .name = "the phone's radio (ath11k)",
 .wireless = true,
 .transmit = lx_link_transmit,
};
void
lx_os_net_attach(const unsigned char *address)
{
 thread_t thread;
 if (lx_link_attached) {
  return;
 }
 lx_queue = kalloc_data(LX_QUEUE * sizeof(*lx_queue), Z_WAITOK | Z_ZERO);
 if (lx_queue == NULL || kernel_thread_start(lx_queue_runs, NULL, &thread) != KERN_SUCCESS) {
  say("lx: no memory or no thread for the frames to send: the radio's frames go nowhere\n");
  return;
 }
 thread_set_thread_name(thread, "wifi-send");
 thread_deallocate(thread);
 memcpy(lx_link.address, address, sizeof(lx_link.address));
 lx_link_attached = redwood_net_attach(&lx_link);
 say("lx: %s\n", lx_link_attached ? "the radio is the link behind en0" : "EN0 HAS A LINK ALREADY: the radio's frames go "
     "nowhere");
}
void
redwood_lx_net_early(void)
{
 uint8_t address[6];
 if (redwood_wifi_radio_address(address)) {
  lx_os_net_attach(address);
 }
}
void
lx_os_net_link(int up)
{
 if (lx_link_attached) {
  redwood_net_link_state(up != 0);
 }
}
void
lx_os_net_input(const unsigned char *frame, unsigned long length)
{
 if (lx_link_attached && length <= REDWOOD_NET_FRAME_MAX) {
  redwood_net_input(frame, length);
 }
}
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void gic_v3_spi_detach(uint32_t intid);
extern uint64_t gic_v3_spi_count(uint32_t intid);
#define LX_VECTORS 32
static struct {
 void (*function)(void *);
 void *argument;
 bool attached;
} lx_vector[LX_VECTORS];
static uint32_t lx_irq_first;
static uint32_t lx_irq_pending;
static uint32_t lx_irq_running;
static thread_t lx_irq_thread;
static LCK_MTX_DECLARE(lx_irq_lock, &lx_lock_group);
static void
lx_irq_arrived(__unused void *context, uint32_t intid)
{
 gic_v3_spi_mask(intid);
 os_atomic_or(&lx_irq_pending, 1u << (intid - lx_irq_first), relaxed);
 thread_wakeup((event_t)&lx_irq_pending);
}
static void
lx_irq_runs(__unused void *parameter, __unused wait_result_t result)
{
 for (;;) {
  uint32_t pending;
  for (;;) {
   assert_wait((event_t)&lx_irq_pending, THREAD_UNINT);
   if ((pending = os_atomic_xchg(&lx_irq_pending, 0, acquire)) != 0) {
    clear_wait(current_thread(), THREAD_AWAKENED);
    break;
   }
   thread_block(THREAD_CONTINUE_NULL);
  }
  for (unsigned v = 0; v < LX_VECTORS; v++) {
   if (!(pending & 1u << v)) {
    continue;
   }
   lck_mtx_lock(&lx_irq_lock);
   if (!lx_vector[v].attached) {
    lck_mtx_unlock(&lx_irq_lock);
    continue;
   }
   lx_irq_running |= 1u << v;
   lck_mtx_unlock(&lx_irq_lock);
   lx_vector[v].function(lx_vector[v].argument);
   lck_mtx_lock(&lx_irq_lock);
   lx_irq_running &= ~(1u << v);
   lck_mtx_unlock(&lx_irq_lock);
   thread_wakeup((event_t)&lx_irq_running);
  }
 }
}
int
lx_os_irq_msi(unsigned long long *address, unsigned *data)
{
 uint32_t first = 0;
 if (!redwood_wifi_radio_msi(address, data, &first)) {
  return 0;
 }
 lx_irq_first = first;
 return 1;
}
int
lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument)
{
 kern_return_t kr;
 if (vector >= LX_VECTORS || lx_irq_first == 0 || function == NULL) {
  return EINVAL;
 }
 if (lx_irq_thread == THREAD_NULL) {
  redwood_wifi_tell("lx: wifi_irq: THE GIC'S DISTRIBUTOR IS WRITTEN for the radio's interrupts: each vector's SPI (%u + v) edge-"
      "triggered, group 1, routed to this processor, then enabled\n", lx_irq_first);
  if (kernel_thread_start(lx_irq_runs, NULL, &lx_irq_thread) != KERN_SUCCESS) {
   lx_irq_thread = THREAD_NULL;
   return ENOMEM;
  }
  thread_set_thread_name(lx_irq_thread, "wifi-irq");
 }
 lck_mtx_lock(&lx_irq_lock);
 lx_vector[vector].function = function;
 lx_vector[vector].argument = argument;
 lx_vector[vector].attached = true;
 lck_mtx_unlock(&lx_irq_lock);
 if ((kr = gic_v3_spi_attach(lx_irq_first + vector, true, lx_irq_arrived, NULL)) != KERN_SUCCESS) {
  lck_mtx_lock(&lx_irq_lock);
  lx_vector[vector].attached = false;
  lck_mtx_unlock(&lx_irq_lock);
  return kr == KERN_NAME_EXISTS ? EBUSY : EINVAL;
 }
 gic_v3_spi_mask(lx_irq_first + vector);
 return 0;
}
void
lx_os_irq_enable(unsigned vector)
{
 if (vector < LX_VECTORS && lx_vector[vector].attached) {
  gic_v3_spi_unmask(lx_irq_first + vector);
 }
}
void
lx_os_irq_disable(unsigned vector)
{
 if (vector >= LX_VECTORS || !lx_vector[vector].attached) {
  return;
 }
 gic_v3_spi_mask(lx_irq_first + vector);
 if (current_thread() == lx_irq_thread) {
  return;
 }
 lck_mtx_lock(&lx_irq_lock);
 while (lx_irq_running & 1u << vector) {
  lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
 }
 lck_mtx_unlock(&lx_irq_lock);
}
void
lx_os_irq_detach_all(void)
{
 for (unsigned v = 0; v < LX_VECTORS; v++) {
  if (lx_vector[v].attached) {
   gic_v3_spi_detach(lx_irq_first + v);
  }
 }
 lck_mtx_lock(&lx_irq_lock);
 for (unsigned v = 0; v < LX_VECTORS; v++) {
  lx_vector[v].attached = false;
 }
 while (lx_irq_running != 0 && current_thread() != lx_irq_thread) {
  lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
 }
 lck_mtx_unlock(&lx_irq_lock);
 os_atomic_store(&lx_irq_pending, 0, relaxed);
}
unsigned long long
lx_os_irq_count(unsigned vector)
{
 return vector < LX_VECTORS && lx_irq_first != 0 ? gic_v3_spi_count(lx_irq_first + vector) : 0;
}
void
lx_os_station(const struct redwood_sta_ops *ops)
{
 redwood_sta_attach(ops);
}
void
lx_os_station_lost(int error)
{
 redwood_sta_lost(error);
}
__attribute__((weak)) int
lx_start(void)
{
 say("lx: THE DRIVER IS NOT BUILT IN: its object was not there when this kernel was built (tools/mk-ath11k.sh, then "
     "build the kernel again)\n");
 return ENXIO;
}
__attribute__((weak)) void
lx_stop(void)
{
}
__attribute__((weak)) int
lx_prepare(void)
{
 return 0;
}
__attribute__((weak)) int
lx_transmit(__unused const unsigned char *frame, __unused unsigned long length)
{
 return ENXIO;
}
int
redwood_lx_start(void)
{
 return lx_start();
}
void
redwood_lx_stop(void)
{
 lx_stop();
}
int
redwood_lx_prepare(void)
{
 return lx_prepare();
}
