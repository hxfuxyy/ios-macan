#ifndef MACAN_DSP_H
#define MACAN_DSP_H
#include <kern/locks.h>
#include <mach/mach_types.h>
extern lck_mtx_t macan_aop_mailbox_lock;
extern lck_mtx_t macan_smem_alloc_lock;
extern lck_mtx_t macan_rpmh_lock;
uint32_t macan_ipcc_take(vm_offset_t base, uint32_t client);
void macan_adsp_init(void);
void macan_adsp_stop(void);
void macan_adsp_quiesce(void);
#endif
