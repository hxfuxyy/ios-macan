#ifndef XNULOADER_PAYLOAD_H
#define XNULOADER_PAYLOAD_H
#include <stdint.h>
#include <stddef.h>
#define PAYLOAD_MAGIC 0x50554e58u
struct payload_header { uint32_t magic, size; };
struct payload_entry { char name[16]; uint32_t size, reserved; };
static inline int payload_name_is(const char name[16], const char *wanted)
{
    size_t i = 0;
    for (; i < 16 && wanted[i]; i++) if (name[i] != wanted[i]) return 0;
    return i < 16 && name[i] == 0;
}
static inline int payload_valid(const void *buffer, uint64_t capacity)
{
    if (!buffer || capacity < sizeof(struct payload_header)) return 0;
    const struct payload_header *h = buffer;
    if (h->magic != PAYLOAD_MAGIC || h->size > capacity - sizeof(*h)) return 0;
    const uint8_t *p = (const uint8_t *)(h + 1);
    uint64_t left = h->size;
    unsigned kernels = 0, ramdisks = 0;
    while (left >= sizeof(struct payload_entry)) {
        const struct payload_entry *e = (const void *)p;
        if (e->name[0] == 0) return left == sizeof(*e) && e->size == 0 && e->reserved == 0;
        unsigned i = 0;
        while (i < sizeof(e->name) && e->name[i]) i++;
        uint64_t padded = ((uint64_t)e->size + 15) & ~15ULL;
        if (i == sizeof(e->name) || e->reserved || padded > left - sizeof(*e)) return 0;
        if (payload_name_is(e->name, "kernel") && (++kernels > 1 || e->size < 32)) return 0;
        if (payload_name_is(e->name, "ramdisk") && ++ramdisks > 1) return 0;
        if (payload_name_is(e->name, "cmdline") || payload_name_is(e->name, "physbase") ||
            payload_name_is(e->name, "rdres") || payload_name_is(e->name, "checkpoint") ||
            payload_name_is(e->name, "usb")) {
            const uint8_t *text = p + sizeof(*e);
            if (!e->size || text[e->size - 1] != 0) return 0;
        }
        p += sizeof(*e) + padded;
        left -= sizeof(*e) + padded;
    }
    return 0;
}
static inline uint32_t payload_crc32(const void *data, uint64_t bytes)
{
    const uint8_t *p=data;
    uint32_t crc=0xffffffffu;
    while (bytes--) {
        crc ^= *p++;
        for (unsigned i=0; i<8; i++) crc=(crc>>1) ^ (0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
#endif
