#define LX_QMI_AS_IT_IS
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/skbuff.h>
#include "core.h"
#include "ce.h"
#include "debug.h"
#include "dp.h"
#include "dp_peer.h"
#include "wifi7/hal_desc.h"
#include "wifi7/hal.h"
#include "wifi7/hal_rx.h"
#include "hal.h"
#include "hif.h"
#include "htc.h"
#include "qmi.h"
#include "mac.h"
#include "wmi.h"
#include <lx_os.h>
#include "lx.h"
int ath12k_wifi7_hw_init(struct ath12k_base *ab);
int ath12k_wifi7_arch_init(struct ath12k_base *ab);
#define RADIO_REGISTERS 0x41e00000ul
#define RADIO_REGISTERS_SIZE 0x200000ul
#define WINDOW 0x80000u
#define WINDOW_OF(offset) (((offset) >> 19) & 0x3f)
#define UMAC_WINDOW 0x14
#define CE_WINDOW 0x37
#define CMEM_WINDOW 0x02
static struct {
 unsigned how;
 bool mission, closed;
 unsigned reads, writes, said, refused;
 u32 last, last_value;
 unsigned again;
 volatile bool looking, looks, data;
 unsigned same;
} bus;
static u32
window(u32 offset)
{
 if (offset < WINDOW)
  return (offset);
 switch (WINDOW_OF(offset)) {
 case UMAC_WINDOW:
  return (1 * WINDOW + (offset & (WINDOW - 1)));
 case CE_WINDOW:
  return (2 * WINDOW + (offset & (WINDOW - 1)));
 case CMEM_WINDOW:
  return (3 * WINDOW + (offset & (WINDOW - 1)));
 }
 return (~0u);
}
static bool
refused(u32 offset)
{
 bool known = window(offset) != ~0u;
 if (known && bus.mission && !bus.closed)
  return (false);
 if (bus.refused++ < 8)
  lx_tell("lx12: the chip's %#x is reached for %s: NOT DONE\n", offset, !known ? "which no window shows" :
      bus.closed ? "after the bus was stopped" : "while the firmware is not in mission mode");
 return (true);
}
#define SAID_FIRST 600
static void
say_repeats(void)
{
 if (bus.again != 0) {
  printf("lx12: ... and %u more to the chip's %#x, the last %#x\n", bus.again, bus.last, bus.last_value);
  bus.again = 0;
 }
}
static bool
said_write(u32 offset, u32 value)
{
 static struct {
  u32 offset, value;
 } pointers[32];
 static unsigned row;
 if (bus.looks) {
  for (unsigned i = 0; i < nitems(pointers); i++) {
   if (pointers[i].offset == offset || pointers[i].offset == 0) {
    bool moved = pointers[i].offset == 0 || pointers[i].value != value;
    pointers[i].offset = offset;
    pointers[i].value = value;
    bus.same += !moved;
    return (moved && bus.said < SAID_FIRST);
   }
  }
 }
 row = offset == bus.last ? row + 1 : 0;
 if (row >= 3) {
  bus.again++;
  bus.last_value = value;
  return (false);
 }
 say_repeats();
 bus.last = offset;
 return (bus.said < SAID_FIRST);
}
static u32
bus_read32(struct ath12k_base *ab, u32 offset)
{
 u32 value = 0;
 if (refused(offset))
  return (0);
 bus.reads++;
 if (bus.how >= 2 && ab->mem != NULL)
  value = ioread32((u8 *)ab->mem + window(offset));
 if (bus.said < SAID_FIRST) {
  say_repeats();
  bus.last = ~0u;
  bus.said++;
  printf("lx12: the chip's %#x reads %#x%s\n", offset, value, bus.how >= 2 ? "" : " (NOT READ: wifi_lx=1)");
 }
 return (value);
}
static void
bus_write32(struct ath12k_base *ab, u32 offset, u32 value)
{
 if (refused(offset))
  return;
 bus.writes++;
 if (said_write(offset, value)) {
  bus.said++;
  printf("lx12: %s the chip's %#x (at +%#x): %#x\n", bus.how >= 2 ? "about to write" : "NOT WRITTEN (wifi_lx=1):",
      offset, window(offset), value);
 }
 if (bus.how >= 2 && ab->mem != NULL) {
  __asm __volatile("dsb sy");
  iowrite32(value, (u8 *)ab->mem + window(offset));
 }
}
#define DATA_BUDGET 64
static void bus_rx_say(struct ath12k_base *ab);
static void bus_fw_stats(struct ath12k_base *ab);
static void
bus_looks(void *argument)
{
 struct ath12k_base *ab = argument;
 unsigned long long said = lx_os_nanoseconds();
 bus.looks = true;
 while (bus.looking) {
  bool more = false;
  if (lx_os_nanoseconds() - said > 10000000000ull) {
   said = lx_os_nanoseconds();
   if (bus.data) {
    bus_rx_say(ab);
    bus_fw_stats(ab);
   }
   lx_station_say();
   lx_memory_say();
  }
  for (unsigned i = 0; i < ab->hw_params->ce_count; i++)
   ath12k_ce_per_engine_service(ab, i);
  for (unsigned i = 0; bus.data && i < ATH12K_EXT_IRQ_GRP_NUM_MAX; i++)
   more |= ath12k_dp_service_srng(ath12k_ab_to_dp(ab), &ab->ext_irq_grp[i], DATA_BUDGET) >= DATA_BUDGET;
  if (!more)
   lx_pause_ms(1);
 }
 bus.looks = false;
}
static void
bus_quiet(void)
{
 bus.data = false;
 bus.looking = false;
 while (bus.looks)
  lx_pause_ms(1);
 say_repeats();
}
struct lx12_ddr_range {
 u64 start, size;
};
struct lx12_host_cap {
 u8 num_clients_valid;
 u32 num_clients;
 u8 bdf_support_valid, bdf_support;
 u8 cal_done_valid, cal_done;
 u8 ddr_range_valid;
 struct lx12_ddr_range ddr_range[3];
 u8 host_build_type_valid;
 s32 host_build_type;
 u8 ddr_type_valid;
 u32 ddr_type;
};
#define LX12_FIELD(type,len,size,array,tlv,field,sub) \
 { .data_type = type, .elem_len = len, .elem_size = size, .array_type = array, .tlv_type = tlv, \
   .offset = offsetof(struct lx12_host_cap, field), .ei_array = sub }
static const struct qmi_elem_info lx12_ddr_range_ei[] = {
 { .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = 8, .offset = offsetof(struct lx12_ddr_range, start) },
 { .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = 8, .offset = offsetof(struct lx12_ddr_range, size) },
 { .data_type = QMI_EOTI, .tlv_type = QMI_COMMON_TLV_TYPE },
};
static const struct qmi_elem_info lx12_host_cap_ei[] = {
 LX12_FIELD(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x10, num_clients_valid, NULL),
 LX12_FIELD(QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0x10, num_clients, NULL),
 LX12_FIELD(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x14, bdf_support_valid, NULL),
 LX12_FIELD(QMI_UNSIGNED_1_BYTE, 1, 1, NO_ARRAY, 0x14, bdf_support, NULL),
 LX12_FIELD(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x1a, cal_done_valid, NULL),
 LX12_FIELD(QMI_UNSIGNED_1_BYTE, 1, 1, NO_ARRAY, 0x1a, cal_done, NULL),
 LX12_FIELD(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x1f, ddr_range_valid, NULL),
 LX12_FIELD(QMI_STRUCT, 3, sizeof(struct lx12_ddr_range), STATIC_ARRAY, 0x1f, ddr_range, lx12_ddr_range_ei),
 LX12_FIELD(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x20, host_build_type_valid, NULL),
 LX12_FIELD(QMI_SIGNED_4_BYTE_ENUM, 1, 4, NO_ARRAY, 0x20, host_build_type, NULL),
 LX12_FIELD(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x2c, ddr_type_valid, NULL),
 LX12_FIELD(QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0x2c, ddr_type, NULL),
 { .data_type = QMI_EOTI, .tlv_type = QMI_COMMON_TLV_TYPE },
};
struct lx12_mac_req {
 u8 mac_addr_valid;
 u8 mac_addr[ETH_ALEN];
};
static const struct qmi_elem_info lx12_mac_req_ei[] = {
 { .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = 1, .array_type = NO_ARRAY, .tlv_type = 0x10,
   .offset = offsetof(struct lx12_mac_req, mac_addr_valid) },
 { .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = ETH_ALEN, .elem_size = 1, .array_type = STATIC_ARRAY, .tlv_type = 0x10,
   .offset = offsetof(struct lx12_mac_req, mac_addr) },
 { .data_type = QMI_EOTI, .tlv_type = QMI_COMMON_TLV_TYPE },
};
struct lx12_mac_resp {
 struct qmi_response_type_v01 resp;
};
static const struct qmi_elem_info lx12_mac_resp_ei[] = {
 { .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01), .array_type = NO_ARRAY,
   .tlv_type = 0x02, .offset = offsetof(struct lx12_mac_resp, resp), .ei_array = qmi_response_type_v01_ei },
 { .data_type = QMI_EOTI, .tlv_type = QMI_COMMON_TLV_TYPE },
};
static void
bus_send_mac(struct qmi_handle *qmi, struct sockaddr_qrtr *sq)
{
 static struct lx12_mac_req req;
 static struct lx12_mac_resp resp;
 unsigned how = lx_os_option("wifi_fw_mac");
 struct qmi_txn txn;
 u8 mac[ETH_ALEN];
 int ret;
 if (how == 0 || !lx_os_radio_address(mac)) {
  lx_tell("lx12: the firmware is not told the station's address\n");
  return;
 }
 memset(&req, 0, sizeof(req));
 memset(&resp, 0, sizeof(resp));
 req.mac_addr_valid = 1;
 for (unsigned i = 0; i < ETH_ALEN; i++)
  req.mac_addr[i] = how == 2 ? mac[ETH_ALEN - 1 - i] : mac[i];
 if ((ret = qmi_txn_init(qmi, &txn, lx12_mac_resp_ei, &resp)) < 0) {
  lx_tell("lx12: the station's address for the firmware: no transaction: %d\n", ret);
  return;
 }
 if ((ret = qmi_send_request(qmi, sq, &txn, 0x33, 9, lx12_mac_req_ei, &req)) < 0) {
  qmi_txn_cancel(&txn);
  lx_tell("lx12: the station's address for the firmware: not sent: %d\n", ret);
  return;
 }
 ret = qmi_txn_wait(&txn, msecs_to_jiffies(ATH12K_QMI_WLANFW_TIMEOUT_MS));
 lx_tell("lx12: the station's address (%s order) given to the firmware: wait %d, result %u, error %u\n",
     how == 2 ? "reversed" : "normal", ret, resp.resp.result, resp.resp.error);
}
int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
 lx_tell("lx12: QMI request %#x, transaction %p, max %zu bytes\n", msg_id, txn, largest);
 if (msg_id == QMI_WLANFW_M3_INFO_REQ_V01 || msg_id == 0x5a) {
  u64 address;
  u32 bytes;
  memcpy(&address, structure, sizeof(address));
  memcpy(&bytes, (const u8 *)structure + sizeof(address), sizeof(bytes));
  lx_tell("lx12: firmware DMA %#llx, %u bytes, page offset %#llx\n",
      (unsigned long long)address, bytes, (unsigned long long)(address & (PAGE_SIZE - 1)));
 }
 if (msg_id == QMI_WLANFW_PHY_CAP_REQ_V01) {
  lx_tell("lx12: skip optional PCI PHY capability probe; use driver fallback\n");
  return (-EOPNOTSUPP);
 }
 if (msg_id == QMI_WLANFW_IND_REGISTER_REQ_V01) {
  static struct qmi_wlanfw_ind_register_req_msg_v01 ind;
  ind = *(const struct qmi_wlanfw_ind_register_req_msg_v01 *)structure;
  ind.request_mem_enable_valid = ind.request_mem_enable = 0;
  ind.fw_mem_ready_enable_valid = ind.fw_mem_ready_enable = 0;
  return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, &ind));
 }
 if (msg_id == QMI_WLANFW_HOST_CAP_REQ_V01) {
  const struct qmi_wlanfw_host_cap_req_msg_v01 *req = structure;
  static struct lx12_host_cap cap;
  memset(&cap, 0, sizeof(cap));
  cap.num_clients_valid = 1;
  cap.num_clients = 1;
  cap.bdf_support_valid = 1;
  cap.bdf_support = 1;
  cap.cal_done_valid = 1;
  cap.cal_done = req->cal_done;
  cap.ddr_range_valid = 1;
  cap.ddr_range[0].start = 0xb0000000ull;
  cap.ddr_range[0].size = 0x10000000ull + 0x10000ull;
  cap.ddr_range[1].start = 0xd6e00000ull;
  cap.ddr_range[1].size = 0xc00000ull;
  cap.host_build_type_valid = 1;
  cap.host_build_type = 1;
  cap.ddr_type_valid = 1;
  cap.ddr_type = 8;
  lx_tell("lx12: host capabilities as icnss2 sends them for the WCN7750\n");
  return (qmi_send_request(qmi, sq, txn, msg_id, 1024, lx12_host_cap_ei, &cap));
 }
 if (msg_id == QMI_WLANFW_WLAN_MODE_REQ_V01) {
  u32 mode = ((const struct qmi_wlanfw_wlan_mode_req_msg_v01 *)structure)->mode;
  if (mode != ATH12K_FIRMWARE_MODE_NORMAL)
   bus_quiet();
  else
   bus_send_mac(qmi, sq);
  bus.mission = mode == ATH12K_FIRMWARE_MODE_NORMAL;
  lx_tell("lx12: the firmware is asked for mode %u (0: mission, 4: off, 7: calibration)\n", mode);
 }
 return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure));
}
static int
bus_start(struct ath12k_base *ab)
{
 printf("lx12: the driver starts the bus (%u reads and %u writes of the chip's registers so far)\n", bus.reads, bus.writes);
 ath12k_ce_rx_post_buf(ab);
 say_repeats();
 lx_tell("lx12: the rings are set up and the copy engines have their buffers: the bus is started\n");
 bus.looking = true;
 if (lx_os_thread("wifi-rings", bus_looks, ab) != 0) {
  bus.looking = false;
  return (-ENOMEM);
 }
 return (0);
}
static void
bus_stop(struct ath12k_base *ab)
{
 bus_quiet();
 bus.closed = true;
 ath12k_ce_cleanup_pipes(ab);
 printf("lx12: the driver stops the bus (%u reads and %u writes in all, %u of them a ring's pointer written what it held)\n",
     bus.reads, bus.writes, bus.same);
}
static int
bus_map_service_to_pipe(struct ath12k_base *ab, u16 service, u8 *up, u8 *down)
{
 bool has_up = false, has_down = false;
 for (u32 i = 0; i < ab->hw_params->svc_to_ce_map_len; i++) {
  const struct service_to_pipe *entry = &ab->hw_params->svc_to_ce_map[i];
  u32 direction = __le32_to_cpu(entry->pipedir);
  if (__le32_to_cpu(entry->service_id) != service)
   continue;
  if (direction == PIPEDIR_IN || direction == PIPEDIR_INOUT) {
   *down = (u8)__le32_to_cpu(entry->pipenum);
   has_down = true;
  }
  if (direction == PIPEDIR_OUT || direction == PIPEDIR_INOUT) {
   *up = (u8)__le32_to_cpu(entry->pipenum);
   has_up = true;
  }
 }
 return (has_up && has_down ? 0 : -ENOENT);
}
static int
bus_power_up(struct ath12k_base *ab)
{
 return (-lx_os_rproc_boot());
}
static void
bus_power_down(struct ath12k_base *ab, bool is_suspend)
{
}
static void
bus_irq_enable(struct ath12k_base *ab)
{
 for (unsigned i = 0; i < ATH12K_EXT_IRQ_GRP_NUM_MAX; i++) {
  ab->ext_irq_grp[i].ab = ab;
  ab->ext_irq_grp[i].grp_id = i;
 }
 bus.data = true;
}
static void
bus_fw_stats(struct ath12k_base *ab)
{
 struct ath12k *ar = ab->pdevs[0].ar;
 struct ath12k_fw_stats_pdev *p;
 struct ath12k_fw_stats_vdev *v;
 if (ar == NULL)
  return;
 spin_lock_bh(&ar->data_lock);
 list_for_each_entry(p, &ar->fw_stats.pdevs, list) {
  lx_tell("lx12: radio counters: rx frames %u tx frames %u busy %u of %u cycles, noise %d dBm; fcs bad %u, "
      "ack bad %u, rts bad %u good %u, phy errors %u, beacons missed %u\n",
      p->rx_frame_count, p->tx_frame_count, p->rx_clear_count, p->cycle_count, p->ch_noise_floor,
      p->fcs_bad, p->ack_rx_bad, p->rts_bad, p->rts_good, p->phy_err_count, p->no_beacons);
  lx_tell("lx12: radio rx: statuses %d mpdus to host %d msdus %d, local mpdus %d, phy errors %d dropped %d, "
      "mpdu errors (fcs, mic, enc) %d, route changes %d\n",
      p->status_rcvd, p->htt_mpdus, p->htt_msdus, p->loc_mpdus, p->phy_errs, p->phy_err_drop,
      p->mpdu_errs, p->mid_ppdu_route_change);
 }
 list_for_each_entry(v, &ar->fw_stats.vdevs, list) {
  lx_tell("lx12: vdev %u counters: rx frames %u errors %u discarded %u; tx be %u retries %u failures %u not acked %u; "
      "snr beacon %u data %u\n", v->vdev_id, v->num_rx_frames, v->num_rx_err, v->num_rx_discard,
      v->num_tx_frames[0], v->num_tx_frames_retries[0], v->num_tx_frames_failures[0], v->num_tx_not_acked,
      v->beacon_snr, v->data_snr);
 }
 spin_unlock_bh(&ar->data_lock);
 ath12k_fw_stats_reset(ar);
 if (bus.looking && lx_os_option("wifi_reauth")) {
  struct ath12k_dp *dp = ath12k_ab_to_dp(ab);
  struct ath12k_dp_link_peer *peer;
  u8 addr[ETH_ALEN];
  int vdev = -1;
  spin_lock_bh(&dp->dp_lock);
  list_for_each_entry(peer, &dp->peers, list) {
   if (peer->sta != NULL) {
    ether_addr_copy(addr, peer->addr);
    vdev = peer->vdev_id;
    break;
   }
  }
  spin_unlock_bh(&dp->dp_lock);
  if (vdev >= 0)
   lx_tell("lx12: the access point's peer authorized again: %d\n",
       ath12k_wmi_set_peer_param(ar, addr, vdev, WMI_PEER_AUTHORIZE, 1));
 }
 if (bus.looking && ar->num_started_vdevs != 0 &&
     ath12k_wmi_send_stats_request_cmd(ar, WMI_REQUEST_VDEV_STAT, 0, ath12k_mac_get_target_pdev_id(ar)) != 0)
  lx_tell("lx12: the vdev's counters could not be asked for\n");
 if (bus.looking && ath12k_wmi_send_stats_request_cmd(ar, WMI_REQUEST_PDEV_STAT, 0,
     ath12k_mac_get_target_pdev_id(ar)) != 0)
  lx_tell("lx12: the radio's counters could not be asked for\n");
}
static void
bus_rx_say(struct ath12k_base *ab)
{
 struct ath12k_dp *dp = ath12k_ab_to_dp(ab);
 if (dp == NULL)
  return;
 struct ath12k_device_dp_stats *stats = &dp->device_stats;
 struct hal_srng *command_ring = &dp->hal->srng_list[dp->reo_cmd_ring.ring_id];
 struct hal_srng *status_ring = &dp->hal->srng_list[dp->reo_status_ring.ring_id];
 lx_tell("lx12: REO pointers: command hp %u hw-tp %u status tp %u hw-hp %u\n",
     command_ring->u.src_ring.hp,
     command_ring->u.src_ring.tp_addr ? READ_ONCE(*command_ring->u.src_ring.tp_addr) : 0,
     status_ring->u.dst_ring.tp,
     status_ring->u.dst_ring.hp_addr ? READ_ONCE(*status_ring->u.dst_ring.hp_addr) : 0);
 for (unsigned ring = 0; ring < ARRAY_SIZE(stats->reo_rx); ring++) {
  unsigned count = 0;
  for (unsigned device = 0; device < ARRAY_SIZE(stats->reo_rx[ring]); device++)
   count += stats->reo_rx[ring][device];
  lx_tell("lx12: RX ring %u packets %u destination-errors %u\n",
      ring, count, stats->hal_reo_error[ring]);
 }
 for (unsigned code = 0; code < ARRAY_SIZE(stats->rxdma_error); code++)
  if (stats->rxdma_error[code])
   lx_tell("lx12: RXDMA code %u count %u\n", code, stats->rxdma_error[code]);
 for (unsigned code = 0; code < ARRAY_SIZE(stats->reo_error); code++)
  if (stats->reo_error[code])
   lx_tell("lx12: REO code %u count %u\n", code, stats->reo_error[code]);
 lx_tell("lx12: RX exception packets %u invalid-rbm %u\n", stats->err_ring_pkts, stats->invalid_rbm);
 struct ath12k_dp_link_peer *peer;
 spin_lock_bh(&dp->dp_lock);
 list_for_each_entry(peer, &dp->peers, list) {
  if (peer->dp_peer == NULL || peer->sta == NULL)
   continue;
  for (unsigned tid = 0; tid <= IEEE80211_NUM_TIDS; tid++) {
   struct ath12k_reoq_buf *buf = &peer->dp_peer->rx_tid[tid].qbuf;
   if (!(peer->rx_tid_active_bitmask & (1u << tid)) || buf->vaddr == NULL)
    continue;
   volatile struct hal_rx_reo_queue *q = PTR_ALIGN(buf->vaddr, HAL_LINK_DESC_ALIGN);
   lx_tell("lx12: REO queue peer %u tid %u auth %u cfg %#x mpdu %u msdu %u duplicates %u\n",
       peer->peer_id, tid, peer->is_authorized, le32_to_cpu(q->info0),
       le32_to_cpu(q->processed_mpdus), le32_to_cpu(q->processed_msdus),
       le32_get_bits(q->info3, HAL_RX_REO_QUEUE_INFO3_DUPLICATE_COUNT));
   if (bus.data) {
    struct ath12k_hal_reo_cmd cmd = {};
    cmd.flag = HAL_REO_CMD_FLG_NEED_STATUS;
    cmd.addr_lo = lower_32_bits(buf->paddr_aligned);
    cmd.addr_hi = upper_32_bits(buf->paddr_aligned);
    int number = ath12k_wifi7_hal_reo_cmd_send(ab,
        &dp->hal->srng_list[dp->reo_cmd_ring.ring_id],
        HAL_REO_CMD_GET_QUEUE_STATS, &cmd);
    lx_tell("lx12: REO query tid %u command %d\n", tid, number);
   }
  }
 }
 spin_unlock_bh(&dp->dp_lock);
}
static void
bus_irq_disable(struct ath12k_base *ab)
{
 bus_quiet();
 bus_rx_say(ab);
}
static const struct ath12k_hif_ops bus_ops = {
 .read32 = bus_read32,
 .write32 = bus_write32,
 .irq_enable = bus_irq_enable,
 .irq_disable = bus_irq_disable,
 .start = bus_start,
 .stop = bus_stop,
 .map_service_to_pipe = bus_map_service_to_pipe,
 .power_up = bus_power_up,
 .power_down = bus_power_down,
};
void
lx12_host_cap(struct ath12k_base *ab, struct qmi_wlanfw_host_cap_req_msg_v01 *req)
{
 req->ddr_range_valid = 1;
 req->ddr_range[0].start = 0xb0000000ull;
 req->ddr_range[0].size = 0x10000000ull + 0x10000ull;
 req->ddr_range[1].start = 0xd6e00000ull;
 req->ddr_range[1].size = 0xc00000ull;
 req->host_build_type_valid = 1;
 req->host_build_type = 1;
}
int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
 return (-EINVAL);
}
bool
of_machine_is_compatible(const char *compatible)
{
 return (false);
}
int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
 return (-ENODEV);
}
struct reserved_mem *
of_reserved_mem_lookup(struct device_node *np)
{
 return (NULL);
}
int
linuxkpi_device_get_mac_address(struct device *dev, char *address)
{
 return (lx_os_radio_address((unsigned char *)address) ? 0 : -ENOENT);
}
static struct ath12k_base *volatile driver;
static struct ath12k_hw_params wcn7750;
int
lx_start(void)
{
 static struct device device;
 static struct device_node node;
 struct ath12k_base *ab;
 unsigned long long began;
 bool up;
 int ret;
 bus.how = lx_os_option("wifi_lx");
 if (lx_prepare() != 0) {
  lx_tell("lx12: no thread, or no memory the radio can be given\n");
  return (ENOMEM);
 }
 ath12k_debug_mask = lx_os_option("wifi_quiet") ? 0 : (ATH12K_DBG_QMI | ATH12K_DBG_BOOT | ATH12K_DBG_WMI |
     ATH12K_DBG_REG | ATH12K_DBG_MAC);
 if (lx_os_option("wifi_quiet"))
  bus.said = SAID_FIRST;
 device.of_node = &node;
 device.kobj.name = "wifi";
 ab = ath12k_core_alloc(&device, 0, ATH12K_BUS_PCI);
 if (ab == NULL)
  return (ENOMEM);
 ab->dev = &device;
 ab->hif.ops = &bus_ops;
 ab->fw_mode = ATH12K_FIRMWARE_MODE_NORMAL;
 ab->id.bdf_search = ATH12K_BDF_SEARCH_DEFAULT;
 ab->static_window_map = false;
 ab->target_mem_mode = ATH12K_QMI_MEMORY_MODE_DEFAULT;
 ab->hw_rev = ATH12K_HW_QCC2072_HW10;
 if ((ret = ath12k_wifi7_hw_init(ab)) != 0) {
  lx_tell("lx12: no hardware parameters for the QCC2072: %d\n", ret);
  return (-ret);
 }
 wcn7750 = *ab->hw_params;
 wcn7750.name = "wcn7750 (qcc2072 behind the wpss)";
 wcn7750.qmi_service_ins_id = 3;
 wcn7750.internal_sleep_clock = false;
 wcn7750.qmi_cnss_feature_bitmap = BIT(CNSS_QDSS_CFG_MISS_V01) | BIT(CNSS_AUX_UC_SUPPORT_V01);
 wcn7750.supports_suspend = false;
 wcn7750.supports_aspm = false;
 ab->hw_params = &wcn7750;
 ab->mem = ioremap(RADIO_REGISTERS, RADIO_REGISTERS_SIZE);
 ab->mem_len = RADIO_REGISTERS_SIZE;
 if (ab->mem == NULL)
  return (ENOMEM);
 if ((ret = ath12k_core_pre_init(ab)) != 0 || (ret = ath12k_hal_srng_init(ab)) != 0 ||
     (ret = ath12k_ce_alloc_pipes(ab)) != 0) {
  lx_tell("lx12: the driver does not get as far as its exchange with the firmware: %d\n", ret);
  return (-ret);
 }
 ab->qmi.ce_cfg.tgt_ce = ab->hw_params->target_ce_config;
 ab->qmi.ce_cfg.tgt_ce_len = ab->hw_params->target_ce_count;
 ab->qmi.ce_cfg.svc_to_ce_map = ab->hw_params->svc_to_ce_map;
 ab->qmi.ce_cfg.svc_to_ce_map_len = ab->hw_params->svc_to_ce_map_len;
 ab->qmi.service_ins_id = ab->hw_params->qmi_service_ins_id;
 if ((ret = ath12k_wifi7_arch_init(ab)) != 0) {
  lx_tell("lx12: the driver's Wi-Fi 7 part does not start: %d\n", ret);
  return (-ret);
 }
 printf("lx12: the driver (\"%s\") is set up; its exchange with the firmware begins (registers: %s)\n",
     ab->hw_params->name, bus.how >= 2 ? "for real" : "NOT TOUCHED, wifi_lx=1");
 if ((ret = ath12k_core_init(ab)) != 0) {
  lx_tell("lx12: the driver's start fails: %d\n", ret);
  return (-ret);
 }
 lx_tell("lx12: core initialization returned; waiting for firmware registration\n");
 began = lx_os_nanoseconds();
 while (!test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) && !test_bit(ATH12K_FLAG_QMI_FAIL, &ab->dev_flags) &&
     lx_os_nanoseconds() - began < 60000000000ull)
  lx_pause_ms(10);
 up = test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) && bus.looks;
 lx_tell("lx12: %llu ms after its start the driver %s; its flags: %#lx\n", (lx_os_nanoseconds() - began) / 1000000ull,
     up ? "IS UP: the firmware has said \"unified ready\"" : test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) ?
     "HAS GIVEN UP on the way and wound itself down" : "HAS NOT GOT THROUGH its exchange with the firmware",
     ab->dev_flags);
 if (!up) {
  lx_pause_ms(600);
  ath12k_qmi_deinit_service(ab);
  return (test_bit(ATH12K_FLAG_REGISTERED, &ab->dev_flags) ? ETIMEDOUT : EIO);
 }
 lx_pause_ms(1000);
 driver = ab;
 if (lx_os_option("wifi_station")) {
  if ((ret = lx_station_offer()) == 0)
   return (0);
 } else if (lx_os_option("wifi_join")) {
  if ((ret = lx_station_join()) == 0) {
   if (lx_os_option("wifi_hold")) {
    lx_pause_ms(1000 * lx_os_option("wifi_hold"));
    lx_station_say();
   }
   return (0);
  }
 } else {
  ret = lx_os_option("wifi_scan") ? lx_station_scan() : 0;
 }
 lx_stop();
 return (ret);
}
int
lx_prepare(void)
{
 return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}
void
lx_stop(void)
{
 struct ath12k_base *ab = driver;
 if (ab == NULL)
  return;
 driver = NULL;
 lx_station_withdraw();
 lx_station_leave();
 ath12k_core_deinit(ab);
 lx_tell("lx12: the driver is down again\n");
}
int
lx_transmit(const unsigned char *frame, unsigned long length)
{
 return (driver != NULL ? lx_station_send(frame, length) : ENETDOWN);
}
