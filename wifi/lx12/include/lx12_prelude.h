#ifndef _LX12_PRELUDE_H
#define _LX12_PRELUDE_H
#include <linux/list.h>
#include <linux/skbuff.h>
#ifndef RCU_LOCKDEP_WARN
#define RCU_LOCKDEP_WARN(c,s) do { } while (0)
#endif
#define rcu_read_lock_any_held() 1
#ifndef KHZ_TO_MHZ
#define KHZ_TO_MHZ(freq) ((freq) / 1000)
#endif
#ifndef LIST_HEAD_INIT
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#endif
static inline void
list_cut_before(struct list_head *list, struct list_head *head, struct list_head *entry)
{
 if (head->next == entry) {
  INIT_LIST_HEAD(list);
  return;
 }
 list->next = head->next;
 list->next->prev = list;
 list->prev = entry->prev;
 list->prev->next = list;
 head->next = entry;
 entry->prev = head;
}
static inline int
skb_cow_data(struct sk_buff *skb, int tailbits, struct sk_buff **trailer)
{
 if (skb_tailroom(skb) < tailbits && pskb_expand_head(skb, 0, tailbits, GFP_ATOMIC))
  return -ENOMEM;
 *trailer = skb;
 return 1;
}
static inline void *
pskb_put(struct sk_buff *skb, struct sk_buff *tail, int len)
{
 return skb_put(tail, len);
}
#define IEEE80211_TPE_EIRP_ENTRIES_320MHZ 5
#define IEEE80211_TPE_PSD_ENTRIES_320MHZ 16
#define WLAN_EXT_CAPA11_BCN_PROTECT BIT(4)
#define WLAN_EID_NON_TX_BSSID_CAP 83
#define WLAN_ACTION_ADDBA_RESP 1
#define IEEE80211_EHT_PHY_CAP4_PSR_SR_SUPP 0x02
#define IEEE80211_EHT_OPER_MCS15_DISABLE 0x40
#define link_station_info station_info
#endif
