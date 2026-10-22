struct lx12_qdss_piece {
 u8 total_size_valid;
 u32 total_size;
 u8 seg_id_valid;
 u32 seg_id;
 u8 data_valid;
 u32 data_len;
 u8 data[6144];
 u8 end_valid, end;
};
#define LX12_QDSS(type,len,size,array,tlv,field) \
 { .data_type = type, .elem_len = len, .elem_size = size, .array_type = array, .tlv_type = tlv, \
   .offset = offsetof(struct lx12_qdss_piece, field) }
static const struct qmi_elem_info lx12_qdss_ei[] = {
 LX12_QDSS(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x10, total_size_valid),
 LX12_QDSS(QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0x10, total_size),
 LX12_QDSS(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x11, seg_id_valid),
 LX12_QDSS(QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0x11, seg_id),
 LX12_QDSS(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x12, data_valid),
 LX12_QDSS(QMI_DATA_LEN, 1, 2, NO_ARRAY, 0x12, data_len),
 LX12_QDSS(QMI_UNSIGNED_1_BYTE, 6144, 1, VAR_LEN_ARRAY, 0x12, data),
 LX12_QDSS(QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x13, end_valid),
 LX12_QDSS(QMI_UNSIGNED_1_BYTE, 1, 1, NO_ARRAY, 0x13, end),
 { .data_type = QMI_EOTI, .tlv_type = QMI_COMMON_TLV_TYPE },
};
static void lx12_after_bdf(struct ath12k_base *ab)
{
 static struct lx12_qdss_piece req;
 struct qmi_wlanfw_m3_info_resp_msg_v01 resp;
 const struct firmware *fw;
 struct qmi_txn txn;
 const u8 *data;
 size_t left = 1;
 int ret;
 if (firmware_request_nowarn(&fw, "qdss_trace_config_perf_v1.cfg", ab->dev) == 0) {
  memset(&req, 0, sizeof(req));
  data = fw->data;
  for (left = fw->size; left > 0; ) {
   req.total_size_valid = 1;
   req.total_size = fw->size;
   req.seg_id_valid = 1;
   req.data_valid = 1;
   req.data_len = left > 6144 ? 6144 : left;
   req.end_valid = 1;
   req.end = left <= 6144;
   memcpy(req.data, data, req.data_len);
   memset(&resp, 0, sizeof(resp));
   ret = qmi_txn_init(&ab->qmi.handle, &txn, qmi_wlanfw_m3_info_resp_msg_v01_ei, &resp);
   if (ret < 0)
    break;
   ret = qmi_send_request(&ab->qmi.handle, NULL, &txn, 0x44, 6167, lx12_qdss_ei, &req);
   if (ret < 0) {
    qmi_txn_cancel(&txn);
    break;
   }
   ret = qmi_txn_wait(&txn, msecs_to_jiffies(ATH12K_QMI_WLANFW_TIMEOUT_MS));
   if (ret < 0 || resp.resp.result != QMI_RESULT_SUCCESS_V01)
    break;
   left -= req.data_len;
   data += req.data_len;
   req.seg_id++;
  }
  ath12k_info(ab, "lx12: QDSS configuration %s (%zu bytes)\n", left == 0 ? "accepted" : "NOT accepted",
      (size_t)fw->size);
  release_firmware(fw);
 } else {
  ath12k_warn(ab, "lx12: no QDSS configuration file\n");
 }
 { extern void lx_pause_ms(unsigned); lx_pause_ms(3000); }
 ret = ath12k_qmi_wlanfw_mode_send(ab, (enum ath12k_firmware_mode)7);
 ath12k_info(ab, "lx12: mode 7 (cold-boot calibration) asked: %d; \"ready\" follows the calibration\n", ret);
}
