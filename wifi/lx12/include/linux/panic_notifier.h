#ifndef _LX12_PANIC_NOTIFIER_H
#define _LX12_PANIC_NOTIFIER_H
#include <linux/notifier.h>
#define panic_notifier_list (*(struct atomic_notifier_head *)0)
static inline int lx12_no_panic_notifier(void) { return 0; }
#define atomic_notifier_chain_register(h,nb) lx12_no_panic_notifier()
#define atomic_notifier_chain_unregister(h,nb) lx12_no_panic_notifier()
#endif
