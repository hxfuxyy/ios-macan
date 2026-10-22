#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/codesign.h>
#include <sys/errno.h>
#include <sys/kpi_socket.h>
#include <sys/priv.h>
#include <sys/proc_internal.h>
#include <sys/ubc_internal.h>
#include <sys/vnode_internal.h>
#include <libkern/libkern.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <security/mac_framework.h>
#include <security/mac_policy.h>
void minimac_policy_init(void);
static int
miniamfi_vnode_check_signature(__unused struct vnode *vp, __unused struct label *label, __unused cpu_type_t cpu_type,
    struct cs_blob *cs_blob, unsigned int *cs_flags, __unused unsigned int *signer_type, __unused int flags,
    __unused unsigned int platform, __unused char **fatal_failure_desc, __unused size_t *fatal_failure_desc_len)
{
 if (cs_blob == NULL) {
  return 0;
 }
 *cs_flags |= CS_SIGNED;
 if (*cs_flags & CS_ADHOC) {
  *cs_flags |= CS_PLATFORM_BINARY;
 }
 return 0;
}
#define AMFI_SYSCALL_CHECK_DYLD_POLICY 0x5a
#define AMFI_DYLD_OUTPUT_ALLOW_ALL 0xffULL
#define AMFI_SYSCALL_QUERY_BOOTARG_STATE 0x60
#define AMFI_BOOTARG_STATE_NOT_ENFORCING 0x86ULL
static int
miniamfi_policy_syscall(__unused struct proc *p, int call, user_addr_t arg)
{
 struct {
  uint64_t input_flags;
  user_addr_t output_flags;
 } args;
 uint64_t flags = AMFI_DYLD_OUTPUT_ALLOW_ALL;
 uint64_t state = AMFI_BOOTARG_STATE_NOT_ENFORCING;
 int error;
 if (call == AMFI_SYSCALL_QUERY_BOOTARG_STATE) {
  return copyout(&state, arg, sizeof(state));
 }
 if (call != AMFI_SYSCALL_CHECK_DYLD_POLICY) {
  return ENOPOLICY;
 }
 error = copyin(arg, &args, sizeof(args));
 if (error == 0) {
  error = copyout(&flags, args.output_flags, sizeof(flags));
 }
 return error;
}
static const struct mac_policy_ops miniamfi_ops = {
 .mpo_vnode_check_signature = miniamfi_vnode_check_signature,
 .mpo_policy_syscall = miniamfi_policy_syscall,
};
SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) miniamfi_conf = {
 .mpc_name = "AMFI",
 .mpc_fullname = "Apple Mobile File Integrity (stand-in)",
 .mpc_ops = &miniamfi_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) miniamfi_handle;
#define SANDBOX_SYSCALL_CONTAINER_PATH 4
#define SANDBOX_CONTAINER_VARIABLE "CFFIXED_USER_HOME="
static int
minisandbox_container_path(user_addr_t arg)
{
 struct {
  int64_t pid;
  uint64_t pidversion;
  user_addr_t buffer;
  user_size_t size;
  uint64_t check_pidversion;
 } args;
 const size_t prefix_len = sizeof(SANDBOX_CONTAINER_VARIABLE) - 1;
 proc_t p;
 task_t task;
 vm_map_t map;
 vm_map_copy_t copy = NULL;
 vm_offset_t strings = 0;
 vm_size_t strings_size;
 user_addr_t user_stack;
 size_t argslen, len;
 const char *s, *end;
 int error;
 error = copyin(arg, &args, sizeof(args));
 if (error) {
  return error;
 }
 p = proc_find((int)args.pid);
 if (p == PROC_NULL) {
  return ESRCH;
 }
 if (args.check_pidversion && (uint64_t)proc_pidversion(p) != args.pidversion) {
  proc_rele(p);
  return ESRCH;
 }
 user_stack = p->user_stack;
 argslen = p->p_argslen;
 task = proc_task(p);
 if (user_stack == 0 || argslen == 0 || task == TASK_NULL) {
  proc_rele(p);
  return 0;
 }
 task_reference(task);
 proc_rele(p);
 map = get_task_map_reference(task);
 task_deallocate(task);
 if (map == VM_MAP_NULL) {
  return 0;
 }
 strings_size = vm_map_round_page(argslen, vm_map_page_mask(map));
 if (kmem_alloc(kernel_map, &strings, strings_size, KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
  vm_map_deallocate(map);
  return ENOMEM;
 }
 if (vm_map_copyin(map, (vm_map_address_t)(user_stack - strings_size), strings_size, FALSE, &copy) != KERN_SUCCESS) {
  copy = NULL;
 }
 vm_map_deallocate(map);
 if (copy == NULL || vm_map_copy_overwrite(kernel_map, strings, copy, strings_size, FALSE) != KERN_SUCCESS) {
  if (copy != NULL) {
   vm_map_copy_discard(copy);
  }
  kmem_free(kernel_map, strings, strings_size);
  return 0;
 }
 end = (const char *)(strings + strings_size);
 for (s = end - argslen; s < end; s += len + 1) {
  len = strnlen(s, end - s);
  if (len > prefix_len && s + len < end && strncmp(s, SANDBOX_CONTAINER_VARIABLE, prefix_len) == 0) {
   s += prefix_len;
   len -= prefix_len;
   error = len < args.size ? copyout(s, args.buffer, len + 1) : ENAMETOOLONG;
   break;
  }
 }
 kmem_free(kernel_map, strings, strings_size);
 return error;
}
static uint64_t minisandbox_seen;
static int
minisandbox_policy_syscall(struct proc *p, int call, user_addr_t arg)
{
 if (call >= 0 && call < 64 && (minisandbox_seen & (1ULL << call)) == 0) {
  minisandbox_seen |= 1ULL << call;
  printf("minisandbox: request %d (first seen from %s[%d]): %s\n", call, p->p_comm, proc_getpid(p),
      call == SANDBOX_SYSCALL_CONTAINER_PATH ? "answered" : "allowed");
 }
 if (call == SANDBOX_SYSCALL_CONTAINER_PATH) {
  return minisandbox_container_path(arg);
 }
 return 0;
}
extern kern_return_t miniamfi_entitlement_boolean(struct proc *proc, const char *name);
static const struct {
 int priv;
 const char *entitlement;
} minisandbox_privileges[] = {
 { PRIV_PROC_UUID_POLICY, "com.apple.private.kernel.uuid-policy" },
 { PRIV_GLOBAL_PROC_INFO, "com.apple.private.kernel.global-proc-info" },
 { PRIV_SYSTEM_OVERRIDE, "com.apple.private.kernel.system-override" },
 { PRIV_HW_DEBUG_DATA, "com.apple.private.kernel.hw-debug-data" },
 { PRIV_SELECTIVE_FORCED_IDLE, "com.apple.private.kernel.selective-forced-idle" },
 { PRIV_DARKBOOT, "com.apple.private.kernel.darkboot" },
 { PRIV_AUDIO_LATENCY, "com.apple.private.kernel.audio_latency" },
 { PRIV_KTRACE_BACKGROUND, "com.apple.private.kernel.ktrace-background" },
 { PRIV_SETPRIORITY_DARWIN_ROLE, "com.apple.private.kernel.setpriority-darwin-role" },
 { PRIV_PACKAGE_EXTENSIONS, "com.apple.private.kernel.set-package-extensions" },
 { PRIV_TRIM_ACTIVE_FILE, "com.apple.private.apfs.trim-active-file" },
 { PRIV_PROC_CPUMON_OVERRIDE, "com.apple.private.kernel.override-cpumon" },
 { PRIV_VM_PRESSURE, "vm-pressure-level" },
 { PRIV_VM_JETSAM, "com.apple.private.kernel.jetsam" },
 { PRIV_NET_PRIVILEGED_TRAFFIC_CLASS, "com.apple.private.network.privileged-traffic-class" },
 { PRIV_NET_PRIVILEGED_SOCKET_DELEGATE, "com.apple.private.network.socket-delegate" },
 { PRIV_NET_INTERFACE_CONTROL, "com.apple.private.network.interface-control" },
 { PRIV_NET_PRIVILEGED_NETWORK_STATISTICS, "com.apple.private.network.statistics" },
 { PRIV_NET_PRIVILEGED_NECP_POLICIES, "com.apple.private.necp.policies" },
 { PRIV_NET_RESTRICTED_AWDL, "com.apple.private.network.awdl.restricted" },
 { PRIV_NET_QOSMARKING_POLICY_OVERRIDE, "com.apple.private.network.qos-marking-policy-override" },
 { PRIV_NET_RESTRICTED_MULTIPATH_EXTENDED, "com.apple.developer.networking.multipath_extended" },
 { PRIV_NET_RESTRICTED_ROUTE_NC_READ, "com.apple.developer.networking.route_nc_read" },
 { PRIV_NET_VALIDATED_RESOLVER, "com.apple.private.validated-resolver" },
 { PRIV_NET_CUSTOM_PROTOCOL, "com.apple.developer.networking.custom-protocol" },
 { PRIV_NET_PRIVILEGED_NECP_DROP_ALL_BYPASS, "com.apple.private.necp.drop_all_bypass" },
 { PRIV_NET_PRIVILEGED_IPSEC_WAKE_PACKET, "com.apple.private.network.ipsec-wake-packet" },
 { PRIV_NETINET_RESERVEDPORT, "com.apple.private.network.reserved-port" },
 { PRIV_NETINET_TCP_KA_OFFLOAD, "com.apple.developer.networking.tcp_ka_offload" },
 { PRIV_SKYWALK_REGISTER_USER_PIPE, "com.apple.private.skywalk.register-user-pipe" },
 { PRIV_SKYWALK_REGISTER_KERNEL_PIPE, "com.apple.private.skywalk.register-kernel-pipe" },
 { PRIV_SKYWALK_REGISTER_NET_IF, "com.apple.private.skywalk.register-net-if" },
 { PRIV_SKYWALK_REGISTER_FLOW_SWITCH, "com.apple.private.skywalk.register-flow-switch" },
 { PRIV_SKYWALK_OBSERVE_ALL, "com.apple.private.skywalk.observe-all" },
 { PRIV_SKYWALK_OBSERVE_STATS, "com.apple.private.skywalk.observe-stats" },
 { PRIV_VFS_OPEN_BY_ID, "com.apple.private.vfs.open-by-id" },
 { PRIV_VFS_MOVE_DATA_EXTENTS, "com.apple.private.vfs.move-data-extents" },
 { PRIV_VFS_SNAPSHOT, "com.apple.private.vfs.snapshot" },
 { PRIV_VFS_DATALESS_RESOLVER, "com.apple.private.vfs.dataless-resolver" },
 { PRIV_VFS_SETSIZE, "com.apple.private.vfs.setsize" },
};
static TUNABLE(uint32_t, minisandbox_by_entitlement, "privs", 1);
static int
minisandbox_priv_grant(kauth_cred_t cred, int priv)
{
 static int seen[32];
 static unsigned seen_count;
 proc_t proc = current_proc();
 if (priv == PRIV_WORK_INTERVAL) {
  return 0;
 }
 for (unsigned i = 0; minisandbox_by_entitlement && cred == kauth_cred_get() &&
     i < sizeof(minisandbox_privileges) / sizeof(minisandbox_privileges[0]); i++) {
  if (minisandbox_privileges[i].priv == priv) {
   if (miniamfi_entitlement_boolean(proc, minisandbox_privileges[i].entitlement) == KERN_SUCCESS) {
    return 0;
   }
   break;
  }
 }
 for (unsigned i = 0; i < seen_count; i++) {
  if (seen[i] == priv) {
   return EPERM;
  }
 }
 if (seen_count < sizeof(seen) / sizeof(seen[0])) {
  seen[seen_count++] = priv;
  printf("minisandbox: privilege %d refused to %s[%d] (first time)\n", priv, proc->p_comm,
      proc_getpid(proc));
 }
 return EPERM;
}
static const struct mac_policy_ops minisandbox_ops = {
 .mpo_policy_syscall = minisandbox_policy_syscall,
 .mpo_priv_grant = minisandbox_priv_grant,
};
SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) minisandbox_conf = {
 .mpc_name = "Sandbox",
 .mpc_fullname = "Seatbelt sandbox policy (stand-in)",
 .mpc_ops = &minisandbox_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) minisandbox_handle;
void
minimac_policy_init(void)
{
 mac_policy_register(&miniamfi_conf, &miniamfi_handle, NULL);
 mac_policy_register(&minisandbox_conf, &minisandbox_handle, NULL);
}
