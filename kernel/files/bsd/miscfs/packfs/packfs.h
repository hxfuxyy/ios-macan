#ifndef _MISCFS_PACKFS_H_
#define _MISCFS_PACKFS_H_
#include <stdint.h>
#define PACKFS_MAGIC "PACKFS1"
#define PACKFS_SUPER_SIZE 4096
#define PACKFS_ROOT_NODE 0
#define PACKFS_MAX_NAME 255
struct packfs_super {
 char ps_magic[8];
 uint32_t ps_block_size;
 uint32_t ps_node_count;
 uint64_t ps_nodes_off;
 uint64_t ps_strings_off;
 uint64_t ps_strings_size;
 uint64_t ps_data_off;
 uint64_t ps_image_size;
 uint64_t ps_create_time;
};
struct packfs_dnode {
 uint32_t pd_mode;
 uint32_t pd_uid;
 uint32_t pd_gid;
 uint32_t pd_nlink;
 uint64_t pd_size;
 uint64_t pd_data;
 uint32_t pd_parent;
 uint32_t pd_name;
 uint32_t pd_name_len;
 uint32_t pd_rdev;
 uint64_t pd_mtime;
 uint64_t pd_flags;
};
#define PACKFS_F_CHUNKED 0x1
#define PACKFS_F_ZSTD 0x2
#define PACKFS_CHUNK_SIZE 65536
#ifdef KERNEL
#include <kern/locks.h>
#include <sys/mount.h>
#include <sys/vnode.h>
struct packfs_inode {
 vnode_t pi_vp;
 uint32_t pi_vid;
 uint32_t pi_flags;
};
#define PACKFS_I_CREATING 0x1
#define PACKFS_I_WANTED 0x2
struct packfs_mount {
 mount_t pm_mp;
 vnode_t pm_devvp;
 struct packfs_super pm_super;
 struct packfs_dnode *pm_nodes;
 char *pm_strings;
 struct packfs_inode *pm_inodes;
 const uint8_t *pm_membase;
 uint64_t pm_memsize;
 lck_mtx_t pm_lock;
};
#define PACKFS_MP(mp) ((struct packfs_mount *)vfs_fsprivate(mp))
#define PACKFS_VP_MOUNT(vp) PACKFS_MP(vnode_mount(vp))
#define PACKFS_INO(index) ((uint64_t)(index) + 2)
static inline uint32_t
packfs_vp_index(struct packfs_mount *pm, vnode_t vp)
{
 return (uint32_t)((struct packfs_inode *)vnode_fsnode(vp) - pm->pm_inodes);
}
int packfs_mountroot(mount_t mp, vnode_t devvp, vfs_context_t ctx);
int packfs_node_vnode(struct packfs_mount *pm, uint32_t index, vnode_t dvp, struct componentname *cnp, vnode_t *vpp);
extern int(**packfs_vnodeop_p)(void *);
#endif
#endif
