#include <linux/kernel.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>
#include <lx_os.h>
#include "lx.h"
#include "lx_wpa.h"
int linuxkpi_debug_80211;
#define HEARD_MOST 128
#define ELEMENTS_MOST REDWOOD_STA_ELEMENTS_MOST
struct lx_heard {
 uint8_t sender[ETH_ALEN];
 uint8_t name[32], name_length;
 uint16_t frequency;
 uint8_t says_channel;
 int8_t signal;
 uint16_t capability, interval;
 bool protected;
 unsigned times;
 unsigned long long last;
 uint16_t elements_length;
 uint8_t elements[ELEMENTS_MOST];
};
struct lx_target {
 bool found;
 uint8_t address[ETH_ALEN];
 uint16_t frequency, capability, interval;
 int8_t signal;
 unsigned elements_length;
 uint8_t elements[ELEMENTS_MOST];
};
enum { ANSWER_NONE, ANSWER_AUTHENTICATED, ANSWER_ASSOCIATED, ANSWER_REFUSED, ANSWER_GONE };
struct lx_radio {
 void *memory;
 struct wiphy *wiphy;
 struct ieee80211_hw hw;
 const struct ieee80211_ops *ops;
 bool registered;
 bool kept;
 int applying;
 struct workqueue_struct *work;
 struct linuxkpi_ieee80211_regdomain *regd;
 struct ieee80211_vif *vif;
 void *vif_memory;
 bool started;
 volatile int scan;
 unsigned frames, others;
 unsigned heard_count, heard_lost;
 struct lx_heard heard[HEARD_MOST];
 uint8_t want[32], want_length;
 unsigned width;
 struct lx_target target;
 struct ieee80211_sta *sta;
 void *sta_memory;
 struct ieee80211_chanctx_conf *channel;
 struct cfg80211_bss bss;
 struct cfg80211_bss_ies bss_elements;
 volatile int answer;
 uint16_t answer_status, aid;
 unsigned answer_length;
 uint8_t answer_elements[ELEMENTS_MOST];
 uint8_t rsn[24];
 struct lx_wpa wpa;
 uint16_t data_sequence;
 volatile int handshake;
 const char *handshake_why;
 struct work_struct group_work;
 struct work_struct ba_work;
 volatile uint8_t ba_asked;
 struct {
  bool start;
  bool active;
  bool amsdu;
  uint8_t token;
  uint16_t ssn, size, timeout;
 } ba[8];
 unsigned ba_sessions, ba_refused;
 struct ieee80211_key_conf *pairwise_key, *group_key;
 volatile bool joined;
 volatile bool joining;
 int reached;
 int8_t signal;
 unsigned tim_beacons, tim_ours, tim_group;
 unsigned probes_sent, probe_answers;
 int last_error;
 unsigned sent, sent_lost, received, received_group, eapol, rekeys;
 unsigned rx_ethernet, rx_native_data, rx_native_rejected;
 unsigned rx_source_ours, rx_destination_ours;
 struct { uint8_t bytes[32]; uint32_t flags, length; } rx_headers[4];
 unsigned rx_header_count;
 unsigned long long joined_at;
};
static struct lx_radio *the_radio;
static const uint8_t no_network[ETH_ALEN];
static struct lx_radio *
radio_of(struct ieee80211_hw *hw)
{
 return (__containerof(hw, struct lx_radio, hw));
}
struct ieee80211_hw *
linuxkpi_ieee80211_alloc_hw(size_t private, const struct ieee80211_ops *ops)
{
 size_t ours = roundup2(sizeof(struct lx_radio), 64);
 uint8_t *memory = kzalloc(64 + offsetof(struct wiphy, priv) + ours + private, GFP_KERNEL);
 struct lx_radio *radio;
 struct wiphy *wiphy;
 if (memory == NULL) {
  return (NULL);
 }
 wiphy = (struct wiphy *)roundup2((uintptr_t)memory, 64);
 radio = (struct lx_radio *)wiphy->priv;
 radio->memory = memory;
 radio->wiphy = wiphy;
 radio->ops = ops;
 radio->hw.wiphy = wiphy;
 radio->hw.priv = (uint8_t *)radio + ours;
 radio->work = alloc_ordered_workqueue("wifi-80211", 0);
 mutex_init(&wiphy->mtx);
 return (&radio->hw);
}
struct ieee80211_hw *
linuxkpi_wiphy_to_ieee80211_hw(struct wiphy *wiphy)
{
 return (&((struct lx_radio *)wiphy->priv)->hw);
}
void
linuxkpi_set_ieee80211_dev(struct ieee80211_hw *hw)
{
}
int
linuxkpi_80211_wiphy_register(struct wiphy *wiphy)
{
 return (0);
}
int
linuxkpi_ieee80211_ifattach(struct ieee80211_hw *hw)
{
 struct lx_radio *radio = radio_of(hw);
 char bands[96] = "";
 for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
  if (hw->wiphy->bands[band] != NULL) {
   snprintf(bands + strlen(bands), sizeof(bands) - strlen(bands), " %d in band %u,", hw->wiphy->bands[band]->n_channels,
       band);
  }
 }
 printf("lx: the driver registers a radio: channels:%s (0: 2.4 GHz, 1: 5 GHz, 3: 6 GHz); %s; up to %d names and %u "
     "bytes of extras in a scan\n", bands, is_zero_ether_addr(hw->wiphy->perm_addr) ? "IT HAS NO ADDRESS" :
     "it has an address", hw->wiphy->max_scan_ssids, hw->wiphy->max_scan_ie_len);
 radio->registered = true;
 the_radio = radio;
 return (0);
}
void
linuxkpi_ieee80211_unregister_hw(struct ieee80211_hw *hw)
{
 struct lx_radio *radio = radio_of(hw);
 lx_os_lock();
 radio->registered = false;
 while (radio->applying != 0) {
  lx_os_sleep(&radio->applying, 5 * 1000000ull);
 }
 if (the_radio == radio) {
  the_radio = NULL;
 }
 lx_os_unlock();
}
void
linuxkpi_ieee80211_iffree(struct ieee80211_hw *hw)
{
 struct lx_radio *radio = radio_of(hw);
 if (radio->work != NULL) {
  destroy_workqueue(radio->work);
  radio->work = NULL;
 }
}
void
linuxkpi_wiphy_free(struct wiphy *wiphy)
{
 struct lx_radio *radio = (struct lx_radio *)wiphy->priv;
 kfree(radio->regd);
 kfree(radio->memory);
}
static void
regd_apply(struct lx_radio *radio)
{
 const struct linuxkpi_ieee80211_regdomain *regd = radio->regd;
 char said[160] = "";
 for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
  struct ieee80211_supported_band *has = radio->wiphy->bands[band];
  int allowed = 0;
  for (int i = 0; has != NULL && i < has->n_channels; i++) {
   struct linuxkpi_ieee80211_channel *channel = &has->channels[i];
   const struct ieee80211_reg_rule *rule = NULL;
   if (!radio->kept) {
    channel->orig_flags = channel->flags;
    channel->orig_mpwr = channel->max_power;
   }
   for (int r = 0; r < regd->n_reg_rules && rule == NULL; r++) {
    if (regd->reg_rules[r].freq_range.start_freq_khz <= ((int)channel->center_freq - 10) * 1000 &&
        regd->reg_rules[r].freq_range.end_freq_khz >= ((int)channel->center_freq + 10) * 1000) {
     rule = &regd->reg_rules[r];
    }
   }
   channel->flags = channel->orig_flags & ~(IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR);
   if (rule == NULL) {
    channel->flags |= IEEE80211_CHAN_DISABLED;
    continue;
   }
   if (rule->flags & NL80211_RRF_NO_IR) {
    channel->flags |= IEEE80211_CHAN_NO_IR;
   }
   if (rule->flags & NL80211_RRF_DFS) {
    channel->flags |= IEEE80211_CHAN_RADAR;
   }
   channel->max_reg_power = rule->power_rule.max_eirp / 100;
   channel->max_antenna_gain = rule->power_rule.max_antenna_gain / 100;
   channel->max_power = channel->orig_mpwr != 0 && channel->orig_mpwr < channel->max_reg_power ?
       channel->orig_mpwr : channel->max_reg_power;
   allowed++;
  }
  if (has != NULL) {
   snprintf(said + strlen(said), sizeof(said) - strlen(said), " %d of %d in band %u,", allowed, has->n_channels, band);
  }
 }
 radio->kept = true;
 printf("lx: the regulatory domain \"%c%c\" (%d rules) allows the radio's channels:%s\n", regd->alpha2[0], regd->alpha2[1],
     regd->n_reg_rules, said);
}
int
linuxkpi_regulatory_set_wiphy_regd_sync(struct wiphy *wiphy, struct linuxkpi_ieee80211_regdomain *regd)
{
 struct lx_radio *radio = (struct lx_radio *)wiphy->priv;
 struct linuxkpi_ieee80211_regdomain *copy, *old;
 size_t size;
 if (regd == NULL) {
  return (-EINVAL);
 }
 size = sizeof(*regd) + regd->n_reg_rules * sizeof(regd->reg_rules[0]);
 copy = kmalloc(size, GFP_KERNEL);
 if (copy == NULL) {
  return (-ENOMEM);
 }
 memcpy(copy, regd, size);
 lx_os_lock();
 if (!radio->registered) {
  lx_os_unlock();
  kfree(copy);
  return (-ENODEV);
 }
 radio->applying++;
 lx_os_unlock();
 old = radio->regd;
 radio->regd = copy;
 wiphy->regd = copy;
 regd_apply(radio);
 if (wiphy->reg_notifier != NULL) {
  struct regulatory_request request = { .initiator = NL80211_REGDOM_SET_BY_DRIVER };
  memcpy(request.alpha2, copy->alpha2, sizeof(request.alpha2));
  wiphy->reg_notifier(wiphy, &request);
 }
 lx_os_lock();
 radio->applying--;
 lx_os_wakeup(&radio->applying);
 lx_os_unlock();
 kfree(old);
 return (0);
}
uint32_t
linuxkpi_ieee80211_channel_to_frequency(uint32_t channel, enum nl80211_band band)
{
 switch (band) {
 case NL80211_BAND_2GHZ:
  return (channel == 14 ? 2484 : 2407 + 5 * channel);
 case NL80211_BAND_5GHZ:
  return (channel >= 182 && channel <= 196 ? 4000 + 5 * channel : 5000 + 5 * channel);
 case NL80211_BAND_6GHZ:
  return (channel == 2 ? 5935 : 5950 + 5 * channel);
 default:
  return (0);
 }
}
static unsigned
channel_of(unsigned frequency)
{
 return (frequency == 2484 ? 14 : frequency < 2484 ? (frequency - 2407) / 5 : frequency < 5950 ? (frequency - 5000) / 5 :
        (frequency - 5950) / 5);
}
uint32_t
linuxkpi_ieee80211_frequency_to_channel(uint32_t frequency, uint32_t flags)
{
 return (channel_of(frequency));
}
struct linuxkpi_ieee80211_channel *
linuxkpi_ieee80211_get_channel(struct wiphy *wiphy, uint32_t frequency)
{
 for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
  struct ieee80211_supported_band *has = wiphy->bands[band];
  for (int i = 0; has != NULL && i < has->n_channels; i++) {
   if (has->channels[i].center_freq == frequency) {
    return (&has->channels[i]);
   }
  }
 }
 return (NULL);
}
void
linuxkpi_ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work)
{
 queue_work(radio_of(hw)->work, work);
}
void
linuxkpi_ieee80211_queue_delayed_work(struct ieee80211_hw *hw, struct delayed_work *work, int delay)
{
 queue_delayed_work(radio_of(hw)->work, work, delay);
}
void
linuxkpi_ieee80211_iterate_interfaces(struct ieee80211_hw *hw, enum ieee80211_iface_iter flags,
    void (*function)(void *, uint8_t *, struct ieee80211_vif *), void *argument)
{
 struct ieee80211_vif *vif = radio_of(hw)->vif;
 if (vif != NULL) {
  function(argument, vif->addr, vif);
 }
}
void
linuxkpi_ieee80211_iterate_stations_atomic(struct ieee80211_hw *hw, void (*function)(void *, struct ieee80211_sta *), void *argument)
{
}
void
linuxkpi_ieee80211_iterate_chan_contexts(struct ieee80211_hw *hw,
    void (*function)(struct ieee80211_hw *, struct ieee80211_chanctx_conf *, void *), void *argument)
{
}
struct ieee80211_sta *
linuxkpi_ieee80211_find_sta(struct ieee80211_vif *vif, const u8 *address)
{
 struct lx_radio *radio = the_radio;
 return (radio != NULL && radio->sta != NULL && memcmp(radio->sta->addr, address, ETH_ALEN) == 0 ? radio->sta : NULL);
}
struct ieee80211_sta *
linuxkpi_ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const uint8_t *address, const uint8_t *own)
{
 struct lx_radio *radio = radio_of(hw);
 return (radio->sta != NULL && memcmp(radio->sta->addr, address, ETH_ALEN) == 0 ? radio->sta : NULL);
}
struct cfg80211_bss *
linuxkpi_cfg80211_get_bss(struct wiphy *wiphy, struct linuxkpi_ieee80211_channel *channel, const uint8_t *bssid,
    const uint8_t *ssid, size_t ssid_length, enum ieee80211_bss_type type, enum ieee80211_privacy privacy)
{
 struct lx_radio *radio = (struct lx_radio *)wiphy->priv;
 if (!radio->target.found || bssid == NULL || memcmp(radio->target.address, bssid, ETH_ALEN) != 0) {
  return (NULL);
 }
 radio->bss_elements.data = radio->target.elements;
 radio->bss_elements.len = radio->target.elements_length;
 radio->bss.ies = radio->bss.beacon_ies = &radio->bss_elements;
 radio->bss.signal = radio->target.signal * 100;
 return (&radio->bss);
}
void
linuxkpi_cfg80211_put_bss(struct wiphy *wiphy, struct cfg80211_bss *bss)
{
}
void
linuxkpi_ieee80211_wake_queues(struct ieee80211_hw *hw)
{
}
void
linuxkpi_ieee80211_stop_queues(struct ieee80211_hw *hw)
{
}
void
linuxkpi_ieee80211_handle_wake_tx_queue(struct ieee80211_hw *hw, struct ieee80211_txq *queue)
{
}
void
linuxkpi_ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb)
{
 dev_kfree_skb_any(skb);
}
void
linuxkpi_ieee80211_tx_status_ext(struct ieee80211_hw *hw, struct ieee80211_tx_status *status)
{
 if (status->skb != NULL) {
  if (status->skb->protocol == cpu_to_be16(ETH_P_PAE)) {
   printf("lx: EAPOL tx completed: ack=%d flags=%#x\n",
       !!(IEEE80211_SKB_CB(status->skb)->flags & IEEE80211_TX_STAT_ACK),
       IEEE80211_SKB_CB(status->skb)->flags);
  }
  dev_kfree_skb_any(status->skb);
 }
}
void
linuxkpi_ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb, int status)
{
 dev_kfree_skb_any(skb);
}
void
linuxkpi_ieee80211_beacon_loss(struct ieee80211_vif *vif)
{
 printf("lx: the driver reports beacons lost\n");
}
void
linuxkpi_ieee80211_connection_loss(struct ieee80211_vif *vif)
{
 printf("lx: the driver reports the connection lost\n");
}
void
linuxkpi_ieee80211_restart_hw(struct ieee80211_hw *hw)
{
}
static const uint8_t *
element(const uint8_t *elements, unsigned length, unsigned number)
{
 for (unsigned at = 0; at + 2 <= length && at + 2 + elements[at + 1] <= length; at += 2 + elements[at + 1]) {
  if (elements[at] == number) {
   return (elements + at);
  }
 }
 return (NULL);
}
static const uint8_t *
vendor_element(const uint8_t *elements, unsigned length, const uint8_t *begins)
{
 for (unsigned at = 0; at + 2 <= length && at + 2 + elements[at + 1] <= length; at += 2 + elements[at + 1]) {
  if (elements[at] == 221 && elements[at + 1] >= 4 && memcmp(elements + at + 2, begins, 4) == 0) {
   return (elements + at);
  }
 }
 return (NULL);
}
static const uint8_t wmm[4] = { 0x00, 0x50, 0xf2, 0x02 };
static void
heard(struct lx_radio *radio, const uint8_t *frame, unsigned length, unsigned frequency, int signal)
{
 struct lx_heard *network = NULL;
 const uint8_t *elements = frame + 36, *name = element(elements, length - 36, 0);
 lx_os_lock();
 if (name != NULL && radio->want_length != 0 && name[1] == radio->want_length &&
     memcmp(name + 2, radio->want, radio->want_length) == 0 && (!radio->target.found || signal > radio->target.signal) &&
     length - 36 <= ELEMENTS_MOST && radio->sta == NULL) {
  radio->target.found = true;
  memcpy(radio->target.address, frame + 16, ETH_ALEN);
  radio->target.frequency = (uint16_t)frequency;
  radio->target.interval = (uint16_t)(frame[32] | frame[33] << 8);
  radio->target.capability = (uint16_t)(frame[34] | frame[35] << 8);
  radio->target.signal = (int8_t)signal;
  radio->target.elements_length = length - 36;
  memcpy(radio->target.elements, elements, length - 36);
 }
 for (unsigned i = 0; i < radio->heard_count && network == NULL; i++) {
  if (memcmp(radio->heard[i].sender, frame + 16, ETH_ALEN) == 0) {
   network = &radio->heard[i];
  }
 }
 if (network == NULL) {
  if (radio->heard_count == HEARD_MOST) {
   radio->heard_lost++;
   lx_os_unlock();
   return;
  }
  network = &radio->heard[radio->heard_count++];
  bzero(network, sizeof(*network));
  memcpy(network->sender, frame + 16, ETH_ALEN);
  network->signal = -128;
 }
 network->times++;
 network->last = lx_os_nanoseconds();
 if (frame[0] == 0x50 && radio->joined && radio->vif != NULL && memcmp(frame + 4, radio->vif->addr, ETH_ALEN) == 0 &&
     memcmp(frame + 16, radio->target.address, ETH_ALEN) == 0) {
  radio->probe_answers++;
 }
 network->interval = (uint16_t)(frame[32] | frame[33] << 8);
 network->capability = (uint16_t)(frame[34] | frame[35] << 8);
 if (length - 36 <= ELEMENTS_MOST) {
  network->elements_length = (uint16_t)(length - 36);
  memcpy(network->elements, elements, length - 36);
 }
 if (radio->sta != NULL && memcmp(radio->target.address, frame + 16, ETH_ALEN) == 0) {
  const uint8_t *tim = frame[0] == 0x80 ? element(elements, length - 36, 5) : NULL;
  radio->signal = (int8_t)signal;
  if (tim != NULL && tim[1] >= 4 && radio->joined) {
   unsigned offset = (tim[4] & 0xfe) * 8u, bit = radio->aid;
   radio->tim_beacons++;
   radio->tim_group += tim[4] & 1;
   radio->tim_ours += bit >= offset && bit < offset + (tim[1] - 3) * 8u && (tim[5 + (bit - offset) / 8] >> (bit % 8) & 1);
  }
 }
 if (signal > network->signal) {
  network->signal = (int8_t)signal;
  network->frequency = (uint16_t)frequency;
 }
 if (name != NULL && name[1] <= sizeof(network->name) && (name[1] != 0 || network->name_length == 0)) {
  network->name_length = name[1];
  memcpy(network->name, name + 2, name[1]);
 }
 if (element(elements, length - 36, 3) != NULL && element(elements, length - 36, 3)[1] == 1) {
  network->says_channel = element(elements, length - 36, 3)[2];
 }
 network->protected = element(elements, length - 36, 48) != NULL;
 lx_os_unlock();
}
void
linuxkpi_ieee80211_scan_completed(struct ieee80211_hw *hw, struct cfg80211_scan_info *info)
{
 struct lx_radio *radio = radio_of(hw);
 lx_os_lock();
 radio->scan = info != NULL && info->aborted ? 3 : 2;
 lx_os_wakeup(&radio->scan);
 lx_os_unlock();
}
static void
say_heard(struct lx_radio *radio)
{
 unsigned on_channel[200] = { 0 }, low = 0, high = 0, open = 0, nameless = 0;
 int strongest = -128, weakest = 127;
 char line[400] = "";
 bool said[HEARD_MOST] = { false };
 for (unsigned n = 0; n < radio->heard_count; n++) {
  const struct lx_heard *network = NULL;
  char name[33];
  unsigned which = 0, channel;
  bool wanted;
  for (unsigned i = 0; i < radio->heard_count; i++) {
   if (!said[i] && (network == NULL || radio->heard[i].signal > network->signal)) {
    network = &radio->heard[i];
    which = i;
   }
  }
  said[which] = true;
  for (unsigned i = 0; i < network->name_length; i++) {
   name[i] = network->name[i] >= 0x20 && network->name[i] < 0x7f ? (char)network->name[i] : '?';
  }
  name[network->name_length] = 0;
  channel = channel_of(network->frequency);
  if (channel < nitems(on_channel)) {
   on_channel[channel]++;
  }
  *(network->frequency < 4000 ? &low : &high) += 1;
  open += !(network->capability & 0x10);
  nameless += network->name_length == 0 || network->name[0] == 0;
  strongest = network->signal > strongest ? network->signal : strongest;
  weakest = network->signal < weakest ? network->signal : weakest;
  wanted = radio->want_length != 0 && network->name_length == radio->want_length &&
      memcmp(network->name, radio->want, radio->want_length) == 0;
  printf("lx: heard: channel %3u (%u MHz) %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x  %s, %u times  \"%s\"\n", channel,
      network->frequency, network->signal, network->sender[0], network->sender[1], network->sender[2], network->sender[3],
      network->sender[4], network->sender[5], !(network->capability & 0x10) ? "open" : network->protected ? "RSN" :
      "protected", network->times, wanted ? "(the network that is wanted)" : name);
 }
 for (unsigned channel = 0; channel < nitems(on_channel); channel++) {
  if (on_channel[channel] != 0) {
   snprintf(line + strlen(line), sizeof(line) - strlen(line), " %u on %u,", on_channel[channel], channel);
  }
 }
 printf("lx: THE SCAN HEARD %u NETWORKS (access points): %u at 2.4 GHz, %u at 5 GHz; by channel:%s the strongest at %d dBm, "
     "the weakest at %d; %u open, %u without a name; %u frames in all, %u of them neither beacon nor answer to a probe%s\n",
     radio->heard_count, low, high, line, radio->heard_count ? strongest : 0, radio->heard_count ? weakest : 0, open, nameless,
     radio->frames, radio->others, radio->heard_lost ? "; MORE WERE HEARD THAN THERE WAS ROOM FOR" : "");
}
static struct sk_buff *
frame_new(struct lx_radio *radio, unsigned length, uint8_t **bytes)
{
 struct sk_buff *skb = dev_alloc_skb(length + 256);
 struct ieee80211_tx_info *info;
 if (skb == NULL) {
  return (NULL);
 }
 skb_reserve(skb, 128);
 *bytes = skb_put(skb, length);
 info = IEEE80211_SKB_CB(skb);
 bzero(info, sizeof(*info));
 info->control.vif = radio->vif;
 info->band = radio->channel != NULL ? radio->channel->def.chan->band : NL80211_BAND_2GHZ;
 return (skb);
}
static void
frame_send(struct lx_radio *radio, struct sk_buff *skb)
{
 struct ieee80211_tx_control control = { .sta = radio->sta };
 radio->ops->tx(&radio->hw, &control, skb);
}
static uint8_t *
management(struct lx_radio *radio, uint8_t *frame, unsigned kind)
{
 bzero(frame, 24);
 frame[0] = (uint8_t)kind;
 memcpy(frame + 4, radio->target.address, ETH_ALEN);
 memcpy(frame + 10, radio->vif->addr, ETH_ALEN);
 memcpy(frame + 16, radio->target.address, ETH_ALEN);
 return (frame + 24);
}
static bool
ethernet_send(struct lx_radio *radio, const uint8_t *frame, unsigned length)
{
 struct sk_buff *skb;
 uint8_t *bytes;
 bool control_port = length >= 14 && frame[12] == 0x88 && frame[13] == 0x8e;
 bool native = control_port || (radio->vif != NULL && !(radio->vif->offload_flags & IEEE80211_OFFLOAD_ENCAP_ENABLED));
 if (radio->vif == NULL || radio->sta == NULL || (skb = frame_new(radio, length + (native ? 18 : 0), &bytes)) == NULL) {
  radio->sent_lost++;
  return (false);
 }
 skb->protocol = cpu_to_be16((unsigned)frame[12] << 8 | frame[13]);
 if (native) {
  bzero(bytes, 24);
  lx_os_lock();
  uint16_t sequence = (uint16_t)((radio->data_sequence++ & 0x0fff) << 4);
  lx_os_unlock();
  bytes[22] = (uint8_t)sequence;
  bytes[23] = (uint8_t)(sequence >> 8);
  bytes[0] = 0x08;
  bytes[1] = 0x01 | (!control_port && radio->pairwise_key != NULL ? 0x40 : 0);
  memcpy(bytes + 4, radio->target.address, ETH_ALEN);
  memcpy(bytes + 10, radio->vif->addr, ETH_ALEN);
  memcpy(bytes + 16, frame, ETH_ALEN);
  memcpy(bytes + 24, "\xaa\xaa\x03\x00\x00\x00", 6);
  memcpy(bytes + 30, frame + 12, 2);
  memcpy(bytes + 32, frame + 14, length - 14);
  IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_CTL_ASSIGN_SEQ;
  IEEE80211_SKB_CB(skb)->control.hw_key = control_port ? NULL : radio->pairwise_key;
 } else {
  memcpy(bytes, frame, length);
  IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_CTL_HW_80211_ENCAP;
  IEEE80211_SKB_CB(skb)->control.hw_key = radio->pairwise_key;
 }
 radio->sent++;
 frame_send(radio, skb);
 return (true);
}
static bool
to_ethernet(struct sk_buff *skb, bool cipher_header_gone)
{
 const uint8_t *f = skb->data;
 unsigned control = f[0] | f[1] << 8, header = 24;
 uint8_t to[ETH_ALEN], from[ETH_ALEN], type[2], *e;
 if ((control & 0x000c) != 0x0008 || skb->len < 32) {
  return (false);
 }
 memcpy(to, control & 0x0100 ? f + 16 : f + 4, ETH_ALEN);
 memcpy(from, (control & 0x0300) == 0x0300 ? f + 24 : control & 0x0200 ? f + 16 : f + 10, ETH_ALEN);
 header += (control & 0x0300) == 0x0300 ? 6 : 0;
 header += control & 0x0080 ? 2 : 0;
 header += (control & 0x8080) == 0x8080 ? 4 : 0;
 header += (control & 0x4000) && !cipher_header_gone ? 8 : 0;
 if (skb->len < header + 8 || f[header] != 0xaa || f[header + 1] != 0xaa || f[header + 2] != 0x03) {
  return (false);
 }
 memcpy(type, f + header + 6, 2);
 e = skb_pull(skb, header + 8 - 14);
 memcpy(e, to, ETH_ALEN);
 memcpy(e + 6, from, ETH_ALEN);
 memcpy(e + 12, type, 2);
 return (true);
}
static void
handshake_message(struct lx_radio *radio, const uint8_t *frame, unsigned length)
{
 uint8_t reply[14 + LX_WPA_REPLY_MOST];
 unsigned reply_length = 0;
 const char *why = "";
 int said;
 radio->eapol++;
 if (radio->sta == NULL || memcmp(frame + 6, radio->target.address, ETH_ALEN) != 0) {
  return;
 }
 said = lx_wpa_message(&radio->wpa, frame + 14, length - 14, reply + 14, &reply_length, &why);
 if (reply_length != 0) {
  memcpy(reply, radio->target.address, ETH_ALEN);
  memcpy(reply + 6, radio->vif->addr, ETH_ALEN);
  reply[12] = 0x88;
  reply[13] = 0x8e;
  ethernet_send(radio, reply, 14 + reply_length);
 }
 printf("lx: the handshake: a message of %u bytes from the access point: %s%s\n", length - 14, said == LX_WPA_ANSWER ?
     "its first; answered with ours (message 2)" : said == LX_WPA_KEYS ? "its second, SIGNED WITH THE KEY WE HAVE TOO, and "
     "with the group's key; answered (message 4)" : said == LX_WPA_GROUP ? "a new key for the group; answered" :
     said == LX_WPA_BAD ? "NOT GOOD: " : "not one to answer", why);
 if (said == LX_WPA_GROUP && radio->joined) {
  queue_work(radio->work, &radio->group_work);
 }
 if (said != LX_WPA_NOTHING) {
  lx_os_lock();
  radio->handshake = said;
  radio->handshake_why = why;
  lx_os_wakeup(&radio->handshake);
  lx_os_unlock();
 }
}
static void
ethernet_received(struct lx_radio *radio, struct sk_buff *skb)
{
 if (skb->len >= 14 && skb->data[12] == 0x88 && skb->data[13] == 0x8e) {
  handshake_message(radio, skb->data, skb->len);
 } else if (radio->joined && skb->len >= 14) {
  radio->rx_source_ours += memcmp(skb->data + 6, radio->vif->addr, ETH_ALEN) == 0;
  radio->rx_destination_ours += memcmp(skb->data, radio->vif->addr, ETH_ALEN) == 0;
  *(skb->data[0] & 1 ? &radio->received_group : &radio->received) += 1;
  lx_os_net_input(skb->data, skb->len);
 }
}
static void
management_received(struct lx_radio *radio, const uint8_t *f, unsigned length)
{
 int answer = ANSWER_NONE;
 unsigned status = 0;
 if (radio->sta == NULL || length < 26 || memcmp(f + 10, radio->target.address, ETH_ALEN) != 0 ||
     memcmp(f + 4, radio->vif->addr, ETH_ALEN) != 0) {
  return;
 }
 if (f[0] == 0xb0 && length >= 30) {
  status = f[28] | f[29] << 8;
  answer = (f[26] | f[27] << 8) != 2 ? ANSWER_NONE : status == 0 ? ANSWER_AUTHENTICATED : ANSWER_REFUSED;
 } else if (f[0] == 0x10 && length >= 30) {
  status = f[26] | f[27] << 8;
  answer = status == 0 ? ANSWER_ASSOCIATED : ANSWER_REFUSED;
  lx_os_lock();
  radio->aid = (uint16_t)((f[28] | f[29] << 8) & 0x3fff);
  radio->answer_length = length - 30 < ELEMENTS_MOST ? length - 30 : ELEMENTS_MOST;
  memcpy(radio->answer_elements, f + 30, radio->answer_length);
  lx_os_unlock();
 } else if (f[0] == 0xd0 && length >= 28 && f[24] == 3 && radio->reached >= IEEE80211_STA_ASSOC && lx_os_option("wifi_ba")) {
  unsigned tid;
  lx_os_lock();
  if (f[25] == 0 && length >= 33) {
   unsigned params = f[27] | f[28] << 8;
   tid = params >> 2 & 7;
   radio->ba[tid].start = true;
   radio->ba[tid].amsdu = params & 1;
   radio->ba[tid].size = (uint16_t)(params >> 6);
   radio->ba[tid].token = f[26];
   radio->ba[tid].timeout = (uint16_t)(f[29] | f[30] << 8);
   radio->ba[tid].ssn = (uint16_t)((f[31] | f[32] << 8) >> 4);
   radio->ba_asked |= (uint8_t)(1u << tid);
  } else if (f[25] == 2) {
   tid = (f[26] | f[27] << 8) >> 12 & 7;
   radio->ba[tid].start = false;
   radio->ba_asked |= (uint8_t)(1u << tid);
  }
  lx_os_unlock();
  queue_work(radio->work, &radio->ba_work);
  return;
 } else if (f[0] == 0xc0 || f[0] == 0xa0) {
  status = f[24] | f[25] << 8;
  answer = ANSWER_GONE;
  lx_tell("lx: THE ACCESS POINT %s US, reason %u\n", f[0] == 0xc0 ? "DEAUTHENTICATES" : "DISASSOCIATES", status);
  if (radio->joined) {
   radio->joined = false;
   radio->last_error = ECONNRESET;
   lx_os_net_link(0);
   lx_os_station_lost(ECONNRESET);
  }
 }
 if (answer != ANSWER_NONE) {
  lx_os_lock();
  radio->answer = answer;
  radio->answer_status = (uint16_t)status;
  lx_os_wakeup(&radio->answer);
  lx_os_unlock();
 }
}
void
linuxkpi_ieee80211_rx(struct ieee80211_hw *hw, struct sk_buff *skb, struct ieee80211_sta *sta, struct napi_struct *napi,
    struct list_head *list)
{
 struct lx_radio *radio = radio_of(hw);
 const struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
 radio->frames++;
 if (skb->len >= 32 && !(status->flag & RX_FLAG_8023) && (skb->data[0] & 0x0c) == 0x08) {
  unsigned index = radio->rx_header_count++ % 4;
  memcpy(radio->rx_headers[index].bytes, skb->data, 32);
  radio->rx_headers[index].length = skb->len;
  radio->rx_headers[index].flags = status->flag;
 }
 if (status->flag & RX_FLAG_8023) {
  radio->rx_ethernet++;
  ethernet_received(radio, skb);
 } else if (skb->len >= 36 && (skb->data[0] == 0x80 || skb->data[0] == 0x50)) {
  heard(radio, skb->data, skb->len, status->freq, status->signal);
 } else if (skb->len >= 24 && (skb->data[0] & 0x0c) == 0x00) {
  radio->others++;
  management_received(radio, skb->data, skb->len);
 } else if (to_ethernet(skb, (status->flag & RX_FLAG_IV_STRIPPED) != 0)) {
  radio->rx_native_data++;
  ethernet_received(radio, skb);
 } else {
  if (skb->len >= 2 && (skb->data[0] & 0x0c) == 0x08) {
   if (radio->rx_native_rejected++ < 8)
    printf("lx: RX native rejected: control %#x flags %#x bytes %u\n",
        skb->data[0] | skb->data[1] << 8, status->flag, skb->len);
  }
  radio->others++;
 }
 dev_kfree_skb_any(skb);
}
static int
station_up(struct lx_radio *radio)
{
 struct ieee80211_hw *hw = &radio->hw;
 struct ieee80211_vif *vif;
 int ret;
 if (radio->vif != NULL) {
  return (0);
 }
 if (is_zero_ether_addr(hw->wiphy->perm_addr)) {
  lx_tell("lx: THE RADIO HAS NO ADDRESS (the kernel was given none, and the firmware has none): no interface is made\n");
  return (EADDRNOTAVAIL);
 }
 radio->vif_memory = kzalloc(64 + sizeof(*vif) + hw->vif_data_size, GFP_KERNEL);
 if (radio->vif_memory == NULL) {
  return (ENOMEM);
 }
 vif = (struct ieee80211_vif *)roundup2((uintptr_t)radio->vif_memory, 64);
 vif->type = NL80211_IFTYPE_STATION;
 memcpy(vif->addr, hw->wiphy->perm_addr, ETH_ALEN);
 memcpy(vif->bss_conf.addr, hw->wiphy->perm_addr, ETH_ALEN);
 vif->bss_conf.vif = vif;
 vif->bss_conf.bssid = no_network;
 vif->link_conf[0] = &vif->bss_conf;
 if (ieee80211_hw_check(hw, SUPPORTS_TX_ENCAP_OFFLOAD) && !lx_os_option("wifi_native")) {
  vif->offload_flags |= IEEE80211_OFFLOAD_ENCAP_ENABLED;
 }
 if (ieee80211_hw_check(hw, SUPPORTS_RX_DECAP_OFFLOAD) && !lx_os_option("wifi_rx_native")) {
  vif->offload_flags |= IEEE80211_OFFLOAD_DECAP_ENABLED;
 }
 mutex_lock(&hw->wiphy->mtx);
 ret = radio->ops->start(hw);
 printf("lx: the radio is started: %d\n", ret);
 if (ret == 0) {
  radio->started = true;
  ret = radio->ops->add_interface(hw, vif);
  printf("lx: an interface in station mode is added to it: %d (Ethernet frames %s the firmware, %s it)\n", ret,
      vif->offload_flags & IEEE80211_OFFLOAD_ENCAP_ENABLED ? "to" : "NOT to", vif->offload_flags &
      IEEE80211_OFFLOAD_DECAP_ENABLED ? "from" : "NOT from");
  if (ret == 0) {
   radio->vif = vif;
  }
 }
 mutex_unlock(&hw->wiphy->mtx);
 return (-ret);
}
static void
station_down(struct lx_radio *radio)
{
 struct ieee80211_hw *hw = &radio->hw;
 mutex_lock(&hw->wiphy->mtx);
 if (radio->vif != NULL) {
  radio->ops->remove_interface(hw, radio->vif);
  radio->vif = NULL;
 }
 if (radio->started) {
  radio->ops->stop(hw, false);
  radio->started = false;
 }
 mutex_unlock(&hw->wiphy->mtx);
 kfree(radio->vif_memory);
 radio->vif_memory = NULL;
 printf("lx: the interface is removed and the radio stopped\n");
}
static int
station_scan(struct lx_radio *radio, const uint8_t *name, unsigned name_length, bool quiet, const struct redwood_sta_scan *what)
{
 struct ieee80211_hw *hw = &radio->hw;
 struct ieee80211_scan_request *request;
 struct cfg80211_ssid asked;
 unsigned long long began;
 int channels = 0, ret;
 for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
  channels += hw->wiphy->bands[band] != NULL ? hw->wiphy->bands[band]->n_channels : 0;
 }
 request = kzalloc(sizeof(*request) + channels * sizeof(request->req.channels[0]), GFP_KERNEL);
 if (request == NULL) {
  return (ENOMEM);
 }
 request->req.wiphy = hw->wiphy;
 if (!quiet) {
  bzero(&asked, sizeof(asked));
  if (name_length != 0) {
   memcpy(asked.ssid, name, name_length);
  }
  asked.ssid_len = (uint8_t)name_length;
  request->req.ssids = &asked;
  request->req.n_ssids = 1;
 }
 if (what != NULL && what->dwell_ms != 0) {
  request->req.duration = what->dwell_ms;
 }
 radio->frames = radio->others = radio->heard_count = radio->heard_lost = 0;
 int error = 0, completed = 0;
 for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
  struct ieee80211_supported_band *has = hw->wiphy->bands[band];
  request->req.n_channels = 0;
  for (int i = 0; has != NULL && i < has->n_channels; i++) {
   bool wanted = what == NULL || what->frequency_count == 0;
   for (unsigned k = 0; what != NULL && k < what->frequency_count && !wanted; k++) {
    wanted = what->frequencies[k] == has->channels[i].center_freq;
   }
   if (wanted && !(has->channels[i].flags & IEEE80211_CHAN_DISABLED)) {
    request->req.channels[request->req.n_channels++] = &has->channels[i];
   }
  }
  if (request->req.n_channels == 0) {
   continue;
  }
  radio->scan = 1;
  began = lx_os_nanoseconds();
  mutex_lock(&hw->wiphy->mtx);
  ret = radio->ops->hw_scan(hw, radio->vif, request);
  mutex_unlock(&hw->wiphy->mtx);
  printf("lx: SCAN band=%u channels=%d first=%u MHz quiet=%d: %d\n", band,
      request->req.n_channels, request->req.channels[0]->center_freq, quiet, ret);
  if (ret != 0) {
   error = -ret;
   break;
  }
  lx_os_lock();
  while (radio->scan == 1 && lx_os_nanoseconds() - began < 40000000000ull) {
   lx_os_sleep(&radio->scan, 100 * 1000000ull);
  }
  lx_os_unlock();
  printf("lx: band=%u scan %s after %llu ms; %u networks total\n", band,
      radio->scan == 2 ? "OVER" : "INCOMPLETE", (lx_os_nanoseconds() - began) / 1000000ull,
      radio->heard_count);
  if (radio->scan != 2) {
   if (radio->scan == 1 && radio->ops->cancel_hw_scan != NULL) {
    mutex_lock(&hw->wiphy->mtx);
    radio->ops->cancel_hw_scan(hw, radio->vif);
    mutex_unlock(&hw->wiphy->mtx);
   }
   error = ETIMEDOUT;
   break;
  }
  completed++;
 }
 say_heard(radio);
 kfree(request);
 return (error != 0 ? error : completed != 0 ? 0 : EINVAL);
}
int
lx_station_scan(void)
{
 struct lx_radio *radio = the_radio;
 int error;
 if (radio == NULL || !radio->registered) {
  printf("lx: no radio is registered: nothing to scan with\n");
  return (ENXIO);
 }
 if ((error = station_up(radio)) == 0) {
  error = station_scan(radio, NULL, 0, true, NULL);
 }
 station_down(radio);
 return (error);
}
static void
ba_handle(struct work_struct *work)
{
 struct lx_radio *radio = __containerof(work, struct lx_radio, ba_work);
 struct ieee80211_hw *hw = &radio->hw;
 for (unsigned tid = 0; tid < 8; tid++) {
  struct ieee80211_ampdu_params params = { .sta = radio->sta, .tid = (uint8_t)tid };
  struct sk_buff *skb;
  uint8_t *frame;
  bool asked;
  int ret = 0;
  lx_os_lock();
  asked = (radio->ba_asked & 1u << tid) != 0;
  radio->ba_asked &= (uint8_t)~(1u << tid);
  params.ssn = radio->ba[tid].ssn;
  params.timeout = radio->ba[tid].timeout;
  params.amsdu = radio->ba[tid].amsdu;
  params.buf_size = radio->ba[tid].size == 0 || radio->ba[tid].size > hw->max_rx_aggregation_subframes ?
      hw->max_rx_aggregation_subframes : radio->ba[tid].size;
  lx_os_unlock();
  if (!asked || radio->sta == NULL || radio->reached < IEEE80211_STA_ASSOC) {
   continue;
  }
  mutex_lock(&hw->wiphy->mtx);
  if (radio->ba[tid].active) {
   params.action = IEEE80211_AMPDU_RX_STOP;
   radio->ops->ampdu_action(hw, radio->vif, &params);
   radio->ba[tid].active = false;
  }
  if (radio->ba[tid].start) {
   params.action = IEEE80211_AMPDU_RX_START;
   ret = radio->ops->ampdu_action(hw, radio->vif, &params);
   radio->ba[tid].active = ret == 0;
  }
  mutex_unlock(&hw->wiphy->mtx);
  if (!radio->ba[tid].start) {
   continue;
  }
  radio->ba_sessions += ret == 0;
  radio->ba_refused += ret != 0;
  lx_tell("lx: BLOCK ACK for traffic class %u: the access point asks to send aggregates of up to %u frames; the driver "
      "takes %u: %d\n", tid, radio->ba[tid].size, params.buf_size, ret);
  if ((skb = frame_new(radio, 24 + 9, &frame)) == NULL) {
   continue;
  }
  frame = management(radio, frame, 0xd0);
  frame[0] = 3;
  frame[1] = 1;
  frame[2] = radio->ba[tid].token;
  frame[3] = ret == 0 ? 0 : 37;
  frame[4] = 0;
  {
   unsigned answer = (params.amsdu ? 1u : 0u) | 2u | tid << 2 | (unsigned)params.buf_size << 6;
   frame[5] = (uint8_t)answer;
   frame[6] = (uint8_t)(answer >> 8);
  }
  frame[7] = (uint8_t)params.timeout;
  frame[8] = (uint8_t)(params.timeout >> 8);
  frame_send(radio, skb);
 }
}
static int
answer_wait(struct lx_radio *radio, unsigned ms)
{
 unsigned long long began = lx_os_nanoseconds();
 int answer;
 lx_os_lock();
 while (radio->answer == ANSWER_NONE && lx_os_nanoseconds() - began < ms * 1000000ull) {
  lx_os_sleep(&radio->answer, 20 * 1000000ull);
 }
 answer = radio->answer;
 lx_os_unlock();
 return (answer);
}
static uint32_t
rates_of(const struct ieee80211_supported_band *band, const uint8_t *rates, unsigned count, uint32_t *basic)
{
 uint32_t has = 0;
 for (unsigned i = 0; i < count; i++) {
  for (int r = 0; r < band->n_bitrates; r++) {
   if (band->bitrates[r].bitrate == (rates[i] & 0x7f) * 5u) {
    has |= 1u << r;
    *basic |= rates[i] & 0x80 ? 1u << r : 0;
   }
  }
 }
 return (has);
}
static void
station_abilities(struct lx_radio *radio, const uint8_t *elements, unsigned length)
{
 struct ieee80211_supported_band *band = radio->wiphy->bands[radio->channel->def.chan->band];
 struct ieee80211_sta *sta = radio->sta;
 struct ieee80211_bss_conf *conf = &radio->vif->bss_conf;
 const uint8_t *e;
 uint32_t rates = 0, basic = 0;
 if ((e = element(elements, length, 1)) != NULL) {
  rates |= rates_of(band, e + 2, e[1], &basic);
 }
 if ((e = element(elements, length, 50)) != NULL) {
  rates |= rates_of(band, e + 2, e[1], &basic);
 }
 sta->deflink.supp_rates[band->band] = rates;
 conf->basic_rates = basic;
 sta->wme = conf->qos = vendor_element(elements, length, wmm) != NULL;
 sta->deflink.bandwidth = radio->width == 80 ? IEEE80211_STA_RX_BW_80 : radio->width == 40 ? IEEE80211_STA_RX_BW_40 :
     IEEE80211_STA_RX_BW_20;
 sta->deflink.rx_nss = 1;
 sta->deflink.smps_mode = IEEE80211_SMPS_OFF;
 bool legacy = lx_os_option("wifi_legacy") != 0;
 if (!legacy && (e = element(elements, length, 45)) != NULL && e[1] >= 26 && band->ht_cap.ht_supported) {
  struct ieee80211_sta_ht_cap *ht = &sta->deflink.ht_cap;
  ht->ht_supported = true;
  ht->cap = (uint16_t)((e[2] | e[3] << 8) & (radio->width > 20 ? 0xffff : ~(IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
      IEEE80211_HT_CAP_SGI_40)));
  ht->ampdu_factor = e[4] & 3;
  ht->ampdu_density = e[4] >> 2 & 7;
  memcpy(&ht->mcs, e + 5, sizeof(ht->mcs));
  sta->deflink.rx_nss = ht->mcs.rx_mask[1] != 0 ? 2 : 1;
 }
 if (!legacy && (e = element(elements, length, 191)) != NULL && e[1] >= 12 && band->vht_cap.vht_supported) {
  struct ieee80211_sta_vht_cap *vht = &sta->deflink.vht_cap;
  vht->vht_supported = true;
  vht->cap = e[2] | e[3] << 8 | e[4] << 16 | (uint32_t)e[5] << 24;
  memcpy(&vht->vht_mcs, e + 6, sizeof(vht->vht_mcs));
 }
 if ((e = element(elements, length, 61)) != NULL && e[1] >= 22) {
  conf->ht_operation_mode = (uint16_t)(e[4] | e[5] << 8);
 }
 if ((e = element(elements, length, 5)) != NULL && e[1] >= 2 && e[3] != 0) {
  conf->dtim_period = e[3];
 }
}
static unsigned
request_elements(struct lx_radio *radio, uint8_t *out, unsigned group_cipher)
{
 const struct lx_target *target = &radio->target;
 struct linuxkpi_ieee80211_channel *channel = radio->channel->def.chan;
 struct ieee80211_supported_band *band = radio->wiphy->bands[channel->band];
 uint8_t *at = out;
 int count = band->n_bitrates;
 *at++ = 0;
 *at++ = radio->want_length;
 memcpy(at, radio->want, radio->want_length);
 at += radio->want_length;
 *at++ = 1;
 *at++ = (uint8_t)(count < 8 ? count : 8);
 for (int r = 0; r < count; r++) {
  if (r == 8) {
   *at++ = 50;
   *at++ = (uint8_t)(count - 8);
  }
  *at++ = (uint8_t)(band->bitrates[r].bitrate / 5 | (radio->vif->bss_conf.basic_rates & 1u << r ? 0x80 : 0));
 }
 if (target->capability & 0x0100) {
  *at++ = 33;
  *at++ = 2;
  *at++ = 0;
  *at++ = (uint8_t)channel->max_power;
  *at++ = 36;
  *at++ = 2;
  *at++ = (uint8_t)channel_of(channel->center_freq);
  *at++ = 1;
 }
 if (group_cipher != 0) {
  radio->rsn[0] = 48;
  radio->rsn[1] = 20;
  memcpy(radio->rsn + 2, "\x01\x00\x00\x0f\xac\x04\x01\x00\x00\x0f\xac\x04\x01\x00\x00\x0f\xac\x02\x00\x00", 20);
  radio->rsn[7] = (uint8_t)group_cipher;
  memcpy(at, radio->rsn, 22);
  at += 22;
 }
 if (element(target->elements, target->elements_length, 45) != NULL && band->ht_cap.ht_supported &&
     !lx_os_option("wifi_legacy")) {
  uint16_t cap = (uint16_t)(band->ht_cap.cap & (radio->width > 20 ? 0xffff : ~(IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
      IEEE80211_HT_CAP_SGI_40)));
  *at++ = 45;
  *at++ = 26;
  bzero(at, 26);
  at[0] = (uint8_t)cap;
  at[1] = (uint8_t)(cap >> 8);
  at[2] = (uint8_t)(band->ht_cap.ampdu_factor | band->ht_cap.ampdu_density << 2);
  memcpy(at + 3, &band->ht_cap.mcs, sizeof(band->ht_cap.mcs));
  at += 26;
 }
 if (element(target->elements, target->elements_length, 191) != NULL && band->vht_cap.vht_supported &&
     !lx_os_option("wifi_legacy")) {
  *at++ = 191;
  *at++ = 12;
  at[0] = (uint8_t)band->vht_cap.cap;
  at[1] = (uint8_t)(band->vht_cap.cap >> 8);
  at[2] = (uint8_t)(band->vht_cap.cap >> 16);
  at[3] = (uint8_t)(band->vht_cap.cap >> 24);
  memcpy(at + 4, &band->vht_cap.vht_mcs, sizeof(band->vht_cap.vht_mcs));
  at += 12;
 }
 if (vendor_element(target->elements, target->elements_length, wmm) != NULL) {
  memcpy(at, "\xdd\x07\x00\x50\xf2\x02\x00\x01\x00", 9);
  at += 9;
 }
 return ((unsigned)(at - out));
}
static const char *
protection(const struct lx_target *target, unsigned *group_cipher)
{
 const uint8_t *rsn = element(target->elements, target->elements_length, 48), *at, *end;
 bool ccmp = false, passphrase = false;
 unsigned count;
 if (rsn == NULL) {
  return (target->capability & 0x10 ? "it is protected by something older than WPA2" : "it is not protected at all: an "
         "open network is not joined by this");
 }
 at = rsn + 2;
 end = at + rsn[1];
 if (end - at < 8 || at[2] != 0x00 || at[3] != 0x0f || at[4] != 0xac) {
  return ("its RSN element is not one this reads");
 }
 *group_cipher = at[5];
 count = at[6] | at[7] << 8;
 at += 8;
 for (; count != 0 && end - at >= 4; count--, at += 4) {
  ccmp |= at[0] == 0x00 && at[1] == 0x0f && at[2] == 0xac && at[3] == 4;
 }
 if (end - at < 2) {
  return ("its RSN element names no kind of key management");
 }
 count = at[0] | at[1] << 8;
 at += 2;
 for (; count != 0 && end - at >= 4; count--, at += 4) {
  passphrase |= at[0] == 0x00 && at[1] == 0x0f && at[2] == 0xac && at[3] == 2;
 }
 if (*group_cipher != 4) {
  return ("it ciphers frames to everybody with something other than CCMP (TKIP, for old stations), which is not done here");
 }
 if (!ccmp) {
  return ("it does not offer CCMP");
 }
 if (!passphrase) {
  return ("it does not take a passphrase's key of the WPA2 kind (WPA3 only, or an enterprise network)");
 }
 if (end - at >= 2 && (at[0] & 0x40)) {
  return ("it requires protected management frames, which are not done here");
 }
 return (NULL);
}
static struct ieee80211_key_conf *
key_set(struct lx_radio *radio, const uint8_t *key, unsigned index, bool pairwise, int *error)
{
 struct ieee80211_key_conf *conf = kzalloc(sizeof(*conf) + 16, GFP_KERNEL);
 if (conf == NULL) {
  *error = -ENOMEM;
  return (NULL);
 }
 conf->cipher = WLAN_CIPHER_SUITE_CCMP;
 conf->keyidx = (uint8_t)index;
 conf->keylen = 16;
 conf->iv_len = 8;
 conf->icv_len = 8;
 conf->flags = pairwise ? IEEE80211_KEY_FLAG_PAIRWISE : 0;
 conf->link_id = -1;
 memcpy(conf->key, key, 16);
 mutex_lock(&radio->wiphy->mtx);
 *error = radio->ops->set_key(&radio->hw, SET_KEY, radio->vif, pairwise ? radio->sta : NULL, conf);
 mutex_unlock(&radio->wiphy->mtx);
 return (conf);
}
static void
key_drop(struct lx_radio *radio, struct ieee80211_key_conf **conf, bool pairwise)
{
 if (*conf != NULL) {
  mutex_lock(&radio->wiphy->mtx);
  radio->ops->set_key(&radio->hw, DISABLE_KEY, radio->vif, pairwise ? radio->sta : NULL, *conf);
  mutex_unlock(&radio->wiphy->mtx);
  explicit_bzero((*conf)->key, 16);
  kfree(*conf);
  *conf = NULL;
 }
}
static void
group_rekey(struct work_struct *work)
{
 struct lx_radio *radio = __containerof(work, struct lx_radio, group_work);
 struct ieee80211_key_conf *old = radio->group_key;
 int error = 0;
 radio->rekeys++;
 radio->group_key = key_set(radio, radio->wpa.group, radio->wpa.group_index, false, &error);
 lx_tell("lx: the group's new key (number %u) is given to the driver: %d\n", radio->wpa.group_index, error);
 key_drop(radio, &old, false);
}
static int
station_state(struct lx_radio *radio, enum ieee80211_sta_state from, enum ieee80211_sta_state to)
{
 int ret;
 mutex_lock(&radio->wiphy->mtx);
 ret = radio->ops->sta_state(&radio->hw, radio->vif, radio->sta, from, to);
 mutex_unlock(&radio->wiphy->mtx);
 return (ret);
}
static void
bss_changed(struct lx_radio *radio, u64 what)
{
 mutex_lock(&radio->wiphy->mtx);
 if (radio->ops->bss_info_changed != NULL) {
  radio->ops->bss_info_changed(&radio->hw, radio->vif, &radio->vif->bss_conf, what);
 } else {
  const u64 vif_mask = BSS_CHANGED_ASSOC | BSS_CHANGED_SSID | BSS_CHANGED_PS |
      BSS_CHANGED_IDLE | BSS_CHANGED_ARP_FILTER;
  if ((what & ~vif_mask) != 0 && radio->ops->link_info_changed != NULL) {
   radio->ops->link_info_changed(&radio->hw, radio->vif, &radio->vif->bss_conf, what & ~vif_mask);
  }
  if ((what & vif_mask) != 0 && radio->ops->vif_cfg_changed != NULL) {
   radio->ops->vif_cfg_changed(&radio->hw, radio->vif, what & vif_mask);
  }
 }
 mutex_unlock(&radio->wiphy->mtx);
}
static void
station_leave(struct lx_radio *radio)
{
 struct ieee80211_hw *hw = &radio->hw;
 struct ieee80211_vif *vif = radio->vif;
 int state = radio->reached;
 if (radio->joined) {
  radio->joined = false;
  lx_os_net_link(0);
 }
 if (radio->sta != NULL && state >= IEEE80211_STA_AUTH && radio->answer != ANSWER_GONE) {
  struct sk_buff *skb;
  uint8_t *frame;
  if ((skb = frame_new(radio, 26, &frame)) != NULL) {
   frame = management(radio, frame, 0xc0);
   frame[0] = 3;
   frame[1] = 0;
   frame_send(radio, skb);
   lx_pause_ms(50);
  }
 }
 cancel_work_sync(&radio->group_work);
 cancel_work_sync(&radio->ba_work);
 radio->ba_asked = 0;
 bzero(radio->ba, sizeof(radio->ba));
 key_drop(radio, &radio->group_key, false);
 key_drop(radio, &radio->pairwise_key, true);
 explicit_bzero(&radio->wpa, sizeof(radio->wpa));
 for (; radio->sta != NULL && state > IEEE80211_STA_NOTEXIST; state--) {
  station_state(radio, state, state - 1);
 }
 if (vif != NULL && vif->cfg.assoc) {
  vif->cfg.assoc = false;
  vif->cfg.aid = 0;
  bss_changed(radio, BSS_CHANGED_ASSOC);
 }
 if (radio->channel != NULL) {
  mutex_lock(&hw->wiphy->mtx);
  radio->ops->unassign_vif_chanctx(hw, vif, &vif->bss_conf, radio->channel);
  vif->bss_conf.chanctx_conf = NULL;
  radio->ops->remove_chanctx(hw, radio->channel);
  mutex_unlock(&hw->wiphy->mtx);
  kfree(radio->channel);
  radio->channel = NULL;
 }
 if (vif != NULL) {
  vif->bss_conf.bssid = no_network;
  bss_changed(radio, BSS_CHANGED_BSSID);
 }
 kfree(radio->sta_memory);
 radio->sta_memory = radio->sta = NULL;
 radio->reached = IEEE80211_STA_NOTEXIST;
}
static void
station_width(struct lx_radio *radio, struct linuxkpi_ieee80211_channel *primary, struct cfg80211_chan_def *def)
{
 const struct lx_target *target = &radio->target;
 struct ieee80211_supported_band *band = radio->wiphy->bands[primary->band];
 const uint8_t *ht = element(target->elements, target->elements_length, 61);
 const uint8_t *vht = element(target->elements, target->elements_length, 192);
 unsigned offset;
 def->chan = primary;
 def->width = NL80211_CHAN_WIDTH_20;
 def->center_freq1 = primary->center_freq;
 radio->width = 20;
 if (!lx_os_option("wifi_wide") || ht == NULL || ht[1] < 22 || !band->ht_cap.ht_supported ||
     !(band->ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40) || !(ht[3] & 4)) {
  return;
 }
 if (vht != NULL && vht[1] >= 3 && vht[2] == 1 && primary->band == NL80211_BAND_5GHZ && band->vht_cap.vht_supported) {
  unsigned center = 5000 + 5 * vht[3];
  bool all = true;
  for (unsigned frequency = center - 30; frequency <= center + 30; frequency += 20) {
   struct linuxkpi_ieee80211_channel *c = linuxkpi_ieee80211_get_channel(radio->wiphy, frequency);
   all &= c != NULL && !(c->flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_80MHZ));
  }
  if (all && primary->center_freq >= center - 30 && primary->center_freq <= center + 30) {
   def->width = NL80211_CHAN_WIDTH_80;
   def->center_freq1 = center;
   radio->width = 80;
   return;
  }
 }
 offset = ht[3] & 3;
 if (offset == 1 || offset == 3) {
  struct linuxkpi_ieee80211_channel *second = linuxkpi_ieee80211_get_channel(radio->wiphy, offset == 1 ?
      primary->center_freq + 20 : primary->center_freq - 20);
  if (second != NULL && !(second->flags & IEEE80211_CHAN_DISABLED) && !(primary->flags & (offset == 1 ?
      IEEE80211_CHAN_NO_HT40PLUS : IEEE80211_CHAN_NO_HT40MINUS))) {
   def->width = NL80211_CHAN_WIDTH_40;
   def->center_freq1 = offset == 1 ? primary->center_freq + 10 : primary->center_freq - 10;
   radio->width = 40;
  }
 }
}
static int
station_join(struct lx_radio *radio, const struct redwood_sta_join *what)
{
 const uint8_t *name = what->ssid;
 unsigned name_length = what->ssid_length;
 bool open = what->security == REDWOOD_STA_OPEN;
 struct ieee80211_hw *hw = &radio->hw;
 struct ieee80211_vif *vif;
 struct linuxkpi_ieee80211_channel *channel;
 struct lx_target *target = &radio->target;
 struct sk_buff *skb;
 uint8_t *frame;
 const uint8_t *e;
 const char *obstacle;
 unsigned long long began;
 unsigned group_cipher = 0, length;
 int ret, error = 0, answer = ANSWER_NONE;
 if (name_length == 0 || name_length > sizeof(radio->want)) {
  return (EINVAL);
 }
 if (!open && what->security != REDWOOD_STA_WPA2_PSK) {
  lx_tell("lx: a network without WPA2 and a passphrase is asked for: that is not built\n");
  return (ENOTSUP);
 }
 if (radio->sta != NULL) {
  station_leave(radio);
  lx_tell("lx: the network joined before is left\n");
 }
 INIT_WORK(&radio->group_work, group_rekey);
 INIT_WORK(&radio->ba_work, ba_handle);
 radio->ba_asked = 0;
 bzero(radio->ba, sizeof(radio->ba));
 memcpy(radio->want, name, name_length);
 radio->want_length = (uint8_t)name_length;
 bzero(target, sizeof(*target));
 if ((ret = station_up(radio)) != 0) {
  return (ret);
 }
 vif = radio->vif;
 lx_os_lock();
 for (unsigned i = 0; i < radio->heard_count; i++) {
  const struct lx_heard *network = &radio->heard[i];
  if (network->name_length == name_length && memcmp(network->name, name, name_length) == 0 && network->elements_length != 0 &&
      (is_zero_ether_addr(what->bssid) || memcmp(what->bssid, network->sender, ETH_ALEN) == 0) &&
      (!target->found || network->signal > target->signal)) {
   target->found = true;
   memcpy(target->address, network->sender, ETH_ALEN);
   target->frequency = network->frequency;
   target->interval = network->interval;
   target->capability = network->capability;
   target->signal = network->signal;
   target->elements_length = network->elements_length;
   memcpy(target->elements, network->elements, network->elements_length);
  }
 }
 lx_os_unlock();
 for (unsigned attempt = 0; attempt < 3 && !target->found; attempt++) {
  station_scan(radio, name, name_length, false, NULL);
 }
 if (!target->found) {
  lx_tell("lx: THE NETWORK THAT IS WANTED WAS NOT HEARD (a name of %u bytes), in three scans\n", name_length);
  return (ENOENT);
 }
 channel = linuxkpi_ieee80211_get_channel(hw->wiphy, target->frequency);
 if ((e = element(target->elements, target->elements_length, 3)) != NULL && e[1] == 1 && channel != NULL &&
     linuxkpi_ieee80211_get_channel(hw->wiphy, linuxkpi_ieee80211_channel_to_frequency(e[2], channel->band)) != NULL) {
  channel = linuxkpi_ieee80211_get_channel(hw->wiphy, linuxkpi_ieee80211_channel_to_frequency(e[2], channel->band));
 }
 obstacle = open ? ((target->capability & 0x10) || element(target->elements, target->elements_length, 48) != NULL ?
     "an open network was requested but this access point is protected" : NULL) : protection(target, &group_cipher);
 lx_tell("lx: the network that is wanted: an access point on channel %u (%u MHz) at %d dBm, capabilities %#x, beacons every "
     "%u units; HT %s, VHT %s, WMM %s; %s%s\n", channel != NULL ? channel_of(channel->center_freq) : 0, channel != NULL ?
     channel->center_freq : 0, target->signal, target->capability, target->interval, element(target->elements,
     target->elements_length, 45) ? "yes" : "no", element(target->elements, target->elements_length, 191) ? "yes" : "no",
     vendor_element(target->elements, target->elements_length, wmm) ? "yes" : "no", open && obstacle == NULL ?
     "explicitly requested open network" : obstacle == NULL ? "WPA2 with a "
     "passphrase and CCMP: it can be joined" : "IT CANNOT BE JOINED: ", obstacle == NULL ? "" : obstacle);
 if (obstacle != NULL || channel == NULL || (channel->flags & IEEE80211_CHAN_DISABLED)) {
  return (ENOTSUP);
 }
 began = lx_os_nanoseconds();
 bzero(&radio->wpa, sizeof(radio->wpa));
 if (what->passphrase_length != 0) {
  lx_wpa_pmk(what->passphrase, what->passphrase_length, name, name_length, radio->wpa.pmk);
 } else {
  memcpy(radio->wpa.pmk, what->pmk, sizeof(radio->wpa.pmk));
 }
 memcpy(radio->wpa.own, vif->addr, ETH_ALEN);
 memcpy(radio->wpa.peer, target->address, ETH_ALEN);
 get_random_bytes(radio->wpa.own_nonce, sizeof(radio->wpa.own_nonce));
 radio->wpa.rsn = radio->rsn;
 radio->wpa.rsn_length = 22;
 radio->handshake = LX_WPA_NOTHING;
 printf("lx: the key of the name (%u bytes) and the passphrase (%u bytes; 0: the key itself was given) is there: %llu ms\n",
     name_length, what->passphrase_length, (lx_os_nanoseconds() - began) / 1000000ull);
 vif->bss_conf.bssid = target->address;
 vif->bss_conf.beacon_int = target->interval;
 vif->bss_conf.assoc_capability = target->capability;
 vif->bss_conf.dtim_period = 1;
 vif->bss_conf.use_short_slot = (target->capability & 0x0400) != 0 || channel->band != NL80211_BAND_2GHZ;
 vif->bss_conf.use_short_preamble = (target->capability & 0x0020) != 0;
 memcpy(vif->cfg.ap_addr, target->address, ETH_ALEN);
 memcpy(vif->cfg.ssid, name, name_length);
 vif->cfg.ssid_len = name_length;
 bss_changed(radio, BSS_CHANGED_BSSID);
 radio->channel = kzalloc(sizeof(*radio->channel) + hw->chanctx_data_size + 64, GFP_KERNEL);
 radio->sta_memory = kzalloc(64 + sizeof(*radio->sta) + hw->sta_data_size, GFP_KERNEL);
 if (radio->channel == NULL || radio->sta_memory == NULL) {
  error = ENOMEM;
  goto out;
 }
 station_width(radio, channel, &radio->channel->def);
 radio->channel->min_def = radio->channel->def;
 radio->channel->rx_chains_static = radio->channel->rx_chains_dynamic = 2;
 vif->bss_conf.chanreq.oper = radio->channel->def;
 mutex_lock(&hw->wiphy->mtx);
 ret = radio->ops->add_chanctx(hw, radio->channel);
 if (ret == 0) {
  vif->bss_conf.chanctx_conf = radio->channel;
  ret = radio->ops->assign_vif_chanctx(hw, vif, &vif->bss_conf, radio->channel);
 }
 mutex_unlock(&hw->wiphy->mtx);
 lx_tell("lx: the channel is taken, %u MHz wide around %u MHz: %d\n", radio->width, radio->channel->def.center_freq1, ret);
 if (ret != 0) {
  error = -ret;
  goto out;
 }
 radio->sta = (struct ieee80211_sta *)roundup2((uintptr_t)radio->sta_memory, 64);
 memcpy(radio->sta->addr, target->address, ETH_ALEN);
 memcpy(radio->sta->deflink.addr, target->address, ETH_ALEN);
 radio->sta->deflink.sta = radio->sta;
 radio->sta->link[0] = &radio->sta->deflink;
 station_abilities(radio, target->elements, target->elements_length);
 ret = station_state(radio, IEEE80211_STA_NOTEXIST, IEEE80211_STA_NONE);
 printf("lx: the access point is made known to the driver: %d\n", ret);
 if (ret != 0) {
  kfree(radio->sta_memory);
  radio->sta_memory = radio->sta = NULL;
  error = -ret;
  goto out;
 }
 radio->reached = IEEE80211_STA_NONE;
 for (unsigned attempt = 0; attempt < 3 && answer == ANSWER_NONE; attempt++) {
  radio->answer = ANSWER_NONE;
  if ((skb = frame_new(radio, 30, &frame)) == NULL) {
   break;
  }
  frame = management(radio, frame, 0xb0);
  memcpy(frame, "\x00\x00\x01\x00\x00\x00", 6);
  frame_send(radio, skb);
  answer = answer_wait(radio, 500);
 }
 lx_tell("lx: AUTHENTICATION: %s (status %u)\n", answer == ANSWER_AUTHENTICATED ? "the access point accepts" : answer ==
     ANSWER_NONE ? "NO ANSWER from the access point to three requests" : "THE ACCESS POINT REFUSES", radio->answer_status);
 if (answer != ANSWER_AUTHENTICATED || (ret = station_state(radio, IEEE80211_STA_NONE, IEEE80211_STA_AUTH)) != 0) {
  error = answer == ANSWER_NONE ? ETIMEDOUT : ECONNREFUSED;
  goto out;
 }
 radio->reached = IEEE80211_STA_AUTH;
 answer = ANSWER_NONE;
 for (unsigned attempt = 0; attempt < 3 && answer == ANSWER_NONE; attempt++) {
  radio->answer = ANSWER_NONE;
  if ((skb = frame_new(radio, 28 + 300, &frame)) == NULL) {
   break;
  }
  frame = management(radio, frame, 0x00);
  frame[0] = (uint8_t)(0x01 | (open ? 0 : 0x10) | (target->capability & 0x20));
  frame[1] = (uint8_t)((target->capability & 0x0500) >> 8);
  frame[2] = 10;
  frame[3] = 0;
  length = request_elements(radio, frame + 4, group_cipher);
  skb_trim(skb, 28 + length);
  frame_send(radio, skb);
  answer = answer_wait(radio, 500);
 }
 lx_tell("lx: ASSOCIATION: %s (status %u); we are its station number %u; its answer has %u bytes of elements\n", answer ==
     ANSWER_ASSOCIATED ? "THE ACCESS POINT ACCEPTS" : answer == ANSWER_NONE ? "NO ANSWER from the access point to three "
     "requests" : "THE ACCESS POINT REFUSES", radio->answer_status, radio->aid, radio->answer_length);
 if (answer != ANSWER_ASSOCIATED) {
  error = answer == ANSWER_NONE ? ETIMEDOUT : ECONNREFUSED;
  goto out;
 }
 station_abilities(radio, radio->answer_elements, radio->answer_length);
 radio->sta->aid = radio->aid;
 if ((ret = station_state(radio, IEEE80211_STA_AUTH, IEEE80211_STA_ASSOC)) != 0) {
  error = -ret;
  goto out;
 }
 radio->reached = IEEE80211_STA_ASSOC;
 vif->cfg.assoc = true;
 vif->cfg.aid = radio->aid;
 bss_changed(radio, BSS_CHANGED_ASSOC | BSS_CHANGED_BEACON_INT | BSS_CHANGED_ERP_SLOT | BSS_CHANGED_ERP_PREAMBLE |
     BSS_CHANGED_QOS);
 if ((e = vendor_element(radio->answer_elements, radio->answer_length, wmm)) != NULL && e[1] >= 24 && e[6] == 1) {
  mutex_lock(&hw->wiphy->mtx);
  for (unsigned i = 0; i < 4; i++) {
   const uint8_t *record = e + 10 + 4 * i;
   static const uint8_t classes[4] = { IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_VI, IEEE80211_AC_VO };
   struct ieee80211_tx_queue_params params = { .aifs = record[0] & 0x0f, .cw_min = (uint16_t)((1u << (record[1] &
                                        0x0f)) - 1), .cw_max = (uint16_t)((1u << (record[1] >> 4)) - 1),
                                        .txop = (uint16_t)(record[2] | record[3] << 8), .acm = record[0] >> 4 &
                                        1 };
   radio->ops->conf_tx(hw, vif, 0, classes[record[0] >> 5 & 3], &params);
  }
  mutex_unlock(&hw->wiphy->mtx);
 }
 printf("lx: the driver is told of the association (rates %#x, HT %s, VHT %s, %u streams)\n",
     radio->sta->deflink.supp_rates[channel->band], radio->sta->deflink.ht_cap.ht_supported ? "yes" : "no",
     radio->sta->deflink.vht_cap.vht_supported ? "yes" : "no", radio->sta->deflink.rx_nss);
 if (open) {
  ret = 0;
  goto authorize;
 }
 began = lx_os_nanoseconds();
 lx_os_lock();
 while (radio->handshake != LX_WPA_KEYS && radio->handshake != LX_WPA_BAD && radio->answer != ANSWER_GONE &&
     lx_os_nanoseconds() - began < 8000000000ull) {
  lx_os_sleep(&radio->handshake, 20 * 1000000ull);
 }
 lx_os_unlock();
 if (radio->handshake != LX_WPA_KEYS) {
  lx_tell("lx: THE HANDSHAKE DID NOT COME THROUGH in %llu ms: %s%s (%u of its messages came)\n", (lx_os_nanoseconds() - began) /
      1000000ull, radio->answer == ANSWER_GONE ? "the access point has shown us the door (a wrong passphrase gives this)" :
      radio->handshake == LX_WPA_BAD ? "a message was not good: " : radio->handshake == LX_WPA_ANSWER ? "our message 2 was "
      "not answered (a wrong passphrase gives this)" : "the access point did not begin it", radio->handshake == LX_WPA_BAD ?
      radio->handshake_why : "", radio->eapol);
  error = EAUTH;
  goto out;
 }
 lx_pause_ms(30);
 radio->pairwise_key = key_set(radio, radio->wpa.pairwise, 0, true, &ret);
 if (ret == 0) {
  radio->group_key = key_set(radio, radio->wpa.group, radio->wpa.group_index, false, &ret);
 }
 lx_tell("lx: THE KEYS ARE GIVEN TO THE DRIVER, %llu ms after the association: ours with the access point, and the group's "
     "(number %u, %u bytes): %d\n", (lx_os_nanoseconds() - began) / 1000000ull, radio->wpa.group_index,
     radio->wpa.group_length, ret);
authorize:
 if (ret != 0 || (!lx_os_option("wifi_no_authorize") &&
     (ret = station_state(radio, IEEE80211_STA_ASSOC, IEEE80211_STA_AUTHORIZED)) != 0)) {
  error = -ret;
  goto out;
 }
 radio->reached = lx_os_option("wifi_no_authorize") ? IEEE80211_STA_ASSOC : IEEE80211_STA_AUTHORIZED;
 radio->sent = radio->sent_lost = radio->received = radio->received_group = radio->rekeys = 0;
 radio->tim_beacons = radio->tim_ours = radio->tim_group = 0;
 radio->probes_sent = radio->probe_answers = 0;
 radio->joined_at = lx_os_nanoseconds();
 radio->signal = target->signal;
 radio->joined = true;
 lx_os_net_link(1);
 lx_tell("lx: THE NETWORK IS JOINED: frames can pass\n");
 return (0);
out:
 station_leave(radio);
 return (error);
}
int
lx_station_join(void)
{
 static struct redwood_sta_join what;
 struct lx_radio *radio = the_radio;
 unsigned name_length = sizeof(what.ssid), passphrase_length = sizeof(what.passphrase);
 int error;
 if (radio == NULL || !radio->registered) {
  lx_tell("lx: no radio is registered: nothing to join with\n");
  return (ENXIO);
 }
 bzero(&what, sizeof(what));
 if (!lx_os_network(what.ssid, &name_length, what.passphrase, &passphrase_length)) {
  return (ENOENT);
 }
 what.ssid_length = (uint8_t)name_length;
 what.passphrase_length = (uint8_t)passphrase_length;
 what.security = REDWOOD_STA_WPA2_PSK;
 lx_os_net_attach(radio->wiphy->perm_addr);
 error = station_join(radio, &what);
 explicit_bzero(&what, sizeof(what));
 return (error);
}
void
lx_station_say(void)
{
 struct lx_radio *radio = the_radio;
 if (radio != NULL) {
  lx_tell("lx: the network is %s, %llu s after it was joined: %u frames sent (%u could not be), %u received for us and "
      "%u sent to everybody; %u messages of the handshake, the group's key changed %u times; block ack: %u classes "
      "taken, %u refused\n", radio->joined ? "joined" : "NOT joined", radio->joined_at != 0 ? (lx_os_nanoseconds() -
      radio->joined_at) / 1000000000ull : 0, radio->sent, radio->sent_lost, radio->received, radio->received_group,
      radio->eapol, radio->rekeys, radio->ba_sessions, radio->ba_refused);
  lx_tell("lx: RX boundary: ethernet %u native-data %u native-rejected %u\n",
      radio->rx_ethernet, radio->rx_native_data, radio->rx_native_rejected);
  lx_tell("lx: RX Ethernet addresses: source-ours %u destination-ours %u\n",
      radio->rx_source_ours, radio->rx_destination_ours);
  lx_tell("lx: beacons of the network since joining %u: keeping frames for us (number %u) in %u, for all in %u\n",
      radio->tim_beacons, radio->aid, radio->tim_ours, radio->tim_group);
  if (radio->joined && radio->vif != NULL && lx_os_option("wifi_probe_ap")) {
   struct ieee80211_supported_band *band = radio->wiphy->bands[radio->channel->def.chan->band];
   unsigned count = band->n_bitrates < 8 ? band->n_bitrates : 8;
   struct sk_buff *skb;
   uint8_t *frame;
   if ((skb = frame_new(radio, 24 + 2 + radio->want_length + 2 + count, &frame)) != NULL) {
    frame = management(radio, frame, 0x40);
    *frame++ = 0;
    *frame++ = radio->want_length;
    memcpy(frame, radio->want, radio->want_length);
    frame += radio->want_length;
    *frame++ = 1;
    *frame++ = (uint8_t)count;
    for (unsigned r = 0; r < count; r++) {
     *frame++ = (uint8_t)(band->bitrates[r].bitrate / 5);
    }
    radio->probes_sent++;
    frame_send(radio, skb);
   }
   lx_tell("lx: probe requests to the access point alone %u, its answers to us %u\n", radio->probes_sent,
       radio->probe_answers);
  }
  for (unsigned n = 0; n < 4 && n < radio->rx_header_count; n++) {
   unsigned i = (radio->rx_header_count - 1 - n) % 4;
   const uint8_t *h = radio->rx_headers[i].bytes;
   lx_tell("lx: RX header: flags %#x bytes %u fc %#x a1-ours %u a2-ours %u a3-ours %u a2-ap %u llc %02x%02x%02x type %02x%02x\n",
       radio->rx_headers[i].flags, radio->rx_headers[i].length, h[0] | h[1] << 8,
       memcmp(h + 4, radio->vif ? radio->vif->addr : no_network, 6) == 0,
       memcmp(h + 10, radio->vif ? radio->vif->addr : no_network, 6) == 0,
       memcmp(h + 16, radio->vif ? radio->vif->addr : no_network, 6) == 0,
       memcmp(h + 10, radio->target.address, 6) == 0,
       h[24], h[25], h[26], h[30], h[31]);
  }
 }
}
void
lx_station_leave(void)
{
 struct lx_radio *radio = the_radio;
 if (radio == NULL || radio->vif == NULL) {
  return;
 }
 lx_station_say();
 if (radio->sta != NULL) {
  station_leave(radio);
  lx_tell("lx: the network is left\n");
 }
 station_down(radio);
}
int
lx_station_send(const unsigned char *frame, unsigned long length)
{
 struct lx_radio *radio = the_radio;
 if (radio == NULL || !radio->joined) {
  return (ENETDOWN);
 }
 return (ethernet_send(radio, frame, (unsigned)length) ? 0 : ENOBUFS);
}
static int
boundary_power(bool on)
{
 struct lx_radio *radio = the_radio;
 if (radio == NULL || !radio->registered) {
  return (ENXIO);
 }
 if (on) {
  return (station_up(radio));
 }
 if (radio->sta != NULL) {
  station_leave(radio);
  lx_tell("lx: the network is left\n");
 }
 station_down(radio);
 return (0);
}
static int
boundary_scan(const struct redwood_sta_scan *what)
{
 static unsigned said;
 struct lx_radio *radio = the_radio;
 unsigned long long began;
 int error;
 if (radio == NULL || !radio->registered) {
  return (ENXIO);
 }
 if ((error = station_up(radio)) != 0) {
  return (error);
 }
 if ((radio->joined || radio->joining) && !lx_os_option("wifi_scan_joined")) {
  static unsigned kept;
  if (kept++ < 8) {
   lx_tell("lx: a scan for the kernel while joined: not made, the %u access points of the last one are "
       "answered\n", radio->heard_count);
  }
  return (0);
 }
 began = lx_os_nanoseconds();
 error = station_scan(radio, what->ssid, what->ssid_length <= sizeof(what->ssid) ? what->ssid_length : 0, what->passive, what);
 if (said < 24) {
  said++;
  lx_tell("lx: a scan for the kernel: %u channels named (0: all), %s, %u ms on each (0: the radio's own): %d; %u access "
      "points heard, %u frames, in %llu ms\n", what->frequency_count, what->passive ? "listening" : "asking",
      what->dwell_ms, error, radio->heard_count, radio->frames, (lx_os_nanoseconds() - began) / 1000000ull);
 }
 return (error);
}
static void
boundary_heard(void (*each)(const struct redwood_sta_bss *, void *), void *context)
{
 static struct redwood_sta_bss bss;
 struct lx_radio *radio = the_radio;
 for (unsigned i = 0; radio != NULL && i < radio->heard_count; i++) {
  const struct lx_heard *network = &radio->heard[i];
  lx_os_lock();
  bzero(&bss, sizeof(bss));
  memcpy(bss.bssid, network->sender, ETH_ALEN);
  memcpy(bss.ssid, network->name, network->name_length);
  bss.ssid_length = network->name_length;
  bss.frequency = network->says_channel != 0 ? (uint16_t)linuxkpi_ieee80211_channel_to_frequency(network->says_channel,
      network->frequency < 4000 ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ) : network->frequency;
  bss.rssi = network->signal;
  bss.beacon_interval = network->interval;
  bss.capability = network->capability;
  bss.age_ms = (uint32_t)((lx_os_nanoseconds() - network->last) / 1000000ull);
  bss.elements_length = network->elements_length;
  memcpy(bss.elements, network->elements, network->elements_length);
  lx_os_unlock();
  each(&bss, context);
 }
}
static int
boundary_join(const struct redwood_sta_join *what)
{
 struct lx_radio *radio = the_radio;
 int error;
 if (radio == NULL || !radio->registered) {
  return (ENXIO);
 }
 radio->joining = true;
 error = station_join(radio, what);
 radio->last_error = error;
 radio->joining = false;
 return (error);
}
static int
boundary_leave(void)
{
 struct lx_radio *radio = the_radio;
 if (radio != NULL && radio->sta != NULL) {
  lx_station_say();
  station_leave(radio);
  radio->last_error = 0;
  lx_tell("lx: the network is left\n");
 }
 return (0);
}
static void
boundary_state(struct redwood_sta_state *state)
{
 struct lx_radio *radio = the_radio;
 bzero(state, sizeof(*state));
 if (radio == NULL || radio->vif == NULL) {
  return;
 }
 lx_os_lock();
 state->phase = radio->joined ? REDWOOD_STA_JOINED : radio->joining ? REDWOOD_STA_JOINING : radio->scan == 1 ?
     REDWOOD_STA_SCANNING : REDWOOD_STA_IDLE;
 state->last_error = radio->last_error;
 if ((radio->joined || radio->joining) && radio->target.found) {
  memcpy(state->bss.bssid, radio->target.address, ETH_ALEN);
  memcpy(state->bss.ssid, radio->want, radio->want_length);
  state->bss.ssid_length = radio->want_length;
  state->bss.frequency = radio->channel != NULL && radio->channel->def.chan != NULL ?
      radio->channel->def.chan->center_freq : radio->target.frequency;
  state->bss.rssi = radio->joined ? radio->signal : radio->target.signal;
  state->bss.beacon_interval = radio->target.interval;
  state->bss.capability = radio->target.capability;
  state->bss.elements_length = (uint16_t)radio->target.elements_length;
  memcpy(state->bss.elements, radio->target.elements, radio->target.elements_length);
 }
 lx_os_unlock();
}
static unsigned
boundary_channels(struct redwood_sta_channel *channels, unsigned most)
{
 struct lx_radio *radio = the_radio;
 unsigned count = 0;
 for (unsigned band = 0; radio != NULL && band < NUM_NL80211_BANDS; band++) {
  const struct ieee80211_supported_band *has = radio->wiphy->bands[band];
  for (int i = 0; has != NULL && i < has->n_channels && count < most; i++) {
   if (!(has->channels[i].flags & IEEE80211_CHAN_DISABLED)) {
    channels[count].frequency = (uint16_t)has->channels[i].center_freq;
    channels[count++].flags = (uint16_t)((has->channels[i].flags & IEEE80211_CHAN_NO_IR ? REDWOOD_STA_CHANNEL_PASSIVE :
        0) | (has->channels[i].flags & IEEE80211_CHAN_RADAR ? REDWOOD_STA_CHANNEL_RADAR : 0));
   }
  }
 }
 return (count);
}
static struct redwood_sta_ops boundary = {
 .power = boundary_power,
 .scan = boundary_scan,
 .heard = boundary_heard,
 .join = boundary_join,
 .leave = boundary_leave,
 .state = boundary_state,
 .channels = boundary_channels,
};
int
lx_station_offer(void)
{
 struct lx_radio *radio = the_radio;
 int error;
 if (radio == NULL || !radio->registered) {
  lx_tell("lx: no radio is registered: there is no station to offer\n");
  return (ENXIO);
 }
 lx_os_net_attach(radio->wiphy->perm_addr);
 if ((error = station_up(radio)) != 0) {
  return (error);
 }
 memcpy(boundary.address, radio->wiphy->perm_addr, ETH_ALEN);
 lx_os_station(&boundary);
 return (0);
}
void
lx_station_withdraw(void)
{
 lx_os_station(NULL);
}
