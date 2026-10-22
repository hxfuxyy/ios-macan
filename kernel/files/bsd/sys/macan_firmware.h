#ifndef _SYS_MACAN_FIRMWARE_H_
#define _SYS_MACAN_FIRMWARE_H_
#include <mach/mach_types.h>
#include <stdint.h>
const uint8_t *macan_image_file(const char *directory, const char *name, uint64_t *size);
#endif
