#ifndef _SYS_REDWOOD_NET_H_
#define _SYS_REDWOOD_NET_H_
#ifdef KERNEL
#include <sys/kpi_mbuf.h>
#define REDWOOD_NET_FRAME_MAX 1514
struct redwood_net_link {
 const char * name;
 uint8_t address[6];
 bool wireless;
 bool up;
 bool (*transmit)(mbuf_t packet, size_t length);
 unsigned (*poll)(void);
};
bool redwood_net_attach(const struct redwood_net_link *link);
void redwood_net_input(const uint8_t *frame, size_t length);
void redwood_net_link_state(bool up);
void redwood_net_init(void);
errno_t redwood_apple80211_ioctl(u_long command, void *data);
void redwood_virtio_net_init(void);
#endif
#endif
