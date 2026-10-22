#ifndef _LX12_QCOM_RPROC_H
#define _LX12_QCOM_RPROC_H
struct notifier_block;
static inline void *qcom_register_ssr_notifier(const char *name, struct notifier_block *nb) { return (void *)1; }
static inline int qcom_unregister_ssr_notifier(void *notify, struct notifier_block *nb) { return 0; }
#endif
