#include "efi.h"
#include "payload.h"
#include "platform.h"
#include "macan-memory.h"
#if defined(XNU_PLATFORM_MACAN)
#include "pac-probe.h"
#endif
#if defined(XNU_PLATFORM_MACAN)
#include "../uefi/mu/Platforms/OnePlus/macanPkg/Applications/DarwinMenu/HackIOSUi.h"
static void hackios_ready_screen(void);
#endif
#include "identity.h"
#if defined(XNU_PLATFORM_MACAN)
static uint64_t payload_base, panic_log_base;
#define PAYLOAD_BASE payload_base
#define PANIC_LOG_BASE panic_log_base
#else
#define PAYLOAD_BASE 0xF0000000ULL
#define PANIC_LOG_BASE (PAYLOAD_BASE + PAYLOAD_MAX_SIZE - 0x100000ULL)
#endif
static uint64_t payload_bytes;
static int kernel_hung;
#define KERNEL_VIRT_BASE 0xFFFFFFF004000000ULL
#define KERNEL_LINK_OFFSET 0x03000000ULL
#define BASE_ALIGN 0x02000000ULL
#define MAX_MEM_SIZE 0x180000000ULL
#define RESIDENT_RD_OFFSET 0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL
#define PAGE_SIZE_MAX 0x4000ULL
#define LOG_PARTITION u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE 0x80000u
#define NVRAM_BANK_SIZE 0x2000u
#define PAYLOAD_MAX_SIZE 0x04000000ULL
struct boot_video {
 uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};
#define BOOT_LINE_LENGTH 608
struct boot_args {
 uint16_t revision;
 uint16_t version;
 uint64_t virtBase;
 uint64_t physBase;
 uint64_t memSize;
 uint64_t topOfKernelData;
 struct boot_video video;
 uint32_t machineType;
 uint64_t deviceTreeP;
 uint32_t deviceTreeLength;
 char commandLine[BOOT_LINE_LENGTH];
 uint64_t bootFlags;
 uint64_t memSizeActual;
};
#define MH_MAGIC_64 0xFEEDFACFu
#define LC_SEGMENT_64 0x19
#define LC_UNIXTHREAD 0x5
struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
 uint32_t cmd, cmdsize; char segname[16];
 uint64_t vmaddr, vmsize, fileoff, filesize;
 uint32_t maxprot, initprot, nsects, flags;
};
static efi_system_table *ST;
static efi_boot_services *BS;
static char log_buf[32768];
static uint32_t log_len;
void *
memset(void *dst, int c, size_t n)
{
 uint8_t *d = dst;
 while (n--) {
  *d++ = (uint8_t)c;
 }
 return dst;
}
void *
memcpy(void *dst, const void *src, size_t n)
{
 uint8_t *d = dst;
 const uint8_t *s = src;
 if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
  uint64_t *dw = (uint64_t *)d;
  const uint64_t *sw = (const uint64_t *)s;
  for (; n >= 32; n -= 32, dw += 4, sw += 4) {
   uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
   dw[0] = a;
   dw[1] = b;
   dw[2] = c;
   dw[3] = e;
  }
  for (; n >= 8; n -= 8) {
   *dw++ = *sw++;
  }
  d = (uint8_t *)dw;
  s = (const uint8_t *)sw;
 }
 while (n--) {
  *d++ = *s++;
 }
 return dst;
}
static size_t
str_len(const char *s)
{
 size_t n = 0;
 while (s[n]) {
  n++;
 }
 return n;
}
static int
str_eq(const char *a, const char *b)
{
 while (*a && *a == *b) {
  a++, b++;
 }
 return *a == *b;
}
static int screen_quiet;
static void
put_char(char c)
{
 char16 w[3] = { 0, 0, 0 };
 if (log_len < sizeof(log_buf) - 1) {
  log_buf[log_len++] = c;
 }
 if (ST->con_out != NULL && !screen_quiet) {
  if (c == '\n') {
   w[0] = '\r', w[1] = '\n';
  } else {
   w[0] = (char16)c;
  }
  ST->con_out->output_string(ST->con_out, w);
 }
}
static void
print(const char *fmt, ...)
{
 __builtin_va_list ap;
 __builtin_va_start(ap, fmt);
 for (; *fmt; fmt++) {
  if (*fmt != '%') {
   put_char(*fmt);
   continue;
  }
  fmt++;
  if (*fmt == 's') {
   const char *s = __builtin_va_arg(ap, const char *);
   while (*s) {
    put_char(*s++);
   }
  } else if (*fmt == 'x' || *fmt == 'd') {
   uint64_t v = __builtin_va_arg(ap, uint64_t);
   unsigned base = (*fmt == 'x') ? 16 : 10;
   char tmp[24];
   int i = 0;
   do {
    tmp[i++] = "0123456789abcdef"[v % base];
    v /= base;
   } while (v);
   while (i) {
    put_char(tmp[--i]);
   }
  } else {
   put_char(*fmt);
  }
 }
 __builtin_va_end(ap);
}
#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })
static void
flush_dcache(uint64_t start, uint64_t end)
{
 uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
 for (uint64_t p = start & ~(line - 1); p < end; p += line) {
  __asm__ volatile ("dc civac, %0" : : "r" (p));
 }
 __asm__ volatile ("dsb sy");
}
static void
clean_dcache(uint64_t start, uint64_t end)
{
 uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
 for (uint64_t p = start & ~(line - 1); p < end; p += line) {
  __asm__ volatile ("dc cvac, %0" : : "r" (p));
 }
 __asm__ volatile ("dsb sy");
}
static __attribute__((unused)) efi_block_io *
find_log_partition(void)
{
 static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
 static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
 static const char16 want[] = LOG_PARTITION;
 efi_handle *handles;
 uint64_t count;
 if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
  return NULL;
 }
 for (uint64_t i = 0; i < count; i++) {
  efi_partition_info *info;
  efi_block_io *bio;
  int match = 1;
  if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
   continue;
  }
  for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
   if (info->name[k] != want[k]) {
    match = 0;
    break;
   }
  }
  if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
   return bio;
  }
 }
 return NULL;
}
static char out_buf[2 * 1024 * 1024];
static const char *staged_data = out_buf;
static uint64_t staged_len;
static void
compose_logs(const char *klog, uint64_t klog_len)
{
 static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
 uint64_t n = log_len;
 staged_data = out_buf;
 memset(out_buf, 0, sizeof(out_buf));
 memcpy(out_buf, log_buf, log_len);
 if (klog != NULL) {
  if (klog_len > sizeof(out_buf) - n - 8192) {
   klog_len = sizeof(out_buf) - n - 8192;
  }
  memcpy(out_buf + n, sep, sizeof(sep) - 1);
  n += sizeof(sep) - 1;
  memcpy(out_buf + n, klog, klog_len);
  n += klog_len;
  memcpy(out_buf + n, end, sizeof(end) - 1);
  n += sizeof(end) - 1;
 }
 staged_len = n;
}
static void
write_logs(const char *klog, uint64_t klog_len)
{
 #if defined(XNU_PLATFORM_MACAN)
    efi_block_io *bio = NULL;
#else
    efi_block_io *bio = find_log_partition();
#endif
 compose_logs(klog, klog_len);
 if (bio == NULL) {
  if (klog != NULL) {
   print("\n== KERNEL LOG ==\n");
   for (uint64_t i = 0; i < klog_len; i++) {
    put_char(klog[i]);
   }
   print("\n== END OF LOG ==\n");
  }
  return;
 }
 uint64_t bs = bio->media->block_size;
 uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;
 if (!EFI_ERROR(bio->write_blocks(bio, bio->media->media_id, 0, len, out_buf))) {
  bio->flush_blocks(bio);
 }
}
static void
flush_log(void)
{
 write_logs(NULL, 0);
}
struct dt_writer {
 uint8_t *base, *cur;
 uint32_t *counts[16];
 int depth;
};
static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
 memset(w->cur, 0, 32);
 memcpy(w->cur, name, str_len(name));
 *(uint32_t *)(w->cur + 32) = len;
 memset(w->cur + 36, 0, (len + 3) & ~3u);
 memcpy(w->cur + 36, data, len);
 w->cur += 36 + ((len + 3) & ~3u);
 w->counts[w->depth - 1][0]++;
}
static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
 dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}
static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
 dt_prop(w, name, &value, sizeof(value));
}
static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
 dt_prop(w, name, &value, sizeof(value));
}
static uint64_t
firmware_epoch(void)
{
 static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
 struct efi_time t;
 uint64_t days;
 if (ST->rt == NULL || ST->rt->get_time == NULL || EFI_ERROR(ST->rt->get_time(&t, NULL)) ||
     t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12) {
  return 0;
 }
 days = (uint64_t)(t.year - 1970) * 365 + (t.year - 1969) / 4 + before_month[t.month - 1] + (t.day - 1);
 if (t.month > 2 && t.year % 4 == 0) {
  days++;
 }
 uint64_t secs = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
 if (t.timezone != 0x7FF) {
  secs += (int64_t)t.timezone * 60;
 }
 return secs;
}
static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
 uint8_t field[64];
 memset(field, 0, sizeof(field));
 memcpy(field, value, str_len(value));
 dt_prop(w, name, field, size);
}
static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
 for (uint32_t i = 0; i < count; i++) {
  if (str_eq(props[i].name, "artwork-display-gamut")) {
   dt_prop(w, props[i].name, "sRGB", 5);
   continue;
  }
  dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
 }
}
static void
dt_begin(struct dt_writer *w, const char *name)
{
 if (w->depth > 0) {
  w->counts[w->depth - 1][1]++;
 }
 w->counts[w->depth++] = (uint32_t *)w->cur;
 memset(w->cur, 0, 8);
 w->cur += 8;
 dt_prop_str(w, "name", name);
}
static void
dt_end(struct dt_writer *w)
{
 w->depth--;
}
static void
reboot(void)
{
 flush_log();
 ST->rt->reset_system(0 , 0, 0, NULL);
 for (;;) {
 }
}
enum {
 CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
 CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
 CTX_X18, CTX_X19,
 CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
 CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
 CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1,
    CTX_PAC_KEYS, CTX_COUNT = CTX_PAC_KEYS + 10
};
_Static_assert(CTX_PAC_KEYS * 8 == 368, "PAC context assembly offsets");
uint64_t loader_ctx[CTX_COUNT];
int loader_save_ctx(void);
void loader_return_stub(void);
__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mrs x9, s3_3_c4_c4_0\n          str x9, [x10, #328]\n"
        "  mrs x9, s3_3_c4_c4_1\n          str x9, [x10, #336]\n"
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
#if defined(XNU_PLATFORM_MACAN)
        "  .arch_extension pauth\n"
        "  mrs x9, APIAKeyLo_EL1\n str x9, [x10, #368]\n"
        "  mrs x9, APIAKeyHi_EL1\n str x9, [x10, #376]\n"
        "  mrs x9, APIBKeyLo_EL1\n str x9, [x10, #384]\n"
        "  mrs x9, APIBKeyHi_EL1\n str x9, [x10, #392]\n"
        "  mrs x9, APDAKeyLo_EL1\n str x9, [x10, #400]\n"
        "  mrs x9, APDAKeyHi_EL1\n str x9, [x10, #408]\n"
        "  mrs x9, APDBKeyLo_EL1\n str x9, [x10, #416]\n"
        "  mrs x9, APDBKeyHi_EL1\n str x9, [x10, #424]\n"
        "  mrs x9, APGAKeyLo_EL1\n str x9, [x10, #432]\n"
        "  mrs x9, APGAKeyHi_EL1\n str x9, [x10, #440]\n"
#endif
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
        "  msr daifset, #0xf\n"
        "  .inst 0xd500409f\n"
        "  .inst 0xd500407f\n"
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
#if defined(XNU_PLATFORM_MACAN)
        "  ldr x9, [x10, #368]\n msr APIAKeyLo_EL1, x9\n"
        "  ldr x9, [x10, #376]\n msr APIAKeyHi_EL1, x9\n"
        "  ldr x9, [x10, #384]\n msr APIBKeyLo_EL1, x9\n"
        "  ldr x9, [x10, #392]\n msr APIBKeyHi_EL1, x9\n"
        "  ldr x9, [x10, #400]\n msr APDAKeyLo_EL1, x9\n"
        "  ldr x9, [x10, #408]\n msr APDAKeyHi_EL1, x9\n"
        "  ldr x9, [x10, #416]\n msr APDBKeyLo_EL1, x9\n"
        "  ldr x9, [x10, #424]\n msr APDBKeyHi_EL1, x9\n"
        "  ldr x9, [x10, #432]\n msr APGAKeyLo_EL1, x9\n"
        "  ldr x9, [x10, #440]\n msr APGAKeyHi_EL1, x9\n"
#endif
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
        "  ldr x9, [x10, #328]\n msr s3_3_c4_c4_0, x9\n"
        "  ldr x9, [x10, #336]\n msr s3_3_c4_c4_1, x9\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"
        "  stp x2, x3, [x10, #232]\n"
        "  stp x4, x5, [x10, #248]\n"
        "  stp x6, x7, [x10, #264]\n"
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );
__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
 __asm__ volatile (
                "msr daifset, #0xf\n"
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"
                "bic x2, x2, #0x4\n"
                "bic x2, x2, #0x1000\n"
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
 __builtin_unreachable();
}
#define PSCI_CPU_OFF 0x84000002u
#define PSCI_CPU_ON 0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u
#define PSCI_NOT_SUPPORTED (-1)
static struct {
 uint64_t entry, boot_args;
} other_cpu_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));
volatile uint64_t other_cpu_done __attribute__((aligned(64)));
uint64_t psci_by_hvc __attribute__((aligned(64)));
void other_cpu_entry(void);
void other_cpu_return_stub(void);
__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"
        "  msr daifset, #0xf\n"
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  ldp x9, x10, [x0]\n"
        "  mov x0, x10\n"
        "  br x9\n"
        "other_cpu_fault:\n"
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"
        "  msr daifset, #0xf\n"
        "  .inst 0xd500409f\n"
        "  .inst 0xd500407f\n"
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"
        "  stp x2, x3, [x10, #232]\n"
        "  stp x4, x5, [x10, #248]\n"
        "  stp x6, x7, [x10, #264]\n"
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        "  adrp x10, psci_by_hvc\n"
        "  add  x10, x10, :lo12:psci_by_hvc\n"
        "  ldr x9, [x10]\n"
        "  mov x0, #0x0002\n"
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 2f\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n"
        "2: hvc #0\n"
        "  b 1b\n"
        );
static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);
struct kernel_live {
 uint64_t magic;
 uint64_t heartbeat;
 uint64_t log, log_len, log_ring_at, log_wrapped;
 uint64_t log_head, log_ring;
 uint64_t recording;
 uint64_t stop;
};
#define LOADER_BATTERY_MAGIC 0x2154544142574452ULL
#define LOADER_BATTERY_VERSION 1
#define LOADER_BATTERY_VALID (1u << 0)
#define LOADER_BATTERY_PRESENT (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID (1u << 3)
#define LOADER_BATTERY_CHARGING (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED (1u << 5)
struct loader_battery {
 uint64_t magic;
 uint32_t version, size;
 volatile uint32_t sequence;
 uint32_t flags;
 int32_t soc_percent;
 int32_t voltage_mv;
 int32_t current_ma;
 int32_t temperature_c;
 uint32_t charger_action;
 uint32_t polls;
 uint32_t failures;
 uint32_t reserved;
};
struct qcom_battery_status {
 uint32_t status0, status1;
 int32_t state_of_charge;
 int32_t charge_current_ma;
 int32_t battery_voltage_mv;
 int32_t battery_temperature_c;
 uint32_t rest[26];
};
static struct loader_battery charger_shared __attribute__((aligned(0x2000))) = {
 .magic = LOADER_BATTERY_MAGIC,
 .version = LOADER_BATTERY_VERSION,
 .size = sizeof(struct loader_battery),
 .soc_percent = -1,
 .voltage_mv = -1,
 .current_ma = 0,
 .temperature_c = -273,
 .charger_action = 0xffffffffu,
};
static efi_charger_ex *CHARGER_EX;
static efi_qcom_charger *QCOM_CHARGER;
static int charger_enabled;
static const char *recording_data;
static uint64_t recording_len;
#define KERNEL_LIVE_MAGIC 0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED 1000
#define OTHER_CPU_HUNG 1001
#define START_SECONDS 5
#define SILENCE_SECONDS 50
#define NOTE_SECONDS 24
static void wdt_pet(void);
static int wdt_armed;
static const struct kernel_live *running_live;
static uint64_t running_slide;
static void usb_poll_command(void);
static uint64_t
seconds_since(uint64_t then)
{
 return (READ_SYSREG(CNTVCT_EL0) - then) / READ_SYSREG(CNTFRQ_EL0);
}
static void
reverse(char *p, uint64_t n)
{
 for (uint64_t i = 0; i < n / 2; i++) {
  char c = p[i];
  p[i] = p[n - 1 - i];
  p[n - 1 - i] = c;
 }
}
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
 char *log = (char *)(live->log + slide);
 uint32_t len, ring_at;
 loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
 len = *(volatile uint32_t *)(live->log_len + slide);
 ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
 if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
  reverse(log + live->log_head, ring_at);
  reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
  reverse(log + live->log_head, live->log_ring);
  len = (uint32_t)(live->log_head + live->log_ring);
 }
 if (len <= live->log_head + live->log_ring) {
  loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
  loader_ctx[CTX_LOG_LEN] = len;
 }
}
static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
 static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
 const char *log = (const char *)(live->log + slide);
 uint32_t len = *(volatile uint32_t *)(live->log_len + slide);
 uint32_t ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
 int wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);
 uint64_t n, room = sizeof(out_buf) - 8192;
 compose_logs(NULL, 0);
 n = staged_len;
#define PUT(from,size) do { \
 uint64_t take = (size); \
 if (n + take > room) { \
         take = room - n; \
 } \
 memcpy(out_buf + n, (from), take); \
 n += take; \
} while (0)
 PUT(sep, sizeof(sep) - 1);
 if (!wrapped || ring_at >= live->log_ring) {
  PUT(log, len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring);
 } else {
  PUT(log, live->log_head);
  PUT(log + live->log_head + ring_at, live->log_ring - ring_at);
  PUT(log + live->log_head, ring_at);
 }
#undef PUT
 staged_len = n;
}
static void print(const char *fmt, ...);
static void flush_log(void);
static void put_char(char c);
static int
soc_from_voltage(uint32_t mv)
{
 if (mv <= 3300) return 0;
 if (mv < 3700) return (int)((mv - 3300) * 10 / 400);
 if (mv < 3900) return 10 + (int)((mv - 3700) * 40 / 200);
 if (mv < 4200) return 50 + (int)((mv - 3900) * 40 / 300);
 if (mv < 4400) return 90 + (int)((mv - 4200) * 10 / 200);
 return 100;
}
static void
charger_poll(void)
{
 struct qcom_battery_status status;
 uint32_t action = 0xffffffffu, flags = 0, voltage = 0;
 uint8_t present = 0, plugged = 0;
 efi_status s;
 int have_soc = 0, have_voltage = 0;
 if (!charger_enabled) {
  return;
 }
 memset(&status, 0, sizeof(status));
 charger_shared.sequence++;
 __asm__ volatile ("dmb ish" ::: "memory");
 if (CHARGER_EX != NULL) {
  s = CHARGER_EX->get_battery_presence(&present);
  if (!EFI_ERROR(s) && present) flags |= LOADER_BATTERY_PRESENT;
  s = CHARGER_EX->get_charger_presence(&plugged);
  if (!EFI_ERROR(s) && plugged) flags |= LOADER_BATTERY_CHARGER_PRESENT;
  s = CHARGER_EX->get_battery_voltage(&voltage);
  if (!EFI_ERROR(s) && voltage >= 2500 && voltage <= 5000) {
   have_voltage = 1;
   flags |= LOADER_BATTERY_VOLTAGE_VALID;
   charger_shared.voltage_mv = (int32_t)voltage;
  }
 }
 if (QCOM_CHARGER != NULL) {
  s = QCOM_CHARGER->get_charging_action(&action, &status, 0);
  if (!EFI_ERROR(s)) {
   charger_shared.charger_action = action;
   if (status.battery_voltage_mv >= 2500 && status.battery_voltage_mv <= 5000) {
    charger_shared.voltage_mv = status.battery_voltage_mv;
    have_voltage = 1;
    flags |= LOADER_BATTERY_VOLTAGE_VALID;
   }
   if (status.state_of_charge >= 0 && status.state_of_charge <= 100) {
    int vmv = have_voltage ? charger_shared.voltage_mv : 0;
    if (status.state_of_charge < 5 && vmv > 3600) {
    } else {
     charger_shared.soc_percent = status.state_of_charge;
     have_soc = 1;
     flags |= LOADER_BATTERY_VALID;
    }
   }
   charger_shared.current_ma = status.charge_current_ma;
   charger_shared.temperature_c = status.battery_temperature_c;
  }
 }
 if (!have_soc && have_voltage) {
  charger_shared.soc_percent = soc_from_voltage((uint32_t)charger_shared.voltage_mv);
  flags |= LOADER_BATTERY_VALID | LOADER_BATTERY_SOC_ESTIMATED;
 }
 if (plugged && charger_shared.soc_percent < 100) {
  flags |= LOADER_BATTERY_CHARGING;
 }
 charger_shared.flags = flags;
 charger_shared.polls++;
 if (!(flags & LOADER_BATTERY_VALID)) charger_shared.failures++;
 __asm__ volatile ("dmb ish" ::: "memory");
 charger_shared.sequence++;
 clean_dcache((uint64_t)(uintptr_t)&charger_shared,
     (uint64_t)(uintptr_t)&charger_shared + sizeof(charger_shared));
}
static void
charger_start(void)
{
 static const efi_guid charger_ex_guid = QCOM_CHARGER_EX_GUID;
 static const efi_guid qcom_charger_guid = QCOM_CHARGER_GUID;
 efi_status ex_status, qcom_status, init_status = 0;
 if (charger_enabled || (PLAT != &platform_redwood && PLAT != &platform_macan)) {
  return;
 }
 ex_status = BS->locate_protocol(&charger_ex_guid, NULL, (void **)&CHARGER_EX);
 qcom_status = BS->locate_protocol(&qcom_charger_guid, NULL, (void **)&QCOM_CHARGER);
 if (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL) {
  init_status = QCOM_CHARGER->initialize(1);
 }
 charger_enabled = (!EFI_ERROR(ex_status) && CHARGER_EX != NULL) ||
     (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL);
 print("battery: ChargerEx=%x QcomCharger=%x init=%x shared=%x\n", ex_status, qcom_status, init_status,
     (uint64_t)(uintptr_t)&charger_shared);
 if (CHARGER_EX != NULL) {
  uint8_t present = 0, plugged = 0;
  uint32_t voltage = 0;
  efi_status a = CHARGER_EX->get_battery_presence(&present), b = CHARGER_EX->get_charger_presence(&plugged),
      c = CHARGER_EX->get_battery_voltage(&voltage);
  print("battery: ChargerEx revision %x: battery %x (%d), charger %x (%d), voltage %x (%d mV)\n",
      CHARGER_EX->revision, a, (uint64_t)present, b, (uint64_t)plugged, c, (uint64_t)voltage);
 }
 charger_poll();
 print("battery: flags=%x SOC=%d%% V=%d mV I=%d mA T=%d C action=%d\n", (uint64_t)charger_shared.flags,
     (uint64_t)(int64_t)charger_shared.soc_percent, (uint64_t)(int64_t)charger_shared.voltage_mv,
     (uint64_t)(int64_t)charger_shared.current_ma, (uint64_t)(int64_t)charger_shared.temperature_c,
     (uint64_t)charger_shared.charger_action);
}
static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
 const char *log;
 uint32_t len, from;
 if (live == NULL) {
  print("%d s\n", seconds);
  return;
 }
 log = (const char *)(live->log + slide);
 flush_dcache(live->log_len + slide, live->log_len + slide + 4);
 len = *(volatile uint32_t *)(live->log_len + slide);
 if (len > live->log_head + live->log_ring) {
  len = 0;
 }
 from = len > 240 ? len - 240 : 0;
 flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
 print("%d s: the kernel has logged %d bytes, heartbeat %d; the last of it:\n", seconds, (uint64_t)len, live->heartbeat);
 for (uint32_t i = from; i < len; i++) {
  put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
 }
 print("\n");
}
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
 int64_t status;
 if (PLAT->psci_hvc) {
  register uint64_t x0 __asm__("x0") = function;
  register uint64_t x1 __asm__("x1") = a1;
  register uint64_t x2 __asm__("x2") = a2;
  register uint64_t x3 __asm__("x3") = a3;
  __asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
  status = (int64_t)x0;
 } else {
  smc_x3 = a3;
  status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
  smc_x3 = 0;
 }
 return status;
}
static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
 uint64_t daif = 0, group1 = 0, started, last_pet, last_battery, last_beat_at, last_beat = 0, noted = 0;
 int64_t status;
 int arrived = 0, watching_only = 0;
 running_live = live;
 running_slide = slide;
 other_cpu_start.entry = entry;
 other_cpu_start.boot_args = boot_args_pa;
 other_cpu_stage = 0;
 other_cpu_done = 0;
 psci_by_hvc = PLAT->psci_hvc;
 clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
 clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
 clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
 clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
 clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
 clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));
 status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start);
 started = last_pet = last_battery = last_beat_at = READ_SYSREG(CNTVCT_EL0);
 print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
 if (PLAT->log_other_cpu) {
  flush_log();
 }
 while (status == 0) {
  if (!watching_only && seconds_since(started) >= noted + 2) {
   noted = seconds_since(started);
   if (PLAT->log_other_cpu) {
    if (!arrived) {
     print("%d s: the core has not arrived\n", noted);
    } else {
     note_kernel_progress(live, slide, noted);
    }
    flush_log();
   }
  }
  if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
   watching_only = 1;
   daif = READ_SYSREG(DAIF);
   __asm__ volatile ("msr daifset, #0xf");
   group1 = READ_SYSREG(S3_0_C12_C12_7);
   __asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" ((uint64_t)0));
  }
  flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
  if (other_cpu_done) {
   flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
   break;
  }
  if (!arrived) {
   flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
   arrived = other_cpu_stage != 0;
   if (!arrived && seconds_since(started) >= START_SECONDS) {
    status = OTHER_CPU_NOT_STARTED;
   }
  }
  if (wdt_armed && seconds_since(last_pet) >= 5 && seconds_since(started) < budget + 60) {
   wdt_pet();
   last_pet = READ_SYSREG(CNTVCT_EL0);
  }
  if (charger_enabled && seconds_since(last_battery) >= 5) {
   charger_poll();
   last_battery = READ_SYSREG(CNTVCT_EL0);
  }
  if (arrived && live != NULL) {
   if (live->heartbeat != last_beat) {
    last_beat = live->heartbeat;
    last_beat_at = READ_SYSREG(CNTVCT_EL0);
   } else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
    log_of_hung_kernel(live, slide);
    status = OTHER_CPU_HUNG;
   }
  }
  usb_poll_command();
  for (volatile int i = 0; i < 20000; i++) {
  }
 }
 running_live = NULL;
 if (watching_only) {
  __asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
  __asm__ volatile ("msr daif, %0" : : "r" (daif));
 }
 return status;
}
#define VIRT_WDT_CONTROL 0x86000005u
#define VIRT_WDT_STATUS 0x86000006u
#define VIRT_WDT_PET 0x86000007u
#define VIRT_WDT_SET_TIME 0x86000008u
#define WDT_BITE_MS 30000u
static const struct platform *PLAT;
static int wdt_armed;
static uint64_t smc_x3;
static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
 register uint64_t x0 __asm__("x0") = id;
 register uint64_t x1 __asm__("x1") = a1;
 register uint64_t x2 __asm__("x2") = a2;
 register uint64_t x3 __asm__("x3") = smc_x3;
 __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
 if (r1 != NULL) {
  *r1 = x1;
 }
 if (r2 != NULL) {
  *r2 = x2;
 }
 return x0;
}
static void
wdt_pet(void)
{
 if (wdt_armed) {
  smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
 }
}
static void
wdt_set_ms(uint64_t ms)
{
 if (ms > 65000) {
  ms = 65000;
 }
 if (wdt_armed) {
  smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
  smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
 }
}
static void
wdt_arm(void)
{
 uint64_t ctl = 0, since = 0;
 if (PLAT != &platform_redwood) {
  return;
 }
 uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
 uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
 uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
 uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
 print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
     r_ctl, r_pet);
 wdt_armed = (r_pet == 0);
}
extern char __ImageBase;
static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
 print("\nUEFI EXCEPTION type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
     ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
 print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
     ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);
 flush_log();
 ST->rt->reset_system(0 , 0, 0, NULL);
 for (;;) {
 }
}
static __attribute__((unused)) void
install_exception_hook(void)
{
 static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
 efi_cpu_arch *cpu;
 if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
  return;
 }
 for (int64_t type = 0; type <= 3; type += 3) {
  cpu->register_interrupt_handler(cpu, type, NULL);
  efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
  if (EFI_ERROR(s)) {
   print("exception hook %d: status %x\n", (uint64_t)type, s);
  }
 }
}
struct payload_info {
 const uint8_t *kernel;
 const uint8_t *ramdisk;
 uint32_t ramdisk_size;
 uint32_t kernel_size;
 const char *cmdline;
 uint64_t checkpoint;
 uint64_t forced_base;
 uint64_t resident_rd;
 int usb_mode;
};
static uint64_t
parse_number(const char *c, unsigned base)
{
 uint64_t v = 0;
 for (; *c; c++) {
  uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
  if (d >= base) {
   break;
  }
  v = v * base + d;
 }
 return v;
}
static int
parse_payload(struct payload_info *pi, int verbose)
{
 struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;
 memset(pi, 0, sizeof(*pi));
 pi->cmdline = "";
 if (!payload_valid(ph, payload_bytes)) {
  print("no payload at %x (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
  return 0;
 }
 const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
 while (p + sizeof(struct payload_entry) <= pend) {
  const struct payload_entry *e = (const struct payload_entry *)p;
  const char *text = (const char *)(p + sizeof(*e));
  if (e->name[0] == 0) {
   break;
  }
  if (verbose) {
   print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
  }
  if (str_eq(e->name, "ramdisk")) {
   pi->ramdisk = p + sizeof(*e);
   pi->ramdisk_size = e->size;
  } else if (str_eq(e->name, "kernel")) {
   pi->kernel = p + sizeof(*e);
            pi->kernel_size = e->size;
  } else if (str_eq(e->name, "cmdline")) {
   pi->cmdline = text;
  } else if (str_eq(e->name, "physbase")) {
   pi->forced_base = parse_number(text, 16);
  } else if (str_eq(e->name, "rdres")) {
   pi->resident_rd = parse_number(text, 16);
  } else if (str_eq(e->name, "checkpoint")) {
   pi->checkpoint = parse_number(text, 10);
  } else if (str_eq(e->name, "usb")) {
   pi->usb_mode = 1;
  }
  p += sizeof(*e) + ((e->size + 15) & ~15u);
 }
 return 1;
}
static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
 uint16_t blocks = (uint16_t)(bytes / 16), sum;
 memset(h, 0, 16);
 h[0] = sig;
 h[2] = (uint8_t)blocks;
 h[3] = (uint8_t)(blocks >> 8);
 memcpy(h + 4, name, str_len(name));
 sum = h[0];
 for (int i = 2; i < 16; i++) {
  sum += h[i];
 }
 while (sum > 0xff) {
  sum = (sum & 0xff) + (sum >> 8);
 }
 h[1] = (uint8_t)sum;
}
static void
build_nvram_image(uint8_t *image)
{
 uint32_t lo = 1, hi = 0, generation = 1;
 memset(image, 0, NVRAM_BANK_SIZE);
 nvram_chrp_header(image, 0x5a, 0x20, "nvram");
 nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
 memcpy(image + 20, &generation, 4);
 for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
  lo = (lo + image[i]) % 65521;
  hi = (hi + lo) % 65521;
 }
 uint32_t adler = (hi << 16) | lo;
 memcpy(image + 16, &adler, 4);
}
static efi_gop *GOP;
static struct {
 uint64_t base;
 uint32_t width, height, stride;
} SCREEN;
static uint64_t win_base, win_size;
static uint64_t resident_rd_size;
static char last_result[64] = "idle";
static void
set_result(const char *what, uint64_t a, uint64_t b)
{
 uint32_t saved = log_len;
 uint32_t start = log_len;
 print("%s %d %x", what, a, b);
 uint32_t n = log_len - start;
 if (n > sizeof(last_result) - 1) {
  n = sizeof(last_result) - 1;
 }
 memcpy(last_result, log_buf + start, n);
 last_result[n] = 0;
 log_len = saved;
}
static void
print_memory_map(void)
{
 static uint8_t map_buf[65536];
 uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
 uint32_t desc_ver;
 if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
  print("GetMemoryMap failed\n");
  return;
 }
 if (desc_size < sizeof(efi_memory_descriptor) || map_size % desc_size) { print("invalid EFI map layout\n"); return; }
 memset(totals, 0, sizeof(totals));
 print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
 for (uint64_t off = 0; off < map_size; off += desc_size) {
  efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
  if (d->type < 16) {
   totals[d->type] += d->pages << 12;
  }
  if (d->pages >= 256) {
   print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
       d->pages >> 8);
  }
 }
 for (int t = 0; t < 16; t++) {
  if (totals[t] != 0) {
   print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
  }
 }
}
#if defined(XNU_PLATFORM_MACAN)
static efi_status
allocate_safe_pages(uint64_t bytes, uint64_t *address)
{
    static uint8_t map[65536];
    uint64_t size=sizeof(map), key, stride;
    uint32_t version;
    efi_status status=BS->get_memory_map(&size,map,&key,&stride,&version);
    if (EFI_ERROR(status)) return status;
    if (stride < sizeof(efi_memory_descriptor) || size % stride) return EFI_ERROR_BIT | 2;
    for (uint64_t off=0; off<size; off+=stride) {
        const efi_memory_descriptor *d=(const void *)(map+off);
        if (d->type != EfiConventionalMemory || d->pages > (UINT64_MAX-d->phys_start)/4096) continue;
        uint64_t base=0, length=0;
        macan_largest_gap(d->phys_start,d->phys_start+d->pages*4096,4096,&base,&length);
        if (length < bytes || !macan_range_allowed(base,bytes)) continue;
        status=BS->allocate_pages(AllocateAddress,EfiLoaderData,bytes>>12,&base);
        if (!EFI_ERROR(status)) { *address=base; return status; }
    }
    return EFI_ERROR_BIT | 9;
}
#endif
static int
choose_window(const struct payload_info *pi)
{
 static uint8_t map_buf[65536];
 uint64_t map_size = sizeof(map_buf), map_key, desc_size;
 uint32_t desc_ver;
 uint64_t best_base = 0, best_size = 0;
 uint64_t pay_lo = PAYLOAD_BASE, pay_hi = PAYLOAD_BASE + PAYLOAD_MAX_SIZE;
 if (win_size != 0) {
  return 1;
 }
 if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
  print("GetMemoryMap failed\n");
  return 0;
 }
 if (desc_size < sizeof(efi_memory_descriptor) || map_size % desc_size) return 0;
 for (uint64_t off = 0; off < map_size; off += desc_size) {
  efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
  if (d->type != EfiConventionalMemory) {
   continue;
  }
  uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
  if (lo < pay_hi && hi > pay_lo) {
   if (pay_lo - lo > hi - pay_hi && pay_lo > lo) {
    hi = pay_lo;
   } else {
    lo = pay_hi;
   }
  }
  lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
  hi &= ~(BASE_ALIGN - 1);
  #if defined(XNU_PLATFORM_MACAN)
        macan_largest_gap(lo,hi,BASE_ALIGN,&best_base,&best_size);
#else
        if (hi > lo && hi-lo > best_size) { best_base=lo; best_size=hi-lo; }
#endif
 }
 print("largest free block %x size %x\n", best_base, best_size);
 if (pi->forced_base != 0) {
  best_base = pi->forced_base;
  best_size = MAX_MEM_SIZE;
 }
 if (best_size > MAX_MEM_SIZE) {
  best_size = MAX_MEM_SIZE;
 }
 uint64_t claim = best_base;
 efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
 print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
 if (EFI_ERROR(s)) {
  return 0;
 }
 win_base = best_base;
 win_size = best_size;
 return 1;
}
static int starts_with(const char *s, const char *prefix);
static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
 static uint8_t map_buf[65536];
 uint64_t map_size, map_key, desc_size;
 uint32_t desc_ver;
 struct payload_info pi;
 if (!parse_payload(&pi, 1) || pi.kernel == NULL) {
  print("no kernel in payload\n");
  return 0;
 }
 if (!have_checkpoint) {
        checkpoint = pi.checkpoint;
    }
#if defined(XNU_PLATFORM_MACAN)
    if (pi.forced_base || pi.resident_rd) {
        print("macan: externally forced RAM addresses are disabled\n");
        return 0;
    }
#ifndef MACAN_KERNEL_EXECUTION
    if (checkpoint != CHECKPOINT_SELFTEST) {
        print("macan: kernel execution is gated until the rebuilt GIC kernel is verified; use selftest\n");
        return 0;
    }
#else
    if (pi.kernel_size != MACAN_KERNEL_SIZE || payload_crc32(pi.kernel, pi.kernel_size) != MACAN_KERNEL_CRC32) {
        print("macan: kernel does not match this rebuilt GIC image\n");
        return 0;
    }
    if (checkpoint != CHECKPOINT_SELFTEST && !(checkpoint >= 1 && checkpoint <= 5) &&
        !(checkpoint >= 10 && checkpoint <= 21) && checkpoint != 0) {
        print("macan: unsupported checkpoint\n");
        return 0;
    }
#endif
    const char *safe = pi.cmdline;
    while (*safe) {
        while (*safe == ' ') safe++;
        const char *end = safe;
        while (*end && *end != ' ') end++;
        if (end == safe) break;
        int numeric_ok = 0;
        const char *keys[] = {"loader_return_after=", "epoch=", "sb_delay=", "fbscale=", "macan_gpu_pause=", "wifi_pause=", "wifi_hold=", "wifi_ready=", "wifi_listen=", "rec_secs=", "rec_start=", "rec_mb=", "torch_max_s="};
        for (unsigned k=0; k<sizeof(keys)/sizeof(keys[0]); k++) {
            if (!starts_with(safe, keys[k])) continue;
            const char *value = safe;
            for (const char *key=keys[k]; *key; key++) value++;
            uint64_t number=0;
            if (value == end || end-value > 10) continue;
            const char *digit=value;
            for (; digit<end && *digit>='0' && *digit<='9'; digit++)
                number=number*10+(uint64_t)(*digit-'0');
            if (digit == end && number <= (k==1 ? 0xffffffffULL : k==3 ? 3 : 3600)) numeric_ok=1;
        }
        const char *digit_keys[] = {"wifi_lx=", "wifi_scan=", "wifi_join=", "wifi_station=", "wifi_loud=", "wifi_irq=",
                                    "wifi_wide=", "wifi_ba=", "wifi_native=", "wifi_rx_single=", "wifi_rx_native=", "wifi_open_test=", "wifi_legacy=", "wifi_scan_joined=", "wifi_no_authorize=", "wifi_reauth=", "wifi_probe_ap=", "wifi_fw_mac=", "wifi=", "user_pac=", "user_arm64e=", "macan_cpufreq=", "macan_gpu_zap=", "macan_gpu_keep=", "haptic=", "torch=", "adsp=", "adsp_vote=", "island="};
        for (unsigned k=0; k<sizeof(digit_keys)/sizeof(digit_keys[0]); k++) {
            unsigned n=0;
            while (digit_keys[k][n]) n++;
            if (end-safe == (long)n+1 && starts_with(safe, digit_keys[k]) && safe[n]>='0' && safe[n]<='9') numeric_ok=1;
        }
        if (end-safe == 26 && starts_with(safe, "wifi_mac=")) {
            int good=1;
            for (int i=0; i<17; i++) {
                char c=safe[9+i];
                if (i%3==2 ? c!=':' : !((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) good=0;
            }
            if (good) numeric_ok=1;
        }
        if (!((end-safe==13 && starts_with(safe,"macan_touch=1")) ||
              (end-safe==25 && starts_with(safe,"launchd_proxyless_spawn=") &&
               (safe[24]=='0' || safe[24]=='1')) ||
              (end-safe==19 && starts_with(safe,"macan_touch_probe=1")) ||
              (end-safe==18 && starts_with(safe,"macan_touch_diag=1")) ||
              (end-safe==11 && starts_with(safe,"macan_gpu=") && safe[10]>='1' && safe[10]<='9') ||
              (end-safe==12 && starts_with(safe,"macan_gpu=1") && safe[11]>='0' && safe[11]<='9') ||
              (end-safe==18 && starts_with(safe,"macan_gpu_return=1")) ||
              (end-safe==9 && starts_with(safe,"battery=1")) ||
              (end-safe==11 && starts_with(safe,"wifi_step=") && safe[10]>='1' && safe[10]<='9') ||
              (end-safe==12 && starts_with(safe,"wifi_step=1") && safe[11]>='0' && safe[11]<='6') ||
              (end-safe==13 && starts_with(safe,"wifi_return=1")) ||
              (end-safe==11 && starts_with(safe,"wifi_smmu=") && safe[10]>='1' && safe[10]<='2') ||
              (end-safe==10 && starts_with(safe,"wifi_cal=1")) || (end-safe==14 && starts_with(safe,"wifi_mission=1")) ||
              (end-safe==11 && starts_with(safe,"wifi_keep=1")) || numeric_ok || (end-safe == 6 && starts_with(safe, "rd=md1")) ||
              (end-safe == 2 && starts_with(safe, "-v")) ||
              (end-safe == 6 && starts_with(safe, "cpus=") && safe[5]>='1' && safe[5]<='8') ||
              (end-safe == 9 && starts_with(safe, "bootcpu=") && safe[8] >= '0' && safe[8] <= '7') ||
              (end-safe == 9 && starts_with(safe, "debug=0x8")))) {
            print("macan: unsupported bring-up command line\n");
            return 0;
        }
        safe = end;
    }
#endif
 const uint8_t *kernel = pi.kernel;
 const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
 if (pi.kernel_size < sizeof(*mh) || mh->magic != MH_MAGIC_64 || mh->cputype != 0x0100000c ||
        mh->sizeofcmds > pi.kernel_size - sizeof(*mh)) {
  print("kernel is not a 64-bit Mach-O\n");
  return 0;
 }
 uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
 const struct load_command *lc = (const struct load_command *)(mh + 1);
    const uint8_t *commands_end = (const uint8_t *)(mh + 1) + mh->sizeofcmds;
 for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
  if ((const uint8_t *)lc > commands_end || (uint64_t)(commands_end - (const uint8_t *)lc) < sizeof(*lc) ||
            lc->cmdsize < sizeof(*lc) || lc->cmdsize > (uint64_t)(commands_end - (const uint8_t *)lc)) {
            print("invalid Mach-O load command bounds\n");
            return 0;
        }
        if (lc->cmd == LC_SEGMENT_64) {
            if (lc->cmdsize < sizeof(struct segment_command_64)) return 0;
            const struct segment_command_64 *sg = (const void *)lc;
            if (sg->filesize > sg->vmsize || sg->fileoff > pi.kernel_size ||
                sg->filesize > pi.kernel_size - sg->fileoff || sg->vmsize > UINT64_MAX - sg->vmaddr) {
                print("invalid Mach-O segment bounds\n");
                return 0;
            }
   if (sg->vmsize == 0) {
    continue;
   }
   if (sg->vmaddr < vm_lo) {
    vm_lo = sg->vmaddr;
   }
   if (sg->vmaddr + sg->vmsize > vm_hi) {
    vm_hi = sg->vmaddr + sg->vmsize;
   }
  } else if (lc->cmd == LC_UNIXTHREAD) {
   if (lc->cmdsize < 16 + 33 * sizeof(uint64_t)) return 0;
   entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
  }
 }
 print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
 if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || vm_hi <= vm_lo ||
        vm_hi - KERNEL_VIRT_BASE > MAX_MEM_SIZE || entry_va < vm_lo || entry_va >= vm_hi) {
  print("unexpected kernel layout\n");
  return 0;
 }
 if (!choose_window(&pi)) {
  return 0;
 }
 uint64_t phys_base = win_base;
 uint64_t slide = phys_base - KERNEL_VIRT_BASE;
 uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
 uint64_t rd_pa = kern_hi;
 uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
 int rd_resident = 0;
 uint64_t rd1_pa = 0, rd1_size = 0;
 if (pi.resident_rd != 0) {
  resident_rd_size = pi.resident_rd;
 }
 if (resident_rd_size != 0) {
  uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
  uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
  if (kern_hi + rd_size > res_pa) {
   print("kernel and ramdisk overlap the resident image\n");
   return 0;
  }
  if (rd_size == 0) {
   rd_pa = res_pa;
   rd_size = res_size;
   rd_resident = 1;
  } else {
   rd1_pa = res_pa;
   rd1_size = res_size;
  }
 }
 uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
 uint64_t dt_max = 0x10000;
 uint64_t ba_pa = dt_pa + dt_max;
 uint64_t top = ba_pa + PAGE_SIZE_MAX;
 if (win_size < top - phys_base + 0x4000000ULL) {
  print("not enough contiguous RAM\n");
  return 0;
 }
 lc = (const struct load_command *)(mh + 1);
 for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
  const struct segment_command_64 *sg = (const void *)lc;
  if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
   continue;
  }
  memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
  memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
 }
 print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
 if (rd_resident) {
  print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
 } else if (rd_size != 0) {
  memset((void *)rd_pa, 0, rd_size);
  memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
  print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
 }
 if (rd1_size != 0) {
  print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
 }
 uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
 int more_cores = 0, battery_requested = 0;
 for (const char *c = pi.cmdline; *c; c++) {
  if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
   kernel_mpidr = platform_mpidr(PLAT, c[8] - '0');
  }
  more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
  battery_requested |= starts_with(c, "battery=1") && (c == pi.cmdline || c[-1] == ' ') &&
      (c[9] == 0 || c[9] == ' ');
 }
 if (battery_requested) {
  charger_start();
 }
 if (checkpoint == CHECKPOINT_SELFTEST) {
        kernel_mpidr = this_mpidr;
    }
#if defined(XNU_PLATFORM_MACAN)
    if (checkpoint != CHECKPOINT_SELFTEST && checkpoint != 0 && kernel_mpidr == this_mpidr) {
        print("macan: native checkpoint requires a separate bootcpu for USB observation\n");
        return 0;
    }
#endif
 const struct kernel_live *kernel_live = NULL;
 uint64_t kernel_budget = 0;
 for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
  if (*q == KERNEL_LIVE_MAGIC) {
   kernel_live = (const struct kernel_live *)q;
   break;
  }
 }
 for (const char *c = pi.cmdline; *c; c++) {
  if (starts_with(c, "loader_return_after=")) {
   for (c += 20; *c >= '0' && *c <= '9'; c++) {
    kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
   }
   break;
  }
 }
 for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
  if (*q == 0x545352444C554E58ULL ) {
   q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
       (uint64_t)(uintptr_t)&loader_return_stub;
   q[2] = checkpoint;
   q[3] = slide;
   print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
   break;
  }
 }
 struct boot_args *ba = (struct boot_args *)ba_pa;
 memset(ba, 0, sizeof(*ba));
 ba->revision = 2;
 ba->version = 2;
 ba->virtBase = KERNEL_VIRT_BASE;
 ba->physBase = phys_base;
 ba->memSize = win_size;
 ba->memSizeActual = win_size;
 ba->topOfKernelData = top;
 if (SCREEN.base != 0) {
  int verbose = 0;
  for (const char *c = pi.cmdline; *c; c++) {
   verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
  }
  ba->video.v_baseAddr = SCREEN.base;
  ba->video.v_display = verbose ? 0 : 1;
  ba->video.v_rowBytes = SCREEN.stride;
  ba->video.v_width = SCREEN.width;
  ba->video.v_height = SCREEN.height;
  ba->video.v_depth = 32;
 }
 int own_serial = 0;
 for (const char *c = pi.cmdline; *c; c++) {
  own_serial |= starts_with(c, "serial=");
 }
 const char *base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " :
     PLAT == &platform_macan ? "debug=0x8 macan=1 " : "debug=0x8 ";
 uint32_t n = 0;
 for (const char *c = base_cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
  ba->commandLine[n++] = *c;
 }
 if (!more_cores) {
  for (const char *c = "cpus=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
   ba->commandLine[n++] = *c;
  }
 }
 int own_root = 0;
 for (const char *c = pi.cmdline; *c; c++) {
  own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
 }
 if (rd_size != 0 && !own_root) {
  for (const char *c = "rd=md0 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
   ba->commandLine[n++] = *c;
  }
 }
 if (wdt_armed) {
  for (const char *c = "hv_wdt_pet=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
   ba->commandLine[n++] = *c;
  }
 }
 if (battery_requested) {
  static const char prefix[] = "battery_shared_pa=0x";
  static const char hex[] = "0123456789abcdef";
  uint64_t address = (uint64_t)(uintptr_t)&charger_shared;
  for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
   ba->commandLine[n++] = *c;
  }
  for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
   ba->commandLine[n++] = hex[(address >> shift) & 0xf];
  }
  if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
 }
 for (const char *c = pi.cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
  ba->commandLine[n++] = *c;
 }
 const struct platform *plat = PLAT;
 struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
 uint64_t cntfrq = READ_SYSREG(CNTFRQ_EL0);
 uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
 uint64_t reg2[2];
 uint8_t seed[64];
 for (unsigned i = 0; i < sizeof(seed); i++) {
  seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
 }
 dt_begin(&w, "device-tree");
 dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
 dt_prop_str(&w, "platform-name", "t8015");
 dt_prop_padded(&w, "serial-number", plat == &platform_macan ? "MACAN00ACE6T" : "RDW00POCOX5P", 32);
 dt_prop_padded(&w, "mlb-serial-number", plat == &platform_macan ? "MACAN00ACE6TMLB00" : "RDW00POCOX5PMLB00", 32);
 dt_prop_padded(&w, "model-number", "MQAC2", 32);
 dt_prop_padded(&w, "region-info", "LL/A", 32);
 dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
 dt_prop_u32(&w, "#address-cells", 2);
 dt_prop_u32(&w, "#size-cells", 2);
 dt_begin(&w, "chosen");
 dt_prop(&w, "random-seed", seed, sizeof(seed));
 dt_prop_u64(&w, "dram-base", phys_base);
 dt_prop_u64(&w, "dram-size", win_size);
 dt_prop_u32(&w, "debug-enabled", 1);
 dt_prop_u32(&w, "display-scale", 3);
 dt_prop_u32(&w, "display-rotation", 0);
 dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
 uint64_t epoch = firmware_epoch();
 if (epoch != 0) {
  dt_prop_u64(&w, "boot-epoch", epoch);
 }
 static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
 static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
 static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
 static const uint8_t no_behavior[16];
 dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
 dt_prop_u32(&w, "chip-id", 0x8015);
 dt_prop_u32(&w, "board-id", 0x06);
 dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
 dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
 dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
 dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
 dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
 dt_prop_u32(&w, "certificate-production-status", 1);
 dt_prop_u32(&w, "certificate-security-mode", 1);
 dt_prop_u32(&w, "effective-production-status-ap", 1);
 dt_prop_u32(&w, "effective-security-mode-ap", 1);
 dt_prop_u32(&w, "secure-boot", 1);
 dt_prop_u32(&w, "production-cert", 1);
 dt_prop_u32(&w, "development-cert", 0);
 dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
 static uint8_t nvram_image[NVRAM_BANK_SIZE];
 build_nvram_image(nvram_image);
 dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
 dt_prop_u32(&w, "nvram-bank-count", 1);
 dt_prop_u32(&w, "nvram-current-bank", 0);
 dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
 dt_begin(&w, "memory-map");
 if (rd_size != 0 || rd1_size != 0) {
  uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
  dt_prop(&w, "RAMDisk", first, sizeof(first));
 }
 if (rd_size != 0 && rd1_size != 0) {
  uint64_t second[2] = { rd_pa, rd_size };
  dt_prop(&w, "RAMDisk1", second, sizeof(second));
 }
 dt_end(&w);
 if (more_cores && plat->timeout_scale != 0) {
  dt_begin(&w, "machine-timeouts");
  dt_prop_u32(&w, "global-scale", plat->timeout_scale);
  dt_end(&w);
 }
 dt_end(&w);
 dt_begin(&w, "defaults");
 dt_prop_u32(&w, "kern.vm_compressor", 2);
 dt_end(&w);
 dt_begin(&w, "product");
 dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
 dt_end(&w);
 dt_begin(&w, "options");
 dt_end(&w);
 uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
 memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
 dt_begin(&w, "pram");
 dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
 dt_end(&w);
 uint64_t cpu_mpidr[8] = { kernel_mpidr };
 int cpu_count = 1, boot_core = platform_core_index(plat, kernel_mpidr);
 if (more_cores) {
  for (int distance = 1; distance < plat->cores; distance++) {
   for (int side = 0; side < 2; side++) {
    int core = side == 0 ? boot_core + distance : boot_core - distance;
    uint64_t mpidr = platform_mpidr(plat, core);
    int64_t state;
    if (core < 0 || core >= plat->cores || mpidr == this_mpidr) {
     continue;
    }
    state = psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
    if (state == 1 || state == PSCI_NOT_SUPPORTED) {
     cpu_mpidr[cpu_count++] = mpidr;
    } else if (state >= 0) {
     print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
    }
   }
  }
  print("cores for the kernel, in the order it takes them:");
  for (int i = 0; i < cpu_count; i++) {
   print(" %x", cpu_mpidr[i]);
  }
  print("\n");
 }
 dt_begin(&w, "cpus");
 for (int i = 0; i < cpu_count; i++) {
  char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
  dt_begin(&w, name);
  dt_prop_str(&w, "device_type", "cpu");
  dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
  dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
  dt_prop_u32(&w, "cpu-id", (uint32_t)i);
  dt_prop_u64(&w, "timebase-frequency", cntfrq);
  dt_prop_u64(&w, "fixed-frequency", cntfrq);
  dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
  dt_prop_u64(&w, "bus-frequency", 100000000ULL);
  dt_prop_u64(&w, "memory-frequency", 100000000ULL);
  dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
  dt_end(&w);
 }
 dt_end(&w);
 dt_begin(&w, "arm-io");
 dt_prop_str(&w, "device_type", plat->name);
 dt_prop(&w, "ranges", ranges, sizeof(ranges));
 dt_prop_u32(&w, "#address-cells", 2);
 dt_prop_u32(&w, "#size-cells", 2);
 dt_begin(&w, "gic");
 dt_prop_str(&w, "device_type", "interrupt-controller");
 dt_prop_str(&w, "compatible", "arm,gic-v3");
 uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base, plat->gic_redist_size };
 dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
    dt_prop_u64(&w, "redistributor-stride", plat->gic_redist_stride);
 dt_end(&w);
 if (plat->uart != 0) {
  dt_begin(&w, "uart0");
  dt_prop_str(&w, "device_type", "uart");
  dt_prop_str(&w, "compatible", "arm,pl011");
  reg2[0] = plat->uart - plat->io_base;
  reg2[1] = 0x1000;
  dt_prop(&w, "reg", reg2, sizeof(reg2));
  dt_prop_u32(&w, "boot-console", 1);
  dt_end(&w);
 }
 dt_end(&w);
 dt_end(&w);
 uint32_t dt_len = (uint32_t)(w.cur - w.base);
 ba->deviceTreeP = dt_pa - slide;
 ba->deviceTreeLength = dt_len;
 print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);
 print("cmdline: %s\n", ba->commandLine);
 if (kernel_mpidr != this_mpidr) {
  print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
 }
 print("jumping to %x\n", entry_va + slide);
 flush_log();
 wdt_set_ms(60000);
 if (loader_save_ctx() == 0) {
  map_size = sizeof(map_buf);
  if (!EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
   for (uint64_t off = 0; off < map_size; off += desc_size) {
    efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
    if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
     continue;
    }
    if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
     clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
    }
   }
  }
  uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
  clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
  flush_dcache(phys_base, top);
  if (checkpoint == CHECKPOINT_SELFTEST) {
   jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
  }
  if (kernel_mpidr != this_mpidr) {
   int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);
   wdt_set_ms(WDT_BITE_MS);
   if (status == OTHER_CPU_HUNG) {
                kernel_hung = 1;
    print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", kernel_mpidr,
        (uint64_t)SILENCE_SECONDS);
    set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
    write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
    return 1;
   } else if (status != 0) {
    if (status == OTHER_CPU_NOT_STARTED) {
     print("core %x was started but never arrived\n", kernel_mpidr);
    } else {
     print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
    }
    set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
    write_logs(NULL, 0);
    return 1;
   }
  } else {
   jump_to_kernel(entry_va + slide, ba_pa);
  }
 }
 wdt_set_ms(WDT_BITE_MS);
 for (int i = 1; i < cpu_count; i++) {
  int64_t state = psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);
  if (state != 1 && state != PSCI_NOT_SUPPORTED) {
   print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
       (uint64_t)state);
  }
 }
 recording_data = NULL;
 recording_len = 0;
 if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
  const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);
  if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
   recording_data = (const char *)recording[0];
   recording_len = recording[1];
   print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
  }
 }
 const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
 uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
 uint64_t reason = loader_ctx[CTX_REASON];
 if (checkpoint == CHECKPOINT_SELFTEST) {
  print("return-path self test passed\n");
  set_result("selftest-ok", 0, 0);
  klog = NULL;
  klog_len = 0;
 } else {
  print("kernel returned: log at %x, %d bytes\n", (uint64_t)(uintptr_t)klog, klog_len);
  if (reason == 2) {
   print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
   set_result("checkpoint", loader_ctx[CTX_ESR], 0);
  } else if (reason == 1) {
   print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
       loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
   print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
   set_result("early-exception", 0, loader_ctx[CTX_ELR]);
   klog_len = klog_len > (1u << 20) ? 0 : klog_len;
  } else {
   set_result("returned", klog_len, 0);
  }
 }
 write_logs(klog, klog_len);
 return 1;
}
#define USB_EP_OUT 0x01
#define USB_EP_IN 0x81
#define USB_CHUNK (1024 * 1024)
static usb_device_protocol *USB;
static int download_to_ramdisk;
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;
static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0xe0, 0x4e, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0xe0, 0x4e, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
 9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
 9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
 7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
 7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
 9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
 9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
 7, 5, USB_EP_IN, 2, 0x00, 0x04, 0, 6, 0x30, 4, 0, 0, 0,
 7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0, 6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
 5, 0x0f, 42, 0, 3,
 7, 0x10, 0x02, 0x06, 0, 0, 0,
 10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
 20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };
static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}
static int
usb_start(void)
{
 static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
 static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
 static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
 static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
 static usb_device_descriptor_set set;
 void *event;
 if (!EFI_ERROR(BS->create_event_ex(0x200 , 8 , usb_dummy_notify, NULL,
     &init_guid, &event))) {
  BS->signal_event(event);
  BS->close_event(event);
 }
 if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
  print("usb: device protocol not found\n");
  return 0;
 }
 set.device_descriptor = usb_dev_desc;
 set.descriptors = &cfg;
 set.ss_device_descriptor = usb_ss_dev_desc;
 set.ss_descriptors = &ss_cfg;
 set.device_qualifier = usb_qualifier;
 set.bos = usb_bos;
 set.string_count = 5;
 set.strings = strings;
 efi_status s = USB->start_ex(&set);
 if (EFI_ERROR(s)) {
  print("usb: start failed %x\n", s);
  return 0;
 }
 if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
     EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
  print("usb: no transfer buffers\n");
  return 0;
 }
 print("usb: started\n");
 return 1;
}
static int64_t
usb_wait(uint8_t endpoint)
{
 static uint64_t last_pet;
 uint64_t freq = READ_SYSREG(CNTFRQ_EL0), began = READ_SYSREG(CNTVCT_EL0);
 for (;;) {
  uint32_t msg = UsbDeviceEventNoEvent;
  uint64_t size = 0;
  usb_device_event_data data;
  USB->handle_event(&msg, &size, &data);
  if (msg == UsbDeviceEventDeviceStateChange) {
   usb_connected = (data.device_state == UsbDeviceStateConnected);
   return -1;
  }
  if (running_live != NULL && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
   return -1;
  }
  if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
   return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
  }
  if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
   last_pet = READ_SYSREG(CNTVCT_EL0);
   wdt_pet();
  }
 }
}
static int usb_rx_queued;
static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
 if (usb_rx_queued) {
  usb_rx_queued = 0;
 } else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
  return -1;
 }
 return usb_wait(USB_EP_OUT);
}
static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
 if (data != usb_tx_buf) {
  memcpy(usb_tx_buf, data, len);
 }
 if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
  return -1;
 }
 return usb_wait(USB_EP_IN);
}
static void
fb_reply(const char *code, const char *text)
{
 uint32_t n = 0;
 for (; *code; code++) {
  usb_tx_buf[n++] = (uint8_t)*code;
 }
 for (; *text && n < 63; text++) {
  usb_tx_buf[n++] = (uint8_t)*text;
 }
 usb_tx(usb_tx_buf, n);
}
static void
fb_reply_data(uint64_t len)
{
 char msg[13] = "DATA00000000";
 for (int i = 0; i < 8; i++) {
  msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
 }
 usb_tx((const uint8_t *)msg, 12);
}
static int
starts_with(const char *s, const char *prefix)
{
 while (*prefix) {
  if (*s++ != *prefix++) {
   return 0;
  }
 }
 return 1;
}
static void
fb_command(char *cmd)
{
 int busy = running_live != NULL || kernel_hung;
 {
  if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
      starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
      starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap") || starts_with(cmd, "oem pac-probe"))) {
   fb_reply("FAIL", "a kernel is running (oem stop ends it)");
  } else if (starts_with(cmd, "oem stop")) {
   if (running_live != NULL) {
    ((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
   }
   fb_reply("OKAY", "");
  } else if (starts_with(cmd, "getvar:")) {
   const char *var = cmd + 7;
   if (str_eq(var, "product")) {
    fb_reply("OKAY", "xnuloader");
   } else if (str_eq(var, "version")) {
    fb_reply("OKAY", "0.5");
   } else if (str_eq(var, "max-download-size")) {
    fb_reply("OKAY", download_to_ramdisk ? "0xffffffff" : "0x03f00000");
   } else if (str_eq(var, "result")) {
    fb_reply("OKAY", last_result);
   } else if (str_eq(var, "screen") && SCREEN.base != 0) {
    uint32_t mark = log_len;
    print("%d %d %d", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, (uint64_t)SCREEN.stride);
    log_buf[log_len] = 0;
    fb_reply("OKAY", log_buf + mark);
    log_len = mark;
   } else {
    fb_reply("OKAY", "");
   }
  } else if (starts_with(cmd, "oem ramdisk")) {
#if defined(XNU_PLATFORM_MACAN) && !defined(MACAN_KERNEL_EXECUTION)
            fb_reply("FAIL", "resident root disk disabled during handoff selftest");
            return;
#endif
   struct payload_info pi;
   if (starts_with(cmd, "oem ramdisk none")) {
    resident_rd_size = 0;
    fb_reply("OKAY", "");
    return;
   }
   if (!parse_payload(&pi, 0)) {
                fb_reply("FAIL", "invalid payload");
                return;
            }
#if defined(XNU_PLATFORM_MACAN)
            if (pi.forced_base || pi.resident_rd) {
                fb_reply("FAIL", "forced RAM addresses disabled");
                return;
            }
#endif
   log_len = 0;
   if (!choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
    fb_reply("FAIL", "no memory window");
    return;
   }
   download_to_ramdisk = 1;
   fb_reply("OKAY", "");
  } else if (starts_with(cmd, "download:")) {
   uint64_t total = parse_number(cmd + 9, 16), got = 0;
   uint64_t limit = PAYLOAD_MAX_SIZE - 0x100000;
   uint8_t *dest = (uint8_t *)PAYLOAD_BASE;
   int to_ramdisk = download_to_ramdisk;
   download_to_ramdisk = 0;
   if (to_ramdisk) {
    limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
    dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
    resident_rd_size = 0;
   }
   if (total == 0 || total > limit) {
    fb_reply("FAIL", "bad size");
    return;
   }
   if (!to_ramdisk) payload_bytes = 0;
            fb_reply_data(total);
   while (got < total) {
    uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
    int64_t r = usb_rx(usb_rx_buf, want);
    if (r <= 0 || (uint64_t)r > want) {
     break;
    }
    memcpy(dest + got, usb_rx_buf, (size_t)r);
    got += (uint64_t)r;
   }
   if (got == total) {
                if (!to_ramdisk && !payload_valid(dest, total)) {
                    payload_bytes = 0;
                    fb_reply("FAIL", "invalid XNUP container");
                    return;
                }
                if (!to_ramdisk) payload_bytes = total;
    if (to_ramdisk) {
     resident_rd_size = total;
    }
    fb_reply("OKAY", "");
   } else {
    fb_reply("FAIL", "short download");
   }
  } else if (starts_with(cmd, "upload")) {
   uint64_t sent = 0;
   fb_reply_data(staged_len);
   while (sent < staged_len) {
    uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
    if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
     break;
    }
    sent += chunk;
   }
   fb_reply("OKAY", "");
  } else if (starts_with(cmd, "oem screenshot")) {
   if (SCREEN.base == 0) {
    fb_reply("FAIL", "no display");
    return;
   }
   staged_data = (const char *)(uintptr_t)SCREEN.base;
   staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
   fb_reply("OKAY", "");
  } else if (starts_with(cmd, "oem recording")) {
   if (recording_data == NULL) {
    fb_reply("FAIL", "no recording");
    return;
   }
   staged_data = recording_data;
   staged_len = recording_len;
   fb_reply("OKAY", "");
  } else if (starts_with(cmd, "oem clearscreen")) {
   if (SCREEN.base != 0) {
    memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
   }
   fb_reply("OKAY", "");
  } else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
   int selftest = starts_with(cmd, "oem selftest");
   const char *arg = cmd + 7;
   while (*arg == ' ') {
    arg++;
   }
   int have_cp = selftest || (*arg >= '0' && *arg <= '9');
   uint64_t cp = selftest ? CHECKPOINT_SELFTEST : parse_number(arg, 10);
   set_result("running", cp, 0);
   fb_reply("OKAY", "");
   log_len = 0;
   print("== XNULOADER run (usb) ==\n");
   if (!run_kernel(have_cp, cp)) {
    set_result("not-started", 0, 0);
    compose_logs(NULL, 0);
   }
#if defined(XNU_PLATFORM_MACAN)
            if (!busy && !kernel_hung) hackios_ready_screen();
#endif
#if defined(XNU_PLATFORM_MACAN)
        } else if (str_eq(cmd, "oem pac-probe")) {
            uint64_t isar1 = READ_SYSREG(ID_AA64ISAR1_EL1);
            uint64_t isar2 = READ_SYSREG(ID_AA64ISAR2_EL1);
            uint64_t address = ((isar1 >> 4) & 255) | ((isar2 >> 12) & 15);
            uint64_t generic = ((isar1 >> 24) & 255) | ((isar2 >> 8) & 15);
            print("PAC probe EL=%d ISAR1=%x ISAR2=%x\n", READ_SYSREG(CurrentEL) >> 2, isar1, isar2);
            if (READ_SYSREG(CurrentEL) >> 2 != 1 || !address || !generic) {
                print("PAC probe skipped: requires EL1 address and generic authentication\n");
                fb_reply("FAIL", "PAC capability unavailable");
            } else {
                struct pac_probe_result result = {0};
                pac_probe_el1(&result);
                int pass = result.restore_difference == 0 && result.sctlr == result.restored_sctlr;
                const char *names[] = {"IA", "IB", "DA", "DB"};
                for (unsigned i = 0; i < 4; i++) {
                    int ok = result.signed_pointer[i] != 0x1234 && result.authenticated[i] == 0x1234;
                    print("PAC %s roundtrip=%d\n", names[i], (uint64_t)ok);
                    pass &= ok;
                }
                print("PAC generic-nonzero=%d keys-restored=%d sctlr-restored=%d\n",
                      (uint64_t)(result.generic_code != 0), (uint64_t)(result.restore_difference == 0),
                      (uint64_t)(result.sctlr == result.restored_sctlr));
                pass &= result.generic_code != 0;
                volatile uint64_t *wipe = (volatile uint64_t *)&result;
                for (unsigned i = 0; i < sizeof(result) / sizeof(uint64_t); i++) wipe[i] = 0;
                fb_reply(pass ? "OKAY" : "FAIL", pass ? "EL1 PAC roundtrips passed; state restored" : "PAC verification failed");
            }
#endif
  } else if (starts_with(cmd, "oem hang")) {
   fb_reply("OKAY", "watchdog test: not petting any more");
   for (;;) {
   }
  } else if (starts_with(cmd, "oem memmap")) {
   print_memory_map();
   fb_reply("OKAY", "");
  } else if (starts_with(cmd, "oem log")) {
   if (busy) {
    compose_live_log(running_live, running_slide);
   } else {
    compose_logs(NULL, 0);
   }
   fb_reply("OKAY", "");
  } else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
   fb_reply("OKAY", "");
   BS->stall(200000);
   ST->rt->reset_system(0 , 0, 0, NULL);
  } else {
   fb_reply("FAIL", "unknown command");
  }
 }
}
static void
usb_poll_command(void)
{
 uint32_t msg = UsbDeviceEventNoEvent;
 uint64_t size = 0, n;
 usb_device_event_data data;
 char cmd[128];
 if (USB == NULL) {
  return;
 }
 if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
  usb_rx_queued = 1;
 }
 USB->handle_event(&msg, &size, &data);
 if (msg == UsbDeviceEventDeviceStateChange) {
  usb_connected = (data.device_state == UsbDeviceStateConnected);
  usb_rx_queued = 0;
  return;
 }
 if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
  return;
 }
 usb_rx_queued = 0;
 if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
  return;
 }
 n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
 memcpy(cmd, usb_rx_buf, (size_t)n);
 cmd[n] = 0;
 fb_command(cmd);
}
static void
usb_command_loop(void)
{
 screen_quiet = 1;
 for (;;) {
  char cmd[128];
  int64_t n;
  if (!usb_connected) {
   usb_wait(0xff);
   continue;
  }
  n = usb_rx(usb_rx_buf, 511);
  if (n <= 0) {
   continue;
  }
  if (n > (int64_t)sizeof(cmd) - 1) {
   n = sizeof(cmd) - 1;
  }
  memcpy(cmd, usb_rx_buf, (size_t)n);
  cmd[n] = 0;
#if defined(XNU_PLATFORM_MACAN)
        if (str_eq(cmd, "oem menu")) {
            if (kernel_hung) { fb_reply("FAIL", "kernel still owns RAM; cold reset required"); continue; }
            fb_reply("OKAY", "");
            efi_status stop_status = USB->stop();
            if (EFI_ERROR(stop_status)) {
                print("USB stop failed (%x); adapter remains resident\n", stop_status);
                continue;
            }
            USB->free_transfer_buffer(usb_rx_buf);
            USB->free_transfer_buffer(usb_tx_buf);
            usb_rx_buf = usb_tx_buf = NULL;
            screen_quiet = 0;
            return;
        }
#endif
  fb_command(cmd);
 }
}
#define PHONE_WIDTH 1125
#define PHONE_HEIGHT 2436
#define FW_CFG_BASE 0x09020000ULL
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE 0x10
static uint32_t
be32(uint32_t v)
{
 return __builtin_bswap32(v);
}
static void
fw_cfg_read(void *buf, uint32_t len)
{
 for (uint32_t i = 0; i < len; i++) {
  ((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
 }
}
static __attribute__((unused)) void
ramfb_phone_screen(void)
{
 static struct {
  uint64_t addr;
  uint32_t fourcc, flags, width, height, stride;
 } __attribute__((packed)) cfg;
 static struct {
  uint32_t control, length;
  uint64_t address;
 } dma;
 struct {
  uint32_t size;
  uint16_t select, reserved;
  char name[56];
 } file;
 uint32_t count, select = 0;
 uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * PHONE_HEIGHT;
 *(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
 fw_cfg_read(&count, sizeof(count));
 count = be32(count);
 for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
  fw_cfg_read(&file, sizeof(file));
  if (str_eq(file.name, "etc/ramfb")) {
   select = __builtin_bswap16(file.select);
  }
 }
 if (select == 0 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (bytes + 0xFFF) >> 12, &fb))) {
  return;
 }
 memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
 cfg.addr = __builtin_bswap64(fb);
 cfg.fourcc = be32(0x34325258);
 cfg.flags = 0;
 cfg.width = be32(PHONE_WIDTH);
 cfg.height = be32(PHONE_HEIGHT);
 cfg.stride = be32(PHONE_WIDTH * 4);
 dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
 dma.length = be32(sizeof(cfg));
 dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
 *(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
 if (dma.control != 0) {
  print("ramfb: control %x after the request\n", (uint64_t)be32(dma.control));
  return;
 }
 SCREEN.base = fb;
 SCREEN.width = PHONE_WIDTH;
 SCREEN.height = PHONE_HEIGHT;
 SCREEN.stride = PHONE_WIDTH * 4;
}
#if defined(XNU_PLATFORM_MACAN)
static void hackios_ready_screen(void)
{
    if (!SCREEN.base || SCREEN.width != 1272 || SCREEN.height != 2800) return;
    HxCanvas canvas = {(unsigned int *)(uintptr_t)SCREEN.base,
        (int)SCREEN.width, (int)SCREEN.height, (int)(SCREEN.stride / 4)};
    hx_render(&canvas, 0, 1);
    clean_dcache(SCREEN.base, SCREEN.base + (uint64_t)SCREEN.stride * SCREEN.height);
}
#endif
efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
 static const efi_guid gop_guid = EFI_GOP_GUID;
 struct payload_info pi;
 ST = st;
 BS = st->bs;
#if defined(XNU_PLATFORM_MACAN)
    screen_quiet = 1;
#endif
 BS->set_watchdog_timer(0, 0, 0, NULL);
 print("== XNULOADER v3 ==\n");
 print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
     READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0), (uint64_t)(uintptr_t)&__ImageBase);
 #if defined(XNU_PLATFORM_MACAN)
    PLAT = &platform_macan;
#else
    PLAT = &platform_qemu_virt;
    payload_bytes = PAYLOAD_MAX_SIZE;
#endif
 if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
     GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
  GOP = NULL;
 } else {
  SCREEN.base = GOP->mode->fb_base;
  SCREEN.width = GOP->mode->info->width;
  SCREEN.height = GOP->mode->info->height;
  SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
  #if !defined(XNU_PLATFORM_MACAN)
        if (SCREEN.base == 0xE1000000ULL) {
   PLAT = &platform_redwood;
  } else {
   ramfb_phone_screen();
  }
#endif
  print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
 }
 print("platform %s\n", PLAT->name);
#if defined(XNU_PLATFORM_MACAN)
    if (READ_SYSREG(CurrentEL) >> 2 != 1 || SCREEN.base != 0xFC800000ULL ||
        SCREEN.width != 1272 || SCREEN.height != 2800 || SCREEN.stride != 1272 * 4) {
        print("macan: CPU/GOP does not match verified PLR110; stopped\n");
        return EFI_ERROR_BIT | 3;
    }
    efi_status status = allocate_safe_pages(PAYLOAD_MAX_SIZE, &payload_base);
    if (EFI_ERROR(status)) return status;
    status = allocate_safe_pages(PANIC_LOG_SIZE, &panic_log_base);
    if (EFI_ERROR(status)) {
        BS->free_pages(payload_base, PAYLOAD_MAX_SIZE >> 12);
        payload_base = 0;
        return status;
    }
    memset((void *)payload_base, 0, 32);
    struct payload_header *initial = (void *)payload_base;
    initial->magic = PAYLOAD_MAGIC;
    initial->size = sizeof(struct payload_entry);
    payload_bytes = 32;
    print("macan: owned payload=%x/%x panic=%x/%x\n", payload_base,
          (uint64_t)PAYLOAD_MAX_SIZE, panic_log_base, (uint64_t)PANIC_LOG_SIZE);
#else
    install_exception_hook();
#endif
    wdt_arm();
 int have_payload = parse_payload(&pi, 0);
 #if defined(XNU_PLATFORM_MACAN)
    if (have_payload) pi.usb_mode = 1;
#endif
    int usb_started = have_payload && pi.usb_mode && usb_start();
    if (usb_started) {
  flush_log();
#if defined(XNU_PLATFORM_MACAN)
        hackios_ready_screen();
#endif
  usb_command_loop();
 }
 #if defined(XNU_PLATFORM_MACAN)
    if (win_size) {
        BS->free_pages(win_base, win_size >> 12);
        win_base = win_size = 0;
    }
    BS->free_pages(panic_log_base, PANIC_LOG_SIZE >> 12);
    BS->free_pages(payload_base, PAYLOAD_MAX_SIZE >> 12);
    panic_log_base = payload_base = payload_bytes = 0;
    return usb_started ? 0 : EFI_ERROR_BIT | 3;
#endif
 if (have_payload) {
  run_kernel(0, 0);
 }
 reboot();
 return 0;
}
