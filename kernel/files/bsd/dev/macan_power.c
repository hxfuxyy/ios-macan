#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <sys/macan_dsp.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
int macan_rpmh_vote(const char *resource, uint32_t millivolts, uint32_t mode, uint32_t enable);
void macan_wlan_power(int on);
#define RSC_DRV2 0x18920000UL
#define CMD_DB_PHYS 0x81c60000UL
#define CMD_DB_SIZE 0x20000UL
#define TLMM 0x0f100000UL
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
static vm_offset_t rsc;
static uint32_t
cmd_db_address(const char *name)
{
 static const struct cmd_db_header *db;
 char id[8] = { 0 };
 if (db == NULL) {
  db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
 }
 if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
  return 0;
 }
 strlcpy(id, name, sizeof(id));
 for (unsigned s = 0; s < 8; s++) {
  const struct cmd_db_slave *slave = &db->slave[s];
  const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);
  if (slave->id == 0 || slave->count > 512 || slave->header_offset > CMD_DB_SIZE - 0x1000) {
   continue;
  }
  for (unsigned i = 0; i < slave->count; i++) {
   if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
    return entry[i].address;
   }
  }
 }
 return 0;
}
#define R(off) (*(volatile uint32_t *)(t + (off)))
static int
macan_rpmh_vote_locked(const char *resource, uint32_t millivolts, uint32_t mode, uint32_t enable)
{
 uint32_t base = cmd_db_address(resource), addresses[3], data[3], saved[15];
 unsigned offsets[15], used = 4, count, major, control, status, wait, cmd_enable, msg, stride, tries = 0;
 vm_offset_t t;
 if (base == 0) {
  printf("macan-power: RPMh has no \"%s\"\n", resource);
  return 0;
 }
 if (rsc == 0) {
  rsc = ml_io_map(RSC_DRV2, 0x10000);
 }
 major = (*(volatile uint32_t *)rsc >> 16) & 255;
 if (major < 2 || major > 4) {
  printf("macan-power: unsupported RSC version %u\n", major);
  return 0;
 }
 control = major >= 3 ? 0x24 : 0x14;
 status = major >= 3 ? 0x28 : 0x18;
 wait = major >= 3 ? 0x20 : 0x10;
 cmd_enable = major >= 3 ? 0x2c : 0x1c;
 msg = major >= 3 ? 0x34 : 0x30;
 stride = major >= 3 ? 24 : 20;
 t = rsc + 0xd00;
 if (!(R(status) & 1)) {
  printf("macan-power: active TCS 0 busy: no vote for %s\n", resource);
  return 0;
 }
 offsets[0] = 0;
 offsets[1] = wait;
 offsets[2] = control;
 offsets[3] = cmd_enable;
 for (unsigned i = 0; i < 3; i++) {
  for (unsigned j = 0; j < 3; j++) {
   offsets[used++] = msg + i * stride + j * 4;
  }
 }
 for (unsigned i = 0; i < used; i++) {
  saved[i] = R(offsets[i]);
 }
 saved[2] &= ~(1u << 24);
 R(control) = saved[2] & ~(1u << 24);
 R(control) = saved[2] & ~((1u << 24) | (1u << 16));
 R(8) = 1;
 if (enable) {
  addresses[0] = base; data[0] = millivolts;
  addresses[1] = base + 8; data[1] = mode;
  addresses[2] = base + 4; data[2] = 1;
  count = 3;
 } else {
  addresses[0] = base + 4; data[0] = 0;
  count = 1;
 }
 for (unsigned i = 0; i < count; i++) {
  R(msg + i * stride) = 0x10108;
  R(msg + i * stride + 4) = addresses[i];
  R(msg + i * stride + 8) = data[i];
 }
 R(wait) = (1u << count) - 1;
 R(cmd_enable) = (1u << count) - 1;
 R(0) = saved[0] | 1;
 R(control) = 1u << 16;
 __asm__ volatile ("dsb sy");
 R(control) = (1u << 16) | (1u << 24);
 while (!(R(4) & 1) && ++tries < 2000) {
  delay_for_interval(100, NSEC_PER_USEC);
 }
 if (tries == 2000) {
  printf("macan-power: RPMh timeout for %s; TCS not reused\n", resource);
  return 0;
 }
 R(control) = 1u << 16;
 R(control) = 0;
 R(8) = 1;
 for (unsigned i = used; i > 0; i--) {
  R(offsets[i - 1]) = saved[i - 1];
 }
 printf("macan-power: %s (RPMh %#x) %u mV mode %u: %s\n", resource, base, millivolts, mode, enable ? "on" : "off");
 return 1;
}
int
macan_rpmh_vote(const char *resource, uint32_t millivolts, uint32_t mode, uint32_t enable)
{
 if (!ml_get_interrupts_enabled()) {
  if (!lck_mtx_try_lock(&macan_rpmh_lock)) { return 0; }
 } else { lck_mtx_lock(&macan_rpmh_lock); }
 int result = macan_rpmh_vote_locked(resource, millivolts, mode, enable);
 lck_mtx_unlock(&macan_rpmh_lock);
 return result;
}
static const struct {
 const char *name;
 uint32_t millivolts;
} wlan_rails[] = {
 { "S2D_E0", 864 }, { "S4D_E0", 1840 }, { "L2D_E0", 1800 }, { "L3D_E0", 1200 }, { "S4F_E0", 1256 },
};
void
macan_wlan_power(int on)
{
 static vm_offset_t tlmm, sol;
 static int voted[5];
 if (tlmm == 0) {
  tlmm = ml_io_map(TLMM + 0x1000 * 16, 0x3000);
 }
 if (on) {
  for (unsigned i = 0; i < 5; i++) {
   voted[i] = macan_rpmh_vote(wlan_rails[i].name, wlan_rails[i].millivolts, 7, 1);
  }
  delay_for_interval(10, NSEC_PER_MSEC);
  printf("macan-power: WLAN_EN (gpio 16) was cfg %#x io %#x; SW_CTRL (gpio 18) cfg %#x io %#x\n",
      *(volatile uint32_t *)tlmm, *(volatile uint32_t *)(tlmm + 4),
      *(volatile uint32_t *)(tlmm + 0x2000), *(volatile uint32_t *)(tlmm + 0x2004));
  *(volatile uint32_t *)(tlmm + 0x2000) = 1u << 2 | 1u;
  if (sol == 0) {
   sol = ml_io_map(TLMM + 0x1000 * 95, 0x1000);
  }
  *(volatile uint32_t *)(sol + 4) = 0;
  *(volatile uint32_t *)sol = 1u << 9 | 4u << 6;
  *(volatile uint32_t *)(tlmm + 4) = 2;
  *(volatile uint32_t *)tlmm = 1u << 9 | 4u << 6;
  __asm__ volatile ("dsb sy");
  delay_for_interval(100, NSEC_PER_MSEC);
  printf("macan-power: WLAN_EN high: cfg %#x io %#x; SW_CTRL now cfg %#x io %#x; host-sol cfg %#x\n",
      *(volatile uint32_t *)tlmm, *(volatile uint32_t *)(tlmm + 4), *(volatile uint32_t *)(tlmm + 0x2000),
      *(volatile uint32_t *)(tlmm + 0x2004), *(volatile uint32_t *)sol);
 } else {
  *(volatile uint32_t *)(tlmm + 4) = 0;
  delay_for_interval(10, NSEC_PER_MSEC);
  for (unsigned i = 5; i > 0; i--) {
   if (voted[i - 1]) {
    macan_rpmh_vote(wlan_rails[i - 1].name, 0, 0, 0);
    voted[i - 1] = 0;
   }
  }
  printf("macan-power: WLAN_EN low, the WCN7750's votes dropped\n");
 }
}
