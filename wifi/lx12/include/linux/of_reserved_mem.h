#ifndef _LX12_LINUX_OF_RESERVED_MEM_H
#define _LX12_LINUX_OF_RESERVED_MEM_H
#include_next <linux/of_reserved_mem.h>
#include <linux/types.h>
struct reserved_mem {
 const char *name;
 unsigned long fdt_node;
 const void *ops;
 phys_addr_t base;
 phys_addr_t size;
 void *priv;
};
struct reserved_mem *of_reserved_mem_lookup(struct device_node *np);
#endif
