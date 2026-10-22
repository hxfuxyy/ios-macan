#ifndef _LX_H_
#define _LX_H_
int lx_base_init(void);
int lx_pool_init(void);
long lx_stub(const char *name);
void lx_tell(const char *format, ...) __printflike(1, 2);
void lx_memory_say(void);
void lx_pause_ms(unsigned ms);
void lx_mark(const char *what);
int lx_station_scan(void);
int lx_station_join(void);
void lx_station_say(void);
void lx_station_leave(void);
int lx_station_send(const unsigned char *frame, unsigned long length);
int lx_station_offer(void);
void lx_station_withdraw(void);
#endif
