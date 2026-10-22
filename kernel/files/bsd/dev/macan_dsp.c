#include <sys/macan_dsp.h>
#include <os/atomic_private.h>
static LCK_GRP_DECLARE(dsp_shared_group, "macan-dsp-shared");
LCK_MTX_DECLARE(macan_aop_mailbox_lock, &dsp_shared_group);
LCK_MTX_DECLARE(macan_smem_alloc_lock, &dsp_shared_group);
LCK_MTX_DECLARE(macan_rpmh_lock, &dsp_shared_group);
static uint32_t ipcc_lock;
static uint32_t pending[256];
static uint32_t
ipcc_read(vm_offset_t base)
{
 return *(volatile uint32_t *)(base + 0x10);
}
static void
ipcc_clear(vm_offset_t base, uint32_t who)
{
 *(volatile uint32_t *)(base + 0x1c) = who;
 __asm__ volatile ("dsb sy" : : : "memory");
}
uint32_t
macan_ipcc_take(vm_offset_t base, uint32_t client)
{
 uint32_t result = 0xffffffffu;
 if (!base || client >= 256) { return result; }
 if (!os_atomic_cmpxchg(&ipcc_lock, 0, 1, acquire)) { return result; }
 for (unsigned n = 0; n < 64; n++) {
  uint32_t who = ipcc_read(base);
  if (who == 0xffffffffu) { break; }
  uint32_t c = who >> 16, signal = who & 0xffff;
  if (c >= 256 || signal >= 32) { break; }
  pending[c] |= 1u << signal;
  ipcc_clear(base, who);
 }
 if (pending[client]) {
  unsigned signal = (unsigned)__builtin_ctz(pending[client]);
  pending[client] &= ~(1u << signal);
  result = client << 16 | signal;
 }
 os_atomic_store(&ipcc_lock, 0, release);
 return result;
}
