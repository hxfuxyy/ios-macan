#include <stdarg.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/macan_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
#define EPSS_PHYS 0x18591000UL
#define EPSS_DOMAIN_SIZE 0x1000UL
#define EPSS_DOMAINS 3
#define EPSS_FREQ_TABLE 0x100
#define EPSS_TABLE_ROWS 40
#define EPSS_PERF_STATE 0x320
#define XO_KHZ 19200
#define ALTERNATE_KHZ 300000
static uint32_t
loop_speed(void)
{
 volatile uint64_t sink = 0;
 uint64_t start = mach_absolute_time(), nanos;
 for (uint32_t i = 0; i < 2000000; i++) {
  sink += i;
 }
 absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
 return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}
static uint32_t
domain_of(uint32_t phys_id)
{
 uint32_t core = (phys_id >> 8) & 0xff;
 return core < 4 ? 0 : core < 7 ? 1 : 2;
}
static int cpufreq_khz;
SYSCTL_DECL(_kern_redwood);
SYSCTL_INT(_kern_redwood, OID_AUTO, cpu_khz, CTLFLAG_RD | CTLFLAG_LOCKED, &cpufreq_khz, 0,
    "the speed cpufreq=1 set the fastest group of processors to, in kHz (0: left as the boot loader left it)");
static void
raise_domain(uint32_t domain, uint32_t cores)
{
 uint32_t top = 0, top_khz = 0, was;
 vm_offset_t base;
 base = ml_io_map(EPSS_PHYS + domain * EPSS_DOMAIN_SIZE, EPSS_DOMAIN_SIZE);
 for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
  uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
  uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;
  if (row > 0 && khz <= top_khz) {
   break;
  }
  top = row;
  top_khz = khz;
 }
 was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
 *(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
 delay_for_interval(20, NSEC_PER_MSEC);
 printf("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz\n", domain, cores, was, top,
     top_khz / 1000);
 if ((int)top_khz > cpufreq_khz) {
  cpufreq_khz = (int)top_khz;
 }
}
extern void loader_log_putc(char);
#define MACAN_SCMI_SHMEM_PHYS 0x81f08100UL
#define MACAN_SCMI_SHMEM_SIZE 0x80UL
#define MACAN_SCMI_MBOX_PHYS 0x19980000UL
#define MACAN_SCMI_MBOX_SIZE 0x1000UL
#define MACAN_SCMI_DOORBELL (0x100 + 0 * 8 + 4)
#define MACAN_SCMI_WORDS ((MACAN_SCMI_SHMEM_SIZE - 0x1c) / 4)
#define SCMI_PERF 0x13
#define SCMI_VERSION 0x0
#define SCMI_ATTRIBUTES 0x1
#define SCMI_PERF_DOMAIN 0x3
#define SCMI_PERF_LEVELS 0x4
#define SCMI_PERF_LIMITS_SET 0x5
#define SCMI_PERF_LIMITS_GET 0x6
#define SCMI_PERF_LEVEL_SET 0x7
#define SCMI_PERF_LEVEL_GET 0x8
#define MACAN_PERF_DOMAINS_MAX 4
static vm_offset_t macan_shmem, macan_mbox;
static uint32_t macan_token;
static void
macan_say(const char *format, ...)
{
 char line[256];
 va_list args;
 va_start(args, format);
 vsnprintf(line, sizeof(line), format, args);
 va_end(args);
 for (char *c = line; *c; c++) {
  loader_log_putc(*c);
 }
 printf("%s", line);
}
static volatile uint32_t *
macan_word(unsigned offset)
{
 return (volatile uint32_t *)(macan_shmem + offset);
}
static int
macan_scmi(uint32_t message, const uint32_t *in, unsigned nin, uint32_t *out, unsigned nout, unsigned *nanswer)
{
 uint32_t header, length;
 unsigned i, words;
 for (i = 0; i < 1000 && !(*macan_word(0x4) & 1); i++) {
  delay_for_interval(10, NSEC_PER_USEC);
 }
 if (!(*macan_word(0x4) & 1)) {
  return -100;
 }
 header = ((++macan_token & 0x3ff) << 18) | (SCMI_PERF << 10) | message;
 *macan_word(0x10) = 0;
 *macan_word(0x14) = 4 + 4 * nin;
 *macan_word(0x18) = header;
 for (i = 0; i < nin; i++) {
  *macan_word(0x1c + 4 * i) = in[i];
 }
 __asm__ volatile ("dsb sy");
 *macan_word(0x4) = 0;
 __asm__ volatile ("dsb sy");
 *(volatile uint32_t *)(macan_mbox + MACAN_SCMI_DOORBELL) = 1;
 __asm__ volatile ("dsb sy");
 for (i = 0; i < 5000 && !(*macan_word(0x4) & 1); i++) {
  delay_for_interval(10, NSEC_PER_USEC);
 }
 if (!(*macan_word(0x4) & 1)) {
  return -101;
 }
 if (*macan_word(0x4) & 2) {
  return -102;
 }
 if (*macan_word(0x18) != header) {
  return -103;
 }
 length = *macan_word(0x14);
 words = length >= 8 ? (length - 8) / 4 : 0;
 if (words > MACAN_SCMI_WORDS - 1) {
  words = MACAN_SCMI_WORDS - 1;
 }
 for (i = 0; i < nout && i < words; i++) {
  out[i] = *macan_word(0x20 + 4 * i);
 }
 if (nanswer != NULL) {
  *nanswer = words;
 }
 return (int)*macan_word(0x1c);
}
static uint32_t macan_domains, macan_want[MACAN_PERF_DOMAINS_MAX];
extern processor_t cpu_to_processor(int cpu);
extern processor_t thread_bind(processor_t processor);
extern wait_result_t thread_block(thread_continue_t continuation);
static void
macan_core_speeds(const char *when)
{
 const ml_topology_info_t *topology = ml_get_topology_info();
 char line[160];
 int n = snprintf(line, sizeof(line), "cpufreq: loop speed per core %s:", when);
 for (unsigned int i = 0; i < topology->num_cpus && n > 0 && n < (int)sizeof(line) - 12; i++) {
  processor_t processor = cpu_to_processor((int)topology->cpus[i].cpu_id);
  if (processor == PROCESSOR_NULL) {
   continue;
  }
  thread_bind(processor);
  thread_block(THREAD_CONTINUE_NULL);
  n += snprintf(line + n, sizeof(line) - (size_t)n, " %u:%u", topology->cpus[i].cpu_id, loop_speed());
 }
 thread_bind(PROCESSOR_NULL);
 thread_block(THREAD_CONTINUE_NULL);
 macan_say("%s\n", line);
}
static void
macan_keep_thread(__unused void *parameter, __unused wait_result_t result)
{
 uint32_t in[3], out[1];
 unsigned got, said = 0;
 delay_for_interval(20, NSEC_PER_SEC);
 macan_core_speeds("20 s after the start");
 if (macan_domains > 1) {
  in[0] = 1;
  in[1] = macan_want[1];
  in[2] = macan_want[1];
  int limits = macan_scmi(SCMI_PERF_LIMITS_SET, in, 3, out, 1, &got);
  in[0] = 1;
  if (macan_scmi(SCMI_PERF_LEVEL_GET, in, 1, out, 1, &got) != 0) {
   out[0] = 0;
  }
  macan_say("cpufreq: domain 1 limits set to %u..%u: %d, now at level %u\n", macan_want[1], macan_want[1], limits,
      out[0]);
  delay_for_interval(1, NSEC_PER_SEC);
  macan_core_speeds("after domain 1's floor");
 }
 for (;;) {
  delay_for_interval(10, NSEC_PER_SEC);
  for (uint32_t d = 0; d < macan_domains; d++) {
   in[0] = d;
   if (macan_scmi(SCMI_PERF_LEVEL_GET, in, 1, out, 1, &got) != 0 || out[0] == macan_want[d]) {
    continue;
   }
   uint32_t was = out[0];
   in[1] = macan_want[d];
   int status = macan_scmi(SCMI_PERF_LEVEL_SET, in, 2, out, 1, &got);
   in[0] = d;
   if (macan_scmi(SCMI_PERF_LEVEL_GET, in, 1, out, 1, &got) != 0) {
    out[0] = 0;
   }
   if (said < 12) {
    said++;
    macan_say("cpufreq: domain %u was at level %u, asked for %u again: %d, now at %u\n", d, was,
        macan_want[d], status, out[0]);
   }
  }
 }
}
static void
macan_cpufreq(uint32_t mode)
{
 uint32_t out[MACAN_SCMI_WORDS], in[3], version, domains, before;
 uint32_t top[MACAN_PERF_DOMAINS_MAX] = { 0 }, count[MACAN_PERF_DOMAINS_MAX] = { 0 };
 uint32_t attributes[MACAN_PERF_DOMAINS_MAX] = { 0 };
 thread_t thread;
 unsigned got;
 int status;
 macan_shmem = ml_io_map(MACAN_SCMI_SHMEM_PHYS, MACAN_SCMI_SHMEM_SIZE);
 macan_mbox = ml_io_map(MACAN_SCMI_MBOX_PHYS, MACAN_SCMI_MBOX_SIZE);
 if (!macan_shmem || !macan_mbox) {
  macan_say("cpufreq: the PDP's channel could not be mapped\n");
  return;
 }
 before = loop_speed();
 macan_say("cpufreq: PDP channel status %#x; a loop runs at %u\n", *macan_word(0x4), before);
 status = macan_scmi(SCMI_VERSION, NULL, 0, out, 1, &got);
 if (status != 0) {
  macan_say("cpufreq: the PDP did not answer the version (%d): leaving the speed alone\n", status);
  return;
 }
 version = out[0];
 status = macan_scmi(SCMI_ATTRIBUTES, NULL, 0, out, 1, &got);
 domains = status == 0 ? (out[0] & 0xffff) : 0;
 macan_say("cpufreq: SCMI performance protocol %u.%u, %u domains\n", version >> 16, version & 0xffff, domains);
 if (domains > MACAN_PERF_DOMAINS_MAX) {
  domains = MACAN_PERF_DOMAINS_MAX;
 }
 for (uint32_t d = 0; d < domains; d++) {
  char name[17] = { 0 };
  uint32_t index = 0, total = 0;
  in[0] = d;
  status = macan_scmi(SCMI_PERF_DOMAIN, in, 1, out, 8, &got);
  if (status == 0 && got >= 8) {
   attributes[d] = out[0];
   memcpy(name, &out[4], 16);
   macan_say("cpufreq: domain %u \"%s\": attributes %#x, rate limit %u us, sustained %u kHz at level %u\n",
       d, name, out[0], out[1], out[2], out[3]);
  }
  do {
   unsigned stride = (version >> 16) >= 4 ? 5 : 3, n, remaining;
   in[0] = d;
   in[1] = index;
   status = macan_scmi(SCMI_PERF_LEVELS, in, 2, out, MACAN_SCMI_WORDS - 1, &got);
   if (status != 0 || got < 1) {
    macan_say("cpufreq: domain %u: levels from %u: status %d\n", d, index, status);
    break;
   }
   n = out[0] & 0xfff;
   remaining = out[0] >> 16;
   for (unsigned k = 0; k < n && 1 + (k + 1) * stride <= got; k++) {
    uint32_t *e = &out[1 + k * stride];
    if (e[0] > top[d]) {
     top[d] = e[0];
    }
    if (k == 0 || k + 1 == n || mode >= 3) {
     macan_say("cpufreq: domain %u level %u: perf %u, power %u, latency %u us%s%u\n", d, index + k,
         e[0], e[1], e[2] & 0xffff, stride == 5 ? ", kHz " : "", stride == 5 ? e[3] : 0);
    }
   }
   index += n;
   total = index + remaining;
   if (n == 0) {
    break;
   }
  } while (index < total);
  count[d] = index;
  in[0] = d;
  if (macan_scmi(SCMI_PERF_LIMITS_GET, in, 1, out, 2, &got) == 0) {
   macan_say("cpufreq: domain %u: %u levels, the highest %u; limits %u to %u\n", d, index, top[d], out[1], out[0]);
  }
  if (macan_scmi(SCMI_PERF_LEVEL_GET, in, 1, out, 1, &got) == 0) {
   macan_say("cpufreq: domain %u is at level %u\n", d, out[0]);
  }
 }
 if (mode < 2) {
  return;
 }
 macan_domains = domains;
 for (uint32_t d = 0; d < domains; d++) {
  bool indexed = (attributes[d] >> 25) & 1;
  uint32_t want = indexed ? count[d] - 1 : top[d];
  int level;
  if (top[d] == 0 || count[d] == 0) {
   continue;
  }
  in[0] = d;
  in[1] = want;
  level = macan_scmi(SCMI_PERF_LEVEL_SET, in, 2, out, 1, &got);
  in[0] = d;
  if (macan_scmi(SCMI_PERF_LEVEL_GET, in, 1, out, 1, &got) != 0) {
   out[0] = 0;
  }
  macan_say("cpufreq: domain %u asked for %s %u (%u kHz): %d, now at %u\n", d, indexed ? "level" : "perf", want,
      top[d], level, out[0]);
  macan_want[d] = want;
  if (d == 0) {
   cpufreq_khz = (int)top[d];
  }
 }
 macan_say("cpufreq: a loop ran at %u, now at %u\n", before, loop_speed());
 if (kernel_thread_start(macan_keep_thread, NULL, &thread) == KERN_SUCCESS) {
  thread_deallocate(thread);
 }
}
void
redwood_cpufreq_init(void)
{
 const ml_topology_info_t *topology = ml_get_topology_info();
 uint32_t enabled = 0, cores[EPSS_DOMAINS] = { 0 }, before, here, macan = 0, mode = 0;
 if (PE_parse_boot_argn("macan", &macan, sizeof(macan)) && macan != 0) {
  if (PE_parse_boot_argn("macan_cpufreq", &mode, sizeof(mode)) && mode != 0) {
   macan_cpufreq(mode);
  }
  return;
 }
 if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
  return;
 }
 for (unsigned int i = 0; i < topology->num_cpus; i++) {
  cores[domain_of(topology->cpus[i].phys_id)]++;
 }
 here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
 before = loop_speed();
 for (uint32_t domain = 0; domain < EPSS_DOMAINS; domain++) {
  if (cores[domain] != 0) {
   raise_domain(domain, cores[domain]);
  }
 }
 printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
}
