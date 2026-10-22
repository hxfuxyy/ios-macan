#ifndef _LX_MORE_H_
#define _LX_MORE_H_
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#define IRQF_TRIGGER_RISING 0x0001
#define IRQ_NOAUTOEN (1 << 12)
#define IRQ_DISABLE_UNLAZY (1 << 19)
int enable_irq_wake(unsigned int irq);
int disable_irq_wake(unsigned int irq);
static inline bool device_may_wakeup(struct device *dev) { return (false); }
void *dma_alloc_noncoherent(struct device *dev, size_t size, dma_addr_t *handle,
    enum dma_data_direction dir, gfp_t gfp);
void dma_free_noncoherent(struct device *dev, size_t size, void *vaddr, dma_addr_t handle,
    enum dma_data_direction dir);
#define dma_rmb() rmb()
#define dma_wmb() wmb()
#define dma_mb() mb()
#undef MODULE_DEVICE_TABLE
#define MODULE_DEVICE_TABLE(bus,table)
#define WLAN_CATEGORY_RADIO_MEASUREMENT 5
#define WLAN_RM_ACTION_LINK_MEASUREMENT_REQUEST 2
#define WLAN_RM_ACTION_LINK_MEASUREMENT_REPORT 3
#define WIPHY_FLAG_NOTIFY_REGDOM_BY_DRIVER BIT(24)
#endif
