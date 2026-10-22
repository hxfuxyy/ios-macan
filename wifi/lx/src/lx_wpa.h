#ifndef _LX_WPA_H_
#define _LX_WPA_H_
void lx_wpa_pmk(const unsigned char *passphrase, unsigned passphrase_length, const unsigned char *name, unsigned name_length,
    unsigned char pmk[32]);
struct lx_wpa {
 unsigned char pmk[32];
 unsigned char own[6], peer[6];
 unsigned char own_nonce[32];
 const unsigned char *rsn;
 unsigned rsn_length;
 unsigned char peer_nonce[32];
 unsigned char kck[16], kek[16];
 unsigned char replay[8];
 int have_keys, replay_known;
 unsigned char pairwise[16];
 unsigned char group[32];
 unsigned group_length, group_index;
 unsigned char group_counter[8];
};
#define LX_WPA_REPLY_MOST 256
enum {
 LX_WPA_BAD = -1,
 LX_WPA_NOTHING = 0,
 LX_WPA_ANSWER = 1,
 LX_WPA_KEYS = 2,
 LX_WPA_GROUP = 3,
};
int lx_wpa_message(struct lx_wpa *wpa, const unsigned char *message, unsigned length, unsigned char *reply,
    unsigned *reply_length, const char **why);
#endif
