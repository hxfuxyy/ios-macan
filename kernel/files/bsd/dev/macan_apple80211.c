#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/kern_event.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/macan_input.h>
#include <sys/macan_net.h>
#include <sys/macan_sta.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#define SIOCSA80211 0x802869c8u
#define SIOCGA80211 0xc02869c9u
struct apple80211req {
 char req_if_name[16];
 int32_t req_type;
 int32_t req_val;
 uint32_t req_len;
 user_addr_t req_data __attribute__((aligned(8)));
};
static const char *
type_name(int32_t type)
{
 static const struct {
  int32_t type;
  const char *name;
 } names[] = {
  { 1, "SSID" }, { 2, "AUTH_TYPE" }, { 3, "CIPHER_KEY" }, { 4, "CHANNEL" }, { 5, "POWERSAVE" }, { 6, "PROTMODE" },
  { 7, "TXPOWER" }, { 8, "RATE" }, { 9, "BSSID" }, { 10, "SCAN_REQ" }, { 11, "SCAN_RESULT" }, { 12, "CARD_CAPABILITIES" },
  { 13, "STATE" }, { 14, "PHY_MODE" }, { 15, "OP_MODE" }, { 16, "RSSI" }, { 17, "NOISE" }, { 18, "INT_MIT" }, { 19, "POWER" },
  { 20, "ASSOCIATE" }, { 21, "ASSOCIATE_RESULT" }, { 22, "DISASSOCIATE" }, { 23, "STATUS_DEV_NAME" }, { 24, "IBSS_MODE" },
  { 25, "HOST_AP_MODE" }, { 26, "AP_MODE" }, { 27, "SUPPORTED_CHANNELS" }, { 28, "LOCALE" }, { 29, "DEAUTH" },
  { 30, "COUNTERMEASURES" }, { 32, "RATE_SET" }, { 42, "STATION_LIST" }, { 43, "DRIVER_VERSION" },
  { 44, "HARDWARE_VERSION" }, { 46, "RSN_IE" }, { 47, "BACKGROUND_SCAN" }, { 48, "AP_IE_LIST" }, { 49, "STATS" },
  { 50, "ASSOCIATION_STATUS" }, { 51, "COUNTRY_CODE" }, { 54, "RADIO_INFO" }, { 57, "MCS" }, { 66, "MCS_INDEX_SET" },
  { 79, "STA_STATS" }, { 80, "ROAM_THRESH" }, { 85, "IE" }, { 86, "SCAN_REQ_MULTIPLE" }, { 87, "BTCOEX_MODE" },
  { 90, "SCANCACHE_CLEAR" }, { 94, "VIRTUAL_IF_CREATE" }, { 95, "VIRTUAL_IF_DELETE" }, { 103, "CURRENT_NETWORK" },
  { 106, "RSSI_BOUNDS" }, { 107, "ROAM" }, { 207, "CHANNELS_INFO" }, { 254, "HW_SUPPORTED_CHANNELS" },
 };
 for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
  if (names[i].type == type) {
   return names[i].name;
  }
 }
 return "?";
}
#define CHANNEL_20MHZ 0x2u
#define CHANNEL_2GHZ 0x8u
#define CHANNEL_5GHZ 0x10u
#define CHANNEL_ACTIVE 0x80u
#define CHANNEL_DFS 0x100u
struct apple80211_channel {
 uint32_t version, channel, flags;
};
struct apple80211_channels {
 uint32_t version, count;
 struct apple80211_channel channels[401];
 uint32_t unused;
};
struct apple80211_channels_info {
 uint8_t unknown_0[12];
 uint16_t count;
 uint8_t unknown_1[800];
 uint8_t number[400], indoor[400], dfs[400], passive[400], wide_40[400], wide_80[400];
 uint8_t unknown_2[1602];
 uint32_t flags[400];
 uint8_t unknown_3[3600];
} __attribute__((packed));
struct apple80211_scan_request {
 uint32_t version, bss_type;
 uint8_t bssid[6], unused_0[2];
 uint32_t ssid_length;
 uint8_t ssid[32];
 uint32_t scan_type;
 uint32_t phy_mode;
 uint16_t dwell_ms, unused_1;
 uint32_t rest_ms;
 uint32_t channel_count;
 struct apple80211_channel channels[400];
 uint64_t flags;
 uint32_t unused_2;
 uint32_t name_count;
 uint8_t names[10][56];
 uint32_t short_ssid, unused_3;
};
struct apple80211_scan_result {
 uint32_t version;
 struct apple80211_channel channel;
 uint16_t noise_known;
 int16_t noise, snr, rssi;
 uint16_t beacon_interval, capability;
 uint8_t bssid[6];
 uint8_t rate_count, unused_0;
 uint32_t rates[15];
 uint8_t ssid_length, ssid[32], unused_1[3];
 uint32_t age_ms;
 uint8_t flags, unused_2;
 uint16_t elements_length;
 uint8_t elements[1024];
};
struct apple80211_assoc {
 uint32_t version;
 uint16_t mode, auth_lower, auth_upper, unused_0;
 uint32_t ssid_length;
 uint8_t ssid[32];
 uint8_t bssid[6], unused_1[2];
 uint32_t key_version, key_length, key_cipher;
 uint16_t key_flags, key_index;
 uint8_t key[64];
 uint8_t rest[0x250 - 0x88];
};
#define CIPHER_PMK 6
#define CIPHER_PASSPHRASE 10
struct apple80211_link_changed {
 uint8_t down, unused_0[3];
 int32_t rssi;
 uint16_t snr, noise;
 uint8_t unused_1[4];
 uint8_t involuntary, unused_2[3];
 uint32_t reason, subreason;
 uint8_t unused_3[4];
};
_Static_assert(sizeof(struct apple80211_assoc) == 592 && offsetof(struct apple80211_assoc, key) == 0x48,
    "the association request as iOS 16.5 has it");
_Static_assert(sizeof(struct apple80211_link_changed) == 32 && offsetof(struct apple80211_link_changed, reason) == 0x14,
    "the link event as iOS 16.5 reads it");
_Static_assert(sizeof(struct apple80211_channels) == 4824, "the channel list as iOS 16.5 has it");
_Static_assert(sizeof(struct apple80211_channels_info) == 10016 && offsetof(struct apple80211_channels_info, number) == 0x32e &&
    offsetof(struct apple80211_channels_info, flags) == 0x12d0, "the channels' description as iOS 16.5 has it");
_Static_assert(sizeof(struct apple80211_scan_request) == 5456 && offsetof(struct apple80211_scan_request, channels) == 0x48 &&
    offsetof(struct apple80211_scan_request, flags) == 0x1308 && offsetof(struct apple80211_scan_request, names) == 0x1318,
    "the scan request as iOS 16.5 has it");
_Static_assert(sizeof(struct apple80211_scan_result) == 1164 && offsetof(struct apple80211_scan_result, ssid_length) == 0x60 &&
    offsetof(struct apple80211_scan_result, age_ms) == 0x84 && offsetof(struct apple80211_scan_result, elements) == 0x8c,
    "a scan's result as iOS 16.5 has it");
enum { EVENT_POWER_CHANGED = 1, EVENT_SSID_CHANGED = 2, EVENT_BSSID_CHANGED = 3, EVENT_LINK_CHANGED = 4, EVENT_ASSOC_DONE = 9,
       EVENT_SCAN_DONE = 10 };
static const char interface_name[16] = "en0";
static LCK_GRP_DECLARE(apple80211_lock_group, "redwood-apple80211");
static LCK_MTX_DECLARE(apple80211_lock, &apple80211_lock_group);
static unsigned result_next;
static uint32_t join_result;
static unsigned
channel_number(unsigned frequency)
{
 return frequency == 2484 ? 14 : frequency < 4000 ? (frequency - 2407) / 5 : (frequency - 5000) / 5;
}
static unsigned
channel_frequency(const struct apple80211_channel *channel)
{
 bool low = channel->flags & CHANNEL_2GHZ ? true : channel->flags & CHANNEL_5GHZ ? false : channel->channel <= 14;
 if (channel->channel == 0 || channel->channel > 196 || (channel->flags & 0x2000)) {
  return 0;
 }
 return low ? (channel->channel == 14 ? 2484 : 2407 + 5 * channel->channel) : 5000 + 5 * channel->channel;
}
static void
channel_fill(struct apple80211_channel *channel, unsigned frequency, unsigned flags)
{
 channel->version = 1;
 channel->channel = channel_number(frequency);
 channel->flags = CHANNEL_20MHZ | (frequency < 4000 ? CHANNEL_2GHZ : CHANNEL_5GHZ) | (flags & REDWOOD_STA_CHANNEL_PASSIVE ?
     0 : CHANNEL_ACTIVE) | (flags & REDWOOD_STA_CHANNEL_RADAR ? CHANNEL_DFS : 0);
}
static int
give(const struct apple80211req *request, const void *data, size_t length)
{
 return request->req_data == USER_ADDR_NULL ? 0 : copyout(data, request->req_data, length < request->req_len ? length :
        request->req_len);
}
static int
give_network(const struct apple80211req *request, const struct redwood_sta_bss *bss)
{
 struct apple80211_scan_result *result = kalloc_data(sizeof(*result), Z_WAITOK | Z_ZERO);
 unsigned room = sizeof(result->elements);
 int error;
 if (result == NULL) {
  return ENOMEM;
 }
 if (request->req_len >= sizeof(*result) && copyin(request->req_data, result, offsetof(struct apple80211_scan_result,
     elements)) == 0 && result->elements_length < room) {
  room = result->elements_length;
 }
 bzero(result, sizeof(*result));
 result->version = 1;
 channel_fill(&result->channel, bss->frequency, 0);
 result->rssi = bss->rssi;
 result->beacon_interval = bss->beacon_interval;
 result->capability = bss->capability;
 memcpy(result->bssid, bss->bssid, sizeof(result->bssid));
 result->ssid_length = bss->ssid_length;
 memcpy(result->ssid, bss->ssid, bss->ssid_length);
 result->age_ms = bss->age_ms;
 result->elements_length = (uint16_t)(bss->elements_length <= room ? bss->elements_length : 0);
 memcpy(result->elements, bss->elements, result->elements_length);
 for (unsigned at = 0; at + 2 <= bss->elements_length && at + 2 + bss->elements[at + 1] <= bss->elements_length;
     at += 2 + bss->elements[at + 1]) {
  for (unsigned i = 0; (bss->elements[at] == 1 || bss->elements[at] == 50) && i < bss->elements[at + 1]; i++) {
   unsigned rate = (bss->elements[at + 2 + i] & 0x7f) / 2;
   if (rate != 0 && rate <= 54 && result->rate_count < 15) {
    result->rates[result->rate_count++] = rate;
   }
  }
 }
 error = give(request, result, sizeof(*result));
 kfree_data(result, sizeof(*result));
 return error;
}
static int
get(struct apple80211req *request)
{
 struct redwood_sta_state *state;
 int error = 0;
 switch (request->req_type) {
 case 43:
 case 44: {
  static const char name[256] = "redwood: ath11k for the WCN6750";
  return give(request, name, sizeof(name));
 }
 case 12: {
  uint8_t bits[13] = { 0 };
  static const uint8_t has[] = { 2, 3, 6, 9, 10, 14, 17, 19 };
  for (unsigned i = 0; i < sizeof(has); i++) {
   bits[has[i] / 8] |= (uint8_t)(1u << (has[i] % 8));
  }
  return give(request, bits, sizeof(bits));
 }
 case 19: {
  struct {
   uint32_t version, radios, state[4];
  } answer = { .version = 1, .radios = 1, .state = { !redwood_sta_present() || redwood_sta_powered() } };
  return give(request, &answer, sizeof(answer));
 }
 case 15:
  request->req_val = 1;
  return 0;
 case 27:
 case 254: {
  struct apple80211_channels *answer;
  struct redwood_sta_channel channels[96];
  unsigned count = redwood_sta_channels(channels, 96);
  if (count == 0 || (answer = kalloc_data(sizeof(*answer), Z_WAITOK | Z_ZERO)) == NULL) {
   return count == 0 ? ENXIO : ENOMEM;
  }
  answer->version = 1;
  answer->count = count;
  for (unsigned i = 0; i < count; i++) {
   channel_fill(&answer->channels[i], channels[i].frequency, channels[i].flags);
  }
  error = give(request, answer, sizeof(*answer));
  kfree_data(answer, sizeof(*answer));
  return error;
 }
 case 207: {
  struct apple80211_channels_info *answer;
  struct redwood_sta_channel channels[96];
  unsigned count = redwood_sta_channels(channels, 96);
  if (count == 0 || (answer = kalloc_data(sizeof(*answer), Z_WAITOK | Z_ZERO)) == NULL) {
   return count == 0 ? ENXIO : ENOMEM;
  }
  answer->count = (uint16_t)count;
  for (unsigned i = 0; i < count; i++) {
   struct apple80211_channel channel;
   channel_fill(&channel, channels[i].frequency, channels[i].flags);
   answer->number[i] = (uint8_t)channel.channel;
   answer->dfs[i] = (channels[i].flags & REDWOOD_STA_CHANNEL_RADAR) != 0;
   answer->passive[i] = (channels[i].flags & REDWOOD_STA_CHANNEL_PASSIVE) != 0;
   answer->flags[i] = channel.flags;
  }
  error = give(request, answer, sizeof(*answer));
  kfree_data(answer, sizeof(*answer));
  return error;
 }
 case 21:
  lck_mtx_lock(&apple80211_lock);
  request->req_val = (int32_t)join_result;
  lck_mtx_unlock(&apple80211_lock);
  return 0;
 case 11: {
  struct redwood_sta_bss *bss = kalloc_data(sizeof(*bss), Z_WAITOK);
  unsigned index;
  bool there;
  if (bss == NULL) {
   return ENOMEM;
  }
  lck_mtx_lock(&apple80211_lock);
  index = result_next++;
  lck_mtx_unlock(&apple80211_lock);
  there = redwood_sta_heard(index, bss);
  error = there ? give_network(request, bss) : EIO;
  kfree_data(bss, sizeof(*bss));
  return error;
 }
 default:
  break;
 }
 if ((state = kalloc_data(sizeof(*state), Z_WAITOK)) == NULL) {
  return ENOMEM;
 }
 redwood_sta_state(state);
 switch (request->req_type) {
 case 13:
  request->req_val = state->phase == REDWOOD_STA_JOINED ? 4 : state->phase == REDWOOD_STA_JOINING ? 2 :
      state->phase == REDWOOD_STA_SCANNING ? 1 : 0;
  break;
 case 1:
  if (state->phase != REDWOOD_STA_JOINED) {
   error = ENXIO;
  } else if ((error = give(request, state->bss.ssid, state->bss.ssid_length)) == 0) {
   request->req_len = state->bss.ssid_length;
  }
  break;
 case 9:
  error = state->phase != REDWOOD_STA_JOINED ? ENXIO : give(request, state->bss.bssid, 6);
  break;
 case 4: {
  struct {
   uint32_t version;
   struct apple80211_channel channel;
  } answer = { .version = 1 };
  if (state->phase != REDWOOD_STA_JOINED) {
   error = ENXIO;
  } else {
   channel_fill(&answer.channel, state->bss.frequency, 0);
   error = give(request, &answer, sizeof(answer));
  }
  break;
 }
 case 16: {
  struct {
   uint32_t version, radios, unit;
   int32_t each[4], all, each_ext[4], all_ext;
  } answer = { .version = 1, .radios = 1, .each = { state->bss.rssi }, .all = state->bss.rssi,
        .each_ext = { state->bss.rssi }, .all_ext = state->bss.rssi };
  error = state->phase != REDWOOD_STA_JOINED ? ENXIO : give(request, &answer, sizeof(answer));
  break;
 }
 case 103:
  error = state->phase != REDWOOD_STA_JOINED ? ENXIO : give_network(request, &state->bss);
  break;
 case 2: {
  struct {
   uint32_t version, lower, upper, extended;
  } answer = { .version = 1, .lower = 1, .upper = 8 };
  error = state->phase != REDWOOD_STA_JOINED ? ENXIO : give(request, &answer, sizeof(answer));
  break;
 }
 default:
  error = EOPNOTSUPP;
  break;
 }
 kfree_data(state, sizeof(*state));
 return error;
}
static int
set_scan(const struct apple80211req *request, char *said, size_t said_size)
{
 struct apple80211_scan_request *asked;
 struct redwood_sta_scan *what;
 unsigned unknown = 0;
 int error;
 if (request->req_len != sizeof(*asked)) {
  return EINVAL;
 }
 if (!redwood_sta_present()) {
  return EBUSY;
 }
 asked = kalloc_data(sizeof(*asked), Z_WAITOK);
 what = kalloc_data(sizeof(*what), Z_WAITOK | Z_ZERO);
 if (asked == NULL || what == NULL || (error = copyin(request->req_data, asked, sizeof(*asked))) != 0) {
  error = asked == NULL || what == NULL ? ENOMEM : error;
  goto out;
 }
 if (asked->ssid_length <= sizeof(what->ssid)) {
  what->ssid_length = (uint8_t)asked->ssid_length;
  memcpy(what->ssid, asked->ssid, asked->ssid_length);
 }
 what->passive = asked->scan_type == 2;
 what->dwell_ms = asked->dwell_ms;
 for (unsigned i = 0; i < asked->channel_count && i < 400 && asked->channel_count <= 64; i++) {
  unsigned frequency = channel_frequency(&asked->channels[i]);
  if (frequency != 0) {
   what->frequencies[what->frequency_count++] = (uint16_t)frequency;
  } else {
   unknown++;
  }
 }
 snprintf(said, said_size, " (a scan: type %u, %u channels of which %u are not the radio's, %u ms on each, a name of %u bytes, "
     "%u more names, flags %#llx)", asked->scan_type, asked->channel_count, unknown, asked->dwell_ms, asked->ssid_length,
     asked->name_count, asked->flags);
 if (asked->channel_count != 0 && asked->channel_count <= 64 && what->frequency_count == 0) {
  error = EINVAL;
 } else {
  lck_mtx_lock(&apple80211_lock);
  result_next = 0;
  lck_mtx_unlock(&apple80211_lock);
  error = redwood_sta_ask_scan(what);
 }
out:
 if (asked != NULL) {
  kfree_data(asked, sizeof(*asked));
 }
 if (what != NULL) {
  kfree_data(what, sizeof(*what));
 }
 return error;
}
static int
set_join(const struct apple80211req *request, char *said, size_t said_size)
{
 struct apple80211_assoc *asked;
 struct redwood_sta_join *what;
 int error;
 if (request->req_len < offsetof(struct apple80211_assoc, rest)) {
  return EINVAL;
 }
 asked = kalloc_data(sizeof(*asked), Z_WAITOK | Z_ZERO);
 what = kalloc_data(sizeof(*what), Z_WAITOK | Z_ZERO);
 if (asked == NULL || what == NULL || (error = copyin(request->req_data, asked, offsetof(struct apple80211_assoc, rest))) != 0) {
  error = asked == NULL || what == NULL ? ENOMEM : error;
  goto out;
 }
 snprintf(said, said_size, " (a join: a name of %u bytes, %s access point, authentication %#x/%#x, a key of %u bytes of kind %u)",
     asked->ssid_length, asked->bssid[0] | asked->bssid[1] | asked->bssid[2] | asked->bssid[3] | asked->bssid[4] |
     asked->bssid[5] ? "a given" : "any", asked->auth_lower, asked->auth_upper, asked->key_length, asked->key_cipher);
 if (asked->ssid_length == 0 || asked->ssid_length > sizeof(what->ssid)) {
  error = EINVAL;
  goto out;
 }
 what->ssid_length = (uint8_t)asked->ssid_length;
 memcpy(what->ssid, asked->ssid, asked->ssid_length);
 memcpy(what->bssid, asked->bssid, sizeof(what->bssid));
 if (asked->key_length == 0) {
  what->security = REDWOOD_STA_OPEN;
 } else if (asked->key_cipher == CIPHER_PASSPHRASE && asked->key_length >= 8 && asked->key_length <= 63) {
  what->security = REDWOOD_STA_WPA2_PSK;
  what->passphrase_length = (uint8_t)asked->key_length;
  memcpy(what->passphrase, asked->key, asked->key_length);
 } else if (asked->key_cipher == CIPHER_PMK && asked->key_length == sizeof(what->pmk)) {
  what->security = REDWOOD_STA_WPA2_PSK;
  memcpy(what->pmk, asked->key, sizeof(what->pmk));
 } else {
  error = EINVAL;
  goto out;
 }
 lck_mtx_lock(&apple80211_lock);
 join_result = 0;
 lck_mtx_unlock(&apple80211_lock);
 error = redwood_sta_ask_join(what);
out:
 if (asked != NULL) {
  memset_s(asked, sizeof(*asked), 0, sizeof(*asked));
  kfree_data(asked, sizeof(*asked));
 }
 if (what != NULL) {
  memset_s(what, sizeof(*what), 0, sizeof(*what));
  kfree_data(what, sizeof(*what));
 }
 return error;
}
static int
set(const struct apple80211req *request, char *said, size_t said_size)
{
 switch (request->req_type) {
 case 20:
  return set_join(request, said, said_size);
 case 22:
  return redwood_sta_ask_leave();
 case 19: {
  struct {
   uint32_t version, radios, state[4];
  } asked;
  int error;
  if (request->req_len < sizeof(asked) || (error = copyin(request->req_data, &asked, sizeof(asked))) != 0) {
   return request->req_len < sizeof(asked) ? EINVAL : error;
  }
  snprintf(said, said_size, " (power: %s)", asked.state[0] ? "on" : "off");
  return redwood_sta_ask_power(asked.state[0] != 0);
 }
 case 10:
  return set_scan(request, said, said_size);
 default:
  return EOPNOTSUPP;
 }
}
static void
post(uint32_t code, const void *data, uint32_t length)
{
 struct kev_msg message = { .vendor_code = KEV_VENDOR_APPLE, .kev_class = KEV_IEEE80211_CLASS, .kev_subclass = 1,
                     .event_code = code };
 char name[16];
 memcpy(name, interface_name, sizeof(name));
 message.dv[0].data_length = sizeof(name);
 message.dv[0].data_ptr = name;
 if (length != 0) {
  message.dv[1].data_length = length;
  message.dv[1].data_ptr = __DECONST(void *, data);
 }
 kev_post_msg(&message);
}
void
redwood_apple80211_event(enum redwood_sta_event event, int status)
{
 static unsigned said;
 if (said < 200) {
  said++;
  printf("apple80211: the station says: %s, %d\n", event == REDWOOD_STA_POWER ? "power" : event == REDWOOD_STA_SCAN_DONE ?
      "a scan is over" : event == REDWOOD_STA_JOIN_DONE ? "a join is over" : "the link", status);
 }
 switch (event) {
 case REDWOOD_STA_POWER:
  post(EVENT_POWER_CHANGED, NULL, 0);
  break;
 case REDWOOD_STA_SCAN_DONE:
  lck_mtx_lock(&apple80211_lock);
  result_next = 0;
  lck_mtx_unlock(&apple80211_lock);
  post(EVENT_SCAN_DONE, NULL, 0);
  break;
 case REDWOOD_STA_JOIN_DONE:
  lck_mtx_lock(&apple80211_lock);
  join_result = status == 0 ? 1 : status == EAUTH ? 21 : status == ETIMEDOUT ? 9 : 2;
  lck_mtx_unlock(&apple80211_lock);
  post(EVENT_ASSOC_DONE, NULL, 0);
  break;
 case REDWOOD_STA_LINK: {
  struct apple80211_link_changed link = { .down = status != 1, .involuntary = status != 1 && status != 0,
                                   .reason = status != 1 && status != 0 ? 4 : 0 };
  struct redwood_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);
  if (state != NULL) {
   redwood_sta_state(state);
   link.rssi = status == 1 ? state->bss.rssi : 0;
   kfree_data(state, sizeof(*state));
  }
  post(EVENT_SSID_CHANGED, NULL, 0);
  post(EVENT_BSSID_CHANGED, NULL, 0);
  post(EVENT_LINK_CHANGED, &link, sizeof(link));
  break;
 }
 }
}
static struct {
 int32_t type;
 bool set;
 unsigned times, answered;
 int last_error;
} kinds[96];
static unsigned kind_count;
static void
note(const struct apple80211req *request, bool is_set, int error)
{
 unsigned i;
 lck_mtx_lock(&apple80211_lock);
 for (i = 0; i < kind_count && !(kinds[i].type == request->req_type && kinds[i].set == is_set); i++) {
 }
 if (i < sizeof(kinds) / sizeof(kinds[0])) {
  if (i == kind_count) {
   kinds[kind_count++] = (typeof(kinds[0])) { .type = request->req_type, .set = is_set };
  }
  kinds[i].times++;
  kinds[i].answered += error == 0;
  kinds[i].last_error = error != 0 ? error : kinds[i].last_error;
 }
 lck_mtx_unlock(&apple80211_lock);
}
void
redwood_apple80211_summary(void)
{
 static bool said;
 if (said) {
  return;
 }
 said = true;
 for (unsigned i = 0; i < kind_count; i++) {
  printf("apple80211: in this run: %s %d (%s) %u times, answered %u times%s", kinds[i].set ? "set" : "get", kinds[i].type,
      type_name(kinds[i].type), kinds[i].times, kinds[i].answered, kinds[i].last_error == 0 ? "\n" : "");
  if (kinds[i].last_error != 0) {
   printf("; the last refusal: %d%s\n", kinds[i].last_error, kinds[i].last_error == EOPNOTSUPP ? " (not built)" : "");
  }
 }
}
errno_t
redwood_apple80211_ioctl(u_long command, void *data)
{
 static unsigned said;
 struct apple80211req *request = data;
 char who[32] = "?", what[200] = "";
 bool is_set = command == SIOCSA80211;
 int error;
 if (command != SIOCSA80211 && command != SIOCGA80211) {
  return EOPNOTSUPP;
 }
 proc_selfname(who, sizeof(who));
 error = is_set ? set(request, what, sizeof(what)) : get(request);
 note(request, is_set, error);
 if (said < 800) {
  said++;
  printf("apple80211: %s %s %d (%s), value %d, %u bytes%s: %s", who, is_set ? "sets" : "gets", request->req_type,
      type_name(request->req_type), request->req_val, request->req_len, what, error == 0 ? "answered\n" : error ==
      EOPNOTSUPP ? "REFUSED\n" : "");
  if (error != 0 && error != EOPNOTSUPP) {
   printf("error %d\n", error);
  }
 }
 return error;
}
