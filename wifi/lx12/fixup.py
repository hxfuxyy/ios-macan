#!/usr/bin/env python3

import re
import sys
from pathlib import Path

sys_, ath, ref = (Path(a) for a in sys.argv[1:4])
lkpi = sys_ / 'compat/linuxkpi/common/include'

def edit(path, old, new, marker):
    text = path.read_text()
    if marker in text:
        return
    if text.count(old) != 1:
        raise SystemExit(f'fixup.py: {path}: expected one {old[:50]!r}')
    path.write_text(text.replace(old, new))

edit(lkpi / 'net/cfg80211.h', '\tuint8_t\t\t\t\t\teht_gi;\n};', '\tuint8_t\t\t\t\t\teht_gi;\n\tuint8_t\t\t\t\t\teht_ru_alloc;\n};',
     'eht_ru_alloc')
edit(lkpi / 'net/mac80211.h', 'struct ieee80211_sta {\n\t/* TODO FIXME */\n',
     'struct ieee80211_sta {\n\t/* TODO FIXME */\n\tuint16_t\teml_cap;\t\t/* (Linux 7.0, for ath12k) */\n', 'eml_cap;\t\t/* (Linux 7.0')
edit(lkpi / 'net/mac80211.h', 'struct ieee80211_bss_conf {\n',
     'struct ieee80211_bss_conf {\n\tbool\teht_80mhz_full_bw_ul_mumimo, eht_disable_mcs15;\t/* (Linux 7.0, for ath12k) */\n',
     'eht_disable_mcs15')
edit(lkpi / 'linux/ieee80211.h', '\t\t\t\t} __packed addba_req;\n',
     '\t\t\t\t} __packed addba_req;\n\t\t\t\tstruct {\n\t\t\t\t\tuint8_t action_code;\n\t\t\t\t\tuint8_t dialog_token;\n'
     '\t\t\t\t\t__le16 status;\n\t\t\t\t\t__le16 capab;\n\t\t\t\t\t__le16 timeout;\n'
     '\t\t\t\t\tuint8_t variable[0];\n\t\t\t\t} __packed addba_resp;\n', 'addba_resp;')

radiotap = lkpi / 'net/ieee80211_radiotap.h'
text = radiotap.read_text()
if 'ieee80211_radiotap_eht_data' not in text:
    names = ['eht_known', 'eht_data', 'eht_user_info', 'eht_usig_common', 'eht_usig_mu', 'eht_usig_tb']
    insert = '\n/* (Linux 7.0.14\'s EHT radiotap enum types, for ath12k; the values are net80211\'s macros) */\n' + ''.join(
        f'enum ieee80211_radiotap_{n} {{ LX12_RADIOTAP_{n.upper()}_ }};\n' for n in names)
    end = text.rindex('#endif')
    radiotap.write_text(text[:end] + insert + text[end:])

hw = ath / 'wifi7/hw.c'
hw_text = hw.read_text()
hw_text = re.sub(r'\n\t\.link_sta_statistics\s*=\s*ath12k_mac_op_link_sta_statistics,', '', hw_text)
hw.write_text(hw_text)

mon = ath / 'wifi7/dp_mon.c'
mon_text = mon.read_text()
if '<net/ieee80211_radiotap.h>' not in mon_text:
    mon.write_text(mon_text.replace('#include "hal_desc.h"\n', '#include <net/ieee80211_radiotap.h>\n#include "hal_desc.h"\n', 1))

edit(lkpi / 'net/mac80211.h', '\tIEEE80211_HW_SUPPORTS_MULTI_BSSID,\n',
     '\tIEEE80211_HW_SUPPORTS_MULTI_BSSID,\n\tIEEE80211_HW_MLO_MCAST_MULTI_LINK_TX,\t/* (Linux 7.0, for ath12k) */\n',
     'IEEE80211_HW_MLO_MCAST_MULTI_LINK_TX')
mac = lkpi / 'net/mac80211.h'
text = mac.read_text()
if 'LX12_HELPERS' not in text:
    helpers = r"""
/* LX12_HELPERS: Linux 7.0.14's, for ath12k (EML/EMLSR timings are of multi-link operation: none here) */
#define IEEE80211_RADIOTAP_EHT_USER_INFO_SPATIAL_CONFIG_M	0x3f000000
#define IEEE80211_RADIOTAP_EHT_USER_INFO_SPATIAL_CONFIG_KNOWN_M	0x00000040
#define IEEE80211_RADIOTAP_EHT_KNOWN_TAIL1		0x00004000
#define IEEE80211_RADIOTAP_EHT_KNOWN_RU_MRU_SIZE_OM	0x00400000
#define IEEE80211_RADIOTAP_EHT_KNOWN_RU_MRU_INDEX_OM	0x00800000
#define IEEE80211_RADIOTAP_EHT_KNOWN_DISREGARD_S	0x00000200
#define IEEE80211_RADIOTAP_EHT_KNOWN_DISREGARD_O	0x00000100
#define IEEE80211_RADIOTAP_EHT_KNOWN_CRC1		0x00002000
#define IEEE80211_RADIOTAP_EHT_DATA1_RU_SIZE		0x0000001f
#define IEEE80211_RADIOTAP_EHT_DATA1_RU_INDEX		0x00001fe0
#define IEEE80211_RADIOTAP_EHT_DATA0_DISREGARD_S	0x000c0000
#define IEEE80211_RADIOTAP_EHT_DATA0_DISREGARD_O	0x003c0000
#define IEEE80211_RADIOTAP_EHT_DATA0_CRC1_O		0x03c00000
static inline uint32_t ieee80211_emlsr_pad_delay_in_us(uint16_t eml_cap) { return 0; }
static inline uint32_t ieee80211_emlsr_trans_delay_in_us(uint16_t eml_cap) { return 0; }
static inline uint32_t ieee80211_eml_trans_timeout_in_us(uint16_t eml_cap) { return 0; }
static inline bool ieee80211_queue_stopped(struct ieee80211_hw *hw, int queue) { return false; }
static inline const struct element *
cfg80211_find_ext_elem(uint8_t ext_eid, const uint8_t *ies, size_t len)
{
	const struct element *elem;

	for (elem = (const void *)ies; (const uint8_t *)elem + 2 <= ies + len &&
	    (const uint8_t *)elem + 2 + elem->datalen <= ies + len; elem = (const void *)(elem->data + elem->datalen))
		if (elem->id == 255 && elem->datalen >= 1 && elem->data[0] == ext_eid)
			return elem;
	return NULL;
}
static inline uint8_t
ieee80211_eht_mcs_nss_size(const struct ieee80211_he_cap_elem *he_cap, const struct ieee80211_eht_cap_elem_fixed *eht_cap,
    bool from_ap)
{
	uint8_t count = 0;

	if (he_cap->phy_cap_info[0] & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G)
		return 3;
	if (he_cap->phy_cap_info[0] & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G)
		count += 3;
	if (he_cap->phy_cap_info[0] & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G)
		count += 3;
	if (eht_cap->phy_cap_info[0] & IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ)
		count += 3;
	return count ? count : (from_ap ? 3 : 4);
}
"""
    end = text.rindex('#endif')
    mac.write_text(text[:end] + helpers + text[end:])

edit(lkpi / 'linux/slab.h', 'kmalloc(size_mul((_n) * sizeof(typeof(_p))), default_gfp', 'kmalloc(size_mul((_n), sizeof(typeof(_p))), default_gfp', 'kmalloc(size_mul((_n), sizeof(typeof(_p))), default_gfp')

qmi = ath / 'qmi.c'
qmi_text = qmi.read_text()
if 'lx12_host_cap' not in qmi_text:
    start = qmi_text.index('int ath12k_qmi_host_cap_send(struct ath12k_base *ab)')
    at = qmi_text.index('\tret = qmi_txn_init(', start)
    qmi_text = (qmi_text[:at] + '\t{\n\t\textern void lx12_host_cap(struct ath12k_base *, struct qmi_wlanfw_host_cap_req_msg_v01 *);\n'
                '\n\t\tlx12_host_cap(ab, &req);\t/* (wifi/lx12: the WCN7750\'s memory windows) */\n\t}\n\n' + qmi_text[at:])
    qmi.write_text(qmi_text)

edit(lkpi / 'linux/spinlock.h', 'DEFINE_LOCK_GUARD_1(spinlock_irqsave,',
     'DEFINE_LOCK_GUARD_1(spinlock_bh,\n    spinlock_t,\n    spin_lock_bh(_T->lock),\n    spin_unlock_bh(_T->lock))\n\n'
     'DEFINE_LOCK_GUARD_1(spinlock_irqsave,', 'DEFINE_LOCK_GUARD_1(spinlock_bh,')
qmi_text = qmi.read_text()
if 'lx12_after_bdf' not in qmi_text:
    old = ('\tret = ath12k_qmi_host_cap_send(ab);\n\tif (ret < 0) {\n'
           '\t\tath12k_warn(ab, "failed to send qmi host cap for device id %d: %d\\n",\n'
           '\t\t\t    ab->device_id, ret);\n\t\treturn ret;\n\t}\n')
    assert qmi_text.count(old) == 1, 'host cap'
    qmi_text = qmi_text.replace(old, old + '\tath12k_qmi_driver_event_post(qmi, ATH12K_QMI_EVENT_FW_MEM_READY, NULL);\t/* (wifi/lx12) */\n')
    old = ('\t\t\tath12k_warn(ab, "qmi failed to send aux_uc info req: %d\\n", ret);\n'
           '\t\t\treturn ret;\n\t\t}\n\t}\n')
    assert qmi_text.count(old) == 1, 'aux'
    qmi_text = qmi_text.replace(old, old + '\n\tlx12_after_bdf(ab);\t/* (wifi/lx12) */\n')
    at = qmi_text.index('static int ath12k_qmi_wlanfw_mode_send(')
    qmi_text = qmi_text[:at] + 'static void lx12_after_bdf(struct ath12k_base *ab);\n' + qmi_text[at:]
    qmi_text += (Path(__file__).parent / 'src/lx12_qmi_tail.c').read_text()
    qmi.write_text(qmi_text)
edit(ath / 'qmi.h', '#define QMI_WLFW_FW_READY_IND_V01\t\t0x0038',
     '#define QMI_WLFW_FW_READY_IND_V01\t\t0x0021 /* LX12_WCN7750_READY */',
     'LX12_WCN7750_READY')
edit(qmi, '#include \"debug.h\"\n', '#include \"debug.h\"\n#include <lx_os.h>\n', '<lx_os.h>')
edit(qmi, '\tath12k_dbg(ab, ATH12K_DBG_QMI, "qmi firmware ready\\n");\n',
     '\t/* LX12_CAL_POWER: preserve the WPSS/QMI link, reset the radio power. */\n'
     '\tif (!ab->qmi.cal_done && lx_os_radio_calibrated() != 0) {\n'
     '\t\tath12k_warn(ab, "cold calibration power transition failed\\n");\n\t\treturn;\n\t}\n'
     '\tab->qmi.cal_done = 1;\n'
     '\tath12k_dbg(ab, ATH12K_DBG_QMI, "qmi firmware ready\\n");\n',
     'LX12_CAL_POWER')
edit(ath / 'wifi7/dp_rx.c',
     '\t\tvaddr = kzalloc(hw_desc_sz + HAL_LINK_DESC_ALIGN - 1, GFP_ATOMIC);',
     '\t\t/* LX12_REO_DMA: allocate inside the radio SMMU window. */\n'
     '\t\tvaddr = dma_alloc_noncoherent(ab->dev, hw_desc_sz + HAL_LINK_DESC_ALIGN - 1,\n'
     '\t\t\t\t\t       &paddr_aligned, DMA_BIDIRECTIONAL, GFP_ATOMIC);',
     'LX12_REO_DMA')
edit(ath / 'wifi7/dp_tx.c', '\tswitch (ts->status) {',
     '\t/* LX12_TX_COMPLETION: no payload or credentials in diagnostics. */\n'
     '\t{\n\t\tstatic unsigned said;\n'
     '\t\tif (said++ < 64)\n'
     '\t\t\tath12k_warn(ab, "data completion: status %u peer %u protocol %#x bytes %u\\n",\n'
     '\t\t\t\t    ts->status, ts->peer_id, be16_to_cpu(msdu->protocol), msdu->len);\n'
     '\t}\n\tswitch (ts->status) {', 'LX12_TX_COMPLETION')
edit(ath / 'wifi7/dp_tx.c', '\tdp->device_stats.fw_tx_status[wbm_status]++;',
     '\t/* LX12_FW_TX_COMPLETION: bounded status-only diagnostics. */\n'
     '\t{ static unsigned said; if (said++ < 48)\n'
     '\t\tath12k_warn(dp->ab, "firmware completion: status %u protocol %#x bytes %u\\n",\n'
     '\t\t    wbm_status, be16_to_cpu(desc_params->skb->protocol), desc_params->skb->len); }\n'
     '\tdp->device_stats.fw_tx_status[wbm_status]++;', 'LX12_FW_TX_COMPLETION')
for counter, label in [('rxdma_error', 'RXDMA'), ('reo_error', 'REO')]:
    old='\tdp->device_stats.'+counter+'[rxcb->err_code]++;'
    edit(ath / 'wifi7/dp_rx.c', old,
         '\t/* LX12_RX_'+label+'_STATUS */\n'
         '\t{ static unsigned said; if (said++ < 48)\n'
         '\t\tath12k_warn(dp->ab, "'+label+' drop: code %u peer %u bytes %u bitmap %#x\\n",\n'
         '\t\t    rxcb->err_code, rxcb->peer_id, msdu->len, rx_info->err_bitmap); }\n'+old,
         'LX12_RX_'+label+'_STATUS')
edit(ath / 'dp.c', '#include "core.h"', '#include "core.h"\n#include <lx_os.h>', '<lx_os.h>')
edit(ath / 'dp.c', 'DP_RX_HASH_ENABLE | (reo_dest << 1)',
     '(lx_os_option("wifi_rx_single") ? 0 : DP_RX_HASH_ENABLE) | (reo_dest << 1) /* LX12_RX_SINGLE */',
     'LX12_RX_SINGLE')
edit(ath / 'wifi7/dp_rx.c', '\t\t\tdp->device_stats.hal_reo_error[ring_id]++;',
     '\t\t\t/* LX12_REO_DEST_DROP */\n'
     '\t\t\t{ static unsigned said; if (said++ < 48)\n'
     '\t\t\t\tath12k_warn(ab, "REO destination drop: ring %d reason %u code %u\\n",\n'
     '\t\t\t\t    ring_id, push_reason, le32_get_bits(desc->info0, HAL_REO_DEST_RING_INFO0_ERROR_CODE)); }\n'
     '\t\t\tdp->device_stats.hal_reo_error[ring_id]++;', 'LX12_REO_DEST_DROP')
edit(ath / 'wifi7/dp_rx.c', '"Unable to process msdu %d", ret);',
     '"Unable to process msdu %d", ret);\n'
     '\t\t\t/* LX12_MSDU_DROP */\n'
     '\t\t\t{ static unsigned said; if (said++ < 48)\n'
     '\t\t\t\tath12k_warn(dp->ab, "RX msdu drop: error %d ring %d peer %u tid %u\\n",\n'
     '\t\t\t\t    ret, ring_id, rxcb->peer_id, rxcb->tid); }', 'LX12_MSDU_DROP')
edit(ath / 'wifi7/hal_rx.c', '\tath12k_dbg(ab, ATH12K_DBG_HAL, "Queue stats status:\\n");',
     '\t/* LX12_QUEUE_STATS: read hardware counters, not cached host memory. */\n'
     '\tath12k_warn(ab, "REO query result: command %u status %u mpdu %u msdu %u bytes %u\\n",\n'
     '\t\tstatus->uniform_hdr.cmd_num, status->uniform_hdr.cmd_status,\n'
     '\t\tle32_to_cpu(desc->num_mpdu_frames), le32_to_cpu(desc->num_msdu_frames),\n'
     '\t\tle32_to_cpu(desc->total_bytes));\n'
     '\tath12k_dbg(ab, ATH12K_DBG_HAL, "Queue stats status:\\n");', 'LX12_QUEUE_STATS')
edit(ath / 'wifi7/hal_rx.c',
     '\tdesc = hal->ops->reo_cmd_enc_tlv_hdr(tlv, HAL_REO_GET_QUEUE_STATS,\n'
     '\t\t\t\t\t     sizeof(*desc));\n\tmemset_startat(desc, 0, queue_addr_lo);',
     '\t/* LX12_QUEUE_STATS_SIZE */\n'
     '\tsize_t desc_size = hal->ops->reo_cmd_enc_tlv_hdr == ath12k_hal_encode_tlv32_hdr ?\n'
     '\t\tsizeof(struct hal_reo_get_queue_stats_qcc2072) : sizeof(*desc);\n'
     '\tdesc = hal->ops->reo_cmd_enc_tlv_hdr(tlv, HAL_REO_GET_QUEUE_STATS, desc_size);\n'
     '\tmemset((u8 *)desc + offsetof(struct hal_reo_get_queue_stats, queue_addr_lo), 0,\n'
     '\t       desc_size - offsetof(struct hal_reo_get_queue_stats, queue_addr_lo));', 'LX12_QUEUE_STATS_SIZE')
print('fixup.py: LinuxKPI and ath12k fitted')
