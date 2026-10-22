#ifndef GENERIC_CACHE_SLIDE_V5_H
#define GENERIC_CACHE_SLIDE_V5_H
#include <stdint.h>
#include <stddef.h>
typedef int (*generic_v5_signer)(uint64_t, unsigned, uint64_t, uint64_t *, void *);
static int generic_slide_v5_page(uint8_t *page, size_t page_size, uint16_t start,
    uint64_t user_address, uint64_t value_add, uint64_t slide,
    generic_v5_signer signer, void *context)
{
    if (page_size != 16384) return 0;
    if (start == 0xffff) return 1;
    size_t offset = start;
    for (;;) {
        if ((offset & 7) || offset > page_size - 8) return 0;
        uint64_t raw;
        __builtin_memcpy(&raw, page + offset, 8);
        uint64_t delta = ((raw >> 52) & 0x7ff) * 8;
        uint64_t value = raw & 0x3ffffffffULL;
        if (value_add > UINT64_MAX - value) return 0;
        value += value_add;
        if (slide > UINT64_MAX - value) return 0;
        value += slide;
        if (raw >> 63) {
            uint64_t discriminator = (raw >> 34) & 0xffff;
            if (raw & (1ULL << 50)) {
                if (offset > UINT64_MAX - user_address) return 0;
                discriminator = ((user_address + offset) & 0x0000ffffffffffffULL) |
                    (discriminator << 48);
            }
            unsigned key = raw & (1ULL << 51) ? 2 : 0;
            if (!signer || !signer(value, key, discriminator, &value, context)) return 0;
        } else {
            uint64_t high8 = (raw << 22) & 0xff00000000000000ULL;
            if (high8 > UINT64_MAX - value) return 0;
            value += high8;
        }
        __builtin_memcpy(page + offset, &value, 8);
        if (!delta) return 1;
        if (delta > page_size - offset - 8) return 0;
        offset += (size_t)delta;
    }
}
#endif
