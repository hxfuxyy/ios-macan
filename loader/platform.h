#ifndef XNULOADER_PLATFORM_H
#define XNULOADER_PLATFORM_H
#include <stdint.h>
struct platform {
    const char *name;
    uint64_t io_base, io_size, gic_dist, gic_redist, gic_redist_size, gic_redist_stride;
    uint64_t uart;
    int psci_hvc, log_other_cpu, cores, core_shift;
    uint32_t timeout_scale;
    const uint64_t *mpidrs;
};
static const struct platform platform_qemu_virt = {
    .name="qemu-virt", .io_base=0x08000000, .io_size=0x08000000,
    .gic_dist=0x08000000, .gic_redist=0x080a0000,
    .gic_redist_size=0x100000, .gic_redist_stride=0x20000,
    .uart=0x09000000, .psci_hvc=1, .cores=8, .core_shift=0, .timeout_scale=20,
};
static const struct platform platform_redwood = {
    .name="redwood", .io_base=0x00400000, .io_size=0x3fc00000,
    .gic_dist=0x17a00000, .gic_redist=0x17a60000,
    .gic_redist_size=0x100000, .gic_redist_stride=0x20000,
    .log_other_cpu=1, .cores=8, .core_shift=8,
};
static const uint64_t macan_mpidrs[8] = {0,0x100,0x200,0x300,0x400,0x500,0x10000,0x10100};
static const struct platform platform_macan = {
    .name="macan", .io_base=0x00400000, .io_size=0x3fc00000,
    .gic_dist=0x17000000, .gic_redist=0x17080000,
    .gic_redist_size=0x200000, .gic_redist_stride=0x40000,
    .cores=8, .mpidrs=macan_mpidrs,
};
static inline uint64_t platform_mpidr(const struct platform *p, int index)
{
    if (index < 0 || index >= p->cores) return UINT64_MAX;
    return p->mpidrs ? p->mpidrs[index] : (uint64_t)index << p->core_shift;
}
static inline int platform_core_index(const struct platform *p, uint64_t mpidr)
{
    for (int i=0; i<p->cores; i++) if (platform_mpidr(p,i)==mpidr) return i;
    return -1;
}
#endif
