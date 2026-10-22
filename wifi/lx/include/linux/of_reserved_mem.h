#ifndef _LX_LINUX_OF_RESERVED_MEM_H
#define _LX_LINUX_OF_RESERVED_MEM_H
#include <linux/of.h>
#include <linux/ioport.h>
int of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index,
    struct resource *res);
#endif
