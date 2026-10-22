#ifndef _SYS_REDWOOD_STA_H_
#define _SYS_REDWOOD_STA_H_
#include <stdbool.h>
#include <stdint.h>
#define REDWOOD_STA_ELEMENTS_MOST 1024
struct redwood_sta_bss {
 uint8_t bssid[6];
 uint8_t ssid[32], ssid_length;
 uint16_t frequency;
 int16_t rssi;
 uint16_t beacon_interval;
 uint16_t capability;
 uint32_t age_ms;
 uint16_t elements_length;
 uint8_t elements[REDWOOD_STA_ELEMENTS_MOST];
};
struct redwood_sta_scan {
 uint8_t ssid[32], ssid_length;
 uint16_t frequencies[64], frequency_count;
 bool passive;
 uint16_t dwell_ms;
};
enum { REDWOOD_STA_OPEN, REDWOOD_STA_WPA2_PSK };
struct redwood_sta_join {
 uint8_t ssid[32], ssid_length;
 uint8_t bssid[6];
 int security;
 uint8_t pmk[32];
 uint8_t passphrase[64], passphrase_length;
};
enum redwood_sta_phase { REDWOOD_STA_OFF, REDWOOD_STA_IDLE, REDWOOD_STA_SCANNING, REDWOOD_STA_JOINING, REDWOOD_STA_JOINED };
struct redwood_sta_state {
 enum redwood_sta_phase phase;
 struct redwood_sta_bss bss;
 int last_error;
};
#define REDWOOD_STA_CHANNEL_PASSIVE 1u
#define REDWOOD_STA_CHANNEL_RADAR 2u
struct redwood_sta_channel {
 uint16_t frequency;
 uint16_t flags;
};
struct redwood_sta_ops {
 int (*power)(bool on);
 int (*scan)(const struct redwood_sta_scan *what);
 void (*heard)(void (*each)(const struct redwood_sta_bss *bss, void *context), void *context);
 int (*join)(const struct redwood_sta_join *what);
 int (*leave)(void);
 void (*state)(struct redwood_sta_state *state);
 unsigned (*channels)(struct redwood_sta_channel *channels, unsigned most);
 uint8_t address[6];
};
enum redwood_sta_event {
 REDWOOD_STA_POWER,
 REDWOOD_STA_SCAN_DONE,
 REDWOOD_STA_JOIN_DONE,
 REDWOOD_STA_LINK,
};
#ifdef KERNEL
void redwood_sta_attach(const struct redwood_sta_ops *ops);
void redwood_sta_lost(int error);
bool redwood_sta_present(void);
int redwood_sta_ask_power(bool on);
int redwood_sta_ask_scan(const struct redwood_sta_scan *what);
int redwood_sta_ask_join(const struct redwood_sta_join *what);
int redwood_sta_ask_join_standing(const struct redwood_sta_join *what);
int redwood_sta_ask_leave(void);
bool redwood_sta_busy(void);
bool redwood_sta_powered(void);
bool redwood_sta_heard(unsigned index, struct redwood_sta_bss *bss);
void redwood_sta_state(struct redwood_sta_state *state);
unsigned redwood_sta_channels(struct redwood_sta_channel *channels, unsigned most);
bool redwood_sta_address(uint8_t *address);
void redwood_apple80211_event(enum redwood_sta_event event, int status);
void redwood_apple80211_summary(void);
#endif
#endif
