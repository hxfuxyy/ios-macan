#include <sys/macan_dsp.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <miscfs/packfs/packfs.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/macan_input.h>
#include <sys/macan_qrtr.h>
#include <sys/macan_sta.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
static LCK_GRP_DECLARE(wifi_lock_group, "redwood-wifi");
static uint32_t last_step, pause_ms = 2200, fine, watch_seconds = 3;
static uint32_t step;
static uint32_t new_from;
static char last_write[96] = "nothing written";
extern void loader_log_putc(char c);
static int say(const char *format, ...) __printflike(1, 2);
static LCK_MTX_DECLARE(say_lock, &wifi_lock_group);
static bool leaving;
static bool quiet;
static char kept[64 * 1024];
static uint32_t kept_at;
static bool kept_wrapped;
static void
say_line(const char *line, bool aloud)
{
 if (leaving) {
  for (const char *c = line; *c != 0; c++) {
   loader_log_putc(*c);
  }
  return;
 }
 lck_mtx_lock(&say_lock);
 if (quiet && !aloud) {
  for (const char *c = line; *c != 0; c++) {
   kept[kept_at] = *c;
   if (++kept_at == sizeof(kept)) {
    kept_at = 0;
    kept_wrapped = true;
   }
  }
 } else {
  for (const char *c = line; *c != 0; c++) {
   loader_log_putc(*c);
  }
  printf("%s", line);
 }
 lck_mtx_unlock(&say_lock);
}
static int
say(const char *format, ...)
{
 char line[400];
 va_list list;
 int length;
 va_start(list, format);
 length = vsnprintf(line, sizeof(line), format, list);
 va_end(list);
 say_line(line, false);
 return length;
}
int
redwood_wifi_say(const char *format, ...)
{
 char line[400];
 va_list list;
 va_start(list, format);
 vsnprintf(line, sizeof(line), format, list);
 va_end(list);
 say_line(line, false);
 return 0;
}
int
redwood_wifi_tell(const char *format, ...)
{
 char line[400];
 va_list list;
 va_start(list, format);
 vsnprintf(line, sizeof(line), format, list);
 va_end(list);
 say_line(line, true);
 return 0;
}
#define tell redwood_wifi_tell
static void
say_kept(void)
{
 uint32_t length = kept_wrapped ? sizeof(kept) : kept_at, show = length < 24 * 1024 ? length : 24 * 1024;
 char piece[201];
 quiet = false;
 for (uint32_t i = length - show; i < length; i += sizeof(piece) - 1) {
  uint32_t n = length - i < sizeof(piece) - 1 ? length - i : sizeof(piece) - 1;
  for (uint32_t k = 0; k < n; k++) {
   piece[k] = kept[(kept_wrapped ? kept_at + i + k : i + k) % sizeof(kept)];
  }
  piece[n] = 0;
  say_line(piece, true);
 }
}
#define printf say
static void
sleep_us(uint32_t us)
{
 delay_for_interval(us, NSEC_PER_USEC);
}
static void
sleep_ms(uint32_t ms)
{
 delay_for_interval(ms, NSEC_PER_MSEC);
}
static uint64_t
now_us(void)
{
 uint64_t nanoseconds;
 absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
 return nanoseconds / 1000;
}
static void
settle(void)
{
 __asm__ volatile ("dsb sy");
}
static bool whole;
static volatile bool whole_runs;
uint32_t
redwood_wifi_option(const char *name)
{
 static const struct {
  const char *name;
  uint32_t value;
 } stands_for[] = { { "wifi_lx", 2 }, { "wifi_smmu", 2 }, { "wifi_station", 1 }, { "wifi_keep", 1 }, { "wifi_irq", 1 },
      { "wifi_ba", 1 }, { "wifi_wide", 1 }, { "wifi_fw_mac", 1 } };
 uint32_t value = 0;
 if (strcmp(name, "wifi_quiet") == 0) {
  return quiet;
 }
 if (PE_parse_boot_argn(name, &value, sizeof(value))) {
  return value;
 }
 for (unsigned i = 0; whole && i < sizeof(stands_for) / sizeof(stands_for[0]); i++) {
  if (strcmp(name, stands_for[i].name) == 0) {
   return stands_for[i].value;
  }
 }
 return 0;
}
static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
 uint32_t value;
 __asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
 return value;
}
static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
 __asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}
struct block {
 const char *name;
 uint32_t phys, size;
 vm_offset_t base;
};
static void
need(struct block *b)
{
 if (b->base == 0) {
  b->base = ml_io_map(b->phys, b->size);
 }
}
static void
hold(bool single_access)
{
 if (step >= new_from && pause_ms && (!single_access || fine)) {
  sleep_ms(pause_ms);
 }
}
static void
announce(const char *what)
{
 printf("wifi: step %u: %s\n", step, what);
 hold(false);
}
static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
 uint32_t value;
 need(b);
 if (step >= new_from && fine) {
  printf("wifi: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
  hold(true);
 }
 value = rd(b->base + offset);
 printf("wifi: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
 return value;
}
static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
 need(b);
 printf("wifi: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
 snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
 hold(true);
 wr(b->base + offset, value);
}
static bool macan, wlan_powered;
extern void macan_wlan_power(int on);
static uint64_t smem_phys = 0x80900000UL;
#define SMEM_PHYS smem_phys
#define SMEM_SIZE 0x00200000u
#define SMEM_US 0
#define SMEM_WPSS 13
#define SMEM_GLOBAL 0xfffe
#define SMEM_VERSION (0x40 + 4 * 7)
#define SMEM_VERSION_PARTITIONS 12
#define SMEM_CANARY 0xa5a5
struct smem_table_entry {
 uint32_t offset, size, flags;
 uint16_t host0, host1;
 uint32_t cacheline, reserved[7];
};
struct smem_table {
 uint8_t magic[4];
 uint32_t version, count, reserved[5];
};
struct smem_partition {
 uint8_t magic[4];
 uint16_t host0, host1;
 uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
 uint16_t canary, item;
 uint32_t size;
 uint16_t padding_data, padding_header;
 uint32_t reserved;
};
struct smem_item {
 uint32_t item, offset, size;
};
#define ITEM_SMP2P_OURS 616
#define ITEM_SMP2P_THEIRS 617
#define ITEM_CRASH_REASON 626
static const volatile uint8_t *smem;
static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
 if (smem == NULL) {
  smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
 }
 if (offset + size > SMEM_SIZE) {
  return false;
 }
 for (uint32_t i = 0; i < size; i++) {
  ((uint8_t *)to)[i] = smem[offset + i];
 }
 return true;
}
static void
smem_poke(uint32_t offset, uint32_t value)
{
 wr((vm_offset_t)smem + offset, value);
}
static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
 struct smem_table table;
 struct smem_table_entry entry;
 struct smem_partition header;
 if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
     table.count > 64) {
  return 0;
 }
 for (uint32_t i = 0; i < table.count; i++) {
  if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
      entry.size == 0) {
   continue;
  }
  if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
   continue;
  }
  if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
      memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
   return 0;
  }
  *cacheline = entry.cacheline;
  *size = entry.size;
  return entry.offset;
 }
 return 0;
}
static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
 struct smem_partition header;
 struct smem_entry entry;
 uint32_t at, step_back;
 unsigned count = 0;
 if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
     header.free_cached > header.size) {
  return 0;
 }
 for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
  if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
   break;
  }
  if (count < max) {
   found[count].item = entry.item;
   found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
   found[count].size = entry.size - entry.padding_data;
  }
  count++;
  at += sizeof(entry) + entry.padding_header + entry.size;
 }
 if (cacheline == 0 || (cacheline & (cacheline - 1))) {
  return count;
 }
 step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
 for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
  if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
   break;
  }
  if (count < max) {
   found[count].item = entry.item;
   found[count].offset = partition + at - entry.size;
   found[count].size = entry.size - entry.padding_data;
  }
  count++;
  if (entry.size + step_back > at) {
   break;
  }
  at -= entry.size + step_back;
 }
 return count;
}
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
 static struct smem_item found[256];
 static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_WPSS }, { SMEM_GLOBAL, SMEM_GLOBAL } };
 for (unsigned h = 0; h < 2; h++) {
  uint32_t cacheline = 0, partition_size = 0;
  uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
  unsigned count = smem_items(partition, cacheline, found, 256);
  for (unsigned i = 0; i < count && i < 256; i++) {
   if (found[i].item == item) {
    *size = found[i].size;
    return found[i].offset;
   }
  }
 }
 return 0;
}
struct smp2p {
 uint32_t magic;
 uint32_t version;
 uint16_t local, remote, total, valid;
 uint32_t flags;
 struct {
  char name[16];
  uint32_t value;
 } entry[16];
};
#define SMP2P_MAGIC 0x504d5324
#define SMP2P_VERSION 1
#define SMP2P_RESTART_ACKS 1u
#define SMP2P_RESTART_DONE (1u << 0)
#define SMP2P_RESTART_ACK (1u << 1)
#define SMP2P_AT_VERSION 4
#define SMP2P_AT_PROCESSORS 8
#define SMP2P_AT_COUNTS 12
#define SMP2P_AT_FLAGS 16
#define SMP2P_AT_NAME(i) (20 + 20 * (i))
#define SMP2P_AT_VALUE(i) (36 + 20 * (i))
#define DSP_FATAL (1u << 0)
#define DSP_READY (1u << 1)
#define DSP_HANDOVER (1u << 2)
#define DSP_STOP_ACK (1u << 3)
#define DSP_SHUTDOWN_ACK (1u << 7)
static bool
dsp_words(struct smp2p *words)
{
 static uint32_t at;
 uint32_t size = 0;
 if (at == 0) {
  at = smem_item(ITEM_SMP2P_THEIRS, &size);
  if (at == 0 || size < sizeof(*words)) {
   at = 0;
   return false;
  }
 }
 return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}
static int
smp2p_entry(const struct smp2p *words, const char *name)
{
 for (unsigned i = 0; i < words->valid && i < 16; i++) {
  if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
   return (int)i;
  }
 }
 return -1;
}
static uint32_t
dsp_state(const struct smp2p *words)
{
 int entry = smp2p_entry(words, "slave-kernel");
 return entry < 0 ? 0 : words->entry[entry].value;
}
static void
show_words(const char *prefix, const struct smp2p *words)
{
 char line[320];
 int at = 0;
 line[0] = 0;
 for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
  at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
 }
 printf("wifi: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
     words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}
static void
show_crash_reason(void)
{
 char text[81];
 uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);
 if (at == 0 || size == 0) {
  printf("wifi: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
  return;
 }
 bzero(text, sizeof(text));
 smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
 if (text[0] == 0) {
  printf("wifi: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
 } else {
  printf("wifi: %u: THE DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
 }
}
static void
show_partition(const char *name, uint16_t one, uint16_t other)
{
 static struct smem_item found[256];
 struct smem_partition header;
 uint32_t cacheline = 0, size = 0, partition = smem_partition(one, other, &cacheline, &size);
 unsigned count;
 char line[320];
 if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
  printf("wifi: %u: there is no partition for %s\n", step, name);
  return;
 }
 count = smem_items(partition, cacheline, found, 256);
 printf("wifi: %u: the partition for %s: at %#lx, %#x bytes, of which %#x free; %u items\n", step, name, SMEM_PHYS + partition,
     size, header.free_cached - header.free_uncached, count);
 for (unsigned i = 0; i < count && i < 256;) {
  int at = 0;
  for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
   at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
  }
  printf("wifi: %u:   items (bytes):%s\n", step, line);
 }
}
#define ITEM_MINIDUMP 602
#define MD_ENABLED 0x454e424cu
#define MD_REGION_VALID 0x56414c49u
#define MD_ENCR_DONE 0x444f4e45u
#define MD_ENCR_NOTREQ 0x00004e52u
struct md_region {
 char name[16];
 uint32_t seq_num, valid;
 uint64_t address, size;
};
_Static_assert(sizeof(struct md_region) == 40, "minidump region ABI size");
_Static_assert(__builtin_offsetof(struct md_region, valid) == 20, "minidump valid ABI offset");
extern char loader_log[];
static struct md_region md_regions[1] __attribute__((aligned(64)));
static void
minidump_log(void)
{
 uint32_t size = 0, at = smem_item(ITEM_MINIDUMP, &size), word[3], ss[8];
 uint64_t regions = kvtophys((vm_offset_t)md_regions);
 if (at == 0 || size < 16 + 32 || !smem_peek(at, word, sizeof(word))) {
  printf("wifi: %u: minidump: SMEM item %u is not there (%u bytes)\n", step, ITEM_MINIDUMP, size);
  return;
 }
 printf("wifi: %u: minidump: table of contents init %#x revision %u enabled %#x (%u bytes)\n", step, word[0], word[1], word[2], size);
 for (unsigned s = 0; s < 4 && 16 + 32 * (s + 1) <= size; s++) {
  smem_peek(at + 16 + 32 * s, ss, sizeof(ss));
  printf("wifi: %u: minidump: subsystem %u: init %#x enabled %#x encryption %#x/%#x, %u regions at %#llx\n", step, s, ss[0],
      ss[1], ss[2], ss[3], ss[4], (unsigned long long)ss[6] | (unsigned long long)ss[7] << 32);
 }
 if (word[0] != 1 || (word[2] != MD_ENABLED && word[2] != 1)) {
  printf("wifi: %u: minidump: not enabled: the log is not put in it\n", step);
  return;
 }
 bzero(md_regions, sizeof(md_regions));
 strlcpy(md_regions[0].name, "KLOG", sizeof(md_regions[0].name));
 md_regions[0].valid = MD_REGION_VALID;
 md_regions[0].address = kvtophys((vm_offset_t)loader_log);
 md_regions[0].size = 1536 * 1024;
 for (vm_offset_t line = (vm_offset_t)md_regions; line < (vm_offset_t)md_regions + sizeof(md_regions); line += 64)
  __asm__ volatile ("dc cvac, %0" : : "r" (line) : "memory");
 __asm__ volatile ("dsb sy" : : : "memory");
 smem_poke(at + 16 + 0, 0);
 smem_poke(at + 16 + 4, MD_ENABLED);
 smem_poke(at + 16 + 8, MD_ENCR_DONE);
 smem_poke(at + 16 + 12, MD_ENCR_NOTREQ);
 smem_poke(at + 16 + 16, 1);
 smem_poke(at + 16 + 24, (uint32_t)regions);
 smem_poke(at + 16 + 28, (uint32_t)(regions >> 32));
 __asm__ volatile ("dsb sy" : : : "memory");
 smem_poke(at + 16 + 0, 1);
 __asm__ volatile ("dsb sy" : : : "memory");
 printf("wifi: %u: minidump: THE KERNEL'S LOG (%#llx, 1.5 MB) IS THE HLOS REGION \"KLOG\" of the crash dump (table at %#llx)\n",
     step, md_regions[0].address, regions);
}
void macan_minidump_klog(void);
void
macan_minidump_klog(void)
{
 uint32_t on = 0;
 if (PE_parse_boot_argn("macan", &on, sizeof(on)) && on && smem == NULL) {
  smem_phys = 0x81d00000UL;
 }
 minidump_log();
}
static bool
step_smem(void)
{
 struct smem_table table;
 struct smem_table_entry entry;
 struct smp2p words;
 uint32_t version = 0, size = 0, at;
 announce("map the shared memory area (0x80900000, 2 MB: memory, not registers) and read how the boot loader laid it out");
 if (!smem_peek(SMEM_VERSION, &version, sizeof(version)) || !smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table))) {
  return false;
 }
 printf("wifi: %u: the boot loader's version word is %#x; the table of partitions begins %02x %02x %02x %02x, version %u, "
     "%u entries\n", step, version, table.magic[0], table.magic[1], table.magic[2], table.magic[3], table.version, table.count);
 if (version >> 16 != SMEM_VERSION_PARTITIONS || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 || table.count > 64) {
  printf("wifi: %u: that is not the layout this knows (version 12, everything in partitions)\n", step);
  return false;
 }
 for (uint32_t i = 0; i < table.count; i++) {
  if (smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry))) {
   printf("wifi: %u: partition %u: processors %#x and %#x, at %#lx, %#x bytes, flags %#x, alignment %u\n", step, i,
       entry.host0, entry.host1, SMEM_PHYS + entry.offset, entry.size, entry.flags, entry.cacheline);
  }
 }
 show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
 show_partition("everybody (the global one)", SMEM_GLOBAL, SMEM_GLOBAL);
 at = smem_item(ITEM_SMP2P_OURS, &size);
 printf("wifi: %u: item %u, SMP2P from us to the DSP: %s\n", step, ITEM_SMP2P_OURS, at ? "there" : "not there");
 show_crash_reason();
 if (macan) {
  minidump_log();
 }
 if (dsp_words(&words)) {
  show_words("item 617, SMP2P from the DSP to us, IS THERE (the DSP has run since the phone was started)", &words);
 } else {
  printf("wifi: %u: item %u, SMP2P from the DSP to us: not there (the DSP has not run since the phone was started)\n", step,
      ITEM_SMP2P_THEIRS);
 }
 return true;
}
static struct block aoss = { "mailbox", 0x0c300000, 0x400 };
#define QMP_MAGIC 0x00
#define QMP_MAGIC_VALUE 0x4d41494c
#define QMP_VERSION 0x04
#define QMP_FEATURES 0x08
#define QMP_THEIR_LINK 0x0c
#define QMP_THEIR_LINK_ACK 0x10
#define QMP_THEIR_CHANNEL 0x14
#define QMP_THEIR_CHANNEL_ACK 0x18
#define QMP_THEIR_BOX_SIZE 0x1c
#define QMP_THEIR_BOX 0x20
#define QMP_OUR_LINK 0x24
#define QMP_OUR_LINK_ACK 0x28
#define QMP_OUR_CHANNEL 0x2c
#define QMP_OUR_CHANNEL_ACK 0x30
#define QMP_OUR_BOX_SIZE 0x34
#define QMP_OUR_BOX 0x38
#define QMP_UP 0x0000ffffu
#define QMP_DOWN 0xffff0000u
#define QMP_MESSAGE 64
static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };
#define IPCC_SEND 0x0c
#define IPCC_RECEIVED 0x10
#define IPCC_ENABLE 0x14
#define IPCC_DISABLE 0x18
#define IPCC_CLEAR 0x1c
#define IPCC_NOBODY 0xffffffffu
#define IPCC_AOP 0u
static uint32_t ipcc_wpss = 24u;
#define IPCC_WPSS ipcc_wpss
#define IPCC_SMP2P 2u
static bool
step_mailbox(void)
{
 uint32_t magic, box, size;
 announce("read the description at the start of the always-on processor's message memory (0xc300000)");
 magic = get(&aoss, "magic", QMP_MAGIC);
 get(&aoss, "version", QMP_VERSION);
 get(&aoss, "features", QMP_FEATURES);
 get(&aoss, "their link", QMP_THEIR_LINK);
 get(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK);
 get(&aoss, "their channel", QMP_THEIR_CHANNEL);
 get(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK);
 get(&aoss, "their mailbox's size", QMP_THEIR_BOX_SIZE);
 get(&aoss, "their mailbox", QMP_THEIR_BOX);
 get(&aoss, "our link", QMP_OUR_LINK);
 get(&aoss, "our link, seen by them", QMP_OUR_LINK_ACK);
 get(&aoss, "our channel", QMP_OUR_CHANNEL);
 get(&aoss, "our channel, seen by them", QMP_OUR_CHANNEL_ACK);
 size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
 box = get(&aoss, "our mailbox", QMP_OUR_BOX);
 if (magic != QMP_MAGIC_VALUE) {
  printf("wifi: %u: that is not the mailbox's description\n", step);
  return false;
 }
 if (size >= QMP_MESSAGE + 4 && box >= 0x3c && box + size <= aoss.size) {
  get(&aoss, "the length of the message in our mailbox", box);
 }
 announce("read who has rung our doorbell (IPCC, 0x408000: the first access to that block)");
 get(&ipcc, "received", IPCC_RECEIVED);
 return true;
}
static uint64_t cmd_db_phys = 0x80860000UL;
#define CMD_DB_PHYS cmd_db_phys
#define CMD_DB_SIZE 0x20000UL
struct cmd_db_slave {
 uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
 uint32_t version;
 uint8_t magic[4];
 struct cmd_db_slave slave[8];
 uint32_t checksum, reserved;
 uint8_t data[];
};
struct cmd_db_entry {
 char id[8];
 uint32_t priority[2];
 uint32_t address;
 uint16_t length, offset;
};
static const struct cmd_db_header *cmd_db;
static bool
cmd_db_there(void)
{
 if (cmd_db == NULL) {
  cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
 }
 return cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c;
}
static const struct cmd_db_entry *
rpmh_resource(const char *name, uint32_t address)
{
 const struct cmd_db_entry *best = NULL;
 char id[8] = { 0 };
 if (!cmd_db_there()) {
  return NULL;
 }
 if (name != NULL) {
  strlcpy(id, name, sizeof(id));
 }
 for (unsigned s = 0; s < 8; s++) {
  const struct cmd_db_slave *slave = &cmd_db->slave[s];
  const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);
  if (slave->id == 0 || slave->count > 512) {
   continue;
  }
  for (unsigned i = 0; i < slave->count; i++) {
   if (name != NULL ? memcmp(entry[i].id, id, sizeof(id)) == 0 :
       entry[i].address <= address && address - entry[i].address < 0x10 &&
       (best == NULL || entry[i].address > best->address)) {
    best = &entry[i];
    if (name != NULL) {
     return best;
    }
   }
  }
 }
 return best;
}
static void
show_rpmh_resource(const char *name, const char *what)
{
 const struct cmd_db_entry *entry = rpmh_resource(name, 0);
 const uint8_t *data;
 char levels[160];
 int at = 0;
 if (entry == NULL) {
  printf("wifi: %u: RPMh has no \"%s\" (%s)\n", step, name, what);
  return;
 }
 levels[0] = 0;
 if (strlen(name) > 4 && strcmp(name + strlen(name) - 4, ".lvl") == 0) {
  for (unsigned s = 0; s < 8; s++) {
   const struct cmd_db_slave *slave = &cmd_db->slave[s];
   const struct cmd_db_entry *first = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);
   if (entry < first || entry >= first + slave->count ||
       (unsigned)slave->data_offset + entry->offset + entry->length > CMD_DB_SIZE - 0x1000) {
    continue;
   }
   data = cmd_db->data + slave->data_offset + entry->offset;
   at = snprintf(levels, sizeof(levels), "; levels:");
   for (unsigned l = 0; l < entry->length / 2u && l < 16 && at < (int)sizeof(levels) - 8; l++) {
    unsigned level = data[2 * l] | data[2 * l + 1] << 8;
    if (l > 0 && level == 0) {
     break;
    }
    at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level);
   }
  }
 }
 printf("wifi: %u: RPMh has \"%s\" at %#x (%s)%s\n", step, name, entry->address, what, levels);
}
static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_ID 0x000
#define RSC_CONFIG 0x00c
#define RSC_TCS(n) (0xd00 + 672 * (n))
#define TCS_CONTROL 0x14
#define TCS_STATUS 0x18
#define TCS_ENABLED 0x1c
#define TCS_COMMAND(c) (0x30 + 20 * (c))
static bool
step_rpmh(void)
{
 if (macan) {
  printf("wifi: %u: (not on macan: its RPMh, PMICs and pins are laid out otherwise; only a survey)\n", step);
  return true;
 }
 static const char *const kinds[] = { "at once", "at once", "asleep", "asleep", "asleep", "awake", "awake", "awake" };
 uint32_t id, config, count, commands;
 announce("look up the supplies involved in the boot loader's list of RPMh resources (memory at 0x80860000)");
 if (!cmd_db_there()) {
  printf("wifi: %u: the list is not there\n", step);
  return false;
 }
 show_rpmh_resource("cx.lvl", "the DSP's logic; dt: vdd_cx of qcom,wpss@8a00000, asked 384 while it boots");
 show_rpmh_resource("mx.lvl", "its memories; dt: vdd_mx, the same");
 show_rpmh_resource("xo.lvl", "the crystal oscillator; dt: its clock \"xo\"");
 show_rpmh_resource("smpb7", "PM7325 S7: the radio's vdd-cx-mx");
 show_rpmh_resource("smpb1", "PM7325 S1: the radio's vdd-1.8-xo");
 show_rpmh_resource("smpb8", "PM7325 S8: the radio's vdd-1.3-rfa");
 announce("read the application processor's RPMh sequencer (0x18220000: the first access to that block): its version, "
     "and the commands its slots hold");
 id = get(&rsc, "id", RSC_ID);
 config = get(&rsc, "configuration", RSC_CONFIG);
 if (((id >> 16) & 0xff) >= 3) {
  printf("wifi: %u: version %u of the sequencer has its registers elsewhere: not read\n", step, (id >> 16) & 0xff);
  return true;
 }
 count = (config >> 12) & 0x3f;
 commands = (config >> 27) & 0x1f;
 printf("wifi: %u: the operating system's part has %u TCSs of %u commands\n", step, count, commands);
 for (uint32_t t = 0; t < count && t < 8; t++) {
  uint32_t enabled = rd(rsc.base + RSC_TCS(t) + TCS_ENABLED);
  printf("wifi: %u: TCS %u (%s): commands enabled %#x, control %#x, status %#x\n", step, t, kinds[t], enabled,
      rd(rsc.base + RSC_TCS(t) + TCS_CONTROL), rd(rsc.base + RSC_TCS(t) + TCS_STATUS));
  for (uint32_t c = 0; c < commands && c < 16; c++) {
   uint32_t flags = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c));
   uint32_t address = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 4);
   uint32_t value = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 8);
   const struct cmd_db_entry *entry = rpmh_resource(NULL, address);
   if (address == 0 && !(enabled & 1u << c)) {
    continue;
   }
   printf("wifi: %u:   command %u%s: %#x (%.8s + %u) = %#x, flags %#x\n", step, c, enabled & 1u << c ? "" :
       " (not enabled)", address, entry ? entry->id : "?", entry ? address - entry->address : 0, value, flags);
  }
 }
 return true;
}
static struct block arb_core = { "arbiter", 0x0c440000, 0x1100 };
static struct block arb_cnfg = { "arbiter's owners", 0x0c40a000, 0x1000 };
static struct block arb_observer = { "arbiter's observer", 0x0e600000, 0x10000 };
#define ARB_VERSION 0x000
#define ARB_VERSION_5 0x50000000u
#define ARB_VERSION_7 0x70000000u
#define ARB_FEATURES 0x004
#define ARB_CHANNEL(n) (0x900 + 4 * (n))
#define ARB_OWNER(n) (0x700 + 4 * (n))
#define ARB_SYSTEM 0
#define OBSERVER_COMMAND 0x00
#define OBSERVER_STATUS 0x08
#define OBSERVER_DATA0 0x18
#define OBSERVER_DATA1 0x1c
#define STATUS_DONE 0x1
#define STATUS_FAILURE 0x2
#define STATUS_DENIED 0x4
#define STATUS_DROPPED 0x8
#define COMMAND_READ (1u << 27)
static uint32_t arb_channels;
static int
spmi_channel(uint16_t id)
{
 int found = -1;
 for (uint32_t n = 0; n < arb_channels; n++) {
  uint32_t map = rd(arb_core.base + ARB_CHANNEL(n));
  if (map == 0 || ((map >> 8) & 0xfff) != id) {
   continue;
  }
  if (found < 0 || (rd(arb_cnfg.base + ARB_OWNER(n)) & 7) == ARB_SYSTEM) {
   found = (int)n;
  }
 }
 return found;
}
static const char *
spmi_read(int channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
 vm_offset_t at = arb_observer.base + 0x10000UL * ARB_SYSTEM + 0x80UL * (uint32_t)channel;
 uint64_t start = now_us();
 uint32_t status, data[2];
 wr(at + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
 do {
  status = rd(at + OBSERVER_STATUS);
 } while (!(status & STATUS_DONE) && now_us() - start < 2000);
 if (!(status & STATUS_DONE)) {
  return "no answer from the arbiter";
 }
 if (status & STATUS_DENIED) {
  return "the arbiter refuses";
 }
 if (status & STATUS_FAILURE) {
  return "the transfer failed (nobody at that address?)";
 }
 if (status & STATUS_DROPPED) {
  return "the transfer was dropped";
 }
 data[0] = rd(at + OBSERVER_DATA0);
 data[1] = count > 4 ? rd(at + OBSERVER_DATA1) : 0;
 for (uint32_t i = 0; i < count; i++) {
  bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
 }
 return NULL;
}
#define TYPE_BUCK 0x03
#define TYPE_FTS 0x1c
#define TYPE_ULT_BUCK 0x22
static bool
step_rails(void)
{
 if (macan) {
  printf("wifi: %u: (not on macan: its RPMh, PMICs and pins are laid out otherwise; only a survey)\n", step);
  return true;
 }
 uint32_t version;
 announce("map the PMIC arbiter (0xc440000, 0xc40a000, 0xe600000; the buttons' driver reads through it too) and read "
     "its version");
 version = get(&arb_core, "version", ARB_VERSION);
 arb_channels = get(&arb_core, "features", ARB_FEATURES) & 0x7ff;
 need(&arb_cnfg);
 need(&arb_observer);
 if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
  printf("wifi: %u: this knows version 5 of the arbiter only: the supplies are not read\n", step);
  return true;
 }
 if (arb_channels == 0 || arb_channels > 512) {
  arb_channels = 512;
 }
 announce("ask every peripheral of the PM7325 (slave 1) and the PM8350C (slave 2) what it is, and the switching "
     "regulators among them what they make (the first transfers of ours on the SPMI bus: for each a command word "
     "written to the arbiter, nothing to a PMIC)");
 for (uint16_t slave = 1; slave <= 2; slave++) {
  static uint8_t types[256][2];
  static bool there[256];
  char line[320];
  int at = 0;
  bzero(there, sizeof(there));
  for (uint32_t n = 0; n < arb_channels; n++) {
   uint32_t map = rd(arb_core.base + ARB_CHANNEL(n)), peripheral = (map >> 8) & 0xff;
   if (map == 0 || ((map >> 16) & 0xf) != slave || there[peripheral]) {
    continue;
   }
   there[peripheral] = spmi_read((int)n, 0x04, types[peripheral], 2) == NULL;
  }
  line[0] = 0;
  for (unsigned peripheral = 0; peripheral < 256; peripheral++) {
   if (there[peripheral]) {
    at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x:%x/%x", peripheral, types[peripheral][0],
        types[peripheral][1]);
   }
   if (at > 0 && (at > (int)sizeof(line) - 80 || peripheral == 255)) {
    printf("wifi: %u: slave %u (%s), peripheral:type/subtype:%s\n", step, slave, slave == 1 ? "PM7325" : "PM8350C",
        line);
    at = 0;
   }
  }
  for (unsigned peripheral = 0; peripheral < 256; peripheral++) {
   int channel;
   uint8_t status[2], control[8];
   const char *problem;
   if (!there[peripheral] || (types[peripheral][0] != TYPE_BUCK && types[peripheral][0] != TYPE_FTS &&
       types[peripheral][0] != TYPE_ULT_BUCK)) {
    continue;
   }
   channel = spmi_channel((uint16_t)(slave << 8 | peripheral));
   problem = channel < 0 ? "no channel" : spmi_read(channel, 0x08, status, 2);
   if (problem == NULL) {
    problem = spmi_read(channel, 0x40, control, 8);
   }
   if (problem != NULL) {
    printf("wifi: %u: slave %u, the regulator at %#x: %s\n", step, slave, peripheral << 8, problem);
    continue;
   }
   printf("wifi: %u: slave %u, the regulator at %#x (type %#x/%#x): %s, status %#x %#x (%s); asked %u mV, makes %u "
       "mV; mode %#x; registers 0x40..0x47: %02x %02x %02x %02x %02x %02x %02x %02x\n", step, slave, peripheral << 8,
       types[peripheral][0], types[peripheral][1], control[6] & 0x80 ? "ENABLED" : "NOT ENABLED", status[0], status[1],
       status[0] & 0x80 ? "in regulation" : "not in regulation", (unsigned)(control[0] | control[1] << 8),
       (unsigned)(control[2] | control[3] << 8), control[5], control[0], control[1], control[2], control[3], control[4],
       control[5], control[6], control[7]);
  }
 }
 return true;
}
#define FIRST_PIN 84
static struct block tlmm = { "tlmm", 0x0f100000 + 0x1000 * FIRST_PIN, 0x3000 };
#define GPIO_CFG(n) (0x1000 * ((n) - FIRST_PIN))
#define GPIO_IO(n) (0x1000 * ((n) - FIRST_PIN) + 4)
static bool
step_pins(void)
{
 if (macan) {
  printf("wifi: %u: (not on macan: its RPMh, PMICs and pins are laid out otherwise; only a survey)\n", step);
  return true;
 }
 static const struct {
  unsigned pin;
  const char *what;
 } pins[] = { { 84, "the radio's enable or reset" }, { 85, "Bluetooth's reset" }, { 86, "Bluetooth's sw-ctrl" } };
 announce("read how the firmware left the radio's pins (registers the touch screen's driver uses too)");
 need(&tlmm);
 for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
  uint32_t cfg = rd(tlmm.base + GPIO_CFG(pins[i].pin)), io = rd(tlmm.base + GPIO_IO(pins[i].pin));
  printf("wifi: %u: gpio%u (%s): configuration %#x: function %u, pull %u, %u mA, %s; reads %u\n", step, pins[i].pin,
      pins[i].what, cfg, (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & 1u << 9) ? (io & 2 ? "DRIVES HIGH" :
      "DRIVES LOW") : "an input", io & 1);
 }
 return true;
}
#define PAS_WPSS 6
static uint32_t wpss_memory = 0x9ae00000u;
#define WPSS_MEMORY wpss_memory
#define WPSS_MEMORY_SIZE 0x01900000u
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);
static const struct packfs_dnode *
image_entry(const uint8_t *image, const struct packfs_super *super, const struct packfs_dnode *directory, const char *name)
{
 const struct packfs_dnode *nodes = (const struct packfs_dnode *)(image + super->ps_nodes_off);
 size_t length = strlen(name);
 if (directory == NULL || !S_ISDIR(directory->pd_mode) || directory->pd_data + directory->pd_size > super->ps_node_count) {
  return NULL;
 }
 for (uint64_t i = 0; i < directory->pd_size; i++) {
  const struct packfs_dnode *node = &nodes[directory->pd_data + i];
  if (node->pd_name_len == length && (uint64_t)node->pd_name + length <= super->ps_strings_size &&
      memcmp(image + super->ps_strings_off + node->pd_name, name, length) == 0) {
   return node;
  }
 }
 return NULL;
}
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
 for (int disk = 1; disk >= 0; disk--) {
  const struct packfs_super *super;
  const struct packfs_dnode *file;
  vm_offset_t base = 0;
  uint64_t length = 0;
  dev_t dev = mdevlookup(disk);
  if (dev == (dev_t)-1 || mdevgetkernelrange(dev, &base, &length) != 0 || length < PACKFS_SUPER_SIZE) {
   continue;
  }
  super = (const struct packfs_super *)base;
  if (memcmp(super->ps_magic, PACKFS_MAGIC, sizeof(super->ps_magic)) != 0 || super->ps_image_size > length ||
      super->ps_node_count == 0 ||
      super->ps_nodes_off + (uint64_t)super->ps_node_count * sizeof(struct packfs_dnode) > super->ps_image_size ||
      super->ps_strings_off + super->ps_strings_size > super->ps_image_size) {
   continue;
  }
  file = (const struct packfs_dnode *)(base + super->ps_nodes_off);
  file = image_entry((const uint8_t *)base, super, file, "fw");
  file = image_entry((const uint8_t *)base, super, file, name);
  if (file != NULL && S_ISREG(file->pd_mode) && !(file->pd_flags & PACKFS_F_CHUNKED) &&
      file->pd_data + file->pd_size <= super->ps_image_size) {
   *size = file->pd_size;
   return (const uint8_t *)base + file->pd_data;
  }
 }
 printf("wifi: %u: no fw/%s in the small image: put the phone's /vendor/firmware_mnt/image/%s into fw-wifi/ and run "
     "userland/build.sh\n", step, name, name);
 return NULL;
}
#define MDT_HASH(flags) (((flags) >> 24 & 7) == 2)
#define MAX_SEGMENTS 16
struct elf_segment {
 uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};
static struct {
 const uint8_t *header;
 uint64_t header_size;
 struct {
  uint32_t at;
  uint32_t file_size, memory_size;
  const uint8_t *file;
 } segment[MAX_SEGMENTS];
 unsigned segments;
 uint32_t size;
} image;
static bool
step_firmware(void)
{
 struct elf_segment segment;
 const uint8_t *header;
 uint64_t size = 0, files = 0;
 uint32_t count, hashes = 0, lowest = ~0u, highest = 0;
 announce("find the DSP's image in the small image that came with the kernel, and check it against its own header");
 image.segments = 0;
 header = image_file("wpss.mdt", &size);
 if (header == NULL) {
  return false;
 }
 count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
 if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
  printf("wifi: %u: fw/wpss.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
      header[1], header[2], header[3], count, size);
  return false;
 }
 for (uint32_t i = 0; i < count; i++) {
  char name[16];
  memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
  if (MDT_HASH(segment.flags)) {
   hashes = segment.file_size;
  }
  if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
   continue;
  }
  if (segment.paddr < WPSS_MEMORY || segment.file_size > segment.memory_size ||
      (uint64_t)segment.paddr + segment.memory_size > (uint64_t)WPSS_MEMORY + WPSS_MEMORY_SIZE) {
   printf("wifi: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
       segment.memory_size);
   return false;
  }
  image.segment[image.segments].at = segment.paddr - WPSS_MEMORY;
  image.segment[image.segments].file_size = segment.file_size;
  image.segment[image.segments].memory_size = segment.memory_size;
  image.segment[image.segments].file = NULL;
  if (segment.file_size != 0) {
   uint64_t file_size = 0;
   snprintf(name, sizeof(name), "wpss.b%02u", i);
   image.segment[image.segments].file = image_file(name, &file_size);
   if (image.segment[image.segments].file == NULL) {
    return false;
   }
   if (file_size != segment.file_size) {
    printf("wifi: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
    return false;
   }
   files += file_size;
  }
  printf("wifi: %u: segment %u: %#x bytes at %#x, of which %#x from its file\n", step, i, segment.memory_size, segment.paddr,
      segment.file_size);
  lowest = segment.paddr < lowest ? segment.paddr : lowest;
  highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
  image.segments++;
 }
 if (image.segments == 0 || lowest != WPSS_MEMORY || 52 + 32 * count + (uint64_t)hashes != size) {
  printf("wifi: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
      "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
  image.segments = 0;
  return false;
 }
 image.header = header;
 image.header_size = size;
 image.size = ((highest + 0xfff) & ~0xfffu) - WPSS_MEMORY;
 printf("wifi: %u: the image is whole: a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x bytes "
     "of the DSP's %#x from %#x\n", step, size, hashes, image.segments, files, image.size, WPSS_MEMORY_SIZE, WPSS_MEMORY);
 return true;
}
static uint32_t our_box;
static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
 for (uint32_t i = 0; i < ms * 10; i++) {
  if (rd(aoss.base + offset) == value) {
   return true;
  }
  sleep_us(100);
 }
 return rd(aoss.base + offset) == value;
}
static void
ring(uint32_t client, uint32_t signal, const char *whose)
{
 put(&ipcc, whose, IPCC_SEND, client << 16 | signal);
}
static struct {
 uint32_t glink, smp2p, mailbox, other;
 uint32_t said;
 uint64_t since;
} doorbell;
static void
doorbells(void)
{
 for (unsigned i = 0; i < 8; i++) {
  uint32_t who = macan_ipcc_take(ipcc.base, IPCC_WPSS);
  if (who == IPCC_NOBODY) { who = macan_ipcc_take(ipcc.base, IPCC_AOP); }
  if (who == IPCC_NOBODY) {
   return;
  }
  if (who == (IPCC_WPSS << 16)) {
   doorbell.glink++;
  } else if (who == (IPCC_WPSS << 16 | IPCC_SMP2P)) {
   doorbell.smp2p++;
  } else if (who == (IPCC_AOP << 16)) {
   doorbell.mailbox++;
  } else {
   doorbell.other++;
  }
  if (doorbell.said < 24) {
   uint64_t now = now_us() - doorbell.since;
   doorbell.said++;
   printf("wifi: doorbell: +%llu.%03llu ms: client %u, signal %u; heard and cleared\n", now / 1000, now % 1000, who >> 16,
       who & 0xffff);
  }
 }
}
static bool
step_mailbox_open_locked(void)
{
 uint32_t box, size;
 announce("open the mailbox to the always-on processor: words of the message memory, and its doorbell");
 if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
  printf("wifi: %u: not a mailbox this knows\n", step);
  return false;
 }
 box = get(&aoss, "our mailbox", QMP_OUR_BOX);
 size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
 if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
  printf("wifi: %u: our mailbox is not where a message fits\n", step);
  return false;
 }
 if (get(&aoss, "our link", QMP_OUR_LINK) == QMP_UP && get(&aoss, "our link, seen by them", QMP_OUR_LINK_ACK) == QMP_UP &&
     get(&aoss, "our channel", QMP_OUR_CHANNEL) == QMP_UP && get(&aoss, "their channel", QMP_THEIR_CHANNEL) == QMP_UP &&
     get(&aoss, "our channel, seen by them", QMP_OUR_CHANNEL_ACK) == QMP_UP) {
  printf("wifi: %u: the mailbox is open already (a kernel before this one opened it)\n", step);
  our_box = box;
  return true;
 }
 put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, get(&aoss, "their link", QMP_THEIR_LINK));
 put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
 ring(IPCC_AOP, 0, "the always-on processor's doorbell");
 if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
  printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second (it says %#x); our link is "
      "taken down again\n", step, rd(aoss.base + QMP_OUR_LINK_ACK));
  put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
  ring(IPCC_AOP, 0, "the always-on processor's doorbell");
  return false;
 }
 printf("wifi: %u: the always-on processor has seen our link\n", step);
 put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
 ring(IPCC_AOP, 0, "the always-on processor's doorbell");
 if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
  printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second (it says %#x); ours is taken down "
      "again\n", step, rd(aoss.base + QMP_THEIR_CHANNEL));
  put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
  put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
  ring(IPCC_AOP, 0, "the always-on processor's doorbell");
  return false;
 }
 put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
 ring(IPCC_AOP, 0, "the always-on processor's doorbell");
 if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
  printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second (it says %#x); it is taken "
      "down again\n", step, rd(aoss.base + QMP_OUR_CHANNEL_ACK));
  put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
  put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
  ring(IPCC_AOP, 0, "the always-on processor's doorbell");
  return false;
 }
 printf("wifi: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN; the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
 our_box = box;
 return true;
}
static bool
step_mailbox_open(void)
{
 if (leaving) {
  if (!lck_mtx_try_lock(&macan_aop_mailbox_lock)) { return false; }
 } else { lck_mtx_lock(&macan_aop_mailbox_lock); }
 bool result = step_mailbox_open_locked();
 lck_mtx_unlock(&macan_aop_mailbox_lock);
 return result;
}
static bool
mailbox_send_locked(const char *text)
{
 uint32_t words[QMP_MESSAGE / 4] = { 0 };
 if (our_box == 0) {
  return false;
 }
 strlcpy((char *)words, text, sizeof(words));
 printf("wifi: %u: about to tell the always-on processor \"%s\": %u bytes into our mailbox at %#x, its length in "
     "front, and the doorbell\n", step, text, QMP_MESSAGE, aoss.phys + our_box);
 snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
 hold(true);
 if (rd(aoss.base + our_box) != 0) {
  printf("wifi: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
  return false;
 }
 for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
  wr(aoss.base + our_box + 4 + 4 * i, words[i]);
 }
 wr(aoss.base + our_box, QMP_MESSAGE);
 (void)rd(aoss.base + our_box);
 wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
 if (!mailbox_wait(our_box, 0, 1000)) {
  printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
  wr(aoss.base + our_box, 0);
  return false;
 }
 printf("wifi: %u: the always-on processor has taken it\n", step);
 return true;
}
static bool
mailbox_send(const char *text)
{
 if (leaving) {
  if (!lck_mtx_try_lock(&macan_aop_mailbox_lock)) { return false; }
 } else { lck_mtx_lock(&macan_aop_mailbox_lock); }
 bool result = mailbox_send_locked(text);
 lck_mtx_unlock(&macan_aop_mailbox_lock);
 return result;
}
#define OUR_ENTRY_STOP "master-kernel"
#define OUR_ENTRY_RADIO "wlan"
#define DSP_STOP (1u << 0)
static void glink_reset(void);
static void link_stop(void);
static uint32_t our_item;
static bool smp2p_agreed;
static bool smp2p_restart_acks;
static bool smp2p_restart_acked;
static uint32_t
our_item_find(void)
{
 uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);
 return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}
static void
dsp_listen(const struct smp2p *theirs)
{
 struct smp2p ours;
 if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
  return;
 }
 if (!smp2p_agreed) {
  uint32_t features = (ours.version >> 8) & (theirs->version >> 8);
  if ((theirs->version & 0xff) != (ours.version & 0xff)) {
   return;
  }
  if (features != ours.version >> 8) {
   printf("wifi: %u: about to write our item's features at %#lx: %#x (what both sides have)\n", step,
       SMEM_PHYS + our_item + SMP2P_AT_VERSION, features);
   smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
  }
  smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
  smp2p_agreed = true;
  printf("wifi: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
 }
 if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
  smp2p_restart_acked = !smp2p_restart_acked;
  printf("wifi: %u: the DSP's flags say it has restarted (%#x): about to answer in our flags at %#lx (bit 1 = %u) and ring "
      "its SMP2P doorbell\n", step, theirs->flags, SMEM_PHYS + our_item + SMP2P_AT_FLAGS, smp2p_restart_acked);
  smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
  settle();
  wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_SMP2P);
 }
}
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
 struct smp2p ours, theirs;
 uint32_t at = our_item_find(), state, value_at;
 uint64_t asked, now;
 int entry = -1;
 if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
     (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
  printf("wifi: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
  return false;
 }
 if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
  printf("wifi: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
  return false;
 }
 state = dsp_state(&theirs);
 if (state & DSP_STOP_ACK) {
  printf("wifi: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
  return true;
 }
 if (!(state & DSP_READY) || (state & DSP_FATAL)) {
  printf("wifi: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
  return false;
 }
 value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
 printf("wifi: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx (it holds %#x), and its SMP2P doorbell\n",
     step, OUR_ENTRY_STOP, SMEM_PHYS + value_at, ours.entry[entry].value);
 snprintf(last_write, sizeof(last_write), "step %u asked the DSP to stop", step);
 hold(true);
 smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
 settle();
 wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_SMP2P);
 asked = now = now_us();
 while (now - asked < bound_ms * 1000ull) {
  if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
   break;
  }
  sleep_us(200);
  now = now_us();
 }
 state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
 printf("wifi: %u: %s after %llu.%03llu ms; its entry says %#x, its flags %#x\n", step, state & DSP_STOP_ACK ?
     "THE DSP ACKNOWLEDGES THE STOP" : "THE DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000, (now - asked) % 1000, state,
     theirs.flags);
 smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
 settle();
 wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_SMP2P);
 return (state & DSP_STOP_ACK) != 0;
}
#define SCM_64 (1ull << 30)
#define SCM_SIP (2ull << 24)
#define SCM_CALL(service,command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN SCM_CALL(2, 6)
#define SCM_ONE_VALUE 1
#define SCM_THREE_VALUES 3
#define SCM_VALUE_AND_BUFFER (2 | 2 << 6)
#define SCM_INTERRUPTED 1
static LCK_MTX_DECLARE(dsp_lock, &wifi_lock_group);
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t dirty;
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
 uint64_t session = 0;
 function |= SCM_64 | SCM_SIP;
 printf("wifi: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
     second, third);
 snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
 hold(true);
 for (int i = 0; i < 100; i++) {
  register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
  register uint64_t x1 __asm__("x1") = arguments;
  register uint64_t x2 __asm__("x2") = first;
  register uint64_t x3 __asm__("x3") = second;
  register uint64_t x4 __asm__("x4") = third;
  register uint64_t x5 __asm__("x5") = 0;
  register uint64_t x6 __asm__("x6") = session;
  register uint64_t x7 __asm__("x7") = 0;
  __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
      "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  session = x6;
  if (x0 != SCM_INTERRUPTED) {
   printf("wifi: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
   *answer = x1;
   return (int32_t)x0;
  }
 }
 printf("wifi: %u: TrustZone keeps saying the call was interrupted\n", step);
 return -1;
}
static void radio_space_off(void);
static bool
dsp_shutdown(const char *why)
{
 uint64_t answer = ~0ull;
 int32_t result;
 bool ours = dsp_started;
 printf("wifi: %u: %s: the DSP is asked to stop, then TrustZone to shut it down\n", step, why);
 if (!leaving) {
  redwood_lx_stop();
  redwood_wlfw_stop(step);
  link_stop();
 }
 stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
 result = scm("to shut the Wi-Fi DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_WPSS, 0, 0, &answer);
 if (result != 0 || answer != 0) {
  printf("wifi: %u: TRUSTZONE DOES NOT SHUT THE DSP DOWN: the call returns %d, its answer is %#llx\n", step, result, answer);
  return false;
 }
 printf("wifi: %u: THE WI-FI DSP IS SHUT DOWN\n", step);
 if (wlan_powered) {
  macan_wlan_power(0);
  wlan_powered = false;
 }
 wr(ipcc.base + IPCC_DISABLE, IPCC_WPSS << 16 | IPCC_SMP2P);
 doorbells();
 dsp_started = false;
 mailbox_send("{class: image, res: load_state, name: wpss, val: off}");
 if (ours) {
  radio_space_off();
 }
 return true;
}
static bool
step_header(void)
{
 struct smp2p words;
 vm_offset_t page = 0;
 uint64_t answer = ~0ull;
 uint32_t again = 0;
 int32_t result;
 if (image.segments == 0) {
  return false;
 }
 if (dsp_words(&words)) {
  show_words("the DSP has left words in the shared memory", &words);
  announce("ask TrustZone to shut down a DSP that a kernel before this one may have left running");
  if (!dsp_shutdown("it may be running")) {
   return false;
  }
  if (!stopped_by_its_word && !(PE_parse_boot_argn("wifi_again", &again, sizeof(again)) && again)) {
   printf("wifi: %u: THE DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and a start "
       "after that has reset the phone (see the top of macan_wifi.c; wifi_again=1 to try all the same)\n", step);
   return false;
  }
 } else {
  printf("wifi: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started, and "
      "there is nothing to shut down\n", step);
 }
 announce("hand the image's header to TrustZone to check its signature (image 6), in a page of ours");
 if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
  printf("wifi: %u: no memory\n", step);
  return false;
 }
 memcpy((void *)page, image.header, image.header_size);
 for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
  __asm__ volatile ("dc civac, %0" : : "r" (line));
 }
 settle();
 result = scm("to take the Wi-Fi DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_WPSS, kvtophys(page), 0, &answer);
 if (result != 0 || answer != 0) {
  printf("wifi: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
  return false;
 }
 printf("wifi: %u: TRUSTZONE ACCEPTS THE WI-FI DSP'S SIGNATURE FROM THIS KERNEL\n", step);
 announce("tell TrustZone where the image will be");
 result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_WPSS, WPSS_MEMORY, image.size, &answer);
 if (result != 0 || answer != 0) {
  printf("wifi: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
  return false;
 }
 printf("wifi: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, WPSS_MEMORY);
 return true;
}
static bool image_written;
static uint64_t
segment_word(unsigned s, uint32_t at)
{
 uint64_t word = 0;
 for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
  word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
 }
 return word;
}
static bool
step_image(void)
{
 volatile uint64_t *dsp_memory;
 uint64_t written = 0, cleared = 0, wrong = 0;
 if (image.segments == 0) {
  return false;
 }
 announce("put the image's segments into the DSP's memory (0x9ae00000, 25 MB: memory of nobody's until TrustZone "
     "starts the DSP), with zeroes where a segment is longer than its file, and read them back");
 dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(WPSS_MEMORY, WPSS_MEMORY_SIZE, VM_WIMG_WCOMB);
 printf("wifi: %u: the memory begins %#llx, the image %#llx\n", step, dsp_memory[0], segment_word(0, 0));
 for (unsigned s = 0; s < image.segments; s++) {
  volatile uint64_t *to = dsp_memory + image.segment[s].at / 8;
  if ((image.segment[s].at | image.segment[s].memory_size) & 7) {
   printf("wifi: %u: a segment that does not begin and end at a multiple of eight bytes: not written\n", step);
   return false;
  }
  printf("wifi: %u: about to write %#x bytes at %#x (%#x of them the file's)\n", step, image.segment[s].memory_size,
      WPSS_MEMORY + image.segment[s].at, image.segment[s].file_size);
  snprintf(last_write, sizeof(last_write), "step %u wrote a segment of the image at %#x", step,
      WPSS_MEMORY + image.segment[s].at);
  hold(true);
  for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
   to[i / 8] = segment_word(s, i);
  }
  written += image.segment[s].file_size;
  cleared += image.segment[s].memory_size - image.segment[s].file_size;
 }
 settle();
 for (unsigned s = 0; s < image.segments; s++) {
  volatile uint64_t *from = dsp_memory + image.segment[s].at / 8;
  for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
   wrong += from[i / 8] != segment_word(s, i);
  }
 }
 printf("wifi: %u: %llu bytes written and %llu cleared; read back, %llu words of eight bytes differ\n", step, written, cleared,
     wrong);
 settle();
 ml_io_unmap((vm_offset_t)dsp_memory, WPSS_MEMORY_SIZE);
 image_written = wrong == 0;
 return image_written;
}
static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER 0x000
#define LOCK_US 1u
static bool
our_item_make_locked(uint32_t partition)
{
 struct smem_partition header;
 struct smem_entry entry;
 const uint32_t item_size = (sizeof(struct smp2p) + 7) & ~7u, need = sizeof(entry) + item_size;
 uint32_t owner = ~0u, at;
 bool made = false;
 owner = get(&smem_lock, "owner", LOCK_OWNER);
 printf("wifi: %u: about to take the lock (write %u at %#x, until it reads back so, for a second at most), write an entry "
     "of %u bytes for item %u with %u bytes behind it where the partition's items from the bottom end, move the "
     "partition's mark by %u, and release the lock (write 0)\n", step, LOCK_US, smem_lock.phys + LOCK_OWNER,
     (uint32_t)sizeof(entry), ITEM_SMP2P_OURS, item_size, need);
 snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
 hold(true);
 for (uint32_t ms = 0; ms <= 1000; ms++) {
  wr(smem_lock.base + LOCK_OWNER, LOCK_US);
  owner = rd(smem_lock.base + LOCK_OWNER);
  if (owner == LOCK_US) {
   break;
  }
  sleep_ms(1);
 }
 if (owner != LOCK_US) {
  printf("wifi: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
  return false;
 }
 if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
     header.free_cached > header.size) {
  printf("wifi: %u: the partition's marks make no sense: nothing is written\n", step);
  goto out;
 }
 for (at = sizeof(header); at < header.free_uncached;) {
  if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == ITEM_SMP2P_OURS ||
      entry.size > header.size) {
   break;
  }
  at += sizeof(entry) + entry.padding_header + entry.size;
 }
 if (at != header.free_uncached) {
  printf("wifi: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
      "nothing is written\n", step, at, header.free_uncached);
  goto out;
 }
 if (at + need > header.free_cached) {
  printf("wifi: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need, header.free_cached - at);
  goto out;
 }
 smem_poke(partition + at, SMEM_CANARY | ITEM_SMP2P_OURS << 16);
 smem_poke(partition + at + 4, item_size);
 smem_poke(partition + at + 8, item_size - (uint32_t)sizeof(struct smp2p));
 smem_poke(partition + at + 12, 0);
 settle();
 smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need);
 settle();
 made = true;
out:
 wr(smem_lock.base + LOCK_OWNER, 0);
 printf("wifi: %u: the lock is released (it reads %u)%s\n", step, rd(smem_lock.base + LOCK_OWNER), made ? "; the entry is at "
     "the old mark, the mark moved" : "");
 return made;
}
static bool
our_item_make(uint32_t partition)
{
 if (leaving) {
  if (!lck_mtx_try_lock(&macan_smem_alloc_lock)) { return false; }
 } else { lck_mtx_lock(&macan_smem_alloc_lock); }
 bool result = our_item_make_locked(partition);
 lck_mtx_unlock(&macan_smem_alloc_lock);
 return result;
}
static bool
step_our_item(void)
{
 static const char *const names[] = { OUR_ENTRY_STOP, OUR_ENTRY_RADIO };
 struct smem_partition header;
 struct smp2p ours;
 uint32_t cacheline = 0, size = 0, partition;
 announce("look at our partition with the DSP in the shared memory, and for an SMP2P item of ours (616) in it");
 partition = smem_partition(SMEM_US, SMEM_WPSS, &cacheline, &size);
 if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
  printf("wifi: %u: there is no such partition\n", step);
  return false;
 }
 show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
 printf("wifi: %u: its items from the bottom end at +%#x, those from the top begin at +%#x\n", step, header.free_uncached,
     header.free_cached);
 our_item = our_item_find();
 if (our_item != 0) {
  printf("wifi: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
      ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
 } else {
  announce("make the item: the SoC's lock for the shared memory (0x1f43000: the first access to that block) around it");
  if (!our_item_make(partition)) {
   return false;
  }
  our_item = our_item_find();
  if (our_item == 0) {
   printf("wifi: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
   return false;
  }
  printf("wifi: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
  show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
 }
 announce("fill our item in (340 bytes at the address above): zeroes, then from processor 0 to 13, room for 16 entries, "
     "the entries \"master-kernel\" and \"wlan\" at zero, feature bit 0, and last the version, which makes it valid; "
     "then the DSP's SMP2P doorbell, as Linux rings it");
 snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
 for (uint32_t at = 0; at < sizeof(ours); at += 4) {
  smem_poke(our_item + at, 0);
 }
 settle();
 smem_poke(our_item, SMP2P_MAGIC);
 smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_WPSS << 16);
 for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
  uint32_t name[4] = { 0 };
  strlcpy((char *)name, names[i], sizeof(name));
  for (uint32_t word = 0; word < 4; word++) {
   smem_poke(our_item + SMP2P_AT_NAME(i) + 4 * word, name[word]);
  }
 }
 smem_poke(our_item + SMP2P_AT_COUNTS, 16 | (uint32_t)(sizeof(names) / sizeof(names[0])) << 16);
 smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
 settle();
 smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
 settle();
 smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
 ring(IPCC_WPSS, IPCC_SMP2P, "the DSP's SMP2P doorbell");
 if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
  printf("wifi: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
  return false;
 }
 show_words("our item reads back", &ours);
 announce("see that our two marks in GLINK's descriptor are at zero before the DSP starts");
 glink_reset();
 return true;
}
static bool
step_start(void)
{
 struct smp2p words, last;
 uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
 uint32_t state = 0, changes = 0, wait_seconds = 10;
 bool rung;
 int32_t result;
 bool have, spoke = false, handed_over = false;
 if (!image_written || our_box == 0) {
  printf("wifi: %u: the image is not in place, or the mailbox not open\n", step);
  return false;
 }
 PE_parse_boot_argn("wifi_wait", &wait_seconds, sizeof(wait_seconds));
 bzero(&last, sizeof(last));
 have = dsp_words(&last);
 if (have) {
  show_words("before the start, the DSP's item says (from its last run; believed again once it changes)", &last);
 }
 announce("tell the doorbells that rings from before this kernel have been heard and that the DSP's SMP2P ring is "
     "listened for, tell the always-on processor that the image is loaded, and ask TrustZone to authenticate it and let "
     "the DSP out of reset");
 for (unsigned i = 0; i < 8 && macan_ipcc_take(ipcc.base, IPCC_WPSS) != IPCC_NOBODY; i++) { }
 put(&ipcc, "listened for", IPCC_ENABLE, IPCC_WPSS << 16 | IPCC_SMP2P);
 bzero(&doorbell, sizeof(doorbell));
 lck_mtx_lock(&dsp_lock);
 if (!mailbox_send("{class: image, res: load_state, name: wpss, val: on}")) {
  lck_mtx_unlock(&dsp_lock);
  return false;
 }
 result = scm("to authenticate the Wi-Fi DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_WPSS, 0, 0,
     &answer);
 doorbell.since = started = now_us();
 image_written = false;
 if (result != 0 || answer != 0) {
  printf("wifi: %u: TRUSTZONE DOES NOT START THE DSP: the call returns %d, its answer is %#llx\n", step, result, answer);
  mailbox_send("{class: image, res: load_state, name: wpss, val: off}");
  lck_mtx_unlock(&dsp_lock);
  return false;
 }
 dsp_started = true;
 lck_mtx_unlock(&dsp_lock);
 printf("wifi: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE WI-FI DSP OUT OF RESET; watching the shared memory "
     "and our doorbell for %u s\n", step, wait_seconds);
 limit = started + wait_seconds * 1000000ull;
 for (;;) {
  now = now_us();
  if (now >= limit || !dsp_started) {
   break;
  }
  doorbells();
  spoke = spoke || doorbell.glink + doorbell.smp2p != 0;
  if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
   char when[64];
   snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
       have ? "changes" : "APPEARS");
   if (changes++ < 40) {
    show_words(when, &words);
   }
   last = words;
   have = true;
   spoke = true;
   dsp_listen(&words);
   state = dsp_state(&words);
   if ((state & DSP_READY) && ready_at == 0) {
    ready_at = now;
    limit = now + watch_seconds * 1000000ull;
    printf("wifi: %u: THE WI-FI DSP SAYS IT IS UP: its entry \"slave-kernel\" says %#x (bit 1: \"error ready\"), "
        "%llu.%03llu ms after TrustZone started it\n", step, state, (now - started) / 1000, (now - started) % 1000);
   }
   if ((state & DSP_HANDOVER) && !handed_over) {
    handed_over = true;
    printf("wifi: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start\n", step,
        (now - started) / 1000, (now - started) % 1000);
   }
   if (state & DSP_FATAL) {
    printf("wifi: %u: THE WI-FI DSP SAYS IT HAS GIVEN UP (bit 0 of \"slave-kernel\")\n", step);
    show_crash_reason();
    break;
   }
  }
  sleep_us(200);
 }
 if (have) {
  show_words("at the end of the watch the DSP's item says", &last);
 }
 if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
  show_words("and ours", &words);
 }
 rung = doorbell.glink + doorbell.smp2p != 0;
 printf("wifi: %u: the watch is over after %llu ms; doorbells heard: GLINK %u, SMP2P %u, the mailbox %u, others %u\n", step,
     (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p, doorbell.mailbox, doorbell.other);
 if (state & DSP_FATAL) {
  return false;
 }
 if (ready_at != 0) {
  printf("wifi: %u: the DSP said it was up%s\n", step, handed_over ? ", and that it has asked for its own power" : "");
 } else if (spoke) {
  printf("wifi: %u: THE WI-FI DSP RUNS BUT HAS NOT SAID \"UP\": it has %s%s, with %u entries\n", step, rung ?
      "rung our doorbell" : "", have ? (rung ? " and has its item in the shared memory" :
      "its item in the shared memory") : "", last.valid);
 } else {
  printf("wifi: %u: NOT A WORD FROM THE DSP: no ring, and %s\n", step, have ? "its item in the shared memory is as a "
      "kernel before this one left it" : "no item of its in the shared memory");
 }
 return ready_at != 0 || spoke;
}
#define ITEM_GLINK_DESCRIPTOR 478
#define ITEM_GLINK_OUR_QUEUE 479
#define ITEM_GLINK_THEIR_QUEUE 480
#define GLINK_OUR_TAIL 0
#define GLINK_OUR_HEAD 4
#define GLINK_THEIR_TAIL 8
#define GLINK_THEIR_HEAD 12
#define GLINK_RESERVE 16
#define IPCC_GLINK 0u
enum {
 GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
 GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
 GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1 1
#define GLINK_INTENT_REUSE 1u
#define GLINK_CHANNEL "IPCRTR"
#define GLINK_US 1
struct glink_message {
 uint16_t command, param1;
 uint32_t param2;
};
struct glink_packet {
 struct glink_packet *next;
 uint32_t length;
 uint8_t data[];
};
static LCK_MTX_DECLARE(glink_lock, &wifi_lock_group);
static struct {
 uint32_t descriptor, ours, ours_size, theirs, theirs_size;
 bool up;
 bool broken;
 uint32_t features;
 bool agreed;
 uint32_t their_channel;
 bool asked, acknowledged, open;
 bool request_answered, request_granted;
 struct {
  uint32_t id, size;
  bool there, in_use;
 } their_intents[64];
 struct {
  uint32_t size, fill;
  uint8_t *data;
 } our_intents[24];
 uint32_t our_intent_count;
 struct glink_packet *first, *last;
 uint32_t said, heard, trace;
} glink;
static uint32_t
smem_word(uint32_t offset)
{
 return rd((vm_offset_t)smem + offset);
}
static bool
glink_find(void)
{
 uint32_t size = 0;
 glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
 if (glink.descriptor == 0 || size != 32) {
  return false;
 }
 glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
 glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
 return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
        !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}
static void
glink_reset(void)
{
 uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
 if (descriptor == 0 || size != 32) {
  printf("wifi: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
  return;
 }
 printf("wifi: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
     "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
     smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
 if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
  printf("wifi: %u: about to write our two marks there (+4, +8): 0\n", step);
  hold(true);
  smem_poke(descriptor + GLINK_OUR_HEAD, 0);
  smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
  settle();
 }
}
static uint32_t
glink_waiting(void)
{
 uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
 if (head >= glink.theirs_size || tail >= glink.theirs_size) {
  return 0;
 }
 return head >= tail ? head - tail : glink.theirs_size - tail + head;
}
static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
 uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;
 for (uint32_t i = 0; i < count; i++) {
  ((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
 }
}
static void
glink_advance(uint32_t count)
{
 smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}
static uint32_t
glink_room(void)
{
 uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;
 if (head >= glink.ours_size || tail >= glink.ours_size) {
  return 0;
 }
 room = tail <= head ? glink.ours_size - head + tail : tail - head;
 return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}
static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
 volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
 uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
 uint32_t padded = (length + 7) & ~7u;
 if (!glink.up || glink_room() < padded) {
  return false;
 }
 for (uint32_t i = 0; i < padded; i++) {
  queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
      i < length ? ((const uint8_t *)second)[i - first_length] : 0;
 }
 settle();
 smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
 settle();
 wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_GLINK);
 glink.said++;
 return true;
}
static const char *
glink_name(unsigned command)
{
 static const char *const names[] = {
  "version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
  "data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
 };
 return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}
static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
 struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
 bool sent = glink_put(&message, sizeof(message), more, more_length);
 if (glink.trace > 0 || !sent) {
  glink.trace -= glink.trace > 0;
  printf("wifi: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
 }
}
static bool
glink_offer(uint32_t size)
{
 uint32_t words[2] = { size, glink.our_intent_count + 1 }, i = glink.our_intent_count;
 if (i >= sizeof(glink.our_intents) / sizeof(glink.our_intents[0]) || size == 0 || size > 0x10000) {
  return false;
 }
 if (glink.our_intents[i].data == NULL || glink.our_intents[i].size != size) {
  glink.our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
 }
 if (glink.our_intents[i].data == NULL) {
  return false;
 }
 glink.our_intents[i].size = size;
 glink.our_intents[i].fill = 0;
 glink.our_intent_count++;
 glink_say(GLINK_INTENT, GLINK_US, 1, words, sizeof(words));
 return true;
}
static void
glink_pump(void)
{
 static const struct {
  uint32_t size, count;
 } intents[] = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } };
 struct glink_message message;
 uint32_t words[4];
 char name[33];
 while (glink.up && !glink.broken) {
  uint32_t waiting = glink_waiting(), length = sizeof(message);
  if (waiting < sizeof(message)) {
   return;
  }
  glink_peek(&message, 0, sizeof(message));
  if (message.command == GLINK_OPEN) {
   length += message.param2;
  } else if (message.command == GLINK_INTENT) {
   length += 8 * message.param2;
  } else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
   if (waiting < 16) {
    return;
   }
   glink_peek(words, 8, 8);
   length = 16 + words[0];
  }
  length = (length + 7) & ~7u;
  if (length > glink.theirs_size - 8) {
   printf("wifi: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
       glink_name(message.command), message.param1, message.param2);
   glink.broken = true;
   return;
  }
  if (waiting < length) {
   return;
  }
  glink.heard++;
  if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE) {
   glink.trace--;
   printf("wifi: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
  }
  switch (message.command) {
  case GLINK_VERSION:
   if (message.param1 == GLINK_VERSION_1) {
    glink.features &= message.param2;
   }
   glink_advance(length);
   glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
   break;
  case GLINK_VERSION_ACK:
   glink_advance(length);
   if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
    glink.agreed = true;
   } else if (message.param1 != 0) {
    glink.features &= message.param2;
    glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
   }
   break;
  case GLINK_OPEN:
   bzero(name, sizeof(name));
   glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
   glink_advance(length);
   if (strcmp(name, GLINK_CHANNEL) != 0) {
    printf("wifi: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
        message.param1);
    break;
   }
   printf("wifi: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
       name, message.param1, GLINK_US);
   glink.their_channel = message.param1;
   glink_say(GLINK_OPEN_ACK, glink.their_channel, 0, NULL, 0);
   if (!glink.asked) {
    bzero(name, sizeof(name));
    strlcpy(name, GLINK_CHANNEL, sizeof(name));
    glink_say(GLINK_OPEN, GLINK_US, sizeof(GLINK_CHANNEL), name, (sizeof(GLINK_CHANNEL) + 7) & ~7u);
    glink.asked = true;
   }
   break;
  case GLINK_OPEN_ACK:
   glink_advance(length);
   if (message.param1 == GLINK_US) {
    glink.acknowledged = true;
   }
   break;
  case GLINK_INTENT:
   for (uint32_t i = 0; i < message.param2 && message.param1 == glink.their_channel; i++) {
    glink_peek(words, 8 + 8 * i, 8);
    for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
     if (!glink.their_intents[n].there || glink.their_intents[n].id == words[1]) {
      glink.their_intents[n].there = true;
      glink.their_intents[n].in_use = false;
      glink.their_intents[n].size = words[0];
      glink.their_intents[n].id = words[1];
      break;
     }
    }
    if (glink.trace > 0) {
     printf("wifi: glink:   the DSP will take %u bytes into its intent %u\n", words[0], words[1]);
    }
   }
   glink_advance(length);
   break;
  case GLINK_DONE:
  case GLINK_DONE_REUSE:
   for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
    if (glink.their_intents[n].there && glink.their_intents[n].id == message.param2) {
     glink.their_intents[n].in_use = false;
     glink.their_intents[n].there = message.command == GLINK_DONE_REUSE;
    }
   }
   glink_advance(length);
   break;
  case GLINK_INTENT_REQUEST:
   glink_advance(length);
   if (message.param1 == glink.their_channel) {
    bool granted = glink_offer(message.param2);
    glink_say(GLINK_INTENT_REQUEST_ACK, GLINK_US, granted, NULL, 0);
   }
   break;
  case GLINK_INTENT_REQUEST_ACK:
   glink_advance(length);
   glink.request_answered = true;
   glink.request_granted = message.param2 != 0;
   break;
  case GLINK_DATA:
  case GLINK_DATA_MORE: {
   uint32_t intent = message.param2 - 1;
   if (message.param1 != glink.their_channel || intent >= glink.our_intent_count ||
       words[0] > glink.our_intents[intent].size - glink.our_intents[intent].fill) {
    printf("wifi: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
        message.param1, message.param2);
    glink_advance(length);
    break;
   }
   glink_peek(glink.our_intents[intent].data + glink.our_intents[intent].fill, 16, words[0]);
   glink.our_intents[intent].fill += words[0];
   glink_advance(length);
   if (words[1] == 0) {
    struct glink_packet *packet = kalloc_data(sizeof(*packet) + glink.our_intents[intent].fill, Z_WAITOK);
    if (packet != NULL) {
     packet->next = NULL;
     packet->length = glink.our_intents[intent].fill;
     memcpy(packet->data, glink.our_intents[intent].data, packet->length);
     if (glink.last != NULL) {
      glink.last->next = packet;
     } else {
      glink.first = packet;
     }
     glink.last = packet;
    }
    glink.our_intents[intent].fill = 0;
    glink_say(GLINK_DONE_REUSE, GLINK_US, message.param2, NULL, 0);
   }
   break;
  }
  case GLINK_READ_NOTIFY:
   glink_advance(length);
   wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | IPCC_GLINK);
   break;
  case GLINK_CLOSE:
   glink_advance(length);
   glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
   if (message.param1 == glink.their_channel) {
    printf("wifi: glink: THE DSP CLOSES \"%s\"\n", GLINK_CHANNEL);
    glink.open = false;
    glink.their_channel = 0;
   }
   break;
  case GLINK_CLOSE_ACK:
  case GLINK_SIGNALS:
   glink_advance(length);
   break;
  default:
   printf("wifi: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
       message.param1, message.param2);
   glink.broken = true;
   return;
  }
  if (glink.their_channel != 0 && glink.acknowledged && !glink.open) {
   glink.open = true;
   printf("wifi: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
       GLINK_CHANNEL, GLINK_US, glink.their_channel);
   for (unsigned i = 0; i < sizeof(intents) / sizeof(intents[0]); i++) {
    for (unsigned n = 0; n < intents[i].count; n++) {
     glink_offer(intents[i].size);
    }
   }
  }
 }
}
static int
glink_send(const void *data, uint32_t length)
{
 struct {
  struct glink_message message;
  uint32_t size, left;
 } header = { { GLINK_DATA, GLINK_US, 0 }, length, 0 };
 bool asked = false, fits;
 int found = -1;
 lck_mtx_lock(&glink_lock);
 if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
  lck_mtx_unlock(&glink_lock);
  return EMSGSIZE;
 }
 for (unsigned waited = 0;; waited++) {
  glink_pump();
  if (!glink.up || !glink.open || glink.broken) {
   lck_mtx_unlock(&glink_lock);
   return ENOTCONN;
  }
  found = -1;
  fits = false;
  for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
   if (!glink.their_intents[n].there || glink.their_intents[n].size < length) {
    continue;
   }
   fits = true;
   if (!glink.their_intents[n].in_use && (found < 0 || glink.their_intents[n].size < glink.their_intents[found].size)) {
    found = (int)n;
   }
  }
  if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
   break;
  }
  if (!fits && !asked) {
   glink.request_answered = false;
   glink_say(GLINK_INTENT_REQUEST, GLINK_US, length, NULL, 0);
   asked = true;
  }
  if (waited >= 5000 || (!fits && asked && glink.request_answered && !glink.request_granted)) {
   lck_mtx_unlock(&glink_lock);
   return found < 0 ? ENOBUFS : EAGAIN;
  }
  lck_mtx_unlock(&glink_lock);
  sleep_ms(1);
  lck_mtx_lock(&glink_lock);
 }
 glink.their_intents[found].in_use = true;
 header.message.param2 = glink.their_intents[found].id;
 glink_put(&header, sizeof(header), data, length);
 lck_mtx_unlock(&glink_lock);
 return 0;
}
#define REDWOOD_QRTR_MAX 8192
#define QRTR_US 1u
#define QRTR_CONTROL 0xfffffffeu
#define QRTR_FIRST_PORT 0x4000u
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7 };
struct qrtr_header {
 uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
 uint32_t command;
 uint32_t words[4];
};
struct redwood_qrtr_port {
 uint32_t number;
 redwood_qrtr_receive_t receive;
 redwood_qrtr_server_t server;
 void *context;
 struct {
  uint32_t service, instance;
  bool asked;
  bool fresh;
 } lookups[4];
};
static LCK_MTX_DECLARE(qrtr_lock, &wifi_lock_group);
static LCK_MTX_DECLARE(qrtr_calling, &wifi_lock_group);
static struct redwood_qrtr_port qrtr_ports[16];
static struct {
 uint32_t service, instance, node, port;
 bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer;
static uint32_t qrtr_next_port = QRTR_FIRST_PORT, qrtr_in, qrtr_out, qrtr_hellos;
static thread_t link_thread_is;
static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
 uint32_t padded = (length + 3) & ~3u;
 struct qrtr_header *packet;
 int error;
 if (length == 0 || length > REDWOOD_QRTR_MAX) {
  return EMSGSIZE;
 }
 packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
 if (packet == NULL) {
  return ENOMEM;
 }
 packet->version = 1;
 packet->type = type;
 packet->from_node = QRTR_US;
 packet->from_port = from_port;
 packet->size = length;
 packet->to_node = to_node;
 packet->to_port = to_port;
 memcpy(packet + 1, data, length);
 error = glink_send(packet, (uint32_t)sizeof(*packet) + padded);
 kfree_data(packet, sizeof(*packet) + padded);
 qrtr_out += error == 0;
 return error;
}
static bool
qrtr_matches(uint32_t service, uint32_t instance, unsigned server)
{
 return qrtr_servers[server].there && qrtr_servers[server].service == service &&
        (instance == 0 || qrtr_servers[server].instance == instance);
}
static void
qrtr_tell(uint32_t service, uint32_t instance, uint32_t node, uint32_t port, bool arrived)
{
 for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
  for (unsigned l = 0; l < 4; l++) {
   redwood_qrtr_server_t server = NULL;
   void *context;
   lck_mtx_lock(&qrtr_lock);
   if (qrtr_ports[p].number != 0 && qrtr_ports[p].lookups[l].asked && !qrtr_ports[p].lookups[l].fresh &&
       qrtr_ports[p].lookups[l].service == service &&
       (qrtr_ports[p].lookups[l].instance == 0 || qrtr_ports[p].lookups[l].instance == instance)) {
    server = qrtr_ports[p].server;
   }
   context = qrtr_ports[p].context;
   lck_mtx_unlock(&qrtr_lock);
   if (server != NULL) {
    server(context, arrived, service, instance & 0xff, instance >> 8, node, port);
   }
  }
 }
}
static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
 struct qrtr_control hello = { .command = QRTR_HELLO };
 int found = -1, error;
 switch (control->command) {
 case QRTR_HELLO:
  if (qrtr_hellos++ != 0) {
   break;
  }
  error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
  printf("wifi: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
  break;
 case QRTR_NEW_SERVER:
  lck_mtx_lock(&qrtr_lock);
  for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
   if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
    found = (int)i;
    break;
   }
   if (!qrtr_servers[i].there && found < 0) {
    found = (int)i;
   }
  }
  if (found >= 0) {
   qrtr_servers[found].there = true;
   qrtr_servers[found].service = control->words[0];
   qrtr_servers[found].instance = control->words[1];
   qrtr_servers[found].node = control->words[2];
   qrtr_servers[found].port = control->words[3];
  }
  lck_mtx_unlock(&qrtr_lock);
  printf("wifi: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
      control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
  qrtr_tell(control->words[0], control->words[1], control->words[2], control->words[3], true);
  break;
 case QRTR_DEL_SERVER:
 case QRTR_BYE:
  for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
   bool gone;
   lck_mtx_lock(&qrtr_lock);
   gone = qrtr_servers[i].there && (control->command == QRTR_BYE ? qrtr_servers[i].node == header->from_node :
       qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]);
   qrtr_servers[i].there = qrtr_servers[i].there && !gone;
   lck_mtx_unlock(&qrtr_lock);
   if (gone) {
    printf("wifi: qrtr: service %u at node %u port %u has gone\n", qrtr_servers[i].service, qrtr_servers[i].node,
        qrtr_servers[i].port);
    qrtr_tell(qrtr_servers[i].service, qrtr_servers[i].instance, qrtr_servers[i].node, qrtr_servers[i].port, false);
   }
  }
  break;
 default:
  break;
 }
}
static void
qrtr_input(const uint8_t *data, uint32_t length)
{
 struct qrtr_header header;
 redwood_qrtr_receive_t receive = NULL;
 void *context = NULL;
 uint32_t at;
 if (length >= sizeof(header) && data[0] == 1) {
  memcpy(&header, data, sizeof(header));
  at = sizeof(header);
 } else if (length >= 16 && data[0] == 3) {
  bzero(&header, sizeof(header));
  header.type = data[1];
  header.confirm = data[2] & 1;
  memcpy(&header.size, data + 4, 4);
  header.from_node = (uint32_t)(data[8] | data[9] << 8);
  header.from_port = (uint32_t)(data[10] | data[11] << 8);
  header.to_node = (uint32_t)(data[12] | data[13] << 8);
  header.to_port = (uint32_t)(data[14] | data[15] << 8);
  header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
  header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
  at = 16 + data[3];
 } else {
  printf("wifi: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
  return;
 }
 if (header.size == 0 || at + header.size > length) {
  printf("wifi: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
  return;
 }
 qrtr_in++;
 if (qrtr_peer == 0) {
  qrtr_peer = header.from_node;
 }
 if (header.type == QRTR_RESUME_TX) {
  return;
 }
 if (header.confirm) {
  struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };
  qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
 }
 if (header.to_port == QRTR_CONTROL) {
  struct qrtr_control control = { 0 };
  memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
  qrtr_control(&header, &control);
  return;
 }
 lck_mtx_lock(&qrtr_lock);
 for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
  if (qrtr_ports[p].number == header.to_port && header.type == QRTR_DATA) {
   receive = qrtr_ports[p].receive;
   context = qrtr_ports[p].context;
  }
 }
 lck_mtx_unlock(&qrtr_lock);
 if (receive != NULL) {
  receive(context, header.from_node, header.from_port, data + at, header.size);
 }
}
static void
qrtr_fresh(void)
{
 for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
  for (unsigned l = 0; l < 4; l++) {
   redwood_qrtr_server_t server;
   uint32_t service, instance;
   void *context;
   lck_mtx_lock(&qrtr_lock);
   if (qrtr_ports[p].number == 0 || !qrtr_ports[p].lookups[l].fresh) {
    lck_mtx_unlock(&qrtr_lock);
    continue;
   }
   qrtr_ports[p].lookups[l].fresh = false;
   server = qrtr_ports[p].server;
   service = qrtr_ports[p].lookups[l].service;
   instance = qrtr_ports[p].lookups[l].instance;
   context = qrtr_ports[p].context;
   lck_mtx_unlock(&qrtr_lock);
   for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]) && server != NULL; i++) {
    if (qrtr_matches(service, instance, i)) {
     server(context, true, qrtr_servers[i].service, qrtr_servers[i].instance & 0xff,
         qrtr_servers[i].instance >> 8, qrtr_servers[i].node, qrtr_servers[i].port);
    }
   }
  }
 }
}
static struct redwood_qrtr_port *
qrtr_open(redwood_qrtr_receive_t receive, redwood_qrtr_server_t server, void *context)
{
 struct redwood_qrtr_port *port = NULL;
 lck_mtx_lock(&qrtr_lock);
 for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]) && port == NULL; p++) {
  if (qrtr_ports[p].number == 0) {
   port = &qrtr_ports[p];
   bzero(port, sizeof(*port));
   port->number = qrtr_next_port++;
   port->receive = receive;
   port->server = server;
   port->context = context;
  }
 }
 lck_mtx_unlock(&qrtr_lock);
 return port;
}
static int
qrtr_send(struct redwood_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
 if (port == NULL || port->number == 0 || length > REDWOOD_QRTR_MAX) {
  return EINVAL;
 }
 return qrtr_out_packet(QRTR_DATA, port->number, node, to, data, (uint32_t)length);
}
static int
qrtr_lookup(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
 int error = ENOSPC;
 lck_mtx_lock(&qrtr_lock);
 for (unsigned l = 0; l < 4 && error != 0 && port != NULL && port->number != 0; l++) {
  if (!port->lookups[l].asked) {
   port->lookups[l].service = service;
   port->lookups[l].instance = instance != 0 ? (version & 0xff) | instance << 8 : 0;
   port->lookups[l].asked = true;
   port->lookups[l].fresh = true;
   error = 0;
  }
 }
 lck_mtx_unlock(&qrtr_lock);
 return error;
}
static void
qrtr_close(struct redwood_qrtr_port *port)
{
 struct qrtr_control gone = { .command = QRTR_DEL_CLIENT, .words = { QRTR_US, port ? port->number : 0 } };
 bool outside = current_thread() != link_thread_is;
 if (port == NULL || port->number == 0) {
  return;
 }
 if (outside) {
  lck_mtx_lock(&qrtr_calling);
 }
 lck_mtx_lock(&qrtr_lock);
 port->number = 0;
 lck_mtx_unlock(&qrtr_lock);
 if (outside) {
  lck_mtx_unlock(&qrtr_calling);
 }
 if (qrtr_peer != 0) {
  qrtr_out_packet(QRTR_DEL_CLIENT, gone.words[1], qrtr_peer, QRTR_CONTROL, &gone, sizeof(gone));
 }
}
static const struct redwood_qrtr_ops qrtr_ops = { .open = qrtr_open, .send = qrtr_send, .lookup = qrtr_lookup, .close = qrtr_close };
static volatile bool link_wanted, link_runs;
static void
link_wait(void)
{
 sleep_us(1000);
}
static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
 struct smp2p words, last;
 struct glink_packet *packet;
 bool gave_up = false;
 bzero(&last, sizeof(last));
 dsp_words(&last);
 while (link_wanted) {
  link_wait();
  doorbells();
  if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
   last = words;
   dsp_listen(&words);
   if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
    gave_up = true;
    printf("wifi: link: THE WI-FI DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
    show_crash_reason();
   }
  }
  for (;;) {
   lck_mtx_lock(&glink_lock);
   glink_pump();
   packet = glink.first;
   if (packet != NULL) {
    glink.first = packet->next;
    glink.last = glink.first == NULL ? NULL : glink.last;
   }
   lck_mtx_unlock(&glink_lock);
   if (packet == NULL) {
    break;
   }
   lck_mtx_lock(&qrtr_calling);
   qrtr_input(packet->data, packet->length);
   lck_mtx_unlock(&qrtr_calling);
   kfree_data(packet, sizeof(*packet) + packet->length);
  }
  lck_mtx_lock(&qrtr_calling);
  qrtr_fresh();
  lck_mtx_unlock(&qrtr_calling);
 }
 link_runs = false;
 thread_terminate(current_thread());
}
static void
link_stop(void)
{
 struct glink_packet *packet;
 if (link_runs) {
  link_wanted = false;
  for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
   sleep_ms(1);
  }
  printf("wifi: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams; doorbells heard: "
      "GLINK %u, SMP2P %u, the mailbox %u, others %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
      glink.heard, qrtr_out, qrtr_in, doorbell.glink, doorbell.smp2p, doorbell.mailbox, doorbell.other);
 }
 lck_mtx_lock(&glink_lock);
 glink.up = glink.open = false;
 while ((packet = glink.first) != NULL) {
  glink.first = packet->next;
  kfree_data(packet, sizeof(*packet) + packet->length);
 }
 glink.last = NULL;
 lck_mtx_unlock(&glink_lock);
 lck_mtx_lock(&qrtr_lock);
 bzero(qrtr_servers, sizeof(qrtr_servers));
 qrtr_peer = 0;
 lck_mtx_unlock(&qrtr_lock);
}
static bool
step_queues(void)
{
 struct glink_message message;
 uint32_t head, tail;
 announce("find GLINK's descriptor and the two queues in the shared memory, and read what the DSP has put into its own "
     "(nothing is taken out)");
 if (!glink_find()) {
  printf("wifi: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
      "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
  return false;
 }
 head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
 tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
 printf("wifi: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
     "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
     smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
 for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
  char name[33] = "";
  uint32_t length = sizeof(message);
  glink_peek(&message, at, sizeof(message));
  if (message.command == GLINK_OPEN) {
   glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
   length += message.param2;
  }
  printf("wifi: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
      message.param2, name);
  if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
   break;
  }
  at += (length + 7) & ~7u;
 }
 return true;
}
static bool
step_link(void)
{
 thread_t thread;
 uint64_t began;
 if (!dsp_started || glink.descriptor == 0) {
  return false;
 }
 announce("take up GLINK with the DSP: our version into our queue and its doorbell, then whatever its messages call for "
     "(writes: our queue, our two marks in the descriptor, the doorbell); a thread of ours listens from here on, "
     "every millisecond");
 snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
 lck_mtx_lock(&glink_lock);
 glink.features = GLINK_INTENT_REUSE;
 glink.agreed = glink.asked = glink.acknowledged = glink.open = glink.broken = false;
 glink.their_channel = glink.our_intent_count = glink.said = glink.heard = 0;
 bzero(glink.their_intents, sizeof(glink.their_intents));
 glink.trace = 60;
 glink.up = true;
 qrtr_hellos = qrtr_in = qrtr_out = 0;
 doorbell.since = began = now_us();
 glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
 lck_mtx_unlock(&glink_lock);
 link_wanted = link_runs = true;
 if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
  link_wanted = link_runs = false;
  printf("wifi: %u: no thread\n", step);
  return false;
 }
 link_thread_is = thread;
 thread_deallocate(thread);
 for (unsigned ms = 0; ms < 5000 && !glink.open && !glink.broken; ms++) {
  sleep_ms(1);
 }
 sleep_ms(50);
 printf("wifi: %u: %llu ms after our version: versions %s (features %#x); the channel \"%s\" is %s\n", step,
     (now_us() - began) / 1000, glink.agreed ? "agreed" : "NOT AGREED", glink.features, GLINK_CHANNEL, glink.open ?
     "OPEN" : "NOT OPEN");
 return glink.open;
}
#define SERVICE_WLFW 0x45
static uint32_t wlfw_node, wlfw_port;
static volatile uint32_t wlfw_answer;
static uint8_t wlfw_answer_bytes[64];
static void
wlfw_service(__unused void *context, bool arrived, __unused uint32_t service, __unused uint32_t version, __unused uint32_t instance,
    uint32_t node, uint32_t port)
{
 printf("wifi: qrtr: THE WI-FI FIRMWARE'S SERVICE (%#x) %s node %u port %u\n", SERVICE_WLFW, arrived ? "IS AT" : "has left",
     node, port);
 wlfw_node = arrived ? node : 0;
 wlfw_port = arrived ? port : 0;
}
static void
wlfw_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
 printf("wifi: qrtr: a datagram of %lu bytes from node %u port %u\n", (unsigned long)length, node, port);
 memcpy(wlfw_answer_bytes, data, length < sizeof(wlfw_answer_bytes) ? length : sizeof(wlfw_answer_bytes));
 wlfw_answer = (uint32_t)length;
}
static const uint8_t *image_file(const char *name, uint64_t *size);
const uint8_t *
redwood_wifi_file(const char *name, uint64_t *size)
{
 return image_file(name, size);
}
static void radio_space_faults(void);
static bool
step_driver(uint32_t how)
{
 int error;
 announce(how >= 2 ? "hand the radio to ath11k itself: its exchange with the firmware up to mission mode, then THE RADIO'S "
     "REGISTERS ARE WRITTEN, by ce.c and hal.c: the copy engines' rings (every write is said as it is made)" :
     "hand the firmware to ath11k itself: its own exchange up to mission mode; the radio's registers are neither read "
     "nor written (wifi_lx=1), and nothing is written but datagrams");
 error = redwood_lx_start();
 radio_space_faults();
 printf("wifi: %u: the driver's start returns %d\n", step, error);
 return error == 0;
}
static bool
step_exchange(void)
{
 uint32_t driver = 0;
 if (!glink.open) {
  return false;
 }
 if (macan && !wlan_powered) {
  macan_wlan_power(1);
  wlan_powered = true;
 }
 if ((driver = redwood_wifi_option("wifi_lx")) != 0) {
  return step_driver(driver);
 }
 announce("have the exchange a WCN6750's driver has with the Wi-Fi firmware's service before it touches the radio, by "
     "hand in ath11k's order (macan_wlfw.c): indications, capabilities both ways, the regulatory database and the "
     "board's data; nothing is written but datagrams");
 return redwood_wlfw_exchange(step);
}
#define RADIO_WINDOW 0x80000u
#define RADIO_DATA_PATH 0x00a00000u
#define RADIO_COPY_ENGINES 0x01b80000u
static bool
step_registers(void)
{
 static const struct {
  uint32_t chip;
  const char *name;
 } words[] = {
  { 0x01b80000 + 0x48, "copy engine 0, source ring, interrupt address (low)" },
  { 0x01b80000 + 0x00, "copy engine 0, source ring, base (low)" },
  { 0x01b80000 + 0x08, "copy engine 0, source ring, id" },
  { 0x01b83000 + 0xb0, "copy engine 1, destination ring control" },
  { 0x00a38000 + 0x000, "reorder block, general enable" },
  { 0x00a38000 + 0x1f4, "reorder ring 1, id" },
  { 0x00000224, "hardware version (if this chip has it where its siblings do)" },
 };
 static struct block radio = { "radio", 0, 0 };
 uint64_t address = 0;
 uint32_t size = 0, asked = 0;
 bool mission = false;
 if (!redwood_wlfw_registers(&address, &size, &mission) || address > 0xffffffffull || size < 3 * RADIO_WINDOW) {
  printf("wifi: %u: the firmware has not said where the radio's registers are (step 15)\n", step);
  return false;
 }
 PE_parse_boot_argn("wifi_radio_read", &asked, sizeof(asked));
 if (asked == 0 || (!mission && asked < 2)) {
  printf("wifi: %u: THE RADIO'S REGISTERS (%#llx) ARE NOT READ: %s (see the top of this step in macan_wifi.c)\n", step,
      address, asked == 0 ? "not asked for (wifi_radio_read=1)" : "the firmware is not in mission mode (wifi_mission=1), "
      "and outside it a read there has ended the kernel with a bus error (wifi_radio_read=2 to read all the same)");
  return false;
 }
 announce("read the radio's registers where the firmware says they are, the word ath11k touches first before any other; "
     "if nothing answers there the kernel ends at that read");
 printf("wifi: %u: the registers: %#x bytes at %#llx; the firmware %s\n", step, size, address, mission ? "is in mission mode" :
     "IS NOT IN MISSION MODE (wifi_mission=1 gets it there): the state in which the read failed before");
 radio.phys = (uint32_t)address;
 radio.size = size;
 radio.base = ml_io_map_unmappable(radio.phys, radio.size, VM_WIMG_IO);
 for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
  uint32_t chip = words[i].chip, at;
  at = chip < RADIO_WINDOW ? chip : (chip ^ RADIO_DATA_PATH) < RADIO_WINDOW ? RADIO_WINDOW + (chip & (RADIO_WINDOW - 1)) :
      2 * RADIO_WINDOW + (chip & (RADIO_WINDOW - 1));
  printf("wifi: %u: about to read the radio's %s (the chip's %#x) at %#x\n", step, words[i].name, chip, radio.phys + at);
  snprintf(last_write, sizeof(last_write), "step %u was reading the radio's registers at %#x", step, radio.phys + at);
  sleep_ms(i == 0 ? pause_ms : 50);
  printf("wifi: %u: THE RADIO'S %s (%#x) = %#x\n", step, words[i].name, radio.phys + at, rd(radio.base + at));
 }
 ml_io_unmap(radio.base, radio.size);
 return true;
}
static bool
step_qrtr(void)
{
 struct redwood_qrtr_port *port;
 uint32_t ask = 0, seconds = 3, services = 0;
 if (!glink.open) {
  return false;
 }
 PE_parse_boot_argn("wifi_listen", &seconds, sizeof(seconds));
 announce("listen for QRTR on the channel: the DSP's hello is answered with ours, its services are written down; and a "
     "port of ours looks the Wi-Fi firmware's service up, as the radio's driver will");
 port = qrtr_open(wlfw_receive, wlfw_service, NULL);
 if (port == NULL || qrtr_lookup(port, SERVICE_WLFW, 1, 0) != 0) {
  printf("wifi: %u: no port\n", step);
  return false;
 }
 for (unsigned ms = 0; ms < seconds * 1000 && !(wlfw_port != 0 && ms >= 500); ms++) {
  sleep_ms(1);
 }
 for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
  services += qrtr_servers[i].there;
 }
 printf("wifi: %u: QRTR: the DSP is node %u; hellos from it %u; datagrams received %u, sent %u; services it has told of: "
     "%u; the Wi-Fi firmware's service: %s\n", step, qrtr_peer, qrtr_hellos, qrtr_in, qrtr_out, services, wlfw_port != 0 ?
     "THERE" : "NOT THERE");
 if (wlfw_port != 0 && PE_parse_boot_argn("wifi_ask", &ask, sizeof(ask)) && ask) {
  static const uint8_t question[7] = { 0, 0x01, 0x00, 0x24, 0x00, 0x00, 0x00 };
  int error;
  announce("ask the Wi-Fi firmware's service for its capabilities (QMI message 0x24, seven bytes) and wait a second "
      "for an answer");
  wlfw_answer = 0;
  error = qrtr_send(port, wlfw_node, wlfw_port, question, sizeof(question));
  for (unsigned ms = 0; ms < 1000 && error == 0 && wlfw_answer == 0; ms++) {
   sleep_ms(1);
  }
  if (error != 0 || wlfw_answer == 0) {
   printf("wifi: %u: %s\n", step, error != 0 ? "THE QUESTION COULD NOT BE SENT" : "no answer within a second");
  } else {
   char text[3 * 48 + 1] = "";
   for (uint32_t i = 0; i < wlfw_answer && i < 48; i++) {
    snprintf(text + 3 * i, 4, " %02x", wlfw_answer_bytes[i]);
   }
   printf("wifi: %u: THE SERVICE ANSWERS: %u bytes, beginning%s\n", step, wlfw_answer, text);
  }
 }
 qrtr_close(port);
 return qrtr_hellos != 0 && qrtr_out != 0;
}
static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000 };
#define SMMU_SCR0 0x000
#define SMMU_IDR0 0x020
#define SMMU_IDR1 0x024
#define SMMU_SMR(n) (0x800 + 4 * (n))
#define SMMU_S2CR(n) (0xc00 + 4 * (n))
static uint32_t radio_stream = 0x1c00;
#define RADIO_STREAM radio_stream
#define RADIO_STREAM_MASK 0x0001
static void
smmu_look(void)
{
 static const char *const kinds[] = { "translated by context", "PASSED UNTRANSLATED (bypass; the number is", "MADE TO FAULT "
                               "(the number is", "of a reserved kind (the number is" };
 uint32_t scr0, idr0, idr1, matches, valid = 0, covering = 0;
 printf("wifi: 0: read the global registers of the application processors' SMMU (0x15000000: the first access of ours to "
     "that block; the hypervisor answers for them)\n");
 sleep_ms(pause_ms);
 scr0 = get(&apps_smmu, "scr0", SMMU_SCR0);
 idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
 idr1 = get(&apps_smmu, "idr1", SMMU_IDR1);
 matches = idr0 & 0xff;
 printf("wifi: 0: the SMMU is %s; %u stream match registers, %u contexts; a stream nobody matches %s\n", scr0 & 1 ? "OFF" : "on",
     matches, idr1 & 0xff, scr0 & 1u << 10 ? "faults" : "passes untranslated");
 for (uint32_t n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
  uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
  uint32_t wide = scr0 & 1u << 3, stream = smr & (wide ? 0xffff : 0x7fff), mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);
  if (!(wide ? s2cr & 1u << 10 : smr >> 31)) {
   continue;
  }
  valid++;
  if (((stream ^ RADIO_STREAM) & ~(mask | RADIO_STREAM_MASK)) != 0) {
   continue;
  }
  printf("wifi: 0: match register %u covers the radio's stream: stream %#x mask %#x (%#x), %s %u; s2cr %#x)\n", n, stream,
      mask, smr, kinds[(s2cr >> 16) & 3], s2cr & 0xff, s2cr);
  covering++;
 }
 printf("wifi: 0: %u of the %u match registers are in use; %u cover stream %#x%s\n", valid, matches, covering, RADIO_STREAM,
     covering ? "" : ": THE BOOT CHAIN HAS SET NOTHING UP FOR THE RADIO'S STREAM");
}
static uint64_t radio_addresses = 0xa0000000ull;
#define RADIO_ADDRESSES radio_addresses
#define RADIO_ADDRESSES_SIZE 0x10000000u
#define RADIO_TABLES 32
#define RADIO_PTE (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define RADIO_PTE_DEVICE (RADIO_PTE | 2ull << 2 | 1ull << 53 | 1ull << 54)
#define RADIO_MSI_REGISTER 0x17a10040ull
#define RADIO_FIRST_SPI 768u
#define PTE_ADDRESS 0x0000fffffffff000ull
#define SMMU_SGFSR 0x048
#define SMMU_SGFSYNR0 0x050
#define SMMU_SGFSYNR1 0x054
#define SMMU_CBAR(n) (4 * (n))
#define SMMU_CBAR_STAGE_1 (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n) (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT 1
#define SMMU_CB_SCTLR 0x00
#define SMMU_CB_TCR2 0x10
#define SMMU_CB_TTBR0 0x20
#define SMMU_CB_TTBR1 0x28
#define SMMU_CB_TCR 0x30
#define SMMU_CB_MAIR0 0x38
#define SMMU_CB_MAIR1 0x3c
#define SMMU_CB_FSR 0x58
#define SMMU_CB_FAR 0x60
#define SMMU_CB_FSYNR0 0x68
#define SMMU_CB_TLBIASID 0x610
#define SMMU_CB_TLBSYNC 0x7f0
#define SMMU_CB_TLBSTATUS 0x7f4
#define SMMU_SCTLR_ON (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2 (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0 0xf404ff44
#define SMMU_FSR_ALL 0xc00001fe
static struct {
 uint32_t wanted;
 bool found, on;
 uint32_t page, pages;
 uint32_t match, context;
 uint32_t old_smr, old_s2cr, old_sctlr;
 vm_offset_t tables;
 uint32_t table_count;
 uint64_t next;
} radio_space;
static struct block apps_smmu_second = { "apps smmu", 0, 0x1000 };
static struct block apps_smmu_context = { "apps smmu, the radio's context", 0, 0x1000 };
static uint64_t *
radio_table(uint32_t n)
{
 return (uint64_t *)(radio_space.tables + 4096 * n);
}
static uint64_t *
radio_next(uint64_t *entry)
{
 uint64_t *table;
 if (*entry & 1) {
  for (uint32_t i = 0; i < radio_space.table_count; i++) {
   if (kvtophys((vm_offset_t)radio_table(i)) == (*entry & PTE_ADDRESS)) {
    return radio_table(i);
   }
  }
  return NULL;
 }
 if (radio_space.table_count == RADIO_TABLES) {
  return NULL;
 }
 table = radio_table(radio_space.table_count++);
 *entry = kvtophys((vm_offset_t)table) | 3;
 return table;
}
static bool
radio_enter(uint64_t address, uint64_t physical, uint64_t size, uint64_t attributes)
{
 if (radio_space.tables == 0) {
  if (kmem_alloc(kernel_map, &radio_space.tables, round_page(4096 * RADIO_TABLES), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
      VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
   return false;
  }
  radio_space.table_count = 1;
 }
 for (uint64_t offset = 0; offset < size; offset += 4096) {
  uint64_t at = address + offset, *level2 = radio_next(&radio_table(0)[at >> 30 & 0x1ff]);
  uint64_t *level3 = level2 == NULL ? NULL : radio_next(&level2[at >> 21 & 0x1ff]);
  if (level3 == NULL) {
   printf("wifi: %u: no page table left for the radio's %#llx\n", step, at);
   return false;
  }
  level3[at >> 12 & 0x1ff] = (physical + offset) | attributes;
 }
 for (vm_offset_t line = radio_space.tables; line < radio_space.tables + 4096 * RADIO_TABLES; line += 64) {
  __asm__ volatile ("dc civac, %0" : : "r" (line));
 }
 settle();
 return true;
}
static bool
radio_space_find(void)
{
 bool used[256] = { false };
 uint32_t scr0 = get(&apps_smmu, "scr0", SMMU_SCR0), idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
 uint32_t idr1 = get(&apps_smmu, "idr1", SMMU_IDR1), matches = idr0 & 0xff, contexts = idr1 & 0xff;
 if (scr0 & 1u << 3) {
  printf("wifi: %u: this SMMU numbers its streams with 16 bits, which nothing here is written for\n", step);
  return false;
 }
 radio_space.page = idr1 >> 31 ? 0x10000 : 0x1000;
 radio_space.pages = 2u << (idr1 >> 28 & 7);
 if (2 * radio_space.pages * radio_space.page != 0x100000) {
  printf("wifi: %u: the SMMU says it has 2 x %u pages of %#x bytes, which is not the megabyte the device tree gives "
      "it: its contexts are not looked for\n", step, radio_space.pages, radio_space.page);
  return false;
 }
 radio_space.match = ~0u;
 for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
  uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
  if (smr >> 31) {
   printf("wifi: %u: match register %u is in use: stream %#x mask %#x, s2cr %#x\n", step, n, smr & 0x7fff,
       smr >> 16 & 0x7fff, s2cr);
   used[s2cr & 0xff] = true;
  } else if (radio_space.match == ~0u) {
   radio_space.match = n;
   radio_space.old_smr = smr;
   radio_space.old_s2cr = s2cr;
  }
 }
 for (radio_space.context = 0; radio_space.context < contexts && used[radio_space.context]; radio_space.context++) {
 }
 if (radio_space.match == ~0u || radio_space.context >= contexts) {
  printf("wifi: %u: the SMMU has no match register or no context left\n", step);
  return false;
 }
 apps_smmu_second.phys = apps_smmu.phys + radio_space.page;
 apps_smmu_context.phys = apps_smmu.phys + (radio_space.pages + radio_space.context) * radio_space.page;
 printf("wifi: %u: free: match register %u (smr %#x, s2cr %#x) and context %u, whose registers are at %#x; the SMMU's "
     "pages are of %#x bytes, %u before the contexts'\n", step, radio_space.match, radio_space.old_smr,
     radio_space.old_s2cr, radio_space.context, apps_smmu_context.phys, radio_space.page, radio_space.pages);
 get(&apps_smmu_second, "cbar", SMMU_CBAR(radio_space.context));
 get(&apps_smmu_second, "cba2r", SMMU_CBA2R(radio_space.context));
 radio_space.old_sctlr = get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
 get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0);
 get(&apps_smmu_context, "fsr", SMMU_CB_FSR);
 if (radio_space.old_sctlr & 1) {
  printf("wifi: %u: CONTEXT %u IS SWITCHED ON although no stream is sent through it: it is somebody's, and is left "
      "alone\n", step, radio_space.context);
  return false;
 }
 radio_space.found = true;
 return true;
}
static void
radio_space_faults(void)
{
 uint32_t status, global;
 if (!radio_space.on) {
  return;
 }
 status = rd(apps_smmu_context.base + SMMU_CB_FSR);
 global = rd(apps_smmu.base + SMMU_SGFSR);
 if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
  printf("wifi: %u: the SMMU has refused the radio nothing (context %u: fsr %#x; sgfsr 0)\n", step, radio_space.context,
      status);
  return;
 }
 tell("wifi: %u: THE SMMU HAS REFUSED A REQUEST: context %u: status %#x, address %#llx, syndrome %#x; the SMMU as a whole: "
     "status %#x, syndromes %#x %#x (the second names the stream)\n", step, radio_space.context, status,
     (uint64_t)rd(apps_smmu_context.base + SMMU_CB_FAR + 4) << 32 | rd(apps_smmu_context.base + SMMU_CB_FAR),
     rd(apps_smmu_context.base + SMMU_CB_FSYNR0), global, rd(apps_smmu.base + SMMU_SGFSYNR0),
     rd(apps_smmu.base + SMMU_SGFSYNR1));
}
static bool
radio_space_on(void)
{
 uint32_t smr = SMMU_SMR(radio_space.match), s2cr = SMMU_S2CR(radio_space.match), sees = 1u << 31 | RADIO_STREAM_MASK << 16 |
     RADIO_STREAM;
 uint64_t root = kvtophys(radio_space.tables) | (uint64_t)radio_space.context << 48;
 printf("wifi: %u: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets page tables of ours (root %#llx) and is switched on, and "
     "match register %u sends stream %#x mask %#x through it; before: smr %#x, s2cr %#x, the context's sctlr %#x\n", step,
     radio_space.context, root & PTE_ADDRESS, radio_space.match, RADIO_STREAM, RADIO_STREAM_MASK, radio_space.old_smr,
     radio_space.old_s2cr, radio_space.old_sctlr);
 sleep_ms(pause_ms);
 put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
 put(&apps_smmu_second, "cba2r", SMMU_CBA2R(radio_space.context), SMMU_CBA2R_64_BIT);
 put(&apps_smmu_second, "cbar", SMMU_CBAR(radio_space.context), SMMU_CBAR_STAGE_1);
 put(&apps_smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
 put(&apps_smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
 put(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)root);
 put(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
 put(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, 0);
 put(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
 put(&apps_smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
 put(&apps_smmu_context, "mair1", SMMU_CB_MAIR1, 0);
 put(&apps_smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
 put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, radio_space.context);
 put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
 for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
  if (ms == 100) {
   printf("wifi: %u: the SMMU does not finish forgetting old translations\n", step);
   return false;
  }
  sleep_ms(1);
 }
 put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
 radio_space.on = true;
 put(&apps_smmu, "s2cr", s2cr, radio_space.context);
 put(&apps_smmu, "smr", smr, sees);
 if (get(&apps_smmu, "smr", smr) != sees || (get(&apps_smmu, "s2cr", s2cr) & 0x300ff) != radio_space.context ||
     (get(&apps_smmu_second, "cbar", SMMU_CBAR(radio_space.context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
     !(get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR) & 1) ||
     get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0) != (uint32_t)root ||
     get(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
  printf("wifi: %u: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x is not going through context %u\n", step,
      RADIO_STREAM, radio_space.context);
  return false;
 }
 get(&apps_smmu_context, "tcr", SMMU_CB_TCR);
 get(&apps_smmu_context, "tcr2", SMMU_CB_TCR2);
 get(&apps_smmu_second, "cba2r", SMMU_CBA2R(radio_space.context));
 get(&apps_smmu, "sgfsr", SMMU_SGFSR);
 printf("wifi: %u: THE RADIO'S STREAM GOES THROUGH CONTEXT %u of the SMMU, with our page tables\n", step, radio_space.context);
 radio_space_faults();
 return true;
}
static void
radio_space_off(void)
{
 if (!radio_space.on) {
  return;
 }
 radio_space_faults();
 printf("wifi: %u: the radio's stream and context %u of the SMMU are put back as they were found\n", step, radio_space.context);
 put(&apps_smmu, "smr", SMMU_SMR(radio_space.match), radio_space.old_smr);
 put(&apps_smmu, "s2cr", SMMU_S2CR(radio_space.match), radio_space.old_s2cr);
 put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, radio_space.old_sctlr);
 get(&apps_smmu, "smr", SMMU_SMR(radio_space.match));
 get(&apps_smmu, "s2cr", SMMU_S2CR(radio_space.match));
 get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
 radio_space.on = false;
}
static bool
radio_map(uint64_t physical, uint64_t size, uint64_t attributes, unsigned long long *address)
{
 size = (size + 4095) & ~4095ull;
 if (radio_space.next + size > RADIO_ADDRESSES_SIZE) {
  return false;
 }
 *address = RADIO_ADDRESSES + radio_space.next;
 radio_space.next += size;
 if (whole) {
  radio_space.wanted = 2;
 }
 if (radio_space.wanted != 2) {
  printf("wifi: %u: THE RADIO IS NOT SHOWN the %llu KB of ours at %#llx that the driver will know as %#llx: that takes "
      "a context of the SMMU (wifi_smmu=2)\n", step, size >> 10, physical, *address);
  return true;
 }
 if ((!radio_space.found && !radio_space_find()) || !radio_enter(*address, physical, size, attributes)) {
  return false;
 }
 printf("wifi: %u: the radio is to see %llu KB of ours (%#llx) at %#llx\n", step, size >> 10, physical, *address);
 if (!radio_space.on) {
  return radio_space_on();
 }
 put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, radio_space.context);
 put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
 return true;
}
bool
redwood_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
 return radio_map(physical, size, RADIO_PTE, address);
}
bool
redwood_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi)
{
 static unsigned long long page;
 if (page == 0) {
  tell("wifi: wifi_irq: THE RADIO'S SMMU CONTEXT GETS AN ENTRY for the GIC's register that raises an interrupt (%#llx), "
      "as a device's page\n", RADIO_MSI_REGISTER);
  if (!radio_map(RADIO_MSI_REGISTER & ~4095ull, 4096, RADIO_PTE_DEVICE, &page)) {
   page = 0;
   return false;
  }
 }
 *address = page + (RADIO_MSI_REGISTER & 4095);
 *data = RADIO_FIRST_SPI + 32;
 *first_spi = RADIO_FIRST_SPI + 32;
 tell("wifi: wifi_irq: the radio raises its interrupt vector v by writing %u + v at %#llx\n", *data, *address);
 return true;
}
static void
radio_space_try(void)
{
 bool as_found;
 if (!radio_space_find() || !radio_enter(RADIO_ADDRESSES, 0, 0, RADIO_PTE)) {
  return;
 }
 as_found = radio_space_on();
 sleep_ms(pause_ms);
 radio_space_off();
 printf("wifi: %u: %s\n", step, as_found ? "THE SMMU TOOK A CONTEXT FOR THE RADIO'S STREAM, and it has been taken back" :
     "THE SMMU DID NOT TAKE A CONTEXT FOR THE RADIO'S STREAM");
}
int
redwood_wifi_driver_calibrated(void)
{
 if (!macan || !dsp_started || !glink.open || !wlan_powered)
  return EIO;
 printf("wifi: cold calibration complete: cycling the WCN7750 before mission mode\n");
 macan_wlan_power(0);
 sleep_ms(100);
 macan_wlan_power(1);
 printf("wifi: WCN7750 power restored; WPSS and QMI link retained\n");
 return 0;
}
bool
redwood_wifi_dsp_is_up(void)
{
 printf("wifi: %u: the driver asks for the firmware's processor: it %s\n", step, dsp_started && glink.open ? "has been up "
     "since step 11, with the link of step 13" : "IS NOT UP");
 return dsp_started && glink.open;
}
const uint8_t *
redwood_wifi_driver_file(const char *name, uint64_t *size)
{
 static const char *const files[][2] = { { "board.bin", "bd_m20gl.elf" }, { "regdb.bin", "regdb_xiaomi.bin" },
     { "m3.bin", "phy_ucode.elf" }, { "aux_ucode.bin", "aux_ucode.elf" } };
 const char *last = strrchr(name, '/');
 last = last == NULL ? name : last + 1;
 for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
  if (strcmp(last, files[i][0]) == 0) {
   return image_file(files[i][1], size);
  }
 }
 return macan ? image_file(last, size) : NULL;
}
bool
redwood_wifi_radio_address(uint8_t *address)
{
 char text[32] = "";
 uint8_t bytes[6];
 unsigned count = 0, digits = 0, value = 0;
 bool good;
 if (!PE_parse_boot_arg_str("wifi_mac", text, sizeof(text)) || text[0] == 0) {
  printf("wifi: %u: the driver asks for the radio's address: none was given (wifi_mac)\n", step);
  return false;
 }
 good = true;
 for (const char *c = text;; c++) {
  int digit = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ?
      *c - 'A' + 10 : -1;
  if (digit >= 0 && digits < 2) {
   value = value << 4 | (unsigned)digit;
   digits++;
  } else if ((*c == ':' || *c == 0) && digits != 0 && count < 6) {
   bytes[count++] = (uint8_t)value;
   digits = value = 0;
  } else {
   good = false;
  }
  if (*c == 0 || !good) {
   break;
  }
 }
 good = good && count == 6 && !(bytes[0] & 1) && (bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4] | bytes[5]) != 0;
 printf("wifi: %u: the driver asks for the radio's address: one was given, %lu characters, %s\n", step,
     (unsigned long)strlen(text), good ? "six bytes: it is the radio's for this run" : "WHICH IS NOT A STATION'S ADDRESS: "
     "not used");
 if (good) {
  memcpy(address, bytes, sizeof(bytes));
 }
 return good;
}
bool
redwood_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
 uint64_t size = 0;
 const uint8_t *file = image_file("network", &size), *end = file + size, *line = file;
 unsigned lengths[2] = { 0, 0 }, sizes[2] = { *name_length, *passphrase_length };
 uint8_t *where[2] = { name, passphrase };
 if (file == NULL) {
  printf("wifi: %u: which network to join: none was given (fw/network of the small image)\n", step);
  return false;
 }
 for (unsigned n = 0; n < 2 && line < end; n++) {
  const uint8_t *stop = line;
  while (stop < end && *stop != '\n') {
   stop++;
  }
  lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
  if (lengths[n] > sizes[n]) {
   lengths[n] = 0;
  } else {
   memcpy(where[n], line, lengths[n]);
  }
  line = stop + 1;
 }
 printf("wifi: %u: which network to join: a name of %u bytes and a passphrase of %u bytes were given%s\n", step,
     lengths[0], lengths[1], lengths[0] == 0 || lengths[1] < 8 || lengths[1] > 63 ? ": NOT A NAME OF 1 TO 32 BYTES AND A "
     "PASSPHRASE OF 8 TO 63" : "");
 if (lengths[0] == 0 || lengths[1] < 8 || lengths[1] > 63) {
  return false;
 }
 *name_length = lengths[0];
 *passphrase_length = lengths[1];
 return true;
}
static bool(*const steps[])(void) = {
 step_smem, step_mailbox, step_rpmh, step_rails, step_pins, step_firmware, step_mailbox_open, step_header, step_image,
 step_our_item, step_start, step_queues, step_link, step_qrtr, step_exchange, step_registers,
};
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))
void
redwood_wifi_stop(void)
{
 for (unsigned ms = 0; whole_runs && ms < 40000; ms += 10) {
  sleep_ms(10);
 }
 redwood_apple80211_summary();
 lck_mtx_lock(&dsp_lock);
 if (dsp_started && !dirty) {
  bool down = dsp_shutdown("the run is about to end");
  if (whole) {
   tell("wifi: the radio is off: its DSP %s, and TrustZone %s\n", stopped_by_its_word ? "stopped when asked" :
       "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
  }
 }
 lck_mtx_unlock(&dsp_lock);
}
void
redwood_wifi_quiesce(void)
{
 redwood_apple80211_summary();
 if (!dsp_started || dirty) {
  return;
 }
 leaving = true;
 new_from = ~0u;
 dsp_shutdown("the kernel is on its way back to the loader");
}
#define REDWOOD_SOC_BASE 0x00400000ul
extern vm_offset_t pe_arm_get_soc_base_phys(void);
static void
steps_over(uint32_t done)
{
 lck_mtx_lock(&dsp_lock);
 if (dsp_started && dirty) {
  printf("wifi: THE DSP IS LEFT RUNNING (wifi_dirty), as a kernel that died would leave it\n");
 } else if (dsp_started && done == last_step && redwood_wifi_option("wifi_keep")) {
  printf("wifi: THE DSP STAYS ON while the system runs (wifi_keep)%s\n", link_runs ? ", and the link with it" : "");
 } else if (dsp_started) {
  dsp_shutdown("the steps are over");
 }
 if (!dsp_started) {
  radio_space_off();
 }
 lck_mtx_unlock(&dsp_lock);
}
static void
join_given(uint64_t began)
{
 static struct redwood_sta_join what;
 struct redwood_sta_state *state;
 unsigned name_length = sizeof(what.ssid), passphrase_length = sizeof(what.passphrase);
 uint64_t size = 0;
 uint32_t given = 1;
 int error;
 if (image_file("network", &size) == NULL) {
  tell("wifi: no network to join was given (fw-wifi/network, see the README): the radio waits to be asked\n");
  return;
 }
 if (PE_parse_boot_argn("wifi_file", &given, sizeof(given)) && given == 0) {
  tell("wifi: the network of the file is not joined at the start (wifi_file=0): the radio waits to be asked\n");
  return;
 }
 bzero(&what, sizeof(what));
 if (!redwood_wifi_network(what.ssid, &name_length, what.passphrase, &passphrase_length)) {
  tell("wifi: the network given in fw/network is not a name and a passphrase: nothing is joined\n");
  return;
 }
 what.ssid_length = (uint8_t)name_length;
 what.passphrase_length = (uint8_t)passphrase_length;
 what.security = REDWOOD_STA_WPA2_PSK;
 uint32_t open_test = 0;
 if (PE_parse_boot_argn("wifi_open_test", &open_test, sizeof(open_test)) && open_test == 1) {
  what.security = REDWOOD_STA_OPEN;
 }
 error = redwood_sta_ask_join_standing(&what);
 bzero(&what, sizeof(what));
 for (unsigned waited = 0; error == 0 && redwood_sta_busy() && waited < 300; waited++) {
  sleep_ms(100);
 }
 if ((state = kalloc_data(sizeof(*state), Z_WAITOK)) == NULL) {
  return;
 }
 redwood_sta_state(state);
 if (state->phase == REDWOOD_STA_JOINED) {
  tell("wifi: THE NETWORK OF THE FILE IS JOINED, %llu.%llu s after the kernel began on the radio\n", (now_us() - began) /
      1000000, (now_us() - began) / 100000 % 10);
 } else {
  tell("wifi: THE NETWORK OF THE FILE IS NOT JOINED (%d, %d): the radio waits to be asked. The last of what was said:\n",
      error, state->last_error);
  say_kept();
 }
 kfree_data(state, sizeof(*state));
}
static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
 static const uint8_t needed[] = { 1, 2, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
 uint64_t began = now_us();
 uint32_t done = 0, failed = 0;
 if (redwood_lx_prepare() != 0) {
  tell("wifi: NO MEMORY FOR THE RADIO in one piece: it is not brought up\n");
  whole_runs = false;
  thread_terminate(current_thread());
 }
 for (unsigned i = 0; i < sizeof(needed) && failed == 0; i++) {
  step = needed[i];
  if (step == 15) {
   tell("wifi: the radio's DSP is up and linked, %llu ms in; now the driver\n", (now_us() - began) / 1000);
  }
  if (steps[step - 1]()) {
   done = step;
  } else {
   failed = step;
  }
 }
 last_step = needed[sizeof(needed) - 1];
 if (failed == 0) {
  tell("wifi: THE RADIO IS UP, %llu.%llu s after the kernel began on it\n", (now_us() - began) / 1000000,
      (now_us() - began) / 100000 % 10);
  join_given(began);
 } else {
  tell("wifi: THE RADIO DID NOT COME UP: step %u failed, %llu s in. The last of what was said on the way:\n", failed,
      (now_us() - began) / 1000000);
  say_kept();
 }
 steps_over(failed == 0 ? last_step : 0);
 step = done;
 whole_runs = false;
 thread_terminate(current_thread());
}
void
redwood_wifi_init(void)
{
 uint32_t on = 0, back = 0, done = 0;
 uint64_t size = 0;
 thread_t thread;
 PE_parse_boot_argn("wifi_step", &last_step, sizeof(last_step));
 if (PE_parse_boot_argn("macan", &on, sizeof(on)) && on) {
  macan = true;
  smem_phys = 0x81d00000UL;
  cmd_db_phys = 0x81c60000UL;
  wpss_memory = 0x99f00000u;
  ipcc_wpss = 0x11u;
  ipcc.phys = 0x01106000;
  radio_stream = 0x1400;
  radio_addresses = 0xb0000000ull;
  on = 0;
 }
 if (last_step == 0 && (!PE_parse_boot_argn("wifi", &on, sizeof(on)) || on == 0)) {
  return;
 }
 if (last_step == 0 && on != 1) {
  printf("wifi: wifi=%u is not understood: wifi=1 turns the radio on, wifi_step=N goes the first N steps of that\n", on);
  return;
 }
 if (pe_arm_get_soc_base_phys() != REDWOOD_SOC_BASE) {
  if (last_step != 0 && PE_parse_boot_argn("wifi_lx", &back, sizeof(back)) && back) {
   printf("wifi: this is not the phone; with wifi_lx the driver is started all the same, for what it does before "
       "it needs a radio\n");
   printf("wifi: the driver's start returns %d\n", redwood_lx_start());
  } else {
   printf("wifi: wifi=1 and wifi_step=N are for the phone's radio, and this is not the phone\n");
  }
  return;
 }
 redwood_qmi_set_transport(&qrtr_ops);
 if (last_step == 0) {
  if (image_file("wpss.mdt", &size) == NULL) {
   printf("wifi: wifi=1, but the radio's firmware is not in the small image (fw-wifi/, see the README): no Wi-Fi\n");
   return;
  }
  whole = whole_runs = true;
  quiet = !(PE_parse_boot_argn("wifi_loud", &back, sizeof(back)) && back);
  redwood_lx_net_early();
  pause_ms = watch_seconds = 0;
  new_from = STEPS + 1;
  if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
   whole_runs = false;
   tell("wifi: no thread: the radio is not brought up\n");
   return;
  }
  thread_deallocate(thread);
  tell("wifi: the radio is being brought up beside the system's start (wifi=1)\n");
  return;
 }
 if (last_step > STEPS) {
  last_step = STEPS;
 }
 new_from = last_step;
 PE_parse_boot_argn("wifi_new", &new_from, sizeof(new_from));
 PE_parse_boot_argn("wifi_pause", &pause_ms, sizeof(pause_ms));
 PE_parse_boot_argn("wifi_fine", &fine, sizeof(fine));
 PE_parse_boot_argn("wifi_watch", &watch_seconds, sizeof(watch_seconds));
 PE_parse_boot_argn("wifi_dirty", &dirty, sizeof(dirty));
 printf("wifi: steps up to %u; those from %u on are new and wait %u ms %s\n", last_step, new_from, pause_ms,
     fine ? "before every write and call" : "after each announcement");
 if ((radio_space.wanted = redwood_wifi_option("wifi_smmu")) != 0) {
  smmu_look();
  if (radio_space.wanted == 2 && last_step < 15) {
   uint32_t known_from = new_from;
   new_from = 0;
   radio_space_try();
   new_from = known_from;
  }
 }
 for (step = 1; step <= last_step; step++) {
  if (!steps[step - 1]()) {
   printf("wifi: step %u: FAILED; the steps after it are not attempted\n", step);
   break;
  }
  printf("wifi: step %u: done\n", step);
  done = step;
 }
 step = done;
 new_from = STEPS + 1;
 steps_over(done);
 printf("wifi: over after step %u; %s\n", done, last_write);
 back = 0;
 if (PE_parse_boot_argn("wifi_return", &back, sizeof(back)) && back) {
  printf("wifi: back to the loader (wifi_return)\n");
  loader_return();
 }
 printf("wifi: the system goes on starting\n");
}
