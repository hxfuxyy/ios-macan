#ifndef _SYS_REDWOOD_LX_H_
#define _SYS_REDWOOD_LX_H_
#include "macan_sta.h"
void *lx_os_alloc(unsigned long size);
void lx_os_free(void *memory, unsigned long size);
void *lx_os_radio_memory(unsigned long size, unsigned long long *address);
void *lx_os_map_registers(unsigned long long physical, unsigned long size);
void lx_os_print(const char *text);
void lx_os_tell(const char *text);
void lx_os_panic(const char *text) __attribute__((noreturn));
unsigned long long lx_os_nanoseconds(void);
void *lx_os_thread_self(void);
int lx_os_thread(const char *name, void (*function)(void *), void *argument);
void lx_os_lock(void);
void lx_os_unlock(void);
int lx_os_sleep(const void *address, unsigned long long nanoseconds);
void lx_os_wakeup(const void *address);
void lx_os_random(void *buffer, unsigned long size);
unsigned lx_os_option(const char *name);
int lx_os_firmware(const char *name, const void **data, unsigned long *size);
int lx_os_rproc_boot(void);
int lx_os_radio_calibrated(void);
int lx_os_radio_address(unsigned char *address);
int lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length);
void lx_os_net_attach(const unsigned char *address);
void lx_os_net_link(int up);
void lx_os_net_input(const unsigned char *frame, unsigned long length);
int lx_os_irq_msi(unsigned long long *address, unsigned *data);
int lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument);
void lx_os_irq_enable(unsigned vector);
void lx_os_irq_disable(unsigned vector);
void lx_os_irq_detach_all(void);
unsigned long long lx_os_irq_count(unsigned vector);
void lx_os_station(const struct redwood_sta_ops *ops);
void lx_os_station_lost(int error);
int lx_start(void);
void lx_stop(void);
int lx_transmit(const unsigned char *frame, unsigned long length);
int lx_prepare(void);
#endif
