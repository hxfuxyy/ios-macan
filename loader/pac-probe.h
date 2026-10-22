#ifndef XNU_PAC_PROBE_H
#define XNU_PAC_PROBE_H
struct pac_probe_result {
    uint64_t sctlr, daif, keys[10], signed_pointer[4], authenticated[4];
    uint64_t generic_code, restored_sctlr, restore_difference;
};
_Static_assert(sizeof(struct pac_probe_result) == 184, "PAC assembly result layout");
_Static_assert(__builtin_offsetof(struct pac_probe_result, keys) == 16, "PAC keys offset");
_Static_assert(__builtin_offsetof(struct pac_probe_result, signed_pointer) == 96, "PAC signed offset");
_Static_assert(__builtin_offsetof(struct pac_probe_result, authenticated) == 128, "PAC auth offset");
_Static_assert(__builtin_offsetof(struct pac_probe_result, generic_code) == 160, "PAC generic offset");
_Static_assert(__builtin_offsetof(struct pac_probe_result, restored_sctlr) == 168, "PAC SCTLR offset");
_Static_assert(__builtin_offsetof(struct pac_probe_result, restore_difference) == 176, "PAC restore offset");
#define PAC_SAVE(KEY,OFF) \
    "mrs x9, " KEY "KeyLo_EL1\n" \
    "mrs x10, " KEY "KeyHi_EL1\n" \
    "stp x9, x10, [%0, #" OFF "]\n"
#define PAC_INSTALL(KEY) \
    "msr " KEY "KeyLo_EL1, x9\n" \
    "msr " KEY "KeyHi_EL1, x10\n"
#define PAC_ROUNDTRIP(SIGN,AUTH,SOFF,AOFF) \
    "mov x12, #0x1234\n" \
    SIGN " x12, x13\n" \
    "str x12, [%0, #" SOFF "]\n" \
    AUTH " x12, x13\n" \
    "str x12, [%0, #" AOFF "]\n"
#define PAC_RESTORE(KEY,OFF) \
    "ldp x9, x10, [%0, #" OFF "]\n" \
    PAC_INSTALL(KEY) \
    "mrs x11, " KEY "KeyLo_EL1\n" \
    "mrs x12, " KEY "KeyHi_EL1\n" \
    "eor x11, x11, x9\n" \
    "eor x12, x12, x10\n" \
    "orr x14, x14, x11\n" \
    "orr x14, x14, x12\n"
static void
pac_probe_el1(struct pac_probe_result *result)
{
    __asm__ volatile(
        ".arch_extension pauth\n"
        "mrs x9, sctlr_el1\n"
        "mrs x10, daif\n"
        "msr daifset, #15\n"
        "stp x9, x10, [%0]\n"
        PAC_SAVE("APIA", "16")
        PAC_SAVE("APIB", "32")
        PAC_SAVE("APDA", "48")
        PAC_SAVE("APDB", "64")
        PAC_SAVE("APGA", "80")
        "mov x9, #0x1111\n"
        "mov x10, #0x2222\n"
        PAC_INSTALL("APIA")
        PAC_INSTALL("APIB")
        PAC_INSTALL("APDA")
        PAC_INSTALL("APDB")
        PAC_INSTALL("APGA")
        "ldr x9, [%0]\n"
        "orr x9, x9, #(1 << 31)\n"
        "orr x9, x9, #(1 << 30)\n"
        "orr x9, x9, #(1 << 27)\n"
        "orr x9, x9, #(1 << 13)\n"
        "msr sctlr_el1, x9\n"
        "isb\n"
        "mov x13, #0x4567\n"
        PAC_ROUNDTRIP("pacia", "autia", "96", "128")
        PAC_ROUNDTRIP("pacib", "autib", "104", "136")
        PAC_ROUNDTRIP("pacda", "autda", "112", "144")
        PAC_ROUNDTRIP("pacdb", "autdb", "120", "152")
        "mov x12, #0x1234\n"
        "pacga x11, x12, x13\n"
        "str x11, [%0, #160]\n"
        "mov x14, xzr\n"
        PAC_RESTORE("APIA", "16")
        PAC_RESTORE("APIB", "32")
        PAC_RESTORE("APDA", "48")
        PAC_RESTORE("APDB", "64")
        PAC_RESTORE("APGA", "80")
        "str x14, [%0, #176]\n"
        "ldr x9, [%0]\n"
        "msr sctlr_el1, x9\n"
        "isb\n"
        "mrs x9, sctlr_el1\n"
        "str x9, [%0, #168]\n"
        "ldr x10, [%0, #8]\n"
        "msr daif, x10\n"
        : : "r"(result)
        : "x9", "x10", "x11", "x12", "x13", "x14", "memory");
}
#undef PAC_SAVE
#undef PAC_INSTALL
#undef PAC_ROUNDTRIP
#undef PAC_RESTORE
#endif
