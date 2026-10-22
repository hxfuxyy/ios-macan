#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>
#include "MiniIOSurface.h"
extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);
extern "C" {
bool redwood_gpu_ready(void);
bool redwood_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t redwood_gpu_address(uint64_t size);
void redwood_gpu_address_free(uint64_t address, uint64_t size);
bool redwood_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void redwood_gpu_mapped(void);
void redwood_gpu_unmap(uint64_t address, uint64_t size);
void redwood_gpu_to_memory(void * start, uint64_t size);
uint32_t redwood_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
extern uint32_t gpu_log;
bool redwood_gpu_done(uint32_t fence);
uint32_t redwood_gpu_last(void);
void redwood_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void redwood_gpu_report(const char * why);
void redwood_gpu_stop(void);
void RedwoodGPUStart(void);
void RedwoodGPUStop(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}
enum {
 kDrmGemClose = 0x09,
 kMsmGetParam = 0x40,
 kMsmSetParam = 0x41,
 kMsmGemNew = 0x42,
 kMsmGemInfo = 0x43,
 kMsmGemCpuPrep = 0x44,
 kMsmGemCpuFini = 0x45,
 kMsmGemSubmit = 0x46,
 kMsmWaitFence = 0x47,
 kMsmGemMadvise = 0x48,
 kMsmSubmitQueueNew = 0x4a,
 kMsmSubmitQueueClose = 0x4b,
 kMsmSubmitQueueQuery = 0x4c,
 kRedwoodSurface = 0x7e,
 kRedwoodShow = 0x7f,
};
struct msm_timespec {
 int64_t seconds, nanoseconds;
};
struct msm_param {
 uint32_t pipe, param;
 uint64_t value;
 uint32_t len, pad;
};
struct msm_gem_new {
 uint64_t size;
 uint32_t flags, handle;
};
struct msm_gem_info {
 uint32_t handle, info;
 uint64_t value;
 uint32_t len, pad;
};
struct msm_gem_cpu_prep {
 uint32_t handle, op;
 struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
 uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
 uint64_t relocs;
};
struct msm_gem_submit_bo {
 uint32_t flags, handle;
 uint64_t presumed;
};
struct msm_gem_submit {
 uint32_t flags, fence, nr_bos, nr_cmds;
 uint64_t bos, cmds;
 int32_t fence_fd;
 uint32_t queueid;
 uint64_t in_syncobjs, out_syncobjs;
 uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
 uint32_t fence, flags;
 struct msm_timespec timeout;
 uint32_t queueid;
};
struct msm_gem_madvise {
 uint32_t handle, madv, retained;
};
struct msm_submitqueue {
 uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
 uint64_t data;
 uint32_t id, param, len, pad;
};
struct drm_gem_close {
 uint32_t handle, pad;
};
struct redwood_surface {
 uint32_t surface;
 uint32_t handle;
 uint64_t size;
 uint32_t offset, rowBytes;
 uint32_t width, height, pixelFormat, planes;
};
struct redwood_show {
 uint64_t pixels;
 uint32_t width, height, rowBytes, pad;
};
#define kRedwoodDescribe 0xffffffffu
#define MSM_INFO_GET_OFFSET 0
#define MSM_INFO_GET_IOVA 1
#define MSM_INFO_SET_NAME 2
#define MSM_INFO_GET_FLAGS 5
#define MSM_BO_GPU_READONLY 0x00000002
#define MSM_PREP_NOSYNC 0x04
#define MSM_SUBMIT_CMD_BUF 1
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000
#define GPU_BUFFERS 8192
#define GPU_BUFFER_MAX (256u << 20)
#define GPU_SUBMIT_CMDS 128
#define GPU_SUBMIT_BOS 8192
#define GPU_PATIENCE_MS 5000
#define GPU_ASK_AGAIN_US 200
#define GPU_PUSH_AGAIN_MS 4
static IOLock * gGPULock;
static bool gGPUReported;
static bool gGPUCached;
static struct {
 uint64_t since, flushing, waiting, taking;
 uint64_t flushed;
 uint32_t submissions, commands, buffers, surfaces, waits, over;
 uint32_t made, given_again;
 uint64_t making;
} gGPUStat;
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
 uint64_t began = mach_absolute_time(), patience;
 nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
 for (bool waited = false;; waited = true) {
  if (redwood_gpu_done(fence)) {
   if (waited) {
    gGPUStat.waits++;
    gGPUStat.waiting += mach_absolute_time() - began;
   }
   return true;
  }
  if (mach_absolute_time() - began >= patience || gGPUReported) {
   break;
  }
  IOLockUnlock(gGPULock);
  delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
  IOLockLock(gGPULock);
 }
 if (milliseconds >= 1000 && !gGPUReported) {
  gGPUReported = true;
  redwood_gpu_report("no answer");
 }
 return false;
}
bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
 uint32_t fence = surface->drawn;
 bool done = true;
 if (fence != 0 && gGPULock != NULL) {
  IOLockLock(gGPULock);
  done = !redwood_gpu_ready() || gpuWait(fence, milliseconds);
  IOLockUnlock(gGPULock);
 }
 return done;
}
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);
static void
slowWait(const char * kind, uint64_t began)
{
 uint64_t now = mach_absolute_time(), ns, at;
 if (!gWaitLines) {
  return;
 }
 absolutetime_to_nanoseconds(now - began, &ns);
 if (ns >= NSEC_PER_MSEC) {
  absolutetime_to_nanoseconds(now, &at);
  redwood_tl("tl: f %s %llu at %llu\n", kind, ns / 1000, at / 1000);
 }
}
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
 uint64_t now;
 int64_t left;
 absolutetime_to_nanoseconds(mach_continuous_time(), &now);
 left = when->seconds * (int64_t) NSEC_PER_SEC + when->nanoseconds - (int64_t) now;
 if (left <= 0) {
  return 0;
 }
 return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
 static uint32_t told;
 uint64_t now;
 gGPUStat.over++;
 if (told < 4) {
  told++;
  absolutetime_to_nanoseconds(mach_continuous_time(), &now);
  IOLog("RedwoodGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
      "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
 }
}
class RedwoodGPU : public IOService
{
 OSDeclareDefaultStructors(RedwoodGPU);
public:
 virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(RedwoodGPU, IOService);
bool
RedwoodGPU::start(IOService * provider)
{
 if (!IOService::start(provider)) {
  return false;
 }
 setProperty(kIOUserClientClassKey, "RedwoodGPUUserClient");
 setName("RedwoodGPU");
 registerService();
 return true;
}
void
RedwoodGPUStart(void)
{
 IOService * provider = IOService::getPlatform();
 RedwoodGPU * gpu;
 uint32_t cached = 0;
 gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
 gGPULock = IOLockAlloc();
 gpu = OSTypeAlloc(RedwoodGPU);
 if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
  OSSafeReleaseNULL(gpu);
  IOLog("RedwoodGPU: cannot create the service\n");
  return;
 }
 if (!gpu->start(provider)) {
  gpu->detach(provider);
  IOLog("RedwoodGPU: the service did not start\n");
 }
 gpu->release();
}
void
RedwoodGPUStop(void)
{
 IOLockLock(gGPULock);
 redwood_gpu_stop();
 IOLockUnlock(gGPULock);
}
struct gpu_buffer {
 IOMemoryDescriptor * memory;
 uint8_t * kernel;
 MiniIOSurface * surface;
 uint64_t address;
 uint64_t size;
 uint32_t flags;
 uint32_t seed;
 uint64_t pushed;
 bool seen;
 uint32_t fence;
};
struct gpu_left {
 IOMemoryDescriptor * memory;
 MiniIOSurface * surface;
 uint64_t address, size;
 uint32_t fence, flags;
};
#define GPU_LEFT 64
struct gpu_kept {
 IOMemoryDescriptor * memory;
 uint8_t * kernel;
 uint64_t address, size, since;
 uint32_t flags;
};
#define GPU_KEPT 48
#define GPU_KEPT_BYTES (160ull << 20)
#define GPU_KEPT_SECONDS 60
#define GPU_KEEP_FROM (256u << 10)
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);
class RedwoodGPUUserClient : public IOUserClient
{
 OSDeclareDefaultStructors(RedwoodGPUUserClient);
 task_t fTask;
 struct gpu_buffer * fBuffers;
 uint32_t fNextQueue;
 uint32_t fNextBuffer;
 uint32_t fBufferCount;
 uint64_t fBufferBytes;
 uint32_t fSubmits;
 uint32_t fStrangers;
 struct gpu_left fLeft[GPU_LEFT];
 uint32_t fLeftCount;
 struct gpu_kept fKept[GPU_KEPT];
 uint64_t fKeptBytes;
 struct gpu_buffer * buffer(uint32_t handle);
 struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle);
 void freeBuffer(struct gpu_buffer * b);
 bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags);
 void dropKept(struct gpu_kept * k);
 void reap(void);
 IOReturn gemNew(struct msm_gem_new * request);
 IOReturn surfaceBuffer(struct redwood_surface * request);
 IOReturn gemInfo(struct msm_gem_info * request);
 IOReturn submit(struct msm_gem_submit * request);
 IOReturn show(const struct redwood_show * request);
public:
 virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
 virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
 virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
 virtual void free(void) APPLE_KEXT_OVERRIDE;
 virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
 virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
     IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(RedwoodGPUUserClient, IOUserClient);
bool
RedwoodGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
 if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
  return false;
 }
 fTask = owningTask;
 return true;
}
bool
RedwoodGPUUserClient::start(IOService * provider)
{
 if (!redwood_gpu_ready() || !IOUserClient::start(provider)) {
  return false;
 }
 fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
 return fBuffers != NULL;
}
void
RedwoodGPUUserClient::freeBuffer(struct gpu_buffer * b)
{
 bool busy = redwood_gpu_ready() && b->fence != 0 && !redwood_gpu_done(b->fence);
 if (busy && fLeftCount == GPU_LEFT) {
  busy = !gpuWait(b->fence, 1000);
 }
 if (busy && fLeftCount < GPU_LEFT) {
  struct gpu_left * left = &fLeft[fLeftCount++];
  left->memory = b->memory;
  left->surface = b->surface;
  left->address = b->address;
  left->size = b->size;
  left->fence = b->fence;
  left->flags = b->flags;
 } else {
  if (busy) {
   static uint32_t said;
   if (b->surface == NULL) {
    b->memory->retain();
   } else {
    b->surface->memory->retain();
   }
   if (said++ < 8) {
    IOLog("RedwoodGPU: the GPU is not done with what it was given: a buffer of %llu bytes stays its own\n", b->size);
   }
  } else if (b->surface == NULL && redwood_gpu_ready() && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
   b->memory = NULL;
  } else if (redwood_gpu_ready()) {
   redwood_gpu_unmap(b->address, b->size);
   redwood_gpu_address_free(b->address, b->size);
  }
  if (b->memory != NULL) {
   b->memory->release();
  }
  if (b->surface != NULL) {
   MiniIOSurfaceDrop(b->surface);
  }
 }
 b->memory = NULL;
 b->surface = NULL;
 fBufferCount--;
 fBufferBytes -= b->size;
}
void
RedwoodGPUUserClient::reap(void)
{
 for (uint32_t i = 0; i < fLeftCount;) {
  struct gpu_left * left = &fLeft[i];
  if (redwood_gpu_ready() && !redwood_gpu_done(left->fence)) {
   i++;
   continue;
  }
  if (left->surface == NULL && redwood_gpu_ready() &&
      keep(left->memory, NULL, left->address, left->size, left->flags)) {
   left->memory = NULL;
  } else if (redwood_gpu_ready()) {
   redwood_gpu_unmap(left->address, left->size);
   redwood_gpu_address_free(left->address, left->size);
  }
  if (left->memory != NULL) {
   left->memory->release();
  }
  if (left->surface != NULL) {
   MiniIOSurfaceDrop(left->surface);
  }
  *left = fLeft[--fLeftCount];
 }
}
IOReturn
RedwoodGPUUserClient::clientClose(void)
{
 if (fBuffers != NULL) {
  IOLockLock(gGPULock);
  if (redwood_gpu_ready()) {
   gpuWait(redwood_gpu_last(), 1000);
  }
  for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
   if (fBuffers[i].memory != NULL) {
    freeBuffer(&fBuffers[i]);
   }
  }
  reap();
  for (uint32_t i = 0; i < GPU_KEPT; i++) {
   dropKept(&fKept[i]);
  }
  if (fLeftCount != 0) {
   IOLog("RedwoodGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
  }
  IOLockUnlock(gGPULock);
 }
 if (!isInactive()) {
  terminate();
 }
 return kIOReturnSuccess;
}
void
RedwoodGPUUserClient::free(void)
{
 if (fBuffers != NULL) {
  IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
  fBuffers = NULL;
 }
 IOUserClient::free();
}
struct gpu_buffer *
RedwoodGPUUserClient::buffer(uint32_t handle)
{
 if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
  if (handle != 0 && fStrangers++ < 16) {
   IOLog("RedwoodGPU: buffer %u is asked for and is not there\n", handle);
  }
  return NULL;
 }
 return &fBuffers[handle - 1];
}
struct gpu_buffer *
RedwoodGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle)
{
 struct gpu_buffer * b = NULL;
 bool mapped = true;
 for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
  uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;
  if (fBuffers[i].memory == NULL) {
   b = &fBuffers[i];
   *handle = i + 1;
   fNextBuffer = i + 1;
   break;
  }
 }
 if (b == NULL || (b->address = redwood_gpu_address(size)) == 0) {
  IOLog("RedwoodGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
  return NULL;
 }
 for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
  IOByteCount length = 0;
  addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);
  mapped = phys != 0 && redwood_gpu_map(b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
 }
 redwood_gpu_mapped();
 if (!mapped) {
  redwood_gpu_unmap(b->address, size);
  redwood_gpu_address_free(b->address, size);
  IOLog("RedwoodGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
  return NULL;
 }
 b->size = size;
 b->flags = flags;
 b->seed = 0;
 b->pushed = 0;
 b->seen = false;
 b->fence = 0;
 b->surface = NULL;
 fBufferCount++;
 fBufferBytes += size;
 return b;
}
void
RedwoodGPUUserClient::dropKept(struct gpu_kept * k)
{
 if (k->memory == NULL) {
  return;
 }
 if (redwood_gpu_ready()) {
  redwood_gpu_unmap(k->address, k->size);
  redwood_gpu_address_free(k->address, k->size);
 }
 k->memory->release();
 k->memory = NULL;
 fKeptBytes -= k->size;
}
bool
RedwoodGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
 struct gpu_kept * place = NULL;
 if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
     (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
  return false;
 }
 for (;;) {
  struct gpu_kept * oldest = NULL;
  place = NULL;
  for (uint32_t i = 0; i < GPU_KEPT; i++) {
   if (fKept[i].memory == NULL) {
    place = place != NULL ? place : &fKept[i];
   } else if (oldest == NULL || fKept[i].since < oldest->since) {
    oldest = &fKept[i];
   }
  }
  if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
   break;
  }
  dropKept(oldest);
 }
 place->memory = memory;
 place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
 place->address = address;
 place->size = size;
 place->flags = flags;
 place->since = mach_absolute_time();
 fKeptBytes += size;
 return true;
}
IOReturn
RedwoodGPUUserClient::gemNew(struct msm_gem_new * request)
{
 uint64_t size = round_page_64(request->size), began = mach_absolute_time();
 struct gpu_buffer * b;
 IOBufferMemoryDescriptor * memory;
 if (size == 0 || size > GPU_BUFFER_MAX) {
  return kIOReturnBadArgument;
 }
 IOLockLock(gGPULock);
 struct gpu_kept * found = NULL;
 uint64_t old;
 nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
 for (uint32_t i = 0; i < GPU_KEPT; i++) {
  struct gpu_kept * k = &fKept[i];
  if (k->memory != NULL && began - k->since > old) {
   dropKept(k);
  }
  if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
   found = k;
  }
 }
 for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
  uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;
  if (fBuffers[h].memory == NULL) {
   b = &fBuffers[h];
   bzero(b, sizeof(*b));
   b->memory = found->memory;
   b->kernel = found->kernel;
   b->address = found->address;
   b->size = found->size;
   b->flags = found->flags;
   fNextBuffer = h + 1;
   fBufferCount++;
   fBufferBytes += size;
   request->handle = h + 1;
   found->memory = NULL;
   fKeptBytes -= size;
   gGPUStat.given_again++;
   IOLockUnlock(gGPULock);
   if (gGPUKeep == 2) {
    bzero(b->kernel, (size_t) b->size);
   }
   return kIOReturnSuccess;
  }
 }
 IOLockUnlock(gGPULock);
 memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
     (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
 if (memory == NULL) {
  IOLog("RedwoodGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
      fBufferBytes >> 20);
  return kIOReturnNoMemory;
 }
 if (gGPUCached) {
  redwood_gpu_to_memory(memory->getBytesNoCopy(), size);
 }
 IOLockLock(gGPULock);
 b = newBuffer(memory, 0, size, request->flags, &request->handle);
 if (b != NULL) {
  b->memory = memory;
  b->kernel = (uint8_t *) memory->getBytesNoCopy();
  gGPUStat.made++;
  gGPUStat.making += mach_absolute_time() - began;
 }
 IOLockUnlock(gGPULock);
 if (b == NULL) {
  memory->release();
  return kIOReturnNoMemory;
 }
 return kIOReturnSuccess;
}
IOReturn
RedwoodGPUUserClient::surfaceBuffer(struct redwood_surface * request)
{
 MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
 struct gpu_buffer * b = NULL;
 IOMemoryDescriptor * memory;
 uint64_t size, began = mach_absolute_time();
 if (s == NULL) {
  return kIOReturnNotFound;
 }
 if (s->client != NULL) {
  MiniIOSurfaceDrop(s);
  return kIOReturnUnsupported;
 }
 size = round_page_64(s->info.allocSize);
 memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
 if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
  OSSafeReleaseNULL(memory);
  MiniIOSurfaceDrop(s);
  return kIOReturnNoMemory;
 }
 request->size = size;
 request->offset = s->info.offset;
 request->rowBytes = s->info.bytesPerRow;
 request->width = s->info.width;
 request->height = s->info.height;
 request->pixelFormat = s->info.pixelFormat;
 request->planes = s->info.planeCount;
 if (request->handle == kRedwoodDescribe) {
  request->handle = 0;
  memory->release();
  MiniIOSurfaceDrop(s);
  return kIOReturnSuccess;
 }
 IOLockLock(gGPULock);
 for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
  if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
   b = &fBuffers[i];
   request->handle = i + 1;
   break;
  }
 }
 if (b != NULL) {
  memory->release();
  MiniIOSurfaceDrop(s);
 } else if ((b = newBuffer(s->memory, PAGE_SIZE, size, 0, &request->handle)) != NULL) {
  b->memory = memory;
  b->kernel = s->kbase + PAGE_SIZE;
  b->surface = s;
  gGPUStat.surfaces++;
 }
 gGPUStat.taking += mach_absolute_time() - began;
 IOLockUnlock(gGPULock);
 if (b == NULL) {
  memory->release();
  MiniIOSurfaceDrop(s);
  return kIOReturnNoMemory;
 }
 return kIOReturnSuccess;
}
IOReturn
RedwoodGPUUserClient::gemInfo(struct msm_gem_info * request)
{
 struct gpu_buffer * b;
 IOReturn ret = kIOReturnSuccess;
 IOLockLock(gGPULock);
 b = buffer(request->handle);
 if (b == NULL) {
  ret = kIOReturnBadArgument;
 } else if (request->info == MSM_INFO_GET_OFFSET) {
  request->value = (uint64_t) request->handle << 32;
 } else if (request->info == MSM_INFO_GET_IOVA) {
  request->value = b->address;
 } else if (request->info == MSM_INFO_GET_FLAGS) {
  request->value = b->flags;
 } else if (request->info != MSM_INFO_SET_NAME) {
  ret = kIOReturnUnsupported;
 }
 IOLockUnlock(gGPULock);
 return ret;
}
IOReturn
RedwoodGPUUserClient::submit(struct msm_gem_submit * request)
{
 struct msm_gem_submit_bo * bos = NULL;
 struct msm_gem_submit_cmd * cmds = NULL;
 uint64_t addresses[GPU_SUBMIT_CMDS];
 uint32_t words[GPU_SUBMIT_CMDS], count = 0;
 uint64_t began, nanos, again;
 IOReturn ret = kIOReturnBadArgument;
 if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
  IOLog("RedwoodGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
  return kIOReturnBadArgument;
 }
 bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
 cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
 if (bos == NULL || cmds == NULL) {
  ret = kIOReturnNoMemory;
  goto out;
 }
 if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
     copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
  ret = kIOReturnVMError;
  goto out;
 }
 IOLockLock(gGPULock);
 began = mach_absolute_time();
 nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
 for (uint32_t i = 0; i < request->nr_bos; i++) {
  struct gpu_buffer * b = buffer(bos[i].handle);
  if (b == NULL) {
   continue;
  }
  if (b->surface != NULL) {
   uint32_t seed = b->surface->shared()->seed;
   if (seed == b->seed && !b->seen && began - b->pushed < again) {
    continue;
   }
   b->seed = seed;
   b->pushed = began;
  } else if (!b->seen || !gGPUCached) {
   continue;
  }
  redwood_gpu_to_memory(b->kernel, b->size);
  gGPUStat.flushed += b->size;
 }
 redwood_gpu_to_memory(NULL, 0);
 gGPUStat.flushing += mach_absolute_time() - began;
 gGPUStat.buffers += request->nr_bos;
 for (uint32_t i = 0; i < request->nr_cmds; i++) {
  struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;
  if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
   continue;
  }
  if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
      (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size) {
   IOLog("RedwoodGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
       i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
   IOLockUnlock(gGPULock);
   goto out;
  }
  addresses[count] = b->address + cmds[i].submit_offset;
  words[count++] = cmds[i].size / 4;
 }
 request->fence = redwood_gpu_submit(addresses, words, count);
 if (gpu_log && request->fence != 0) {
  uint32_t big = 0, all_words = 0;
  uint64_t ns;
  for (uint32_t i = 0; i < request->nr_bos; i++) {
   struct gpu_buffer * b = buffer(bos[i].handle);
   big += b != NULL && b->size >= (4u << 20);
  }
  for (uint32_t i = 0; i < count; i++) {
   all_words += words[i];
  }
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  redwood_tl("tl: Q %u at %llu: %u buffers, %u big, %u words\n", request->fence, ns / 1000, request->nr_bos, big, all_words);
 }
 if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
  request->fence_fd = -1;
 }
 for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
  struct gpu_buffer * b = buffer(bos[i].handle);
  if (b != NULL) {
   b->fence = request->fence;
   if (b->surface != NULL) {
    b->surface->drawn = request->fence;
   }
  }
 }
 reap();
 if (request->fence == 0 && !gGPUReported) {
  gGPUReported = true;
  redwood_gpu_report("the ring is full");
 }
 if (fSubmits++ < 4) {
  IOLog("RedwoodGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
      addresses[0], words[0]);
 }
 gGPUStat.submissions++;
 gGPUStat.commands += count;
 if (gGPUStat.since == 0) {
  gGPUStat.since = began;
 }
 absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
 if (nanos >= 5 * NSEC_PER_SEC) {
  uint64_t flushing, waiting, taking, worked;
  uint32_t timed, by[7];
  absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
  absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
  absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
  redwood_gpu_worked(&worked, &timed, by);
  uint64_t making;
  absolutetime_to_nanoseconds(gGPUStat.making, &making);
  IOLog("RedwoodGPU: buffers: %u made in %llu us each, %u kept ones given again (%llu MB kept)\n", gGPUStat.made,
      gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again, fKeptBytes >> 20);
  IOLog("RedwoodGPU: %u submissions in %llu ms: %u command buffers and %u buffers named, %llu KB out of the cache "
      "in %llu us a submission; the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
      "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all); %u waits of %llu us, %u over before they "
      "began; %u IOSurfaces taken as buffers in %llu us each; %u buffers, %llu MB\n",
      gGPUStat.submissions, nanos / NSEC_PER_MSEC, gGPUStat.commands, gGPUStat.buffers,
      (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions,
      timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000,
      gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
      gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
      fBufferBytes >> 20);
  bzero(&gGPUStat, sizeof(gGPUStat));
 }
 IOLockUnlock(gGPULock);
 ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
 if (bos != NULL) {
  IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
 }
 if (cmds != NULL) {
  IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
 }
 return ret;
}
IOReturn
RedwoodGPUUserClient::show(const struct redwood_show * request)
{
 uint64_t size = (uint64_t) request->rowBytes * request->height;
 uint8_t * pixels;
 IOReturn ret = kIOReturnSuccess;
 if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
     request->rowBytes < 4 * request->width) {
  return kIOReturnBadArgument;
 }
 pixels = (uint8_t *) IOMallocData(size);
 if (pixels == NULL) {
  return kIOReturnNoMemory;
 }
 if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
  ret = kIOReturnVMError;
 } else {
  MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
 }
 IOFreeData(pixels, size);
 return ret;
}
IOReturn
RedwoodGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
 struct gpu_buffer * b;
 IOReturn ret = kIOReturnBadArgument;
 IOLockLock(gGPULock);
 b = buffer(type);
 if (b != NULL) {
  b->memory->retain();
  b->seen = true;
  *memory = b->memory;
  *options = 0;
  ret = kIOReturnSuccess;
 }
 IOLockUnlock(gGPULock);
 return ret;
}
IOReturn
RedwoodGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
 union {
  struct msm_param param;
  struct msm_gem_new gemNew;
  struct msm_gem_info gemInfo;
  struct msm_gem_cpu_prep cpuPrep;
  struct msm_gem_submit submit;
  struct msm_wait_fence waitFence;
  struct msm_gem_madvise madvise;
  struct msm_submitqueue queue;
  struct msm_submitqueue_query query;
  struct drm_gem_close close;
  struct redwood_surface surface;
  struct redwood_show show;
 } r;
 size_t size = args->structureInputSize;
 struct gpu_buffer * b;
 IOReturn ret = kIOReturnSuccess;
 uint64_t zero = 0, patience, waitBegan;
 if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
  return kIOReturnBadArgument;
 }
 if (!redwood_gpu_ready() && selector != kRedwoodShow) {
  return kIOReturnNotReady;
 }
 bzero(&r, sizeof(r));
 memcpy(&r, args->structureInput, size);
 switch (selector) {
 case kMsmGetParam:
  ret = redwood_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
  break;
 case kMsmSetParam:
 case kMsmGemCpuFini:
 case kMsmSubmitQueueClose:
  break;
 case kMsmGemNew:
  ret = gemNew(&r.gemNew);
  break;
 case kMsmGemInfo:
  ret = gemInfo(&r.gemInfo);
  break;
 case kMsmGemCpuPrep:
  waitBegan = mach_absolute_time();
  IOLockLock(gGPULock);
  patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
  if (!gpuWait(redwood_gpu_last(), patience)) {
   ret = kIOReturnBusy;
   if (patience == 0 && !(r.cpuPrep.op & MSM_PREP_NOSYNC)) {
    waitOver(&r.cpuPrep.timeout, redwood_gpu_last());
   }
  }
  IOLockUnlock(gGPULock);
  slowWait("cpu-prep", waitBegan);
  break;
 case kMsmGemSubmit:
  ret = submit(&r.submit);
  break;
 case kMsmWaitFence:
  waitBegan = mach_absolute_time();
  IOLockLock(gGPULock);
  patience = millisecondsUntil(&r.waitFence.timeout);
  if (!gpuWait(r.waitFence.fence, patience)) {
   ret = kIOReturnTimeout;
   if (patience == 0) {
    waitOver(&r.waitFence.timeout, r.waitFence.fence);
   }
  }
  IOLockUnlock(gGPULock);
  slowWait("fence", waitBegan);
  break;
 case kMsmGemMadvise:
  r.madvise.retained = 1;
  break;
 case kMsmSubmitQueueNew:
  r.queue.id = ++fNextQueue;
  break;
 case kMsmSubmitQueueQuery:
  if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
   copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
  }
  break;
 case kDrmGemClose:
  IOLockLock(gGPULock);
  b = buffer(r.close.handle);
  if (b == NULL) {
   ret = kIOReturnBadArgument;
  } else {
   freeBuffer(b);
   reap();
  }
  IOLockUnlock(gGPULock);
  break;
 case kRedwoodSurface:
  ret = surfaceBuffer(&r.surface);
  break;
 case kRedwoodShow:
  ret = show(&r.show);
  break;
 default:
  IOLog("RedwoodGPU: request %#x (%zu bytes) is not known here\n", selector, size);
  ret = kIOReturnUnsupported;
  break;
 }
 if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
  if (args->structureOutputSize > size) {
   args->structureOutputSize = (uint32_t) size;
  }
  memcpy(args->structureOutput, &r, args->structureOutputSize);
 }
 return ret;
}
