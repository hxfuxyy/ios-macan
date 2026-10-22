#ifndef _SYS_REDWOOD_QRTR_H_
#define _SYS_REDWOOD_QRTR_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct redwood_qrtr_port;
typedef void (*redwood_qrtr_receive_t)(void *context, uint32_t node, uint32_t port, const void *data, size_t length);
typedef void (*redwood_qrtr_server_t)(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance,
    uint32_t node, uint32_t port);
struct redwood_qrtr_ops {
 struct redwood_qrtr_port *(*open)(redwood_qrtr_receive_t receive, redwood_qrtr_server_t server, void *context);
 int (*send)(struct redwood_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length);
 int (*lookup)(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance);
 void (*close)(struct redwood_qrtr_port *port);
};
void redwood_qmi_set_transport(const struct redwood_qrtr_ops *ops);
#endif
