#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <sys/time.h>
#include <lx_os.h>
#include "lx.h"
int
device_printf(device_t dev, const char *format, ...)
{
 char line[400];
 va_list list;
 va_start(list, format);
 if (strcmp(format, "%pV") == 0) {
  const struct va_format *nested = va_arg(list, const struct va_format *);
  va_list copy;
  va_copy(copy, *nested->va);
  vsnprintf(line, sizeof(line), nested->fmt, copy);
  va_end(copy);
 } else {
  vsnprintf(line, sizeof(line), format, list);
 }
 va_end(list);
 printf("ath12k: %s", line);
 return (0);
}
int
eventratecheck(struct timeval *last, int *count, int most)
{
 return (1);
}
int
lkpi_hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen,
    bool ascii)
{
 const uint8_t *bytes = buf;
 size_t i, at = 0;
 if (rowsize != 16 && rowsize != 32)
  rowsize = 16;
 len = min(len, (size_t)rowsize);
 if (linebuflen > 0)
  linebuf[0] = '\0';
 for (i = 0; i < len && at + 4 < linebuflen; i++)
  at += snprintf(linebuf + at, linebuflen - at, i ? " %02x" : "%02x", bytes[i]);
 return ((int)at);
}
void
si_meminfo(struct sysinfo *si)
{
 memset(si, 0, sizeof(*si));
 si->totalram = (8ull << 30) / PAGE_SIZE;
 si->mem_unit = PAGE_SIZE;
}
void
sx_sysinit(const void *arg)
{
}
const struct property *
of_find_property(const struct device_node *np, const char *name, int *lenp)
{
 return (NULL);
}
struct device_node *
of_parse_phandle(const struct device_node *np, const char *name, int index)
{
 return (NULL);
}
struct sk_buff *
linuxkpi_skb_copy(const struct sk_buff *skb, gfp_t gfp)
{
 unsigned headroom = skb_headroom(skb);
 struct sk_buff *new = linuxkpi_dev_alloc_skb(headroom + skb->len, gfp);
 if (new == NULL)
  return (NULL);
 skb_reserve(new, headroom);
 skb_put(new, skb->len);
 memcpy(new->data - headroom, skb->data - headroom, headroom + skb->len);
 memcpy(new->cb, skb->cb, sizeof(skb->cb));
 return (new);
}
static struct {
 spinlock_t lock;
 struct list_head list;
 struct work_struct work;
 struct workqueue_struct *queue;
 struct wiphy *wiphy;
 bool ready;
} wwk;
static void
wiphy_work_run(struct work_struct *work)
{
 struct wiphy *wiphy = wwk.wiphy;
 struct wiphy_work *wk;
 mutex_lock(&wiphy->mtx);
 spin_lock_bh(&wwk.lock);
 wk = list_first_entry_or_null(&wwk.list, struct wiphy_work, entry);
 if (wk == NULL) {
  spin_unlock_bh(&wwk.lock);
  mutex_unlock(&wiphy->mtx);
  return;
 }
 list_del_init(&wk->entry);
 if (!list_empty(&wwk.list))
  queue_work(wwk.queue, work);
 spin_unlock_bh(&wwk.lock);
 wk->fn(wiphy, wk);
 mutex_unlock(&wiphy->mtx);
}
static void
wiphy_work_setup(struct wiphy *wiphy)
{
 if (!wwk.ready) {
  spin_lock_init(&wwk.lock);
  INIT_LIST_HEAD(&wwk.list);
  INIT_WORK(&wwk.work, wiphy_work_run);
  wwk.queue = create_singlethread_workqueue("wiphy");
  wwk.ready = true;
 }
 wwk.wiphy = wiphy;
}
void
linuxkpi_wiphy_work_queue(struct wiphy *wiphy, struct wiphy_work *wk)
{
 wiphy_work_setup(wiphy);
 spin_lock_bh(&wwk.lock);
 if (list_empty(&wk->entry))
  list_add_tail(&wk->entry, &wwk.list);
 spin_unlock_bh(&wwk.lock);
 queue_work(wwk.queue, &wwk.work);
}
void
linuxkpi_wiphy_work_cancel(struct wiphy *wiphy, struct wiphy_work *wk)
{
 wiphy_work_setup(wiphy);
 spin_lock_bh(&wwk.lock);
 if (!list_empty(&wk->entry))
  list_del_init(&wk->entry);
 spin_unlock_bh(&wwk.lock);
}
void
linuxkpi_wiphy_work_flush(struct wiphy *wiphy, struct wiphy_work *wk)
{
 struct wiphy_work *first;
 wiphy_work_setup(wiphy);
 spin_lock_bh(&wwk.lock);
 if (wk != NULL && list_empty(&wk->entry)) {
  spin_unlock_bh(&wwk.lock);
  return;
 }
 while (!list_empty(&wwk.list)) {
  first = list_first_entry(&wwk.list, struct wiphy_work, entry);
  list_del_init(&first->entry);
  spin_unlock_bh(&wwk.lock);
  first->fn(wiphy, first);
  spin_lock_bh(&wwk.lock);
  if (first == wk)
   break;
 }
 spin_unlock_bh(&wwk.lock);
}
