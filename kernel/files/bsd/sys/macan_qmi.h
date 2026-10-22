#ifndef _SYS_REDWOOD_QMI_H_
#define _SYS_REDWOOD_QMI_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define QMI_REQUEST 0
#define QMI_RESPONSE 2
#define QMI_INDICATION 4
#define QMI_HEADER_SIZE 7
enum qmi_elem_type {
 QMI_EOTI,
 QMI_OPT_FLAG,
 QMI_DATA_LEN,
 QMI_UNSIGNED_1_BYTE,
 QMI_UNSIGNED_2_BYTE,
 QMI_UNSIGNED_4_BYTE,
 QMI_UNSIGNED_8_BYTE,
 QMI_SIGNED_2_BYTE_ENUM,
 QMI_SIGNED_4_BYTE_ENUM,
 QMI_STRUCT,
 QMI_STRING,
};
enum qmi_array_type {
 NO_ARRAY,
 STATIC_ARRAY,
 VAR_LEN_ARRAY,
};
struct qmi_elem_info {
 enum qmi_elem_type data_type;
 uint32_t elem_len;
 uint32_t elem_size;
 enum qmi_array_type array_type;
 uint8_t tlv_type;
 uint32_t offset;
 const struct qmi_elem_info *ei_array;
};
#define QMI_COMMON_TLV_TYPE 0
struct qmi_response_type_v01 {
 uint16_t result;
 uint16_t error;
};
extern const struct qmi_elem_info qmi_response_type_v01_ei[];
#define QMI_RESULT_SUCCESS_V01 0
#define QMI_RESULT_FAILURE_V01 1
#define QMI_ERR_NONE_V01 0
#define QMI_ERR_MALFORMED_MSG_V01 1
#define QMI_ERR_NO_MEMORY_V01 2
#define QMI_ERR_INTERNAL_V01 3
#define QMI_ERR_CLIENT_IDS_EXHAUSTED_V01 5
#define QMI_ERR_INVALID_ID_V01 41
#define QMI_ERR_ENCODING_V01 58
#define QMI_ERR_DISABLED_V01 69
#define QMI_ERR_INCOMPATIBLE_STATE_V01 90
#define QMI_ERR_NOT_SUPPORTED_V01 94
int redwood_qmi_encode(uint8_t type, uint16_t message, uint16_t transaction, const struct qmi_elem_info *ei, const void *structure,
    void *buffer, size_t size);
int redwood_qmi_decode(const void *buffer, size_t length, const struct qmi_elem_info *ei, void *structure);
#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
struct sockaddr_qrtr {
 uint16_t sq_family;
 uint32_t sq_node;
 uint32_t sq_port;
};
struct qmi_handle;
struct qmi_txn;
struct socket;
struct qmi_service {
 unsigned int service, version, instance;
 unsigned int node, port;
 void *priv;
 struct qmi_service *next;
};
struct qmi_ops {
 int (*new_server)(struct qmi_handle *qmi, struct qmi_service *service);
 void (*del_server)(struct qmi_handle *qmi, struct qmi_service *service);
 void (*net_reset)(struct qmi_handle *qmi);
 void (*msg_handler)(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, const void *data, size_t length);
 void (*bye)(struct qmi_handle *qmi, unsigned int node);
 void (*del_client)(struct qmi_handle *qmi, unsigned int node, unsigned int port);
};
struct qmi_msg_handler {
 unsigned int type;
 unsigned int msg_id;
 const struct qmi_elem_info *ei;
 size_t decoded_size;
 void (*fn)(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded);
};
struct qmi_handle {
 struct socket *sock;
};
struct qmi_txn {
 struct qmi_handle *qmi;
 uint16_t id;
 int result;
 const struct qmi_elem_info *ei;
 void *dest;
 struct qmi_txn *next;
 bool done, waiting;
};
int qmi_handle_init(struct qmi_handle *qmi, size_t largest, const struct qmi_ops *ops, const struct qmi_msg_handler *handlers);
void qmi_handle_release(struct qmi_handle *qmi);
int qmi_add_lookup(struct qmi_handle *qmi, unsigned int service, unsigned int version, unsigned int instance);
int qmi_txn_init(struct qmi_handle *qmi, struct qmi_txn *txn, const struct qmi_elem_info *ei, void *structure);
int qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure);
void qmi_txn_cancel(struct qmi_txn *txn);
int redwood_qmi_txn_wait_ms(struct qmi_txn *txn, uint64_t milliseconds);
#ifndef REDWOOD_QMI_JIFFIES_TO_MS
#define REDWOOD_QMI_JIFFIES_TO_MS(j) ((uint64_t)(j) * 1000 / HZ)
#endif
#define qmi_txn_wait(txn,timeout) redwood_qmi_txn_wait_ms((txn), REDWOOD_QMI_JIFFIES_TO_MS(timeout))
int redwood_qmi_connect(struct socket *sock, uint32_t node, uint32_t port);
#ifndef REDWOOD_QMI_NO_KERNEL_CONNECT
static inline int
kernel_connect(struct socket *sock, void *address, __attribute__((unused)) int length, __attribute__((unused)) int flags)
{
 const struct sockaddr_qrtr *sq = address;
 return redwood_qmi_connect(sock, sq->sq_node, sq->sq_port);
}
#endif
#endif
