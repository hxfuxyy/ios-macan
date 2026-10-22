#ifndef _LX12_OF_GRAPH_H
#define _LX12_OF_GRAPH_H
#include <linux/of.h>
static inline struct device_node *of_graph_get_endpoint_by_regs(const struct device_node *p, int port, int reg) { return NULL; }
static inline struct device_node *of_graph_get_remote_endpoint(const struct device_node *n) { return NULL; }
static inline struct device_node *of_graph_get_port_parent(struct device_node *n) { return NULL; }
static inline struct device_node *of_graph_get_next_endpoint(const struct device_node *p, struct device_node *prev) { return NULL; }
static inline struct device_node *of_graph_get_remote_port_parent(const struct device_node *n) { return NULL; }
#define for_each_endpoint_of_node(parent,child) for (child = NULL; child; )
#endif
