#include <kern/assert.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <miscfs/packfs/packfs.h>
#include <sys/attr.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <uuid/uuid.h>
#include <sys/vnode_internal.h>
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);
static LCK_GRP_DECLARE(packfs_lck_grp, "packfs");
#define PACKFS_META_CHUNK 4096
#define PACKFS_MAX_NODES (4u * 1024 * 1024)
#define PACKFS_MAX_STRINGS (256ull * 1024 * 1024)
static int
packfs_read_meta(vnode_t devvp, uint32_t devblock, uint64_t offset, void *dst, uint64_t length)
{
 char *out = dst;
 while (length > 0) {
  buf_t bp = NULL;
  uint64_t chunk = MIN(length, PACKFS_META_CHUNK);
  int error = (int)buf_meta_bread(devvp, (daddr64_t)(offset / devblock), PACKFS_META_CHUNK, NOCRED, &bp);
  if (error) {
   if (bp) {
    buf_brelse(bp);
   }
   return error;
  }
  memcpy(out, (const void *)buf_dataptr(bp), chunk);
  buf_markinvalid(bp);
  buf_brelse(bp);
  out += chunk;
  offset += chunk;
  length -= chunk;
 }
 return 0;
}
static bool
packfs_range_ok(uint64_t start, uint64_t length, uint64_t limit)
{
 return start <= limit && length <= limit - start;
}
static int
packfs_validate(const struct packfs_mount *pm)
{
 const struct packfs_super *ps = &pm->pm_super;
 for (uint32_t i = 0; i < ps->ps_node_count; i++) {
  const struct packfs_dnode *dn = &pm->pm_nodes[i];
  if (dn->pd_name_len > PACKFS_MAX_NAME ||
      !packfs_range_ok(dn->pd_name, (uint64_t)dn->pd_name_len + 1, ps->ps_strings_size) ||
      pm->pm_strings[dn->pd_name + dn->pd_name_len] != '\0' ||
      dn->pd_parent >= ps->ps_node_count || !S_ISDIR(pm->pm_nodes[dn->pd_parent].pd_mode)) {
   return EINVAL;
  }
  switch (dn->pd_mode & S_IFMT) {
  case S_IFDIR:
   if (!packfs_range_ok(dn->pd_data, dn->pd_size, ps->ps_node_count)) {
    return EINVAL;
   }
   for (uint64_t c = 0; c < dn->pd_size; c++) {
    if (pm->pm_nodes[dn->pd_data + c].pd_parent != i || dn->pd_data + c == PACKFS_ROOT_NODE) {
     return EINVAL;
    }
   }
   break;
  case S_IFREG:
   if (dn->pd_size == 0) {
    break;
   }
   if (dn->pd_flags & PACKFS_F_CHUNKED) {
    uint64_t chunks = (dn->pd_size + PACKFS_CHUNK_SIZE - 1) / PACKFS_CHUNK_SIZE;
    uint32_t previous = (uint32_t)((chunks + 1) * sizeof(uint32_t)), offset;
    if (pm->pm_membase == NULL || (dn->pd_data & 7) != 0 || dn->pd_data < ps->ps_data_off ||
        !packfs_range_ok(dn->pd_data, (chunks + 1) * sizeof(uint32_t), ps->ps_image_size)) {
     return pm->pm_membase == NULL ? ENOTSUP : EINVAL;
    }
    for (uint64_t c = 0; c <= chunks; c++) {
     memcpy(&offset, pm->pm_membase + dn->pd_data + c * sizeof(uint32_t), sizeof(offset));
     if (offset < previous || (c == 0 && offset != previous) ||
         (c != 0 && offset - previous > PACKFS_CHUNK_SIZE) ||
         !packfs_range_ok(dn->pd_data, offset, ps->ps_image_size)) {
      return EINVAL;
     }
     previous = offset;
    }
   } else if ((dn->pd_data & (ps->ps_block_size - 1)) != 0 || dn->pd_data < ps->ps_data_off ||
       !packfs_range_ok(dn->pd_data, dn->pd_size, ps->ps_image_size)) {
    return EINVAL;
   }
   break;
  case S_IFLNK:
   if (dn->pd_size == 0 || dn->pd_size >= MAXPATHLEN ||
       !packfs_range_ok(dn->pd_data, dn->pd_size, ps->ps_strings_size)) {
    return EINVAL;
   }
   break;
  default:
   return EINVAL;
  }
 }
 if (!S_ISDIR(pm->pm_nodes[PACKFS_ROOT_NODE].pd_mode) || pm->pm_nodes[PACKFS_ROOT_NODE].pd_parent != PACKFS_ROOT_NODE) {
  return EINVAL;
 }
 return 0;
}
static void
packfs_free_mount(struct packfs_mount *pm)
{
 const struct packfs_super *ps = &pm->pm_super;
 if (pm->pm_inodes) {
  kfree_type(struct packfs_inode, ps->ps_node_count, pm->pm_inodes);
 }
 if (pm->pm_strings) {
  kfree_data(pm->pm_strings, ps->ps_strings_size);
 }
 if (pm->pm_nodes) {
  kfree_data(pm->pm_nodes, (vm_size_t)ps->ps_node_count * sizeof(struct packfs_dnode));
 }
 lck_mtx_destroy(&pm->pm_lock, &packfs_lck_grp);
 kfree_type(struct packfs_mount, pm);
}
static int
packfs_mount_common(mount_t mp, vnode_t devvp, vfs_context_t ctx)
{
 struct packfs_mount *pm;
 struct packfs_super *ps;
 struct vfsstatfs *sp;
 uint32_t devblock = 0;
 char *block0;
 int error;
 if (VNOP_IOCTL(devvp, DKIOCGETBLOCKSIZE, (caddr_t)&devblock, 0, ctx) != 0 ||
     devblock == 0 || (PACKFS_META_CHUNK % devblock) != 0) {
  return EINVAL;
 }
 pm = kalloc_type(struct packfs_mount, Z_WAITOK | Z_ZERO | Z_NOFAIL);
 lck_mtx_init(&pm->pm_lock, &packfs_lck_grp, LCK_ATTR_NULL);
 pm->pm_mp = mp;
 pm->pm_devvp = devvp;
 ps = &pm->pm_super;
 block0 = kalloc_data(PACKFS_SUPER_SIZE, Z_WAITOK | Z_NOFAIL);
 error = packfs_read_meta(devvp, devblock, 0, block0, PACKFS_SUPER_SIZE);
 if (!error) {
  memcpy(ps, block0, sizeof(*ps));
 }
 kfree_data(block0, PACKFS_SUPER_SIZE);
 if (error) {
  goto fail;
 }
 error = EINVAL;
 if (memcmp(ps->ps_magic, PACKFS_MAGIC, sizeof(ps->ps_magic)) != 0) {
  goto fail;
 }
 if (ps->ps_block_size < PACKFS_META_CHUNK || (ps->ps_block_size & (ps->ps_block_size - 1)) != 0 ||
     ps->ps_node_count == 0 || ps->ps_node_count > PACKFS_MAX_NODES ||
     ps->ps_strings_size == 0 || ps->ps_strings_size > PACKFS_MAX_STRINGS ||
     (ps->ps_nodes_off % PACKFS_META_CHUNK) != 0 || (ps->ps_strings_off % PACKFS_META_CHUNK) != 0 ||
     !packfs_range_ok(ps->ps_nodes_off, (uint64_t)ps->ps_node_count * sizeof(struct packfs_dnode), ps->ps_image_size) ||
     !packfs_range_ok(ps->ps_strings_off, ps->ps_strings_size, ps->ps_image_size) ||
     ps->ps_data_off > ps->ps_image_size) {
  printf("packfs: bad superblock\n");
  goto fail;
 }
 pm->pm_nodes = kalloc_data((vm_size_t)ps->ps_node_count * sizeof(struct packfs_dnode), Z_WAITOK);
 pm->pm_strings = kalloc_data(ps->ps_strings_size, Z_WAITOK);
 pm->pm_inodes = kalloc_type(struct packfs_inode, ps->ps_node_count, Z_WAITOK | Z_ZERO);
 if (!pm->pm_nodes || !pm->pm_strings || !pm->pm_inodes) {
  error = ENOMEM;
  goto fail;
 }
 {
  vm_offset_t membase = 0;
  uint64_t memsize = 0;
  if (mdevgetkernelrange(vnode_specrdev(devvp), &membase, &memsize) == 0 && memsize >= ps->ps_image_size) {
   pm->pm_membase = (const uint8_t *)membase;
   pm->pm_memsize = memsize;
  }
 }
 error = packfs_read_meta(devvp, devblock, ps->ps_nodes_off, pm->pm_nodes,
     (uint64_t)ps->ps_node_count * sizeof(struct packfs_dnode));
 if (!error) {
  error = packfs_read_meta(devvp, devblock, ps->ps_strings_off, pm->pm_strings, ps->ps_strings_size);
 }
 if (!error) {
  error = packfs_validate(pm);
  if (error == ENOTSUP) {
   printf("packfs: an image with compressed files mounts only from a RAM disk\n");
  } else if (error) {
   printf("packfs: inconsistent image\n");
  }
 }
 if (error) {
  goto fail;
 }
 vfs_setfsprivate(mp, pm);
 vfs_setflags(mp, MNT_RDONLY | MNT_LOCAL);
 vfs_setlocklocal(mp);
 sp = vfs_statfs(mp);
 sp->f_bsize = ps->ps_block_size;
 sp->f_iosize = ps->ps_block_size;
 sp->f_blocks = ps->ps_image_size / ps->ps_block_size;
 sp->f_bfree = 0;
 sp->f_bavail = 0;
 sp->f_bused = sp->f_blocks;
 sp->f_files = ps->ps_node_count;
 sp->f_ffree = 0;
 sp->f_fsid.val[0] = (int32_t)vnode_specrdev(devvp);
 sp->f_fsid.val[1] = vfs_typenum(mp);
 printf("packfs: image with %u nodes, %llu KB%s\n", ps->ps_node_count, (unsigned long long)ps->ps_image_size / 1024,
     pm->pm_membase ? ", read in place" : "");
 return 0;
fail:
 if (error == EINVAL && pm->pm_nodes == NULL) {
  ps->ps_node_count = 0;
  ps->ps_strings_size = 0;
 }
 packfs_free_mount(pm);
 return error;
}
int
packfs_mountroot(mount_t mp, vnode_t devvp, vfs_context_t ctx)
{
 return packfs_mount_common(mp, devvp, ctx);
}
static int
packfs_mount(mount_t mp, vnode_t devvp, __unused user_addr_t data, vfs_context_t ctx)
{
 if (vfs_isupdate(mp)) {
  return vfs_iswriteupgrade(mp) ? EROFS : 0;
 }
 if (devvp == NULLVP) {
  return EINVAL;
 }
 return packfs_mount_common(mp, devvp, ctx);
}
static int
packfs_unmount(mount_t mp, int mntflags, __unused vfs_context_t ctx)
{
 struct packfs_mount *pm = PACKFS_MP(mp);
 int error;
 error = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE : 0);
 if (error) {
  return error;
 }
 vfs_setfsprivate(mp, NULL);
 packfs_free_mount(pm);
 return 0;
}
static int
packfs_root(mount_t mp, vnode_t *vpp, __unused vfs_context_t ctx)
{
 return packfs_node_vnode(PACKFS_MP(mp), PACKFS_ROOT_NODE, NULLVP, NULL, vpp);
}
static int
packfs_vget(mount_t mp, ino64_t ino, vnode_t *vpp, __unused vfs_context_t ctx)
{
 struct packfs_mount *pm = PACKFS_MP(mp);
 if (ino < PACKFS_INO(0) || ino - PACKFS_INO(0) >= pm->pm_super.ps_node_count) {
  return ENOENT;
 }
 return packfs_node_vnode(pm, (uint32_t)(ino - PACKFS_INO(0)), NULLVP, NULL, vpp);
}
static int
packfs_vfs_getattr(mount_t mp, struct vfs_attr *fsap, __unused vfs_context_t ctx)
{
 const struct packfs_super *ps = &PACKFS_MP(mp)->pm_super;
 uint64_t blocks = ps->ps_image_size / ps->ps_block_size;
 VFSATTR_RETURN(fsap, f_objcount, ps->ps_node_count);
 VFSATTR_RETURN(fsap, f_bsize, ps->ps_block_size);
 VFSATTR_RETURN(fsap, f_iosize, ps->ps_block_size);
 VFSATTR_RETURN(fsap, f_blocks, blocks);
 VFSATTR_RETURN(fsap, f_bfree, 0);
 VFSATTR_RETURN(fsap, f_bavail, 0);
 VFSATTR_RETURN(fsap, f_bused, blocks);
 VFSATTR_RETURN(fsap, f_files, ps->ps_node_count);
 VFSATTR_RETURN(fsap, f_ffree, 0);
 if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
  vol_capabilities_attr_t *cap = &fsap->f_capabilities;
  bzero(cap, sizeof(*cap));
  cap->capabilities[VOL_CAPABILITIES_FORMAT] =
      VOL_CAP_FMT_PERSISTENTOBJECTIDS |
      VOL_CAP_FMT_SYMBOLICLINKS |
      VOL_CAP_FMT_NO_ROOT_TIMES |
      VOL_CAP_FMT_CASE_SENSITIVE |
      VOL_CAP_FMT_CASE_PRESERVING |
      VOL_CAP_FMT_FAST_STATFS |
      VOL_CAP_FMT_2TB_FILESIZE |
      VOL_CAP_FMT_PATH_FROM_ID;
  cap->capabilities[VOL_CAPABILITIES_INTERFACES] =
      VOL_CAP_INT_ADVLOCK |
      VOL_CAP_INT_FLOCK;
  cap->valid[VOL_CAPABILITIES_FORMAT] =
      VOL_CAP_FMT_PERSISTENTOBJECTIDS |
      VOL_CAP_FMT_SYMBOLICLINKS |
      VOL_CAP_FMT_HARDLINKS |
      VOL_CAP_FMT_JOURNAL |
      VOL_CAP_FMT_JOURNAL_ACTIVE |
      VOL_CAP_FMT_NO_ROOT_TIMES |
      VOL_CAP_FMT_SPARSE_FILES |
      VOL_CAP_FMT_ZERO_RUNS |
      VOL_CAP_FMT_CASE_SENSITIVE |
      VOL_CAP_FMT_CASE_PRESERVING |
      VOL_CAP_FMT_FAST_STATFS |
      VOL_CAP_FMT_2TB_FILESIZE |
      VOL_CAP_FMT_OPENDENYMODES |
      VOL_CAP_FMT_HIDDEN_FILES |
      VOL_CAP_FMT_PATH_FROM_ID;
  cap->valid[VOL_CAPABILITIES_INTERFACES] =
      VOL_CAP_INT_SEARCHFS |
      VOL_CAP_INT_ATTRLIST |
      VOL_CAP_INT_NFSEXPORT |
      VOL_CAP_INT_READDIRATTR |
      VOL_CAP_INT_EXCHANGEDATA |
      VOL_CAP_INT_COPYFILE |
      VOL_CAP_INT_ALLOCATE |
      VOL_CAP_INT_VOL_RENAME |
      VOL_CAP_INT_ADVLOCK |
      VOL_CAP_INT_FLOCK |
      VOL_CAP_INT_EXTENDED_SECURITY |
      VOL_CAP_INT_USERACCESS |
      VOL_CAP_INT_MANLOCK |
      VOL_CAP_INT_EXTENDED_ATTR |
      VOL_CAP_INT_NAMEDSTREAMS;
  VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
 }
 VFSATTR_RETURN(fsap, f_filecount, ps->ps_node_count);
 VFSATTR_RETURN(fsap, f_dircount, 0);
 VFSATTR_RETURN(fsap, f_maxobjcount, ps->ps_node_count);
 VFSATTR_RETURN(fsap, f_owner, 0);
 VFSATTR_RETURN(fsap, f_fssubtype, 0);
 VFSATTR_RETURN(fsap, f_signature, 0x4244);
 VFSATTR_RETURN(fsap, f_carbon_fsid, 0);
 VFSATTR_RETURN(fsap, f_quota, 0);
 VFSATTR_RETURN(fsap, f_reserved, 0);
 if (VFSATTR_IS_ACTIVE(fsap, f_fsid)) {
  fsap->f_fsid = vfs_statfs(mp)->f_fsid;
  VFSATTR_SET_SUPPORTED(fsap, f_fsid);
 }
 if (VFSATTR_IS_ACTIVE(fsap, f_create_time) || VFSATTR_IS_ACTIVE(fsap, f_modify_time) ||
     VFSATTR_IS_ACTIVE(fsap, f_access_time) || VFSATTR_IS_ACTIVE(fsap, f_backup_time)) {
  struct timeval boot;
  struct timespec since = { .tv_sec = 0, .tv_nsec = 0 };
  boottime_timeval(&boot);
  since.tv_sec = boot.tv_sec;
  VFSATTR_RETURN(fsap, f_create_time, since);
  VFSATTR_RETURN(fsap, f_modify_time, since);
  VFSATTR_RETURN(fsap, f_access_time, since);
  since.tv_sec = 0;
  VFSATTR_RETURN(fsap, f_backup_time, since);
 }
 if (VFSATTR_IS_ACTIVE(fsap, f_uuid)) {
  static const uuid_t base = { 'r', 'e', 'd', 'w', 'o', 'o', 0x50, 'd', 0x80, 'P', 0, 0, 0, 0, 0, 0 };
  fsid_t fsid = vfs_statfs(mp)->f_fsid;
  memcpy(fsap->f_uuid, base, sizeof(uuid_t));
  memcpy(&fsap->f_uuid[10], &fsid.val[0], 4);
  fsap->f_uuid[14] = (uint8_t)fsid.val[1];
  VFSATTR_SET_SUPPORTED(fsap, f_uuid);
 }
 if (VFSATTR_IS_ACTIVE(fsap, f_attributes)) {
  vol_attributes_attr_t *attr = &fsap->f_attributes;
  attr->validattr.commonattr = ATTR_CMN_VALIDMASK;
  attr->validattr.volattr = ATTR_VOL_VALIDMASK;
  attr->validattr.dirattr = ATTR_DIR_VALIDMASK;
  attr->validattr.fileattr = ATTR_FILE_VALIDMASK;
  attr->validattr.forkattr = 0;
  attr->nativeattr = attr->validattr;
  VFSATTR_SET_SUPPORTED(fsap, f_attributes);
 }
 if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
  strlcpy(fsap->f_vol_name, "packfs", MAXPATHLEN);
  VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
 }
 return 0;
}
static int
packfs_sync(__unused mount_t mp, __unused int waitfor, __unused vfs_context_t ctx)
{
 return 0;
}
static int
packfs_init(__unused struct vfsconf *vfsc)
{
 return 0;
}
struct vfsops packfs_vfsops = {
 .vfs_mount = packfs_mount,
 .vfs_unmount = packfs_unmount,
 .vfs_root = packfs_root,
 .vfs_getattr = packfs_vfs_getattr,
 .vfs_sync = packfs_sync,
 .vfs_vget = packfs_vget,
 .vfs_init = packfs_init,
};
