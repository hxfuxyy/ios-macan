#ifndef REDWOOD_XF86DRM_H
#define REDWOOD_XF86DRM_H
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "drm-uapi/drm.h"
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER 2
#define DRM_CLOEXEC O_CLOEXEC
#define DRM_RDWR O_RDWR
typedef struct _drmVersion {
 int version_major, version_minor, version_patchlevel;
 int name_len;
 char *name;
 int date_len;
 char *date;
 int desc_len;
 char *desc;
} drmVersion, *drmVersionPtr;
int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
                   unsigned flags, uint32_t *first_signaled);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
                           int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
                       uint64_t src_point, uint32_t flags);
void *redwood_drm_map(int fd, uint32_t handle, uint64_t size);
void redwood_drm_unmap(int fd, uint32_t handle, void *address);
struct redwood_drm_surface {
 uint32_t surface;
 uint32_t handle;
 uint64_t size;
 uint32_t offset, row_bytes;
 uint32_t width, height, pixel_format, planes;
};
int redwood_drm_surface(int fd, struct redwood_drm_surface *surface);
#endif
