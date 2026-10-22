#include <mach/mach_types.h>
#include <arm/machine_routines.h>
#include <arm/caches_internal.h>
#include <arm/pmap.h>
#if GENERIC_USER_PAC
#include <kern/startup.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include <kern/sched_prim.h>
unsigned generic_user_pac_enabled;
unsigned generic_user_arm64e_enabled;
uint64_t generic_user_pac_shared_keys[10];
unsigned generic_user_pac_shared_keys_ready;
#define GENERIC_SIGN_WITH_KEY(KEY,OP,BIT) \
    __asm__ volatile( \
        ".arch_extension pauth\n" \
        "mrs x12, daif\n msr daifset, #15\n" \
        "mrs x9, " KEY "KeyLo_EL1\n mrs x10, " KEY "KeyHi_EL1\n" \
        "mrs x11, sctlr_el1\n" \
        "ldp x13, x14, [%[keys]]\n" \
        "msr " KEY "KeyLo_EL1, x13\n msr " KEY "KeyHi_EL1, x14\n" \
        "orr x15, x11, #(1 << " BIT ")\n msr sctlr_el1, x15\n isb\n" \
        "mrs x16, tcr_el1\n bic x17, x16, #63\n orr x17, x17, #16\n" \
        "msr tcr_el1, x17\n isb\n" \
        OP " %[value], %[modifier]\n" \
        "msr " KEY "KeyLo_EL1, x9\n msr " KEY "KeyHi_EL1, x10\n" \
        "msr tcr_el1, x16\n msr sctlr_el1, x11\n isb\n msr daif, x12\n" \
        : [value] "+r"(value) \
        : [keys] "r"(keys), [modifier] "r"(modifier) \
        : "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory")
int generic_user_pac_sign_cache(uint64_t value, unsigned key, uint64_t modifier,
    uint64_t *signed_value, void *context);
int generic_user_pac_auth_function(uint64_t value, uint64_t *plain_value);
int
generic_user_pac_sign_cache(uint64_t value, unsigned key, uint64_t modifier,
    uint64_t *signed_value, void *context __unused)
{
    if (!generic_user_pac_enabled || (key != 0 && key != 2)) return 0;
    const uint64_t *keys = &generic_user_pac_shared_keys[key * 2];
    disable_preemption();
    if (key == 0) {
        GENERIC_SIGN_WITH_KEY("APIA", "pacia", "31");
    } else {
        GENERIC_SIGN_WITH_KEY("APDA", "pacda", "27");
    }
    enable_preemption();
    *signed_value = value;
    return 1;
}
int
generic_user_pac_auth_function(uint64_t value, uint64_t *plain_value)
{
    if (!generic_user_pac_enabled) return 0;
    if (value >> 48) {
        const uint64_t *keys = generic_user_pac_shared_keys;
        uint64_t modifier = 0;
        disable_preemption();
        GENERIC_SIGN_WITH_KEY("APIA", "autia", "31");
        enable_preemption();
        if (value >> 48) return 0;
    }
    *plain_value = value;
    return 1;
}
#undef GENERIC_SIGN_WITH_KEY
static void
generic_user_pac_init(void)
{
    unsigned requested = 0, arm64e = 0;
    PE_parse_boot_argn("user_pac", &requested, sizeof(requested));
    PE_parse_boot_argn("user_arm64e", &arm64e, sizeof(arm64e));
    if (requested != 1) return;
    uint64_t isar1, isar2;
    __asm__ volatile("mrs %0, ID_AA64ISAR1_EL1" : "=r"(isar1));
    __asm__ volatile("mrs %0, ID_AA64ISAR2_EL1" : "=r"(isar2));
    unsigned address = ((isar1 >> 4) & 255) | ((isar2 >> 12) & 15);
    unsigned generic = ((isar1 >> 24) & 255) | ((isar2 >> 8) & 15);
    if (!address || !generic) {
        printf("generic-pac: disabled; missing architectural authentication\n");
        return;
    }
    generic_user_pac_enabled = 1;
    generic_user_arm64e_enabled = arm64e == 1;
    printf("generic-pac: EL0 keys enabled; arm64e gate=%u (experimental)\n",
        generic_user_arm64e_enabled);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, generic_user_pac_init);
#endif
void
ml_enable_monitor(void)
{
}
void
CleanPoC_DcacheRegion_Force_nopreempt_nohid(vm_offset_t va, size_t length)
{
 CleanPoC_DcacheRegion_Force(va, length);
}
int __cxa_atexit(void (*func)(void *), void *arg, void *dso);
int
__cxa_atexit(void (*func)(void *) __unused, void *arg __unused, void *dso __unused)
{
 return 0;
}
uint64_t __llvm_profile_get_size_for_buffer_internal(const void *, const void *, const void *, const void *, const void *, const void *);
int __llvm_profile_write_buffer_internal(char *, const void *, const void *, const void *, const void *, const void *, const void *);
uint64_t
__llvm_profile_get_size_for_buffer_internal(const void *a __unused, const void *b __unused,
    const void *c __unused, const void *d __unused, const void *e __unused, const void *f __unused)
{
 return 0;
}
int
__llvm_profile_write_buffer_internal(char *buf __unused, const void *a __unused, const void *b __unused,
    const void *c __unused, const void *d __unused, const void *e __unused, const void *f __unused)
{
 return -1;
}
