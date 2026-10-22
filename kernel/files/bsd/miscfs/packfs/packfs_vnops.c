#include <kern/assert.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <miscfs/packfs/packfs.h>
#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/ubc.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/vnode_internal.h>
#include <vfs/vfs_support.h>
static const char *
packfs_name(const struct packfs_mount *pm, const struct packfs_dnode *dn)
{
 return pm->pm_strings + dn->pd_name;
}
static enum vtype
packfs_vtype(const struct packfs_dnode *dn)
{
 switch (dn->pd_mode & S_IFMT) {
 case S_IFDIR:
  return VDIR;
 case S_IFLNK:
  return VLNK;
 default:
  return VREG;
 }
}
static uint64_t
packfs_extent_size(const struct packfs_mount *pm, const struct packfs_dnode *dn)
{
 uint64_t block = pm->pm_super.ps_block_size;
 if (dn->pd_flags & PACKFS_F_CHUNKED) {
  uint64_t chunks = (dn->pd_size + PACKFS_CHUNK_SIZE - 1) / PACKFS_CHUNK_SIZE;
  uint32_t end;
  memcpy(&end, pm->pm_membase + dn->pd_data + chunks * sizeof(uint32_t), sizeof(end));
  return end;
 }
 return (dn->pd_size + block - 1) & ~(block - 1);
}
extern size_t lz4raw_decode_buffer(uint8_t *dst_buffer, size_t dst_size, const uint8_t *src_buffer, size_t src_size,
    void *work);
extern int packfs_zstd_decode(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size);
static int
packfs_chunk(const struct packfs_mount *pm, const struct packfs_dnode *dn, uint64_t index, uint8_t *dst, size_t dst_size)
{
 const uint8_t *extent = pm->pm_membase + dn->pd_data;
 uint64_t raw = MIN((uint64_t)PACKFS_CHUNK_SIZE, dn->pd_size - index * PACKFS_CHUNK_SIZE);
 uint32_t begin, end;
 memcpy(&begin, extent + index * sizeof(uint32_t), sizeof(begin));
 memcpy(&end, extent + (index + 1) * sizeof(uint32_t), sizeof(end));
 if (dst_size < raw) {
  return EINVAL;
 }
 if (end - begin == raw) {
  memcpy(dst, extent + begin, raw);
 } else if ((dn->pd_flags & PACKFS_F_ZSTD) ? packfs_zstd_decode(dst, raw, extent + begin, end - begin) != 0 :
     lz4raw_decode_buffer(dst, raw, extent + begin, end - begin, NULL) != raw) {
  printf("packfs: chunk %llu of node %u does not decompress\n", (unsigned long long)index,
      (uint32_t)(dn - pm->pm_nodes));
  return EIO;
 }
 if (dst_size > raw) {
  bzero(dst + raw, dst_size - raw);
 }
 return 0;
}
static int
packfs_read_chunked(vnode_t vp, struct uio *uio, const struct packfs_mount *pm, const struct packfs_dnode *dn)
{
 off_t size = (off_t)dn->pd_size;
 int error = 0;
 while (uio_resid(uio) > 0 && uio_offset(uio) < size) {
  off_t offset = uio_offset(uio);
  int wanted = (int)MIN((off_t)MIN(uio_resid(uio), (user_ssize_t)INT_MAX), size - offset);
  int left = wanted;
  off_t start, length;
  upl_page_info_t *pages;
  vm_offset_t address;
  upl_t upl;
  error = cluster_copy_ubc_data(vp, uio, &left, 0);
  if (error) {
   break;
  }
  if (left != wanted) {
   continue;
  }
  start = offset - (offset % PACKFS_CHUNK_SIZE);
  length = MIN((off_t)PACKFS_CHUNK_SIZE, (off_t)round_page_64((uint64_t)size) - start);
  if (ubc_create_upl_kernel(vp, start, (int)length, &upl, &pages, UPL_SET_LITE, VM_KERN_MEMORY_FILE) != KERN_SUCCESS) {
   error = EIO;
   break;
  }
  if (ubc_upl_map(upl, &address) != KERN_SUCCESS) {
   ubc_upl_abort_range(upl, 0, (upl_size_t)length, UPL_ABORT_FREE_ON_EMPTY);
   error = ENOMEM;
   break;
  }
  error = packfs_chunk(pm, dn, (uint64_t)start / PACKFS_CHUNK_SIZE, (uint8_t *)address, (size_t)length);
  if (!error) {
   error = uiomove((caddr_t)address + (offset - start),
       (int)MIN((off_t)wanted, MIN(size, start + length) - offset), uio);
  }
  ubc_upl_unmap(upl);
  if (error) {
   ubc_upl_abort_range(upl, 0, (upl_size_t)length, UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
   break;
  }
  ubc_upl_commit_range(upl, 0, (upl_size_t)length, UPL_COMMIT_FREE_ON_EMPTY | UPL_COMMIT_INACTIVATE);
 }
 return error;
}
static int
packfs_pagein_chunked(struct vnop_pagein_args *ap, const struct packfs_mount *pm, const struct packfs_dnode *dn)
{
 upl_t upl = ap->a_pl;
 upl_offset_t upl_offset = ap->a_pl_offset;
 off_t offset = ap->a_f_offset, end = ap->a_f_offset + (off_t)ap->a_size;
 off_t size = (off_t)dn->pd_size;
 vm_offset_t address;
 uint8_t *bounce = NULL;
 int error = 0;
 if (upl == NULL) {
  return EINVAL;
 }
 if (offset < 0 || (offset & PAGE_MASK_64) || (ap->a_size & PAGE_MASK) || (upl_offset & PAGE_MASK)) {
  error = EINVAL;
 } else if (ubc_upl_map(upl, &address) != KERN_SUCCESS) {
  error = ENOMEM;
 }
 if (error) {
  if (!(ap->a_flags & UPL_NOCOMMIT)) {
   ubc_upl_abort_range(upl, upl_offset, (upl_size_t)ap->a_size, UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
  }
  return error;
 }
 for (off_t at = offset; at < end && !error;) {
  uint8_t *dst = (uint8_t *)address + upl_offset + (at - offset);
  off_t chunk_start = at - (at % PACKFS_CHUNK_SIZE);
  off_t take = MIN(end, chunk_start + PACKFS_CHUNK_SIZE) - at;
  if (at >= size) {
   bzero(dst, (size_t)(end - at));
   break;
  }
  if (at == chunk_start && take >= MIN((off_t)PACKFS_CHUNK_SIZE, size - chunk_start)) {
   error = packfs_chunk(pm, dn, (uint64_t)chunk_start / PACKFS_CHUNK_SIZE, dst, (size_t)take);
  } else {
   if (bounce == NULL) {
    bounce = kalloc_data(PACKFS_CHUNK_SIZE, Z_WAITOK);
   }
   error = bounce ? packfs_chunk(pm, dn, (uint64_t)chunk_start / PACKFS_CHUNK_SIZE, bounce, PACKFS_CHUNK_SIZE)
       : ENOMEM;
   if (!error) {
    memcpy(dst, bounce + (at - chunk_start), (size_t)take);
   }
  }
  at += take;
 }
 if (bounce != NULL) {
  kfree_data(bounce, PACKFS_CHUNK_SIZE);
 }
 ubc_upl_unmap(upl);
 if (!(ap->a_flags & UPL_NOCOMMIT)) {
  if (error) {
   ubc_upl_abort_range(upl, upl_offset, (upl_size_t)ap->a_size, UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
  } else {
   ubc_upl_commit_range(upl, upl_offset, (upl_size_t)ap->a_size, UPL_COMMIT_FREE_ON_EMPTY);
  }
 }
 return error;
}
int
packfs_node_vnode(struct packfs_mount *pm, uint32_t index, vnode_t dvp, struct componentname *cnp, vnode_t *vpp)
{
 struct packfs_inode *ip = &pm->pm_inodes[index];
 const struct packfs_dnode *dn = &pm->pm_nodes[index];
 struct vnode_fsparam param;
 vnode_t parent = NULLVP;
 vnode_t vp;
 int error;
 lck_mtx_lock(&pm->pm_lock);
 for (;;) {
  uint32_t vid;
  if (ip->pi_flags & PACKFS_I_CREATING) {
   ip->pi_flags |= PACKFS_I_WANTED;
   msleep(ip, &pm->pm_lock, PINOD, "packfs_create", NULL);
   continue;
  }
  vp = ip->pi_vp;
  if (vp == NULLVP) {
   break;
  }
  vid = ip->pi_vid;
  lck_mtx_unlock(&pm->pm_lock);
  if (vnode_getwithvid(vp, vid) == 0) {
   if (cnp && dvp && (cnp->cn_flags & MAKEENTRY)) {
    cache_enter(dvp, vp, cnp);
   }
   *vpp = vp;
   return 0;
  }
  lck_mtx_lock(&pm->pm_lock);
  if (ip->pi_vp == vp && ip->pi_vid == vid) {
   ip->pi_flags |= PACKFS_I_WANTED;
   msleep(ip, &pm->pm_lock, PINOD, "packfs_reclaim", NULL);
  }
 }
 ip->pi_flags |= PACKFS_I_CREATING;
 lck_mtx_unlock(&pm->pm_lock);
 error = 0;
 if (index != PACKFS_ROOT_NODE && dvp == NULLVP) {
  error = packfs_node_vnode(pm, dn->pd_parent, NULLVP, NULL, &parent);
  dvp = parent;
 }
 vp = NULLVP;
 if (!error) {
  bool enter = cnp && (cnp->cn_flags & MAKEENTRY);
  bzero(&param, sizeof(param));
  param.vnfs_mp = pm->pm_mp;
  param.vnfs_vtype = packfs_vtype(dn);
  param.vnfs_str = "packfs";
  param.vnfs_dvp = dvp;
  param.vnfs_fsnode = ip;
  param.vnfs_vops = packfs_vnodeop_p;
  param.vnfs_markroot = (index == PACKFS_ROOT_NODE);
  param.vnfs_marksystem = 0;
  param.vnfs_rdev = 0;
  param.vnfs_filesize = S_ISREG(dn->pd_mode) ? (off_t)dn->pd_size : 0;
  param.vnfs_cnp = enter ? cnp : NULL;
  param.vnfs_flags = enter ? 0 : VNFS_NOCACHE;
  error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &param, &vp);
  if (!error && !enter && index != PACKFS_ROOT_NODE) {
   vnode_update_identity(vp, NULLVP, packfs_name(pm, dn), (int)dn->pd_name_len, 0, VNODE_UPDATE_NAME);
  }
 }
 if (parent != NULLVP) {
  vnode_put(parent);
 }
 lck_mtx_lock(&pm->pm_lock);
 if (!error) {
  ip->pi_vp = vp;
  ip->pi_vid = vnode_vid(vp);
 }
 if (ip->pi_flags & PACKFS_I_WANTED) {
  wakeup(ip);
 }
 ip->pi_flags &= ~(PACKFS_I_CREATING | PACKFS_I_WANTED);
 lck_mtx_unlock(&pm->pm_lock);
 if (!error) {
  *vpp = vp;
 }
 return error;
}
static bool
packfs_find_child(const struct packfs_mount *pm, const struct packfs_dnode *dir, const char *name, size_t len,
    uint32_t *index)
{
 uint64_t lo = 0, hi = dir->pd_size;
 while (lo < hi) {
  uint64_t mid = lo + (hi - lo) / 2;
  const struct packfs_dnode *dn = &pm->pm_nodes[dir->pd_data + mid];
  size_t common = MIN(len, (size_t)dn->pd_name_len);
  int cmp = memcmp(name, packfs_name(pm, dn), common);
  if (cmp == 0) {
   cmp = (len > dn->pd_name_len) - (len < dn->pd_name_len);
  }
  if (cmp == 0) {
   *index = (uint32_t)(dir->pd_data + mid);
   return true;
  }
  if (cmp < 0) {
   hi = mid;
  } else {
   lo = mid + 1;
  }
 }
 return false;
}
static int
packfs_lookup(struct vnop_lookup_args *ap)
{
 vnode_t dvp = ap->a_dvp;
 struct componentname *cnp = ap->a_cnp;
 struct packfs_mount *pm = PACKFS_VP_MOUNT(dvp);
 uint32_t dir_index = packfs_vp_index(pm, dvp);
 const struct packfs_dnode *dir = &pm->pm_nodes[dir_index];
 uint32_t index;
 *ap->a_vpp = NULLVP;
 if (!S_ISDIR(dir->pd_mode)) {
  return ENOTDIR;
 }
 if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
  int error = vnode_get(dvp);
  if (!error) {
   *ap->a_vpp = dvp;
  }
  return error;
 }
 if (cnp->cn_flags & ISDOTDOT) {
  return packfs_node_vnode(pm, dir->pd_parent, NULLVP, NULL, ap->a_vpp);
 }
 if (!packfs_find_child(pm, dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen, &index)) {
  if ((cnp->cn_flags & ISLASTCN) && (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)) {
   return EROFS;
  }
  return ENOENT;
 }
 return packfs_node_vnode(pm, index, dvp, cnp, ap->a_vpp);
}
static int
packfs_open(__unused struct vnop_open_args *ap)
{
 return 0;
}
static int
packfs_close(__unused struct vnop_close_args *ap)
{
 return 0;
}
static int
packfs_getattr(struct vnop_getattr_args *ap)
{
 vnode_t vp = ap->a_vp;
 struct vnode_attr *vap = ap->a_vap;
 struct packfs_mount *pm = PACKFS_VP_MOUNT(vp);
 uint32_t index = packfs_vp_index(pm, vp);
 const struct packfs_dnode *dn = &pm->pm_nodes[index];
 uint64_t size = S_ISDIR(dn->pd_mode) ? pm->pm_super.ps_block_size : dn->pd_size;
 uint64_t alloc = S_ISREG(dn->pd_mode) ? packfs_extent_size(pm, dn) : 0;
 struct timespec when = { .tv_sec = (time_t)dn->pd_mtime, .tv_nsec = 0 };
 VATTR_RETURN(vap, va_rdev, 0);
 VATTR_RETURN(vap, va_nlink, dn->pd_nlink);
 VATTR_RETURN(vap, va_total_size, size);
 VATTR_RETURN(vap, va_total_alloc, alloc);
 VATTR_RETURN(vap, va_data_size, size);
 VATTR_RETURN(vap, va_data_alloc, alloc);
 VATTR_RETURN(vap, va_iosize, pm->pm_super.ps_block_size);
 VATTR_RETURN(vap, va_uid, dn->pd_uid);
 VATTR_RETURN(vap, va_gid, dn->pd_gid);
 VATTR_RETURN(vap, va_mode, dn->pd_mode & ALLPERMS);
 VATTR_RETURN(vap, va_flags, 0);
 VATTR_RETURN(vap, va_create_time, when);
 VATTR_RETURN(vap, va_access_time, when);
 VATTR_RETURN(vap, va_modify_time, when);
 VATTR_RETURN(vap, va_change_time, when);
 VATTR_RETURN(vap, va_fileid, PACKFS_INO(index));
 VATTR_RETURN(vap, va_linkid, PACKFS_INO(index));
 VATTR_RETURN(vap, va_parentid, PACKFS_INO(dn->pd_parent));
 VATTR_RETURN(vap, va_fsid, vfs_statfs(vnode_mount(vp))->f_fsid.val[0]);
 VATTR_RETURN(vap, va_filerev, 0);
 VATTR_RETURN(vap, va_gen, 0);
 if (VATTR_IS_ACTIVE(vap, va_name) && index != PACKFS_ROOT_NODE) {
  strlcpy(vap->va_name, packfs_name(pm, dn), MAXPATHLEN);
  VATTR_SET_SUPPORTED(vap, va_name);
 }
 return 0;
}
static int
packfs_read(struct vnop_read_args *ap)
{
 vnode_t vp = ap->a_vp;
 struct packfs_mount *pm = PACKFS_VP_MOUNT(vp);
 const struct packfs_dnode *dn = &pm->pm_nodes[packfs_vp_index(pm, vp)];
 if (vnode_isdir(vp)) {
  return EISDIR;
 }
 if (!vnode_isreg(vp)) {
  return EINVAL;
 }
 if (uio_offset(ap->a_uio) < 0) {
  return EINVAL;
 }
 if (dn->pd_flags & PACKFS_F_CHUNKED) {
  return packfs_read_chunked(vp, ap->a_uio, pm, dn);
 }
 return cluster_read(vp, ap->a_uio, (off_t)dn->pd_size, ap->a_ioflag);
}
static int
packfs_readdir(struct vnop_readdir_args *ap)
{
#define PACKFS_DIRSIZ(namlen) ((sizeof(struct dirent) - (MAXNAMLEN + 1)) + (((namlen) + 1 + 3) & ~3))
 vnode_t vp = ap->a_vp;
 struct uio *uio = ap->a_uio;
 struct packfs_mount *pm = PACKFS_VP_MOUNT(vp);
 uint32_t dir_index = packfs_vp_index(pm, vp);
 const struct packfs_dnode *dir = &pm->pm_nodes[dir_index];
 off_t start = uio_offset(uio);
 off_t pos = 0;
 uint64_t entry;
 int count = 0;
 int error = 0;
 if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) {
  return EINVAL;
 }
 if (!S_ISDIR(dir->pd_mode)) {
  return ENOTDIR;
 }
 for (entry = 0; entry < dir->pd_size + 2; entry++) {
  struct dirent de;
  const char *name;
  bzero(&de, sizeof(de));
  if (entry == 0) {
   de.d_fileno = (ino_t)PACKFS_INO(dir_index);
   de.d_type = DT_DIR;
   de.d_namlen = 1;
   name = ".";
  } else if (entry == 1) {
   de.d_fileno = (ino_t)PACKFS_INO(dir->pd_parent);
   de.d_type = DT_DIR;
   de.d_namlen = 2;
   name = "..";
  } else {
   uint32_t child = (uint32_t)(dir->pd_data + entry - 2);
   const struct packfs_dnode *dn = &pm->pm_nodes[child];
   de.d_fileno = (ino_t)PACKFS_INO(child);
   de.d_type = (uint8_t)IFTODT(dn->pd_mode);
   de.d_namlen = (uint8_t)dn->pd_name_len;
   name = packfs_name(pm, dn);
  }
  de.d_reclen = (uint16_t)PACKFS_DIRSIZ(de.d_namlen);
  if (pos >= start) {
   if (uio_resid(uio) < (user_ssize_t)de.d_reclen) {
    break;
   }
   memcpy(de.d_name, name, de.d_namlen);
   error = uiomove((caddr_t)&de, de.d_reclen, uio);
   if (error) {
    break;
   }
   count++;
  }
  pos += de.d_reclen;
 }
 if (!error) {
  uio_setoffset(uio, pos);
  if (ap->a_eofflag) {
   *ap->a_eofflag = (entry >= dir->pd_size + 2);
  }
  if (ap->a_numdirent) {
   *ap->a_numdirent = count;
  }
 }
 return error;
#undef PACKFS_DIRSIZ
}
static int
packfs_readlink(struct vnop_readlink_args *ap)
{
 vnode_t vp = ap->a_vp;
 struct packfs_mount *pm = PACKFS_VP_MOUNT(vp);
 const struct packfs_dnode *dn = &pm->pm_nodes[packfs_vp_index(pm, vp)];
 if (!S_ISLNK(dn->pd_mode)) {
  return EINVAL;
 }
 return uiomove(pm->pm_strings + dn->pd_data, (int)dn->pd_size, ap->a_uio);
}
static int
packfs_inactive(__unused struct vnop_inactive_args *ap)
{
 return 0;
}
static int
packfs_reclaim(struct vnop_reclaim_args *ap)
{
 vnode_t vp = ap->a_vp;
 struct packfs_mount *pm = PACKFS_VP_MOUNT(vp);
 struct packfs_inode *ip = vnode_fsnode(vp);
 lck_mtx_lock(&pm->pm_lock);
 if (ip->pi_vp == vp) {
  ip->pi_vp = NULLVP;
  ip->pi_vid = 0;
 }
 if (ip->pi_flags & PACKFS_I_WANTED) {
  ip->pi_flags &= ~PACKFS_I_WANTED;
  wakeup(ip);
 }
 lck_mtx_unlock(&pm->pm_lock);
 vnode_clearfsnode(vp);
 return 0;
}
static int
packfs_pathconf(struct vnop_pathconf_args *ap)
{
 switch (ap->a_name) {
 case _PC_LINK_MAX:
  *ap->a_retval = 1;
  return 0;
 case _PC_NAME_MAX:
  *ap->a_retval = PACKFS_MAX_NAME;
  return 0;
 case _PC_PATH_MAX:
  *ap->a_retval = MAXPATHLEN;
  return 0;
 case _PC_PIPE_BUF:
  *ap->a_retval = PIPE_BUF;
  return 0;
 case _PC_CHOWN_RESTRICTED:
  *ap->a_retval = 200112;
  return 0;
 case _PC_NO_TRUNC:
  *ap->a_retval = 200112;
  return 0;
 case _PC_CASE_SENSITIVE:
 case _PC_CASE_PRESERVING:
  *ap->a_retval = 1;
  return 0;
 case _PC_FILESIZEBITS:
  *ap->a_retval = 64;
  return 0;
 default:
  return EINVAL;
 }
}
static int
packfs_ioctl(__unused struct vnop_ioctl_args *ap)
{
 return ENOTTY;
}
static int
packfs_getxattr(__unused struct vnop_getxattr_args *ap)
{
 return ENOATTR;
}
static int
packfs_listxattr(struct vnop_listxattr_args *ap)
{
 *ap->a_size = 0;
 return 0;
}
static int
packfs_mmap(__unused struct vnop_mmap_args *ap)
{
 return 0;
}
static int
packfs_mnomap(__unused struct vnop_mnomap_args *ap)
{
 return 0;
}
static int
packfs_pagein(struct vnop_pagein_args *ap)
{
 vnode_t vp = ap->a_vp;
 struct packfs_mount *pm = PACKFS_VP_MOUNT(vp);
 const struct packfs_dnode *dn = &pm->pm_nodes[packfs_vp_index(pm, vp)];
 if (dn->pd_flags & PACKFS_F_CHUNKED) {
  return packfs_pagein_chunked(ap, pm, dn);
 }
 return cluster_pagein(vp, ap->a_pl, ap->a_pl_offset, ap->a_f_offset, (int)ap->a_size, (off_t)dn->pd_size, ap->a_flags);
}
static int
packfs_strategy(struct vnop_strategy_args *ap)
{
 return buf_strategy(PACKFS_VP_MOUNT(buf_vnode(ap->a_bp))->pm_devvp, ap);
}
static int
packfs_blockmap(struct vnop_blockmap_args *ap)
{
 vnode_t vp = ap->a_vp;
 struct packfs_mount *pm = PACKFS_VP_MOUNT(vp);
 const struct packfs_dnode *dn = &pm->pm_nodes[packfs_vp_index(pm, vp)];
 uint64_t extent = packfs_extent_size(pm, dn);
 uint32_t devblock = (uint32_t)vfs_devblocksize(vnode_mount(vp));
 if (!vnode_isreg(vp)) {
  return ENOTSUP;
 }
 if (dn->pd_flags & PACKFS_F_CHUNKED) {
  return ENOTSUP;
 }
 if (ap->a_bpn == NULL) {
  return 0;
 }
 if (ap->a_foffset < 0 || (uint64_t)ap->a_foffset >= extent) {
  *ap->a_bpn = -1;
  if (ap->a_run) {
   *ap->a_run = ap->a_size;
  }
 } else {
  *ap->a_bpn = (daddr64_t)((dn->pd_data + (uint64_t)ap->a_foffset) / devblock);
  if (ap->a_run) {
   *ap->a_run = (size_t)MIN((uint64_t)ap->a_size, extent - (uint64_t)ap->a_foffset);
  }
 }
 if (ap->a_poff) {
  *(int *)ap->a_poff = 0;
 }
 return 0;
}
static int
packfs_blktooff(struct vnop_blktooff_args *ap)
{
 *ap->a_offset = (off_t)ap->a_lblkno * PACKFS_VP_MOUNT(ap->a_vp)->pm_super.ps_block_size;
 return 0;
}
static int
packfs_offtoblk(struct vnop_offtoblk_args *ap)
{
 *ap->a_lblkno = (daddr64_t)(ap->a_offset / PACKFS_VP_MOUNT(ap->a_vp)->pm_super.ps_block_size);
 return 0;
}
#define VOPFUNC int (*)(void *)
int(**packfs_vnodeop_p)(void *);
const struct vnodeopv_entry_desc packfs_vnodeop_entries[] = {
 { .opve_op = &vnop_default_desc, .opve_impl = (VOPFUNC)vn_default_error },
 { .opve_op = &vnop_lookup_desc, .opve_impl = (VOPFUNC)packfs_lookup },
 { .opve_op = &vnop_open_desc, .opve_impl = (VOPFUNC)packfs_open },
 { .opve_op = &vnop_close_desc, .opve_impl = (VOPFUNC)packfs_close },
 { .opve_op = &vnop_getattr_desc, .opve_impl = (VOPFUNC)packfs_getattr },
 { .opve_op = &vnop_read_desc, .opve_impl = (VOPFUNC)packfs_read },
 { .opve_op = &vnop_ioctl_desc, .opve_impl = (VOPFUNC)packfs_ioctl },
 { .opve_op = &vnop_getxattr_desc, .opve_impl = (VOPFUNC)packfs_getxattr },
 { .opve_op = &vnop_listxattr_desc, .opve_impl = (VOPFUNC)packfs_listxattr },
 { .opve_op = &vnop_fsync_desc, .opve_impl = (VOPFUNC)nop_fsync },
 { .opve_op = &vnop_readdir_desc, .opve_impl = (VOPFUNC)packfs_readdir },
 { .opve_op = &vnop_readlink_desc, .opve_impl = (VOPFUNC)packfs_readlink },
 { .opve_op = &vnop_inactive_desc, .opve_impl = (VOPFUNC)packfs_inactive },
 { .opve_op = &vnop_reclaim_desc, .opve_impl = (VOPFUNC)packfs_reclaim },
 { .opve_op = &vnop_pathconf_desc, .opve_impl = (VOPFUNC)packfs_pathconf },
 { .opve_op = &vnop_mmap_desc, .opve_impl = (VOPFUNC)packfs_mmap },
 { .opve_op = &vnop_mnomap_desc, .opve_impl = (VOPFUNC)packfs_mnomap },
 { .opve_op = &vnop_pagein_desc, .opve_impl = (VOPFUNC)packfs_pagein },
 { .opve_op = &vnop_strategy_desc, .opve_impl = (VOPFUNC)packfs_strategy },
 { .opve_op = &vnop_blockmap_desc, .opve_impl = (VOPFUNC)packfs_blockmap },
 { .opve_op = &vnop_blktooff_desc, .opve_impl = (VOPFUNC)packfs_blktooff },
 { .opve_op = &vnop_offtoblk_desc, .opve_impl = (VOPFUNC)packfs_offtoblk },
 { .opve_op = (struct vnodeop_desc *)NULL, .opve_impl = (VOPFUNC)NULL }
};
const struct vnodeopv_desc packfs_vnodeop_opv_desc = {
 .opv_desc_vector_p = &packfs_vnodeop_p,
 .opv_desc_ops = packfs_vnodeop_entries
};
