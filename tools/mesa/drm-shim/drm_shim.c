#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include "xf86drm.h"
#define REDWOOD_GPU_SERVICE "RedwoodGPU"
#define REDWOOD_GPU_SURFACE 0x7e
#define REDWOOD_GPU_DESCRIBE 0xffffffffu
static io_connect_t connection;
static int connection_fd = -1;
int
drmOpenWithType(const char *name, const char *busid, int type)
{
 io_service_t service;
 (void)busid;
 (void)type;
 if (strcmp(name, "msm") != 0) {
  return -1;
 }
 if (connection_fd >= 0) {
  return dup(connection_fd);
 }
 service = IOServiceGetMatchingService(0, IOServiceMatching(REDWOOD_GPU_SERVICE));
 if (service == 0) {
  return -1;
 }
 if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
  IOObjectRelease(service);
  return -1;
 }
 IOObjectRelease(service);
 connection_fd = open("/dev/null", O_RDWR);
 return dup(connection_fd);
}
static int
call(unsigned number, void *data, size_t size)
{
 size_t returned = size;
 kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);
 switch (result) {
 case 0:
  return 0;
 case kIOReturnNoMemory:
  return ENOMEM;
 case kIOReturnBadArgument:
  return EINVAL;
 case kIOReturnTimeout:
  return ETIMEDOUT;
 case kIOReturnBusy:
  return EBUSY;
 case kIOReturnNotFound:
  return ENOENT;
 case kIOReturnUnsupported:
  return ENOSYS;
 default:
  return EIO;
 }
}
int
drmIoctl(int fd, unsigned long request, void *arg)
{
 int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));
 (void)fd;
 if (error != 0) {
  errno = error;
  return -1;
 }
 return 0;
}
int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
 (void)fd;
 return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}
int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
 (void)fd;
 return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}
drmVersionPtr
drmGetVersion(int fd)
{
 drmVersionPtr version = calloc(1, sizeof(*version));
 (void)fd;
 if (version != NULL) {
  version->version_major = 1;
  version->version_minor = 7;
  version->name = strdup("msm");
  version->name_len = 3;
  version->date = strdup("");
  version->desc = strdup("Adreno on Darwin");
  version->desc_len = (int)strlen(version->desc);
 }
 return version;
}
void
drmFreeVersion(drmVersionPtr version)
{
 if (version != NULL) {
  free(version->name);
  free(version->date);
  free(version->desc);
  free(version);
 }
}
int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
 (void)fd;
 (void)capability;
 *value = 0;
 return 0;
}
void *
redwood_drm_map(int fd, uint32_t handle, uint64_t size)
{
 mach_vm_address_t address = 0;
 mach_vm_size_t mapped = 0;
 (void)fd;
 (void)size;
 if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
  return (void *)-1;
 }
 return (void *)(uintptr_t)address;
}
void
redwood_drm_unmap(int fd, uint32_t handle, void *address)
{
 (void)fd;
 IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}
int
redwood_drm_surface(int fd, struct redwood_drm_surface *surface)
{
 int error;
 (void)fd;
 surface->handle = REDWOOD_GPU_DESCRIBE;
 error = call(REDWOOD_GPU_SURFACE, surface, sizeof(*surface));
 if (error != 0) {
  errno = error;
  return -1;
 }
 return 0;
}
int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
 struct redwood_drm_surface request = { .surface = (uint32_t)surface };
 int error = call(REDWOOD_GPU_SURFACE, &request, sizeof(request));
 (void)fd;
 if (error != 0) {
  errno = error;
  return -1;
 }
 *handle = request.handle;
 return 0;
}
static int
none(void)
{
 errno = ENOSYS;
 return -1;
}
#define NONE(name,...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjCreate, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t flags,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjDestroy, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle)
NONE(drmSyncobjReset, __attribute__((unused)) int fd, __attribute__((unused)) const uint32_t *handles,
    __attribute__((unused)) uint32_t count)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)
NONE(drmSyncobjImportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int sync_file_fd)
NONE(drmSyncobjExportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *sync_file_fd)
NONE(drmSyncobjWait, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles, __attribute__((unused)) unsigned num_handles,
    __attribute__((unused)) int64_t timeout_nsec, __attribute__((unused)) unsigned flags, __attribute__((unused)) uint32_t *first_signaled)
NONE(drmSyncobjSignal, __attribute__((unused)) int fd, __attribute__((unused)) const uint32_t *handles, __attribute__((unused)) uint32_t handle_count)
NONE(drmSyncobjTimelineSignal, __attribute__((unused)) int fd, __attribute__((unused)) const uint32_t *handles, __attribute__((unused)) uint64_t *points,
    __attribute__((unused)) uint32_t handle_count)
NONE(drmSyncobjTimelineWait, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles, __attribute__((unused)) uint64_t *points,
    __attribute__((unused)) unsigned num_handles, __attribute__((unused)) int64_t timeout_nsec, __attribute__((unused)) unsigned flags,
    __attribute__((unused)) uint32_t *first_signaled)
NONE(drmSyncobjQuery, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles, __attribute__((unused)) uint64_t *points,
    __attribute__((unused)) uint32_t handle_count)
NONE(drmSyncobjTransfer, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t dst_handle, __attribute__((unused)) uint64_t dst_point,
    __attribute__((unused)) uint32_t src_handle, __attribute__((unused)) uint64_t src_point, __attribute__((unused)) uint32_t flags)
