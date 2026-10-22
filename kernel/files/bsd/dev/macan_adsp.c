#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <sys/macan_firmware.h>
#include <sys/macan_dsp.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);
static LCK_GRP_DECLARE(adsp_lock_group, "macan-adsp");
#define STEPS 11
static uint32_t pause_ms, listen_seconds = 3;
static uint32_t step;
static uint32_t new_from;
static char last_write[96] = "nothing written";
static bool leaving;
extern void loader_log_putc(char c);
static LCK_MTX_DECLARE(say_lock, &adsp_lock_group);
static void
say_line(const char *line)
{
 if (leaving) {
  for (const char *c = line; *c != 0; c++) {
   loader_log_putc(*c);
  }
  return;
 }
 lck_mtx_lock(&say_lock);
 for (const char *c = line; *c != 0; c++) { loader_log_putc(*c); }
 printf("%s", line);
 lck_mtx_unlock(&say_lock);
}
static int say(const char *format, ...) __printflike(1, 2);
static int
say(const char *format, ...)
{
 char line[400];
 va_list list;
 int length;
 va_start(list, format);
 length = vsnprintf(line, sizeof(line), format, list);
 va_end(list);
 say_line(line);
 return length;
}
#define printf say
static void
sleep_us(uint32_t us)
{
 if (leaving) {
  uint64_t ticks, start = __builtin_arm_rsr64("CNTVCT_EL0");
  ticks = (__builtin_arm_rsr64("CNTFRQ_EL0") * us + 999999) / 1000000;
  while (__builtin_arm_rsr64("CNTVCT_EL0") - start < ticks) { __asm__ volatile ("yield"); }
 } else { delay_for_interval(us, NSEC_PER_USEC); }
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
hold(void)
{
 if (step >= new_from && pause_ms && !leaving) {
  sleep_ms(pause_ms);
 }
}
static void
announce(const char *what)
{
 printf("adsp: step %u: %s\n", step, what);
 hold();
}
static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
 uint32_t value;
 need(b);
 value = rd(b->base + offset);
 printf("adsp: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
 return value;
}
static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
 need(b);
 printf("adsp: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
 snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
 wr(b->base + offset, value);
}
#define SMEM_PHYS 0x81d00000UL
#define SMEM_SIZE 0x00200000u
#define SMEM_US 0
#define SMEM_ADSP 2
#define SMEM_GLOBAL 0xfffe
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
#define ITEM_SMP2P_OURS 429
#define ITEM_SMP2P_THEIRS 443
#define ITEM_CRASH_REASON 423
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
smem_word(uint32_t offset)
{
 return rd((vm_offset_t)smem + offset);
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
 struct smem_item found[256];
 static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_ADSP }, { SMEM_GLOBAL, SMEM_GLOBAL } };
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
static void
show_partition(void)
{
 static struct smem_item found[256];
 struct smem_partition header;
 uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
 unsigned count;
 char line[320];
 if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
  printf("adsp: %u: there is no partition for us and the audio DSP (0 and 2)\n", step);
  return;
 }
 count = smem_items(partition, cacheline, found, 256);
 printf("adsp: %u: the partition for us and the audio DSP (0 and 2): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
     SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
 for (unsigned i = 0; i < count && i < 256;) {
  int at = 0;
  for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
   at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
  }
  printf("adsp: %u:   items (bytes):%s\n", step, line);
 }
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
#define OUR_ENTRY_STOP "master-kernel"
#define DSP_STOP (1u << 0)
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
 printf("adsp: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
     words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}
static void
show_crash_reason(void)
{
 char text[81];
 uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);
 if (!at || !size) {
  printf("adsp: %u: item 423, the DSP's last words: not there\n", step); return;
 }
 printf("adsp: %u: THE AUDIO DSP'S LAST WORDS (item 423, %u bytes):\n", step, size);
 uint32_t bound = size < 4096 ? size : 4096;
 for (uint32_t offset = 0; offset < bound; offset += 80) {
  uint32_t length = bound - offset < 80 ? bound - offset : 80;
  bzero(text, sizeof(text));
  if (!smem_peek(at + offset, text, length)) { break; }
  printf("adsp: crash423 +%u: %s\n", offset, text[0] ? text : "(empty)");
  if (strnlen(text, length) < length) { return; }
 }
 if (size > bound) { printf("adsp: crash423 truncated at %u bytes\n", bound); }
}
#define QMP_MAGIC 0x00
#define QMP_MAGIC_VALUE 0x4d41494c
#define QMP_VERSION 0x04
#define QMP_THEIR_LINK 0x0c
#define QMP_THEIR_LINK_ACK 0x10
#define QMP_THEIR_CHANNEL 0x14
#define QMP_THEIR_CHANNEL_ACK 0x18
#define QMP_OUR_LINK 0x24
#define QMP_OUR_LINK_ACK 0x28
#define QMP_OUR_CHANNEL 0x2c
#define QMP_OUR_CHANNEL_ACK 0x30
#define QMP_OUR_BOX_SIZE 0x34
#define QMP_OUR_BOX 0x38
#define QMP_UP 0x0000ffffu
#define QMP_DOWN 0xffff0000u
#define QMP_MESSAGE 64
static struct block aoss = { "mailbox", 0x0c300000, 0x400 };
#define IPCC_SEND 0x0c
#define IPCC_RECEIVED 0x10
#define IPCC_ENABLE 0x14
#define IPCC_DISABLE 0x18
#define IPCC_CLEAR 0x1c
#define IPCC_NOBODY 0xffffffffu
#define IPCC_AOP 0u
#define IPCC_LPASS 3u
#define IPCC_GLINK 0u
#define IPCC_SMP2P 2u
static struct block ipcc = { "ipcc", 0x01106000, 0x1000 };
static struct {
 uint32_t glink, smp2p, said;
 uint64_t since;
} doorbell;
static void
doorbells(void)
{
 for (unsigned i = 0; i < 32; i++) {
  uint32_t who = macan_ipcc_take(ipcc.base, IPCC_LPASS);
  if (who == IPCC_NOBODY) { return; }
  doorbell.glink += (who & 0xffff) == IPCC_GLINK;
  doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
 }
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
ring_aop(void)
{
 wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
}
static bool
mailbox_open(void)
{
 uint32_t box, size;
 need(&aoss);
 need(&ipcc);
 if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
  printf("adsp: %u: not a mailbox this knows\n", step);
  return false;
 }
 box = get(&aoss, "our mailbox", QMP_OUR_BOX);
 size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
 if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
  printf("adsp: %u: our mailbox is not where a message fits\n", step);
  return false;
 }
 if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
     rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
     rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
  printf("adsp: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
  our_box = box;
  return true;
 }
 put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
 put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
 ring_aop();
 if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
  printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
  put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
  ring_aop();
  return false;
 }
 put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
 ring_aop();
 if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
  printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
  put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
  put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
  ring_aop();
  return false;
 }
 put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
 ring_aop();
 if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
  printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
  put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
  put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
  ring_aop();
  return false;
 }
 printf("adsp: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
 our_box = box;
 return true;
}
static bool
mailbox_send(const char *text)
{
 uint32_t words[QMP_MESSAGE / 4] = { 0 };
 bool sent = false;
 if (leaving) {
  if (!lck_mtx_try_lock(&macan_aop_mailbox_lock)) { printf("adsp: QMP busy during quiesce\n"); return false; }
 } else { lck_mtx_lock(&macan_aop_mailbox_lock); }
 if (our_box == 0 && (leaving || !mailbox_open())) {
  goto out;
 }
 strlcpy((char *)words, text, sizeof(words));
 printf("adsp: %u: about to tell the always-on processor \"%s\"\n", step, text);
 snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
 if (rd(aoss.base + our_box) != 0) {
  printf("adsp: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
  goto out;
 }
 for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
  wr(aoss.base + our_box + 4 + 4 * i, words[i]);
 }
 wr(aoss.base + our_box, QMP_MESSAGE);
 (void)rd(aoss.base + our_box);
 ring_aop();
 if (!mailbox_wait(our_box, 0, 1000)) {
  printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
  wr(aoss.base + our_box, 0);
  goto out;
 }
 printf("adsp: %u: the always-on processor has taken it\n", step);
 sent = true;
out:
 lck_mtx_unlock(&macan_aop_mailbox_lock);
 return sent;
}
#define CMD_DB_PHYS 0x81c60000UL
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
static struct block rsc = { "rsc", 0x18920000, 0x10000 };
#define RSC_TCS_BASE 0xd00
#define RSC_IRQ_ENABLE (RSC_TCS_BASE + 0x00)
#define RSC_IRQ_STATUS (RSC_TCS_BASE + 0x04)
#define RSC_IRQ_CLEAR (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n) (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE 0x10
#define TCS_CONTROL 0x14
#define TCS_STATUS 0x18
#define TCS_ENABLED 0x1c
#define TCS_COMMAND(c) (0x30 + 20 * (c))
#define TCS_ACTIVE_MODE (1u << 16)
#define TCS_TRIGGER (1u << 24)
#define MSGID_WRITE_WAITED (8u | 1u << 8 | 1u << 16)
#define RPMH_TCS 1
static uint32_t adsp_vote = 1;
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
 char id[8] = { 0 };
 if (cmd_db == NULL) {
  cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
 }
 if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
  printf("adsp: %u: the boot loader's command database is not there\n", step);
  return -1;
 }
 strlcpy(id, name, sizeof(id));
 for (unsigned s = 0; s < 8; s++) {
  const struct cmd_db_slave *slave = &cmd_db->slave[s];
  const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);
  if (slave->id == 0 || slave->count > 512 ||
      (uint64_t)slave->header_offset + slave->count * sizeof(*entry) > CMD_DB_SIZE - sizeof(*cmd_db)) {
   continue;
  }
  for (unsigned i = 0; i < slave->count; i++) {
   const uint8_t *data;
   char levels[160];
   int at;
   if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
    continue;
   }
   if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
    return -1;
   }
   data = cmd_db->data + slave->data_offset + entry[i].offset;
   at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
   for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
    unsigned value = data[2 * l] | data[2 * l + 1] << 8;
    if (l > 0 && value == 0) {
     break;
    }
    at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
   }
   printf("adsp: %u: RPMh has %s\n", step, levels);
   for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
    if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
     *address = entry[i].address;
     return (int)l;
    }
   }
   return -1;
  }
 }
 printf("adsp: %u: RPMh has no \"%s\"\n", step, name);
 return -1;
}
static bool rpmh_failed;
static bool
rpmh_write(const uint32_t *addresses, const uint32_t *values, unsigned count)
{
 bool done = false;
 if (count == 0 || count > 16) { return false; }
 if (leaving) {
  if (!lck_mtx_try_lock(&macan_rpmh_lock)) { return false; }
 } else { lck_mtx_lock(&macan_rpmh_lock); }
 need(&rsc);
 uint32_t t = RSC_TCS(RPMH_TCS);
 uint32_t major = (rd(rsc.base) >> 16) & 0xff;
 uint32_t config = rd(rsc.base + 0x0c);
 uint32_t ncpt = (config >> 27) & 0x1f, tcs_count = (config >> 12) & 0x3f;
 if (rpmh_failed || (major != 2 && major != 3 && major != 4) || count > ncpt || RPMH_TCS >= tcs_count) {
  printf("adsp: RPMh refused version=%u ncpt=%u tcs_count=%u failed=%u\n", major, ncpt, tcs_count, rpmh_failed);
  goto out;
 }
 uint32_t wait_reg = major >= 3 ? 0x20 : 0x10;
 uint32_t control_reg = major >= 3 ? 0x24 : 0x14;
 uint32_t status_reg = major >= 3 ? 0x28 : 0x18;
 uint32_t enable_reg = major >= 3 ? 0x2c : 0x1c;
 uint32_t command_base = major >= 3 ? 0x34 : 0x30;
 uint32_t command_stride = major >= 3 ? 24 : 20;
 if (!(rd(rsc.base + t + status_reg) & 1u)) {
  printf("adsp: RPMh ACTIVE TCS%u busy; no writes\n", RPMH_TCS);
  goto out;
 }
 printf("adsp: RPMh version=%u layout=%s ncpt=%u tcs_count=%u ACTIVE TCS%u\n",
     major, major >= 3 ? "3.0" : "2.7", ncpt, tcs_count, RPMH_TCS);
 uint32_t irq_enable = rd(rsc.base + RSC_IRQ_ENABLE);
 wr(rsc.base + RSC_IRQ_ENABLE, irq_enable | (1u << RPMH_TCS));
 wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
 for (unsigned i = 0; i < count; i++) {
  wr(rsc.base + t + (command_base + command_stride * i), MSGID_WRITE_WAITED);
  wr(rsc.base + t + (command_base + command_stride * i) + 4, addresses[i]);
  wr(rsc.base + t + (command_base + command_stride * i) + 8, values[i]);
 }
 wr(rsc.base + t + wait_reg, (1u << count) - 1);
 wr(rsc.base + t + enable_reg, (1u << count) - 1);
 settle();
 uint32_t control = rd(rsc.base + t + control_reg) & ~TCS_TRIGGER;
 wr(rsc.base + t + control_reg, control);
 wr(rsc.base + t + control_reg, control | TCS_ACTIVE_MODE);
 wr(rsc.base + t + control_reg, control | TCS_ACTIVE_MODE | TCS_TRIGGER);
 for (unsigned i = 0; i < 2000; i++) {
  if (rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS)) { done = true; break; }
  sleep_us(50);
 }
 if (done) {
  wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
  wr(rsc.base + t + control_reg, control & ~TCS_ACTIVE_MODE);
  wr(rsc.base + t + enable_reg, 0);
  wr(rsc.base + RSC_IRQ_ENABLE, irq_enable);
 } else { rpmh_failed = true; }
out:
 lck_mtx_unlock(&macan_rpmh_lock);
 printf("adsp: RPMh drv2 ACTIVE TCS%u count=%u done=%u\n", RPMH_TCS, count, done);
 return done;
}
static bool
rails(uint32_t level)
{
 if (!adsp_vote) { return true; }
 uint32_t addresses[3], values[3];
 static const char *const names[] = { "lcx.lvl", "lmx.lvl" };
 for (unsigned i = 0; i < 2; i++) {
  int position = rail_position(names[i], level, &addresses[i]);
  if (position < 0) { return false; }
  values[i] = (uint32_t)position;
 }
 if (rail_position("xo.lvl", 0, &addresses[2]) < 0) { return false; }
 values[2] = 3;
 return rpmh_write(addresses, values, 3);
}
static bool icc_asked;
static bool
interconnect(bool on)
{
 if (!adsp_vote) { return true; }
 static const char *const names[] = { "LP0", "CE0", "SN2", "SN0", "SH0", "MC0", "ACV" };
 uint32_t addresses[7], values[7];
 for (unsigned n = 0; n < 7; n++) {
  char id[8] = { 0 };
  bool found = false;
  strlcpy(id, names[n], sizeof(id));
  for (unsigned s = 0; s < 8; s++) {
   const struct cmd_db_slave *slave = &cmd_db->slave[s];
   if (!slave->id || slave->count > 512 ||
       (uint64_t)slave->header_offset + slave->count * sizeof(struct cmd_db_entry) > CMD_DB_SIZE - sizeof(*cmd_db)) { continue; }
   const struct cmd_db_entry *entry = (const void *)(cmd_db->data + slave->header_offset);
   for (unsigned i = 0; i < slave->count; i++) {
    if (!memcmp(entry[i].id, id, 8)) { addresses[n] = entry[i].address; found = true; break; }
   }
  }
  if (!found) { printf("adsp: ICC missing BCM %s\n", names[n]); return false; }
  uint32_t vote = on ? 0x3fff : (n >= 3 && n <= 5 ? 1 : 0);
  values[n] = 0x40000000u | (vote ? 0x20000000u : 0) | (vote << 14) | vote;
  if (n == 6) { values[n] = 0x40000000u | (on ? 0x20000002u : 0); }
 }
 if (on) { icc_asked = true; }
 bool done = rpmh_write(addresses, values, 7);
 if (done && !on) { icc_asked = false; }
 printf("adsp: ICC rproc_ddr 22->512 crypto_ddr 45->512 %s done=%u\n", on ? "max" : "release", done);
 return done;
}
static bool
step_survey(void)
{
 struct smp2p words;
 uint32_t size = 0;
 announce("read our partition with the audio DSP in the shared memory (0x81d00000), and its items");
 show_partition();
 if (dsp_words(&words)) {
  show_words("the DSP's item (443) is there: it has run since the phone was started", &words);
 } else {
  printf("adsp: %u: the DSP's SMP2P item (443) is not there: it has not run since the phone was started\n", step);
 }
 printf("adsp: %u: our SMP2P item (429): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
     smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
     smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
 show_crash_reason();
 need(&ipcc);
 printf("adsp: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
 return smem_partition(SMEM_US, SMEM_ADSP, &size, &size) != 0;
}
#define ADSP_MEMORY 0x9f100000u
#define ADSP_MEMORY_SIZE 0x04b00000u
#define DTB_MEMORY 0x9f080000u
#define DTB_MEMORY_SIZE 0x00080000u
#define PAS_ADSP 1
#define PAS_DTB 36
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
 char path[64];
 vfs_context_t context = vfs_context_kernel();
 vnode_t vnode = NULLVP;
 struct vnode_attr attributes;
 uint8_t *data;
 int left = 0, error;
 if (rootvnode == NULLVP) {
  return NULL;
 }
 snprintf(path, sizeof(path), "/fw/%s", name);
 if (vnode_lookup(path, 0, &vnode, context) != 0) {
  return NULL;
 }
 VATTR_INIT(&attributes);
 VATTR_WANTED(&attributes, va_data_size);
 if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
     attributes.va_data_size > ADSP_MEMORY_SIZE) {
  vnode_put(vnode);
  return NULL;
 }
 data = kalloc_data(attributes.va_data_size, Z_WAITOK);
 if (data == NULL) {
  vnode_put(vnode);
  return NULL;
 }
 error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
     vfs_context_ucred(context), &left, vfs_context_proc(context));
 vnode_put(vnode);
 if (error != 0 || left != 0) {
  printf("adsp: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
  kfree_data(data, attributes.va_data_size);
  return NULL;
 }
 *size = attributes.va_data_size;
 return data;
}
static bool from_root;
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
 from_root = false;
 const uint8_t *data = macan_image_file("fw", name, size);
 if (!data) { data = root_file(name, size); from_root = data != NULL; }
 if (!data) { printf("adsp: missing /fw/%s (stage vendor firmware in system.packfs)\n", name); }
 return data;
}
static void
file_free(const uint8_t *data, uint64_t size, bool owned)
{
 if (data && owned) { void *allocation = (void *)(uintptr_t)data; kfree_data(allocation, size); }
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
#define SCM_WAITQ_SLEEP 2
static LCK_MTX_DECLARE(dsp_lock, &adsp_lock_group);
static bool rails_asked;
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
 uint64_t session = 0, call = function | SCM_64 | SCM_SIP, first_arg = first, args = arguments;
 bool resume = false;
 unsigned busy = 0, sleeps = 0;
 function = call;
 printf("adsp: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
     second, third);
 snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
 for (int i = 0; i < 100; i++) {
  register uint64_t x0 __asm__("x0") = resume ? SCM_INTERRUPTED : call;
  register uint64_t x1 __asm__("x1") = args;
  register uint64_t x2 __asm__("x2") = first_arg;
  register uint64_t x3 __asm__("x3") = second;
  register uint64_t x4 __asm__("x4") = third;
  register uint64_t x5 __asm__("x5") = 0;
  register uint64_t x6 __asm__("x6") = session;
  register uint64_t x7 __asm__("x7") = 0;
  __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
      "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  session = x6;
  if (sleeps && x0 != SCM_WAITQ_SLEEP) { printf("adsp: %u: after the resume: %d, %#llx\n", step, (int32_t)x0, x1); }
  if (x0 == SCM_INTERRUPTED) { resume = true; continue; }
  if ((int32_t)x0 == -12 && busy++ < 20) { resume = false; session = 0; sleep_us(30000); continue; }
  if (x0 == SCM_WAITQ_SLEEP && sleeps < 10000) {
   uint64_t queue = x1, context = x2;
   if (sleeps == 0) {
    printf("adsp: %u: TrustZone sleeps on wait queue %#llx (call %#llx): waiting for its wake\n", step, queue, context);
   }
   sleep_us(20000);
   sleeps += 20;
   printf("adsp: %u: WAITQ_RESUME of call %#llx after %u ms\n", step, context, sleeps);
   call = SCM_64 | SCM_SIP | SCM_CALL(0x24, 2);
   args = 1;
   first_arg = context;
   resume = false;
   session = 0;
   i = 0;
   continue;
  }
  if (x0 != SCM_INTERRUPTED) {
   printf("adsp: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
   *answer = x1;
   return (int32_t)x0;
  }
 }
 printf("adsp: %u: TrustZone keeps saying the call was interrupted\n", step);
 return -1;
}
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
   smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
  }
  smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
  smp2p_agreed = true;
  printf("adsp: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
 }
 if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
  smp2p_restart_acked = !smp2p_restart_acked;
  printf("adsp: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
      step, theirs->flags, smp2p_restart_acked);
  smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
  settle();
  wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
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
  printf("adsp: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
  return false;
 }
 if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
  printf("adsp: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
  return false;
 }
 state = dsp_state(&theirs);
 if (state & DSP_STOP_ACK) {
  printf("adsp: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
  return true;
 }
 if (!(state & DSP_READY) || (state & DSP_FATAL)) {
  printf("adsp: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
  return false;
 }
 need(&ipcc);
 value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
 printf("adsp: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell\n", step,
     OUR_ENTRY_STOP, SMEM_PHYS + value_at);
 snprintf(last_write, sizeof(last_write), "step %u asked the audio DSP to stop", step);
 smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
 settle();
 wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
 asked = now = now_us();
 while (now - asked < bound_ms * 1000ull) {
  if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
   break;
  }
  sleep_us(200);
  now = now_us();
 }
 state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
 printf("adsp: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
     "THE AUDIO DSP ACKNOWLEDGES THE STOP" : "THE AUDIO DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
     (now - asked) % 1000, state);
 smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
 settle();
 wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
 return (state & DSP_STOP_ACK) != 0;
}
static void link_stop(void);
static bool
step_mailbox(void)
{
 lck_mtx_lock(&macan_aop_mailbox_lock);
 bool result = mailbox_open();
 lck_mtx_unlock(&macan_aop_mailbox_lock);
 return result;
}
#define MAX_SEGMENTS 64
#define MDT_HASH(f) (((f) >> 24 & 7) == 2)
struct elf_segment {
 uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};
struct pas_image {
 const char *name;
 uint32_t pas, base, capacity, count, lowest, span;
 const uint8_t *mdt;
 uint64_t mdt_size;
 bool owned, touched, authenticated;
 vm_offset_t metadata;
 vm_size_t metadata_size;
 struct elf_segment segment[MAX_SEGMENTS];
};
static struct pas_image images[] = {
 { .name = "adsp_dtb", .pas = PAS_DTB, .base = DTB_MEMORY, .capacity = DTB_MEMORY_SIZE },
 { .name = "adsp", .pas = PAS_ADSP, .base = ADSP_MEMORY, .capacity = ADSP_MEMORY_SIZE },
};
static volatile bool stopping, boot_runs, link_failed;
static bool image_on;
static bool
firmware_check(struct pas_image *im)
{
 char name[32];
 uint32_t hashes = 0, header_bytes, loads = 0;
 uint64_t highest = 0;
 bool relocate = false;
 snprintf(name, sizeof(name), "%s.mdt", im->name);
 im->mdt = image_file(name, &im->mdt_size);
 im->owned = from_root;
 printf("adsp: 2: %s %llu bytes from %s\n", name, im->mdt ? im->mdt_size : 0, !im->mdt ? "nowhere" : im->owned ? "the root" : "the image");
 if (!im->mdt || im->mdt_size < 52 || memcmp(im->mdt, "\177ELF\1\1", 6)) { return false; }
 uint32_t phoff;
 memcpy(&phoff, im->mdt + 28, 4);
 im->count = im->mdt[44] | im->mdt[45] << 8;
 if (phoff != 52 || im->mdt[42] != 32 || im->mdt[43] || im->count < 3 || im->count > MAX_SEGMENTS ||
     52 + 32 * im->count > im->mdt_size) { return false; }
 memcpy(im->segment, im->mdt + 52, im->count * sizeof(struct elf_segment));
 header_bytes = im->segment[0].file_size;
 if (im->segment[0].type == 1 || header_bytes < 52 + im->count * 32) { return false; }
 im->lowest = ~0u;
 for (unsigned i = 0; i < im->count; i++) {
  const struct elf_segment *s = &im->segment[i];
  if (MDT_HASH(s->flags)) {
   if (hashes) { return false; }
   hashes = s->file_size;
   continue;
  }
  if (s->type != 1 || !s->memory_size) { continue; }
  if (s->file_size > s->memory_size) { return false; }
  if (s->paddr < im->lowest) { im->lowest = s->paddr; }
  if ((uint64_t)s->paddr + s->memory_size > highest) { highest = (uint64_t)s->paddr + s->memory_size; }
  relocate |= (s->flags & (1u << 27)) != 0;
  loads++;
 }
 if (!loads || !hashes || (uint64_t)header_bytes + hashes != im->mdt_size || im->mdt_size > 65536) { return false; }
 if (!relocate) { im->lowest = im->base; }
 uint64_t span = ((highest + 4095) & ~4095ull) - im->lowest;
 if (span == 0 || span > im->capacity) { return false; }
 im->span = (uint32_t)span;
 for (unsigned i = 0; i < im->count; i++) {
  const struct elf_segment *s = &im->segment[i];
  if (s->type != 1 || MDT_HASH(s->flags) || !s->memory_size) { continue; }
  if (s->paddr < im->lowest || (uint64_t)s->paddr - im->lowest + s->memory_size > im->capacity) { return false; }
  if (s->file_size) {
   uint64_t size = 0;
   snprintf(name, sizeof(name), "%s.b%02u", im->name, i);
   uint64_t began = mach_absolute_time(), took;
   const uint8_t *file = image_file(name, &size);
   bool owned = from_root, valid = file && size == s->file_size;
   absolutetime_to_nanoseconds(mach_absolute_time() - began, &took);
   printf("adsp: 2: %s %llu bytes from %s in %llu ms%s\n", name, size, !file ? "nowhere" : owned ? "the root" : "the image",
       took / 1000000, valid ? "" : ": NOT AS THE HEADER SAYS");
   file_free(file, size, owned);
   if (!valid) { return false; }
  }
 }
 printf("adsp: MDT %s phdrs=%u loads=%u metadata=%llu relocation=-%#x span=%#x destination=[%#x,%#x)\n",
     im->name, im->count, loads, im->mdt_size, im->lowest - im->base, im->span, im->base, im->base + im->span);
 return true;
}
static bool
pas_load(struct pas_image *im)
{
 uint64_t answer = ~0ull;
 im->metadata_size = round_page(im->mdt_size);
 if (kmem_alloc_contig(kernel_map, &im->metadata, im->metadata_size, PAGE_MASK, 0, 0,
     KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) { return false; }
 memcpy((void *)im->metadata, im->mdt, im->mdt_size);
 for (vm_offset_t p = im->metadata; p < im->metadata + im->metadata_size; p += 64) {
  __asm__ volatile ("dc civac, %0" : : "r" (p) : "memory");
 }
 settle();
 im->touched = true;
 if (scm("PAS init metadata", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, im->pas, kvtophys(im->metadata), 0, &answer) || answer) { return false; }
 if (scm("PAS memory setup", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, im->pas, im->base, im->span, &answer) || answer) { return false; }
 vm_offset_t memory = ml_io_map_unmappable(im->base, im->capacity, VM_WIMG_WCOMB);
 if (!memory) { return false; }
 bool ok = true;
 uint64_t written = 0, cleared = 0;
 for (unsigned i = 0; ok && i < im->count; i++) {
  const struct elf_segment *s = &im->segment[i];
  if (s->type != 1 || MDT_HASH(s->flags) || !s->memory_size) { continue; }
  char name[32];
  uint64_t size = 0;
  const uint8_t *file = NULL;
  bool owned = false;
  if (s->file_size) {
   snprintf(name, sizeof(name), "%s.b%02u", im->name, i);
   file = image_file(name, &size); owned = from_root;
   if (!file || size != s->file_size) { file_free(file, size, owned); ok = false; break; }
  }
  uint32_t offset = s->paddr - im->lowest;
  if (s->paddr < im->lowest || (uint64_t)offset + s->memory_size > im->capacity) {
   file_free(file, size, owned); ok = false; break;
  }
  volatile uint8_t *to = (volatile uint8_t *)(memory + offset);
  uint32_t at = 0;
  if (!(offset & 7)) {
   for (; (uint64_t)at + 8 <= s->memory_size; at += 8) {
    uint64_t value = 0;
    for (unsigned j = 0; j < 8 && at + j < s->file_size; j++) { value |= (uint64_t)file[at + j] << (8 * j); }
    *(volatile uint64_t *)(to + at) = value;
   }
  }
  for (; at < s->memory_size; at++) { to[at] = at < s->file_size ? file[at] : 0; }
  settle();
  for (at = 0; at < s->memory_size; at++) {
   if (to[at] != (at < s->file_size ? file[at] : 0)) { ok = false; break; }
  }
  written += s->file_size; cleared += s->memory_size - s->file_size;
  file_free(file, size, owned);
  if (stopping) { ok = false; }
 }
 settle();
 ml_io_unmap(memory, im->capacity);
 printf("adsp: PAS %u loaded bytes=%llu bss=%llu readback=%s\n", im->pas, written, cleared, ok ? "OK" : "FAIL");
 return ok;
}
static bool
pas_auth(struct pas_image *im)
{
 uint64_t answer = ~0ull;
 if (scm("PAS authenticate/reset", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, im->pas, 0, 0, &answer) || answer) { return false; }
 im->authenticated = true;
 printf("adsp: PAS %u authenticated and running (%s)\n", im->pas, im->name);
 return true;
}
static bool
dsp_shutdown(const char *why)
{
 bool ok = true;
 printf("adsp: stop: %s\n", why);
 if (!leaving) { link_stop(); }
 if (dsp_started) { stopped_by_its_word = dsp_ask_to_stop(leaving ? 1000 : 5000); }
 for (int i = 1; i >= 0; i--) {
  struct pas_image *im = &images[i];
  if (im->touched) {
   uint64_t answer = ~0ull;
   bool down = scm("PAS shutdown", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, im->pas, 0, 0, &answer) == 0 && answer == 0;
   printf("adsp: PAS %u shutdown=%s\n", im->pas, down ? "OK" : "FAIL");
   if (down) { im->touched = im->authenticated = false; } else { ok = false; }
  }
  if (!leaving && !im->touched && im->metadata) {
   kmem_free(kernel_map, im->metadata, im->metadata_size);
   im->metadata = 0;
  }
  if (!leaving) { file_free(im->mdt, im->mdt_size, im->owned); im->mdt = NULL; }
 }
 if (ipcc.base) {
  wr(ipcc.base + IPCC_DISABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
  wr(ipcc.base + IPCC_DISABLE, IPCC_LPASS << 16 | IPCC_GLINK);
  doorbells();
 }
 if (image_on) {
  if (mailbox_send("{class: image, res: load_state, name: adsp, val: off}")) { image_on = false; } else { ok = false; }
 }
 if (icc_asked && !interconnect(false)) { ok = false; }
 if (rails_asked) { if (rails(0)) { rails_asked = false; } else { ok = false; } }
 dsp_started = images[1].authenticated;
 if (!ok) { printf("adsp: FAIL stage=%u shutdown incomplete\n", step); show_crash_reason(); }
 return ok;
}
static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER 0x000
#define LOCK_US 1u
static bool
our_item_make(uint32_t partition, uint32_t item, uint32_t requested)
{
 struct smem_partition header;
 struct smem_entry entry;
 const uint32_t item_size = (requested + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
 uint32_t owner = ~0u, at;
 bool made = false;
 lck_mtx_lock(&macan_smem_alloc_lock);
 need(&smem_lock);
 printf("adsp: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
     "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, item, item_size, need_bytes);
 snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
 for (uint32_t ms = 0; ms <= 1000; ms++) {
  wr(smem_lock.base + LOCK_OWNER, LOCK_US);
  owner = rd(smem_lock.base + LOCK_OWNER);
  if (owner == LOCK_US) {
   break;
  }
  sleep_ms(1);
 }
 if (owner != LOCK_US) {
  printf("adsp: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
  lck_mtx_unlock(&macan_smem_alloc_lock);
  return false;
 }
 if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
     header.free_cached > header.size || header.size > SMEM_SIZE - partition || requested > 0x4000) {
  printf("adsp: %u: the partition's marks make no sense: nothing is written\n", step);
  goto out;
 }
 for (at = sizeof(header); at < header.free_uncached;) {
  if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == item ||
      entry.size > header.size) {
   break;
  }
  at += sizeof(entry) + entry.padding_header + entry.size;
 }
 if (at != header.free_uncached) {
  printf("adsp: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
      "nothing is written\n", step, at, header.free_uncached);
  goto out;
 }
 if (at + need_bytes > header.free_cached) {
  printf("adsp: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
  goto out;
 }
 for (uint32_t i = 0; i < item_size; i += 4) { smem_poke(partition + at + sizeof(entry) + i, 0); }
 smem_poke(partition + at, SMEM_CANARY | item << 16);
 smem_poke(partition + at + 4, item_size);
 smem_poke(partition + at + 8, item_size - requested);
 smem_poke(partition + at + 12, 0);
 settle();
 smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
 settle();
 made = true;
out:
 wr(smem_lock.base + LOCK_OWNER, 0);
 printf("adsp: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
 lck_mtx_unlock(&macan_smem_alloc_lock);
 return made;
}
static void glink_reset(void);
static bool
step_our_item(void)
{
 struct smem_partition header;
 struct smp2p ours;
 uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };
 announce("look for an SMP2P item of ours (429) in our partition with the audio DSP, make it if there is none, and fill it in");
 partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
 if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
  printf("adsp: %u: there is no such partition\n", step);
  return false;
 }
 our_item = our_item_find();
 if (our_item != 0) {
  printf("adsp: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
      ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
 } else {
  if (!our_item_make(partition, ITEM_SMP2P_OURS, sizeof(struct smp2p))) {
   return false;
  }
  our_item = our_item_find();
  if (our_item == 0) {
   printf("adsp: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
   return false;
  }
  printf("adsp: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
 }
 snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
 for (uint32_t at = 0; at < sizeof(ours); at += 4) {
  smem_poke(our_item + at, 0);
 }
 settle();
 smem_poke(our_item, SMP2P_MAGIC);
 smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_ADSP << 16);
 strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
 for (uint32_t word = 0; word < 4; word++) {
  smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
 }
 smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 1u << 16);
 smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
 settle();
 smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
 settle();
 smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
 need(&ipcc);
 wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
 if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
  printf("adsp: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
  return false;
 }
 show_words("our item reads back", &ours);
 uint32_t glink_size = 0;
 if (!smem_item(478, &glink_size) && !our_item_make(partition, 478, 32)) { return false; }
 if (!smem_item(478, &glink_size) || glink_size != 32) { return false; }
 if (!smem_item(479, &glink_size) && !our_item_make(partition, 479, 0x4000)) { return false; }
 if (!smem_item(479, &glink_size) || glink_size < 1024 || (glink_size & 7)) { return false; }
 glink_reset();
 return true;
}
#define ITEM_GLINK_DESCRIPTOR 478
#define ITEM_GLINK_OUR_QUEUE 479
#define ITEM_GLINK_THEIR_QUEUE 480
#define GLINK_OUR_TAIL 0
#define GLINK_OUR_HEAD 4
#define GLINK_THEIR_TAIL 8
#define GLINK_THEIR_HEAD 12
#define GLINK_RESERVE 16
enum {
 GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
 GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
 GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1 1
#define GLINK_INTENT_REUSE 1u
struct glink_message {
 uint16_t command, param1;
 uint32_t param2;
};
struct glink_packet {
 struct glink_packet *next;
 uint32_t length;
 uint8_t data[];
};
enum { CHANNEL_QRTR, CHANNEL_GPR, CHANNELS = 16 };
struct glink_channel {
 char name[33];
 uint32_t ours;
 struct {
  uint32_t size, count;
 } intents[3];
 uint32_t theirs;
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
 uint32_t our_intent_count, queued;
 struct glink_packet *first, *last;
};
static LCK_MTX_DECLARE(glink_lock, &adsp_lock_group);
static struct {
 uint32_t descriptor, ours, ours_size, theirs, theirs_size;
 bool up, broken;
 uint32_t features;
 bool agreed;
 struct glink_channel channel[CHANNELS];
 uint32_t said, heard, trace;
} glink = {
 .channel = {
  [CHANNEL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
  [CHANNEL_GPR] = { .name = "adsp_apps", .ours = 2, .intents = { { 0x200, 20 }, { 0, 0 }, { 0, 0 } } },
 },
};
static struct glink_channel *
glink_theirs(uint32_t theirs)
{
 for (unsigned c = 0; c < CHANNELS; c++) {
  if (glink.channel[c].theirs != 0 && glink.channel[c].theirs == theirs) {
   return &glink.channel[c];
  }
 }
 return NULL;
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
  printf("adsp: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
  return;
 }
 printf("adsp: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
     "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
     smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
 if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
  printf("adsp: %u: about to write our two marks there (+4, +8): 0\n", step);
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
 wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_GLINK);
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
 if (!sent) { glink.broken = true; }
 if (glink.trace > 0 || !sent) {
  glink.trace -= glink.trace > 0;
  printf("adsp: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
 }
}
static bool
glink_offer(struct glink_channel *channel, uint32_t size)
{
 uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;
 if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
  return false;
 }
 if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
  channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
 }
 if (channel->our_intents[i].data == NULL) {
  return false;
 }
 channel->our_intents[i].size = size;
 channel->our_intents[i].fill = 0;
 channel->our_intent_count++;
 glink_say(GLINK_INTENT, channel->ours, 1, words, sizeof(words));
 return true;
}
static void
glink_pump(void)
{
 struct glink_message message;
 struct glink_channel *channel;
 uint32_t words[4];
 char name[33];
 for (unsigned budget = 0; budget < 256 && glink.up && !glink.broken; budget++) {
  uint32_t waiting = glink_waiting(), length = sizeof(message);
  if (waiting < sizeof(message)) {
   return;
  }
  glink_peek(&message, 0, sizeof(message));
  if (message.command == GLINK_OPEN) {
   if (message.param2 == 0 || message.param2 > 32) { glink.broken = true; return; }
   length += message.param2;
  } else if (message.command == GLINK_INTENT) {
   if (message.param2 > 64) { glink.broken = true; return; }
   length += 8 * message.param2;
  } else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
   if (waiting < 16) {
    return;
   }
   glink_peek(words, 8, 8);
   if (words[0] > glink.theirs_size - 16) { glink.broken = true; return; }
   length = 16 + words[0];
  }
  length = (length + 7) & ~7u;
  if (length > glink.theirs_size - 8) {
   printf("adsp: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
       glink_name(message.command), message.param1, message.param2);
   glink.broken = true;
   return;
  }
  if (waiting < length) {
   return;
  }
  glink.heard++;
  if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
      message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
   glink.trace--;
   printf("adsp: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
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
   channel = NULL;
   for (unsigned c = 0; c < CHANNELS; c++) {
    if (strcmp(name, glink.channel[c].name) == 0) {
     channel = &glink.channel[c];
    }
   }
   if (channel == NULL) {
    for (unsigned c = 2; c < CHANNELS; c++) {
     if (!glink.channel[c].name[0]) {
      channel = &glink.channel[c];
      strlcpy(channel->name, name, sizeof(channel->name));
      channel->ours = c + 1;
      channel->intents[0].size = 0x1000;
      channel->intents[0].count = 2;
      break;
     }
    }
    if (!channel) { glink.broken = true; return; }
   }
   printf("adsp: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
       name, message.param1, channel->ours);
   channel->theirs = message.param1;
   glink_say(GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
   if (!channel->asked) {
    uint32_t size = (uint32_t)strlen(channel->name) + 1;
    bzero(name, sizeof(name));
    strlcpy(name, channel->name, sizeof(name));
    glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
    channel->asked = true;
   }
   break;
  case GLINK_OPEN_ACK:
   glink_advance(length);
   for (unsigned c = 0; c < CHANNELS; c++) {
    if (message.param1 == glink.channel[c].ours) {
     glink.channel[c].acknowledged = true;
    }
   }
   break;
  case GLINK_INTENT:
   channel = glink_theirs(message.param1);
   for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
    glink_peek(words, 8 + 8 * i, 8);
    for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
     if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
      channel->their_intents[n].there = true;
      channel->their_intents[n].in_use = false;
      channel->their_intents[n].size = words[0];
      channel->their_intents[n].id = words[1];
      break;
     }
    }
   }
   glink_advance(length);
   break;
  case GLINK_DONE:
  case GLINK_DONE_REUSE:
   channel = glink_theirs(message.param1);
   for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
    if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
     channel->their_intents[n].in_use = false;
     channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
    }
   }
   glink_advance(length);
   break;
  case GLINK_INTENT_REQUEST:
   glink_advance(length);
   if ((channel = glink_theirs(message.param1)) != NULL) {
    bool granted = glink_offer(channel, message.param2);
    glink_say(GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
   }
   break;
  case GLINK_INTENT_REQUEST_ACK:
   glink_advance(length);
   for (unsigned c = 0; c < CHANNELS; c++) {
    if (message.param1 == glink.channel[c].theirs || message.param1 == glink.channel[c].ours) {
     glink.channel[c].request_answered = true;
     glink.channel[c].request_granted = message.param2 != 0;
    }
   }
   break;
  case GLINK_DATA:
  case GLINK_DATA_MORE: {
   uint32_t intent = message.param2 - 1;
   channel = glink_theirs(message.param1);
   if (channel == NULL || intent >= channel->our_intent_count ||
       words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
    printf("adsp: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
        message.param1, message.param2);
    glink_advance(length);
    break;
   }
   glink_peek(channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
   channel->our_intents[intent].fill += words[0];
   glink_advance(length);
   if (words[1] == 0) {
    if (channel->queued >= 32) { glink.broken = true; return; }
    struct glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);
    if (packet != NULL) {
     packet->next = NULL;
     packet->length = channel->our_intents[intent].fill;
     memcpy(packet->data, channel->our_intents[intent].data, packet->length);
     if (channel->last != NULL) {
      channel->last->next = packet;
     } else {
      channel->first = packet;
     }
     channel->last = packet;
     channel->queued++;
    }
    channel->our_intents[intent].fill = 0;
    glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
   }
   break;
  }
  case GLINK_READ_NOTIFY:
   glink_advance(length);
   wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_GLINK);
   break;
  case GLINK_CLOSE:
   glink_advance(length);
   glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
   if ((channel = glink_theirs(message.param1)) != NULL) {
    printf("adsp: glink: THE DSP CLOSES \"%s\"\n", channel->name);
    channel->open = false;
    channel->theirs = 0;
   }
   break;
  case GLINK_CLOSE_ACK:
  case GLINK_SIGNALS:
   glink_advance(length);
   break;
  default:
   printf("adsp: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
       message.param1, message.param2);
   glink.broken = true;
   return;
  }
  for (unsigned c = 0; c < CHANNELS; c++) {
   channel = &glink.channel[c];
   if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
    channel->open = true;
    printf("adsp: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
        channel->name, channel->ours, channel->theirs);
    for (unsigned i = 0; i < 3; i++) {
     for (unsigned n = 0; n < channel->intents[i].count; n++) {
      glink_offer(channel, channel->intents[i].size);
     }
    }
   }
  }
 }
}
static int
glink_send(struct glink_channel *channel, const void *data, uint32_t length)
{
 struct {
  struct glink_message message;
  uint32_t size, left;
 } header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
 bool asked = false, fits;
 int found = -1;
 lck_mtx_lock(&glink_lock);
 if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
  lck_mtx_unlock(&glink_lock);
  return EMSGSIZE;
 }
 for (unsigned waited = 0;; waited++) {
  glink_pump();
  if (stopping || !glink.up || !channel->open || glink.broken) {
   lck_mtx_unlock(&glink_lock);
   return ENOTCONN;
  }
  found = -1;
  fits = false;
  for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
   if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
    continue;
   }
   fits = true;
   if (!channel->their_intents[n].in_use &&
       (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
    found = (int)n;
   }
  }
  if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
   break;
  }
  if (!fits && !asked) {
   channel->request_answered = false;
   glink_say(GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
   asked = true;
  }
  if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
   lck_mtx_unlock(&glink_lock);
   return found < 0 ? ENOBUFS : EAGAIN;
  }
  lck_mtx_unlock(&glink_lock);
  sleep_ms(1);
  lck_mtx_lock(&glink_lock);
 }
 channel->their_intents[found].in_use = true;
 header.message.param2 = channel->their_intents[found].id;
 bool sent = glink_put(&header, sizeof(header), data, length);
 lck_mtx_unlock(&glink_lock);
 return sent ? 0 : EAGAIN;
}
#define QRTR_US 1u
#define QRTR_CONTROL 0xfffffffeu
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };
struct qrtr_header {
 uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
 uint32_t command;
 uint32_t words[4];
};
static struct {
 uint32_t service, instance, node, port;
 bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;
static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
 uint32_t padded = (length + 3) & ~3u;
 struct qrtr_header *packet;
 int error;
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
 error = glink_send(&glink.channel[CHANNEL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
 kfree_data(packet, sizeof(*packet) + padded);
 qrtr_out += error == 0;
 return error;
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
  printf("adsp: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
  if (error) { link_failed = true; break; }
  struct qrtr_control lookup = { .command = QRTR_NEW_LOOKUP };
  if (qrtr_out_packet(QRTR_NEW_LOOKUP, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &lookup, sizeof(lookup))) { link_failed = true; }
  break;
 case QRTR_NEW_SERVER:
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
  printf("adsp: QRTR service id=%u hex=%#x instance_raw=%#x version=%u instance=%u node=%u port=%u\n",
      control->words[0], control->words[0], control->words[1], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
  break;
 case QRTR_DEL_SERVER:
  printf("adsp: qrtr: service %u at node %u port %u has gone\n", control->words[0], control->words[2], control->words[3]);
  for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
   if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
    qrtr_servers[i].there = false;
   }
  }
  break;
 case QRTR_NEW_LOOKUP:
  if (qrtr_lookups++ < 16) {
   printf("adsp: qrtr: the DSP looks for service %u (%#x), instance %u: nobody here offers it\n", control->words[0],
       control->words[0], control->words[1]);
  }
  break;
 case QRTR_BYE:
  printf("adsp: qrtr: BYE from node %u\n", header->from_node);
  bzero(qrtr_servers, sizeof(qrtr_servers));
  break;
 default:
  break;
 }
}
static void
qrtr_input(const uint8_t *data, uint32_t length)
{
 struct qrtr_header header;
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
  printf("adsp: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
  return;
 }
 if (header.size == 0 || at > length || header.size > length - at) {
  printf("adsp: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
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
 if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
     header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP) {
  struct qrtr_control control = { 0 };
  if (header.size < 4 || ((header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER) && header.size < sizeof(control))) { return; }
  memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
  qrtr_control(&header, &control);
  return;
 }
 printf("adsp: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
     header.from_node, header.from_port, header.to_port);
}
#define APM_CMD_GET_SPF_STATE 0x01001021u
#define APM_CMD_RSP_GET_SPF_STATE 0x02001007u
#define GPR_BASIC_RSP_RESULT 0x02001005u
struct gpr_header {
 uint32_t packet, domains, source_port, dest_port, token, opcode;
};
static uint32_t gpr_token, gpr_state, gpr_error, gpr_in, gpr_out;
static bool gpr_answered, audio_up;
static LCK_MTX_DECLARE(gpr_lock, &adsp_lock_group);
static void
gpr_input(const uint8_t *data, uint32_t length)
{
 struct gpr_header h;
 uint32_t words[2] = { 0 };
 if (length < sizeof(h)) { return; }
 memcpy(&h, data, sizeof(h));
 uint32_t header_size = ((h.packet >> 4) & 15) * 4;
 if ((h.packet & 15) != 0 || header_size < sizeof(h) || (h.packet >> 8) != length ||
     header_size > length || (h.domains & 0xffff) != 0x0203 || h.dest_port != 1 || h.source_port != 1) { return; }
 gpr_in++;
 if (length - header_size >= 4) { memcpy(words, data + header_size, 4); }
 if (length - header_size >= 8) { memcpy(words + 1, data + header_size + 4, 4); }
 printf("adsp: GPR reply opcode=%#x token=%u bytes=%u words=%#x,%#x\n", h.opcode, h.token, length - header_size, words[0], words[1]);
 lck_mtx_lock(&gpr_lock);
 if (h.token != gpr_token || os_atomic_load(&gpr_answered, acquire)) { lck_mtx_unlock(&gpr_lock); return; }
 if (h.opcode == APM_CMD_RSP_GET_SPF_STATE && length - header_size >= 4) {
  gpr_state = words[0]; gpr_error = 0;
  os_atomic_store(&gpr_answered, true, release);
 } else if (h.opcode == GPR_BASIC_RSP_RESULT && length - header_size >= 8 && words[0] == APM_CMD_GET_SPF_STATE) {
  if (words[1]) { gpr_error = words[1]; os_atomic_store(&gpr_answered, true, release); }
 }
 lck_mtx_unlock(&gpr_lock);
}
static bool
step_gpr(void)
{
 for (unsigned ms = 0; ms < 5000 && !stopping && !link_failed && !glink.channel[CHANNEL_GPR].open; ms++) { sleep_ms(1); }
 if (!glink.channel[CHANNEL_GPR].open) { return false; }
 uint64_t began = now_us();
 while (!stopping && !link_failed && now_us() - began < 10000000) {
  struct {
   struct gpr_header h;
   uint32_t payload_address_lsw, payload_address_msw, mem_map_handle, payload_size;
  } packet = { .h = { .packet = (40u << 8) | (6u << 4), .domains = 0x0302,
      .source_port = 1, .dest_port = 1, .opcode = APM_CMD_GET_SPF_STATE } };
  lck_mtx_lock(&gpr_lock);
  packet.h.token = ++gpr_token;
  gpr_state = 0; gpr_error = 0;
  os_atomic_store(&gpr_answered, false, release);
  lck_mtx_unlock(&gpr_lock);
  printf("adsp: GPR APM_CMD_GET_SPF_STATE opcode=%#x token=%u\n", packet.h.opcode, packet.h.token);
  if (glink_send(&glink.channel[CHANNEL_GPR], &packet, sizeof(packet))) { return false; }
  gpr_out++;
  for (unsigned ms = 0; ms < 500 && !stopping && !link_failed && !os_atomic_load(&gpr_answered, acquire); ms++) { sleep_ms(1); }
  if (os_atomic_load(&gpr_answered, acquire)) {
   printf("adsp: SPF state=%u status=%#x token=%u\n", gpr_state, gpr_error, packet.h.token);
   if (gpr_state == 1 && !gpr_error) { audio_up = true; return true; }
   if (gpr_error) { return false; }
  }
  sleep_ms(200);
 }
 return false;
}
static volatile bool link_wanted, link_runs;
static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
 struct smp2p words, last;
 struct glink_packet *packet;
 bool gave_up = false;
 bzero(&last, sizeof(last));
 dsp_words(&last);
 while (link_wanted && !stopping && !link_failed) {
  sleep_us(1000);
  doorbells();
  if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
   last = words;
   dsp_listen(&words);
   if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
    gave_up = true;
    printf("adsp: link: THE AUDIO DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
    show_crash_reason();
    link_failed = true;
   }
  }
  for (unsigned budget = 0; budget < 256 && !stopping; budget++) {
   unsigned from = CHANNELS;
   lck_mtx_lock(&glink_lock);
   glink_pump();
   if (glink.broken || !glink.channel[CHANNEL_QRTR].open || !glink.channel[CHANNEL_GPR].open) {
    if (glink.broken) { link_failed = true; }
   }
   packet = NULL;
   for (unsigned c = 0; c < CHANNELS && packet == NULL; c++) {
    struct glink_channel *channel = &glink.channel[c];
    if ((packet = channel->first) != NULL) {
     channel->first = packet->next;
     channel->queued--;
     channel->last = channel->first == NULL ? NULL : channel->last;
     from = c;
    }
   }
   lck_mtx_unlock(&glink_lock);
   if (packet == NULL) {
    break;
   }
   if (from == CHANNEL_QRTR) {
    qrtr_input(packet->data, packet->length);
   } else if (from == CHANNEL_GPR) {
    gpr_input(packet->data, packet->length);
   } else {
    if (glink.trace) { glink.trace--; printf("adsp: channel %s received %u bytes\n", glink.channel[from].name, packet->length); }
   }
   kfree_data(packet, sizeof(*packet) + packet->length);
  }
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
  printf("adsp: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams, GPR %u and %u "
      "packets; its doorbells heard: GLINK %u, SMP2P %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
      glink.heard, qrtr_out, qrtr_in, gpr_out, gpr_in, doorbell.glink, doorbell.smp2p);
 }
 if (link_runs) { return; }
 lck_mtx_lock(&glink_lock);
 glink.up = false;
 for (unsigned c = 0; c < CHANNELS; c++) {
  struct glink_channel *channel = &glink.channel[c];
  channel->open = false;
  while ((packet = channel->first) != NULL) {
   channel->first = packet->next;
   kfree_data(packet, sizeof(*packet) + packet->length);
  }
  channel->last = NULL;
  for (unsigned i = 0; i < 24; i++) {
   if (channel->our_intents[i].data) {
    kfree_data(channel->our_intents[i].data, channel->our_intents[i].size);
    channel->our_intents[i].data = NULL;
   }
  }
  channel->our_intent_count = channel->queued = 0;
 }
 lck_mtx_unlock(&glink_lock);
 bzero(qrtr_servers, sizeof(qrtr_servers));
 qrtr_peer = 0;
}
static bool
step_queues(void)
{
 struct glink_message message;
 uint32_t head, tail;
 announce("find GLINK's descriptor and the two queues, and read what the audio DSP has put into its own");
 if (!glink_find()) {
  printf("adsp: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
      "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
  return false;
 }
 head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
 tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
 printf("adsp: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
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
  printf("adsp: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
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
 announce("take up GLINK with the audio DSP: our version, then whatever its messages call for; a thread of ours listens "
     "from here on, every millisecond");
 snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
 lck_mtx_lock(&glink_lock);
 glink.features = GLINK_INTENT_REUSE;
 glink.agreed = glink.broken = false;
 glink.said = glink.heard = 0;
 for (unsigned c = 0; c < CHANNELS; c++) {
  struct glink_channel *channel = &glink.channel[c];
  channel->asked = channel->acknowledged = channel->open = false;
  channel->theirs = channel->our_intent_count = 0;
  bzero(channel->their_intents, sizeof(channel->their_intents));
 }
 gpr_in = gpr_out = 0;
 audio_up = false;
 glink.trace = 60;
 glink.up = true;
 qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = 0;
 doorbell.since = began = now_us();
 glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
 lck_mtx_unlock(&glink_lock);
 link_wanted = link_runs = true;
 if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
  link_wanted = link_runs = false;
  printf("adsp: %u: no thread\n", step);
  return false;
 }
 thread_deallocate(thread);
 for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
  sleep_ms(1);
 }
 sleep_ms(50);
 printf("adsp: %u: %llu ms after our version: versions %s features=%#x IPCRTR=%s adsp_apps=%s\n", step,
     (now_us() - began) / 1000, glink.agreed ? "agreed" : "NOT AGREED", glink.features,
     glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN", glink.channel[CHANNEL_GPR].open ? "OPEN" : "NOT OPEN");
 return glink.agreed && glink.channel[CHANNEL_QRTR].open && !glink.broken && !link_failed;
}
static bool
step_qrtr(void)
{
 unsigned services = 0;
 if (!glink.channel[CHANNEL_QRTR].open) {
  return false;
 }
 listen_seconds = 5;
 announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
 for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
  sleep_ms(1);
 }
 for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
  services += qrtr_servers[i].there;
 }
 printf("adsp: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
     "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
     services, qrtr_lookups, qrtr_in, qrtr_out);
 return qrtr_hellos != 0 && services != 0 && !link_failed;
}
static struct smp2p before_start;
static bool had_before_start;
static bool
step_ready(void)
{
 struct smp2p words;
 uint64_t began = now_us();
 bool ready = false, handover = false, fresh = !had_before_start;
 while (!stopping && now_us() - began < 10000000) {
  doorbells();
  if (dsp_words(&words)) {
   dsp_listen(&words);
   uint32_t state = dsp_state(&words);
   if (state & DSP_FATAL) { return false; }
   fresh |= memcmp(&words, &before_start, sizeof(words)) != 0;
   if (!fresh || (state & (DSP_STOP_ACK | DSP_SHUTDOWN_ACK))) { sleep_us(200); continue; }
   if (!ready && (state & DSP_READY)) {
    ready = true;
    printf("adsp: SMP2P ready seen state=%#x\n", state);
   }
   if (!handover && (state & DSP_HANDOVER)) {
    handover = true;
    printf("adsp: SMP2P handover seen state=%#x\n", state);
    if (adsp_vote && (!interconnect(false) || !rails(0))) { return false; }
    rails_asked = false;
   }
   if (ready && handover && smp2p_agreed) { return true; }
  }
  sleep_us(200);
 }
 return false;
}
static uint32_t mode;
static void
adsp_thread(__unused void *parameter, __unused wait_result_t result)
{
 bool success = false;
 for (unsigned n = 0; n < 300 && rootvnode == NULLVP && !stopping; n++) { sleep_ms(200); }
 lck_mtx_lock(&dsp_lock);
 if (stopping) { goto out; }
 step = 1;
 if (!step_survey()) { goto fail; }
 step = 2;
 if (!firmware_check(&images[0]) || !firmware_check(&images[1])) { goto fail; }
 step = 3;
 if (!step_mailbox()) { goto fail; }
 step = 4;
 struct smp2p old;
 if (dsp_words(&old)) {
  images[0].touched = images[1].touched = true;
  image_on = true;
  dsp_started = true;
  bool fatal = (dsp_state(&old) & DSP_FATAL) != 0;
  if (!dsp_shutdown("previous ADSP run") || !stopped_by_its_word || fatal) { goto fail; }
  if (!firmware_check(&images[0]) || !firmware_check(&images[1])) { goto fail; }
 }
 if (!step_our_item()) { goto fail; }
 step = 5;
 if (adsp_vote) {
  rails_asked = true;
  if (!rails(256) || !interconnect(true)) { goto fail; }
 } else {
  printf("adsp: adsp_vote=0: skipping LCX/LMX/XO/ICC votes entirely (bootloader state retained)\n");
 }
 image_on = true;
 if (!mailbox_send("{class: image, res: load_state, name: adsp, val: on}")) { goto fail; }
 need(&ipcc);
 doorbells();
 wr(ipcc.base + IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
 wr(ipcc.base + IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_GLINK);
 step = 6;
 if (stopping || !pas_load(&images[0])) { goto fail; }
 step = 7;
 if (stopping || !pas_auth(&images[0]) || !pas_load(&images[1])) { goto fail; }
 step = 8;
 had_before_start = dsp_words(&before_start);
 if (stopping || !pas_auth(&images[1])) { goto fail; }
 dsp_started = true;
 if (!step_ready()) { goto fail; }
 step = 9;
 if (stopping || !step_queues() || !step_link()) { goto fail; }
 step = 10;
 if (stopping || !step_qrtr()) { goto fail; }
 step = 11;
 if (stopping || !step_gpr()) { goto fail; }
 unsigned services = 0;
 for (unsigned i = 0; i < 64; i++) { services += qrtr_servers[i].there; }
 printf("adsp: STAGE1 OK PAS=36,1 SMP2P=ready+handover GLINK=up QRTR node=%u services=%u GPR SPF=1 mode=%u\n",
     qrtr_peer, services, mode);
 success = true;
 if (mode == 2) { stopping = true; bool clean = dsp_shutdown("adsp=2 complete"); printf("adsp: adsp=2 stopped clean=%u\n", clean); }
 lck_mtx_unlock(&dsp_lock);
 while (mode == 1 && !stopping && !link_failed) {
  sleep_ms(50);
  if (glink.broken || !glink.channel[CHANNEL_QRTR].open || !glink.channel[CHANNEL_GPR].open) { link_failed = true; }
 }
 lck_mtx_lock(&dsp_lock);
 if (link_failed) { goto fail; }
 goto out;
fail:
 printf("adsp: FAIL stage=%u last=%s\n", step, last_write);
 show_crash_reason();
 stopping = true;
 dsp_shutdown("failure unwind");
out:
 if (!success && stopping) { printf("adsp: stage 1 stopped before completion\n"); }
 boot_runs = false;
 lck_mtx_unlock(&dsp_lock);
 thread_terminate(current_thread());
}
void
macan_adsp_stop(void)
{
 stopping = true;
 if (!mode) { return; }
 for (unsigned n = 0; n < 2000 && boot_runs; n++) { sleep_ms(1); }
 lck_mtx_lock(&dsp_lock);
 dsp_shutdown("loader return");
 lck_mtx_unlock(&dsp_lock);
}
void
macan_adsp_quiesce(void)
{
 if (!mode) { return; }
 leaving = stopping = true;
 link_wanted = false;
 dsp_shutdown("panic/loader quiesce");
}
void
macan_adsp_init(void)
{
 uint32_t macan = 0;
 thread_t thread;
 PE_parse_boot_argn("adsp", &mode, sizeof(mode));
 PE_parse_boot_argn("adsp_vote", &adsp_vote, sizeof(adsp_vote));
 if (!mode) { return; }
 if (mode > 2 || !PE_parse_boot_argn("macan", &macan, sizeof(macan)) || !macan) {
  printf("adsp: requires macan=1 and adsp=1 or adsp=2\n");
  mode = 0; return;
 }
 new_from = ~0u;
 boot_runs = true;
 if (kernel_thread_start(adsp_thread, NULL, &thread) != KERN_SUCCESS) {
  boot_runs = false;
  printf("adsp: FAIL stage=0 no worker thread\n");
  show_crash_reason();
  return;
 }
 thread_deallocate(thread);
 printf("adsp: stage 1 worker started adsp=%u (0 is default/off)\n", mode);
}
