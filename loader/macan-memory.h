#ifndef MACAN_MEMORY_H
#define MACAN_MEMORY_H
#include <stdint.h>
#include <stddef.h>
struct memory_range { uint64_t lo, hi; };
static const struct memory_range macan_reserved[] = {
    {0x80000000ULL,0x81400000ULL},
    {0x81a00000ULL,0x81a60000ULL},
    {0x81c00000ULL,0x81cf4000ULL},
    {0x81cff000ULL,0x82478000ULL},
    {0x824a0000ULL,0x82600000ULL},
    {0x82700000ULL,0x84800000ULL},
    {0x84a00000ULL,0x8a980000ULL},
    {0x8aa00000ULL,0x9959c000ULL},
    {0x99f00000ULL,0xa3c00000ULL},
    {0xa4080000ULL,0xa4a40000ULL},
    {0xa7f00000ULL,0xa7f08000ULL},
    {0xb8000000ULL,0xb81c0000ULL},
    {0xd4e23000ULL,0xd5100000ULL},
    {0xd63d0000ULL,0xd6bd0000ULL},
    {0xd6e00000ULL,0xd7c00000ULL},
    {0xd8000000ULL,0xdba00000ULL},
    {0xf3800000ULL,0xf7800000ULL},
    {0xf7c00000ULL,0xff300000ULL},
    {0xff800000ULL,0x100000000ULL},
    {0x880000000ULL,0x886000000ULL},
};
static inline uint64_t memory_align(uint64_t x, uint64_t alignment) { return (x + alignment - 1) & ~(alignment - 1); }
static inline void macan_largest_gap(uint64_t lo, uint64_t hi, uint64_t alignment,
                                     uint64_t *best_lo, uint64_t *best_size)
{
    if (hi > (1ULL<<40)) hi = 1ULL<<40;
    if (lo >= hi) return;
    for (size_t i=0; i<=sizeof(macan_reserved)/sizeof(macan_reserved[0]); i++) {
        uint64_t end = hi;
        if (i < sizeof(macan_reserved)/sizeof(macan_reserved[0])) {
            if (macan_reserved[i].hi <= lo) continue;
            if (macan_reserved[i].lo < end) end=macan_reserved[i].lo;
        }
        uint64_t begin=memory_align(lo,alignment);
        end &= ~(alignment-1);
        if (end > begin && end-begin > *best_size) { *best_lo=begin; *best_size=end-begin; }
        if (i == sizeof(macan_reserved)/sizeof(macan_reserved[0]) || macan_reserved[i].hi >= hi) break;
        if (macan_reserved[i].hi > lo) lo=macan_reserved[i].hi;
    }
}
static inline int macan_range_allowed(uint64_t lo, uint64_t bytes)
{
    if (!bytes || lo >= (1ULL<<40) || bytes > (1ULL<<40)-lo) return 0;
    for (size_t i=0; i<sizeof(macan_reserved)/sizeof(macan_reserved[0]); i++)
        if (lo < macan_reserved[i].hi && lo+bytes > macan_reserved[i].lo) return 0;
    return 1;
}
#endif
