#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>
extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;
}
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "MiniIOSurface.h"
extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);
enum {
 kFBMethodGetLayerDefaultSurface = 3,
 kFBMethodSwapBegin = 4,
 kFBMethodSwapEnd = 5,
 kFBMethodSwapWait = 6,
 kFBMethodGetID = 7,
 kFBMethodGetDisplaySize = 8,
 kFBMethodRequestPowerChange = 12,
 kFBMethodSwapSignal = 20,
 kFBMethodGetDigitalOutState = 25,
 kFBMethodGetDotPitch = 28,
 kFBMethodGetDisplayArea = 29,
 kFBMethodSwapCancel = 52,
 kFBMethodGetColorRemapMode = 57,
 kFBMethodNotifications = 72,
 kFBMethodChangeFrameInfo = 73,
 kFBMethodSupportedFrameInfo = 74,
 kFBMethodGetCurrentAbsoluteTime = 77,
 kFBMethodGetBlock = 79,
 kFBMethodSwapCancelAll = 81,
 kFBMethodGetProtectionOptions = 82,
};
#define SWAP_SIZE 0x46c
#define SWAP_ID 0x50
#define SWAP_SURFACE(layer) (0x54 + 4 * (layer))
#define SWAP_SOURCE(layer) (0x64 + 16 * (layer))
#define SWAP_ENABLED(layer) (0xa4 + 4 * (layer))
#define SWAP_DEST(layer) (0xc4 + 16 * (layer))
#define SWAP_LAYERS_SET 0x104
#define SWAP_LAYERS_CLEARED 0x108
#define SWAP_LAYERS 4
#define kFBNotifyVSync 5
#define kFBNotifyTypes 8
#define kFBRefreshHz 60
#define FRAME_TIMES 64
#define FRAME_PAUSE 90
#define SWAP_GPU_MS 500
#define SWAP_HELD_MS 700
#define FOURCC(a,b,c,d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
static uint64_t gFBLogged[2];
static uint8_t gFBCalls[128];
#define kFBDefaultWidth 1125
#define kFBDefaultHeight 2436
#define kFBDotsPerInch 458
#define kFBMacanWidth 1290
#define kFBMacanHeight 2796
#define kFBMacanSubtype 2796
#define kFBMacanDotsPerInch 460
static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
#define kFBPointsWide 375
#define kFBPointsHigh 812
#define kFBDefaultScale 3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
#define SWAP_QUEUE 2
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
 return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);
static inline uint64_t
micros(uint64_t absolute)
{
 uint64_t ns;
 absolutetime_to_nanoseconds(absolute, &ns);
 return ns / 1000;
}
static TUNABLE(uint32_t, gMacan, "macan", 0);
static TUNABLE(uint32_t, gMacan14, "macan_14pm", 0);
static inline uint32_t
refreshHz(void)
{
 if (gRefreshHz == kFBRefreshHz && gMacan != 0) {
  return 120;
 }
 return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static void
macanButtonsNode(void)
{
 IORegistryEntry * root = IORegistryEntry::fromPath("/", gIODTPlane);
 IORegistryEntry * buttons = IORegistryEntry::fromPath("/buttons", gIODTPlane);
 uint32_t type = 2;
 if (buttons == NULL && root != NULL) {
  buttons = new IORegistryEntry;
  if (buttons != NULL && (!buttons->init() || !buttons->attachToParent(root, gIODTPlane))) {
   buttons->release();
   buttons = NULL;
  } else if (buttons != NULL) {
   buttons->setName("buttons", gIODTPlane);
   buttons->setName("buttons");
   buttons->retain();
  }
 }
 if (buttons != NULL) {
  buttons->setProperty("home-button-type", &type, sizeof(type));
  buttons->release();
 }
 if (root != NULL) {
  root->release();
 }
}
static TUNABLE(uint32_t, gMacanHaptic, "haptic", 5);
static TUNABLE(uint32_t, gMacanTorch, "torch", 3);
static IORegistryEntry *
macanProductChild(IORegistryEntry *product, const char *name)
{
 char path[80];
 snprintf(path, sizeof(path), "/product/%s", name);
 IORegistryEntry *child = IORegistryEntry::fromPath(path, gIODTPlane);
 if (child != NULL) {
  return child;
 }
 child = new IORegistryEntry;
 if (child != NULL && (!child->init() || !child->attachToParent(product, gIODTPlane))) {
  child->release();
  return NULL;
 }
 if (child != NULL) {
  child->setName(name, gIODTPlane);
  child->setName(name);
 }
 return child;
}
static void
macanPeripheralNodes(void)
{
 IORegistryEntry *product = IORegistryEntry::fromPath("/product", gIODTPlane);
 uint32_t yes = 1;
 if (product == NULL) {
  return;
 }
 if (gMacanHaptic != 0) {
  IORegistryEntry *haptics = macanProductChild(product, "haptics");
  if (haptics != NULL) {
   haptics->setProperty("closed-loop", &yes, sizeof(yes));
   haptics->setProperty("supports-3rd-party-haptics", &yes, sizeof(yes));
   haptics->release();
  }
 }
 if (gMacanTorch != 0) {
  IORegistryEntry *camera = macanProductChild(product, "camera");
  if (camera != NULL) {
   camera->setProperty("flash", &yes, sizeof(yes));
   camera->release();
  }
 }
 product->release();
}
extern "C" bool redwood_gpu_ready(void);
extern "C" void redwood_gpu_to_memory(void *start, uint64_t size);
static inline uint32_t
recStep(void)
{
 return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP recStep()
struct rec_header {
 char magic[8];
 uint32_t width, height;
 uint32_t frames;
 uint32_t oldest;
 uint64_t began;
};
struct rec_frame {
 uint64_t nanoseconds;
};
extern "C" uint64_t loader_recording[3];
extern "C" void loader_log_putc(char);
static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
 uint32_t value;
 memcpy(&value, swap + offset, sizeof(value));
 return value;
}
struct RowJob;
class IOMobileFramebuffer : public IOService
{
 OSDeclareDefaultStructors(IOMobileFramebuffer);
public:
 IOLock * fLock;
 uint8_t * fBase;
 uint32_t fPanelWidth, fPanelHeight, fRowBytes;
 uint32_t fWidth, fHeight;
 MiniIOSurface * fDefaultSurface;
 uint32_t fSwapID;
 uint32_t fSwapsShown;
 uint32_t fLayersShown;
 bool fGraphics;
 IOLock * fSwapLock;
 uint8_t fSwap[SWAP_QUEUE][SWAP_SIZE];
 MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS];
 unsigned fSwapFirst, fSwapsWaiting;
 uint64_t fBeat;
 uint64_t fReady;
 uint32_t fStatOnBeat;
 uint64_t fStatBeatWait;
 uint32_t fStatSurfaces[8];
 uint32_t fStatHeld, fStatLost;
 uint64_t fSwapTime;
 uint32_t fStatSwapsLate;
 uint64_t fStatWaiting;
 uint32_t fStatFrames;
 uint64_t fStatSince, fStatDrawing;
 uint64_t fLastFrame;
 uint32_t fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
 bool fDiff;
 uint64_t fDiffPixels;
 uint32_t fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
 uint32_t * fColumn;
 uint16_t * fColumnWeight;
 uint32_t * fBlendRow;
 uint32_t fBlendRowSize;
 uint8_t * fRecBase;
 uint64_t fRecSize, fRecUsed;
 uint64_t fFirstFrame, fRecFirst;
 bool fRecDone;
 bool fRecWrapped;
 virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
 void publishModes(void);
 MiniIOSurface * defaultSurface(void);
 void present(const uint8_t * swap);
 void show(const uint8_t * swap, MiniIOSurface ** surfaces);
 void presenter(void);
 static void presenterMain(void * self, wait_result_t result);
 void reportFrameTimes(void);
 void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
 void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
 static void helperMain(void * self, wait_result_t result);
 unsigned fThreads;
 IOLock * fHelperLock;
 const struct RowJob * fHelperJob;
 uint64_t fHelperGeneration;
 unsigned fHelpersBusy, fHelpersStarted;
 uint32_t * fHelperBlend[7];
 void record(MiniIOSurface * s);
};
OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);
static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
 OSNumber * n = OSNumber::withNumber(value, 32);
 if (n != NULL) {
  dict->setObject(key, n);
  n->release();
 }
}
#define kFBTimingID 1
#define kFBColorID 1
void
IOMobileFramebuffer::publishModes(void)
{
 OSDictionary * timing = OSDictionary::withCapacity(8);
 OSDictionary * horizontal = OSDictionary::withCapacity(4);
 OSDictionary * vertical = OSDictionary::withCapacity(4);
 OSDictionary * color = OSDictionary::withCapacity(8);
 OSArray * timings = OSArray::withCapacity(1);
 OSArray * colors = OSArray::withCapacity(1);
 const uint32_t rate = refreshHz() << 16;
 if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
  return;
 }
 setNumber(color, "ID", kFBColorID);
 setNumber(color, "PixelEncoding", 0);
 setNumber(color, "Depth", 8);
 setNumber(color, "Colorimetry", 1);
 setNumber(color, "EOTF", 1);
 setNumber(color, "DynamicRange", 0);
 setNumber(color, "Score", 1);
 color->setObject("IsVirtual", kOSBooleanFalse);
 colors->setObject(color);
 setNumber(horizontal, "Active", fWidth);
 setNumber(horizontal, "Total", fWidth);
 setNumber(horizontal, "SyncRate", rate);
 setNumber(horizontal, "PreciseSyncRate", rate);
 setNumber(vertical, "Active", fHeight);
 setNumber(vertical, "Total", fHeight);
 setNumber(vertical, "SyncRate", rate);
 setNumber(vertical, "PreciseSyncRate", rate);
 setNumber(timing, "ID", kFBTimingID);
 setNumber(timing, "Score", 1);
 timing->setObject("IsVirtual", kOSBooleanFalse);
 timing->setObject("IsOverscanned", kOSBooleanFalse);
 timing->setObject("HorizontalAttributes", horizontal);
 timing->setObject("VerticalAttributes", vertical);
 timing->setObject("ColorModes", colors);
 timings->setObject(timing);
 setProperty("TimingElements", timings);
 setProperty("ColorElements", colors);
 OSDictionary * refresh = OSDictionary::withCapacity(2);
 if (refresh != NULL) {
  OSNumber * interval = OSNumber::withNumber((1ULL << 32) / refreshHz(), 64);
  OSNumber * step = OSNumber::withNumber(0ULL, 64);
  if (interval != NULL && step != NULL) {
   refresh->setObject("displayMinRefreshInterval", interval);
   refresh->setObject("displayRefreshStepMachTime", step);
   setProperty("IOMFBDisplayRefresh", refresh);
  }
  OSSafeReleaseNULL(interval);
  OSSafeReleaseNULL(step);
  OSSafeReleaseNULL(refresh);
 }
 OSSafeReleaseNULL(timings);
 OSSafeReleaseNULL(colors);
 OSSafeReleaseNULL(timing);
 OSSafeReleaseNULL(horizontal);
 OSSafeReleaseNULL(vertical);
 OSSafeReleaseNULL(color);
}
bool
IOMobileFramebuffer::start(IOService * provider)
{
 thread_t thread;
 if (!IOService::start(provider)) {
  return false;
 }
 if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
  IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
      vinfo.v_depth);
  return false;
 }
 fLock = IOLockAlloc();
 fSwapLock = IOLockAlloc();
 if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
  IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
  return false;
 }
 thread_set_thread_name(thread, "frames to the panel");
 thread_deallocate(thread);
 fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
 if (gMacan != 0 && gThreads == 3) {
  fThreads = 6;
 }
 fHelperLock = IOLockAlloc();
 for (unsigned i = 0; i + 1 < fThreads; i++) {
  if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
   fThreads = i + 1;
   break;
  }
  thread_set_thread_name(thread, "frames to the panel: a helper");
  thread_deallocate(thread);
 }
 fBase = (uint8_t *) vinfo.v_baseaddr;
 fPanelWidth = vinfo.v_width;
 fPanelHeight = vinfo.v_height;
 fRowBytes = vinfo.v_rowbytes;
 fWidth = gFBWidth ? gFBWidth : fPanelWidth;
 fHeight = gFBHeight ? gFBHeight : fPanelHeight;
 if (gMacan != 0) {
  macanPeripheralNodes();
 }
 if (gMacan != 0 && gMacan14 != 0 && gFBWidth == kFBDefaultWidth && gFBHeight == kFBDefaultHeight &&
     (gFBScale == 0 || gFBScale == kFBDefaultScale)) {
  uint32_t subtype = kFBMacanSubtype;
  IORegistryEntry * product = IORegistryEntry::fromPath("/product", gIODTPlane);
  fWidth = kFBMacanWidth;
  fHeight = kFBMacanHeight;
  if (product != NULL) {
   product->setProperty("artwork-device-subtype", &subtype, sizeof(subtype));
   product->release();
  }
  macanButtonsNode();
 }
 if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
  static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
  uint32_t scale = gFBScale;
  fWidth = kFBPointsWide * scale;
  fHeight = kFBPointsHigh * scale;
  for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
   IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);
   if (node != NULL) {
    node->setProperty(places[i][1], &scale, sizeof(scale));
    node->release();
   }
  }
 }
 fColumn = IONewZero(uint32_t, fPanelWidth);
 fColumnWeight = IONewZero(uint16_t, fPanelWidth);
 fDiff = gDiff != 0;
 fDiffLeft = fDiffTop = ~0u;
 IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
     fPanelWidth, fPanelHeight, fRowBytes);
 if (gRecSeconds != 0) {
  IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
      kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);
  if (memory != NULL) {
   struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();
   fRecBase = (uint8_t *) header;
   fRecSize = (uint64_t) gRecMegabytes << 20;
   fRecUsed = sizeof(*header);
   bzero(header, sizeof(*header));
   memcpy(header->magic, "RWREC003", 8);
   header->width = fWidth / REC_STEP;
   header->height = fHeight / REC_STEP;
   loader_recording[0] = memory->getPhysicalSegment(0, NULL);
   loader_recording[1] = fRecUsed;
   loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
   IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
       gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
       gRecRing ? ", the last of it kept (rec_ring)" : "");
  } else {
   IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
  }
 }
 setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
 setProperty("RedwoodWaitsForGPU", kOSBooleanTrue);
 publishModes();
 setName("disp0");
 registerService();
 return true;
}
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
 struct rec_header * header = (struct rec_header *) fRecBase;
 uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
 struct rec_frame * frame;
 uint8_t * to;
 if (fFirstFrame == 0) {
  fFirstFrame = now;
 }
 if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
  return;
 }
 absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
 if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
  return;
 }
 if (fRecFirst == 0) {
  uint64_t began;
  fRecFirst = now;
  absolutetime_to_nanoseconds(now, &began);
  header->began = began;
  IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
      began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
 }
 absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
 frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
 if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
  fRecDone = true;
  IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
  return;
 }
 if (fRecUsed + frame_size > fRecSize) {
  fRecUsed = sizeof(*header);
  fRecWrapped = true;
 }
 frame = (struct rec_frame *) (fRecBase + fRecUsed);
 frame->nanoseconds = since_start;
 to = (uint8_t *) (frame + 1);
 for (uint32_t y = 0; y < header->height; y++) {
  const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);
  for (uint32_t x = 0; x < header->width; x++) {
   uint32_t pixel = from[x * REC_STEP + 1];
   *to++ = (uint8_t) (pixel >> 16);
   *to++ = (uint8_t) (pixel >> 8);
   *to++ = (uint8_t) pixel;
  }
 }
 fRecUsed += frame_size;
 if (!fRecWrapped) {
  header->frames++;
  loader_recording[1] = fRecUsed;
 } else {
  header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
  if (header->oldest >= header->frames) {
   header->oldest = 0;
  }
  loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
 }
}
MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
 IOLockLock(fLock);
 if (fDefaultSurface == NULL) {
  fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
 }
 IOLockUnlock(fLock);
 return fDefaultSurface;
}
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
 uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
 uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;
 return rb | ga;
}
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
 int64_t at = ((2 * i + 1) * in - out) * 128 / out;
 return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}
struct RowJob {
 const uint8_t * pixels;
 uint32_t stride;
 int32_t sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
 bool filter, unscaled;
};
void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
 const uint8_t * pixels = job->pixels;
 uint32_t stride = job->stride;
 int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
 int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
 bool filter = job->filter;
 for (int32_t y = y0; y < y1; y++) {
  const uint32_t * from;
  uint32_t * to;
  if (y < 0 || (uint32_t) y >= fPanelHeight) {
   continue;
  }
  to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
  if (job->unscaled) {
   memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
   continue;
  }
  if (filter) {
   int64_t at = sample_at(y - py0, sh, py1 - py0);
   uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
   const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;
   if (weight == 0 || row + 1 >= (uint32_t) sh) {
    from = upper;
   } else {
    const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
    uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];
    uint32_t * __restrict mixed = blendRow;
    for (uint32_t c = first; c <= last; c++) {
     uint32_t a = upper[c], b = lower[c];
     mixed[c] = a == b ? a : blend(a, b, weight);
    }
    from = blendRow;
   }
  } else {
   from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
  }
  if (fDiff) {
   for (int32_t x = cx0; x < cx1; x++) {
    uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
        from[fColumn[x]];
    if (to[x] != value) {
     fDiffPixels++;
     fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
     fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
     fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
     fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
    }
   }
  }
  if (filter) {
   const uint32_t * __restrict column = fColumn + cx0;
   const uint16_t * __restrict weights = fColumnWeight + cx0;
   uint32_t * __restrict out = to + cx0;
   for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
    uint32_t c = column[i], w = weights[i], a = from[c];
    out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
   }
  } else {
   for (int32_t x = cx0; x < cx1; x++) {
    to[x] = from[fColumn[x]];
   }
  }
 }
}
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
 IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
 unsigned index;
 uint64_t done = 0;
 IOLockLock(fb->fHelperLock);
 index = fb->fHelpersStarted++;
 IOLockUnlock(fb->fHelperLock);
 for (;;) {
  const RowJob * job;
  int32_t rows, y0, y1;
  uint32_t * blend;
  IOLockLock(fb->fHelperLock);
  while (fb->fHelperGeneration == done) {
   IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
  }
  done = fb->fHelperGeneration;
  job = fb->fHelperJob;
  IOLockUnlock(fb->fHelperLock);
  rows = job->py1 - job->py0;
  y0 = job->py0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
  y1 = job->py0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
  blend = fb->fHelperBlend[index];
  fb->drawRows(job, y0, y1, blend);
  IOLockLock(fb->fHelperLock);
  fb->fHelpersBusy--;
  if (fb->fHelpersBusy == 0) {
   IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
  }
  IOLockUnlock(fb->fHelperLock);
 }
}
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
 int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
 int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
 int32_t px0, py0, px1, py1, cx0, cx1;
 bool filter = gFilter != 0;
 RowJob job;
 if (sw <= 0 || sh <= 0) {
  sx = sy = 0;
  sw = (int32_t) s->info.width;
  sh = (int32_t) s->info.height;
 }
 if (dw <= 0 || dh <= 0) {
  dx = dy = 0;
  dw = (int32_t) fWidth;
  dh = (int32_t) fHeight;
 }
 if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
  return;
 }
 px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
 py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
 px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
 py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
 if (px1 <= px0 || py1 <= py0) {
  return;
 }
 if (filter && fBlendRowSize < (uint32_t) sw) {
  if (fBlendRow != NULL) {
   IODelete(fBlendRow, uint32_t, fBlendRowSize);
  }
  for (unsigned i = 0; i + 1 < fThreads; i++) {
   if (fHelperBlend[i] != NULL) {
    IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
   }
  }
  fBlendRowSize = (uint32_t) sw;
  fBlendRow = IONew(uint32_t, fBlendRowSize);
  for (unsigned i = 0; i + 1 < fThreads; i++) {
   fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
   if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
    IODelete(fBlendRow, uint32_t, fBlendRowSize);
    fBlendRow = NULL;
   }
  }
  if (fBlendRow == NULL) {
   fBlendRowSize = 0;
  }
 }
 filter = filter && fBlendRow != NULL;
 cx0 = px0 < 0 ? 0 : px0;
 cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
 for (int32_t x = cx0; x < cx1; x++) {
  if (filter) {
   int64_t at = sample_at(x - px0, sw, px1 - px0);
   fColumn[x] = (uint32_t) (at >> 8);
   fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
  } else {
   fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
  }
 }
 job.pixels = s->pixels();
 job.stride = s->info.bytesPerRow;
 job.sx = sx;
 job.sy = sy;
 job.sw = sw;
 job.sh = sh;
 job.px0 = px0;
 job.py0 = py0;
 job.px1 = px1;
 job.py1 = py1;
 job.cx0 = cx0;
 job.cx1 = cx1;
 job.filter = filter;
 job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
 if (redwood_gpu_ready()) {
  redwood_gpu_to_memory((void *) (job.pixels + (size_t) sy * job.stride), (uint64_t) sh * job.stride);
 }
 if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
  drawRows(&job, py0, py1, fBlendRow);
  return;
 }
 IOLockLock(fHelperLock);
 fHelperJob = &job;
 fHelpersBusy = fThreads - 1;
 fHelperGeneration++;
 IOLockWakeup(fHelperLock, &fHelperGeneration, false);
 IOLockUnlock(fHelperLock);
 drawRows(&job, py0, py0 + (int32_t) ((int64_t) (py1 - py0) / fThreads), fBlendRow);
 IOLockLock(fHelperLock);
 while (fHelpersBusy != 0) {
  IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
 }
 IOLockUnlock(fHelperLock);
}
void
IOMobileFramebuffer::reportFrameTimes(void)
{
 static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
 uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;
 for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
  for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
   if (ms < limits[i]) {
    within[i] += fFrameTimes[ms];
    break;
   }
  }
  total += fFrameTimes[ms];
 }
 for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
  sum += fFrameTimes[ms];
  if (half == 0 && 2 * sum >= total) {
   half = ms + 1;
  }
  if (most == 0 && 100 * sum >= 95 * total) {
   most = ms + 1;
  }
 }
 if (total != 0) {
  IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
      "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
      within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
 }
 bzero(fFrameTimes, sizeof(fFrameTimes));
 fFramePauses = fFrameLongest = 0;
}
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
 AbsoluteTime deadline;
 uint64_t now = mach_absolute_time(), period, pause;
 nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
 nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
 clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
 if (gTouchLat) {
  redwood_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
 }
 IOLockLock(fSwapLock);
 fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
 fSwapTime = now;
 fStatHeld += fSwapsWaiting >= swapQueue();
 while (fSwapsWaiting >= swapQueue()) {
  if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
      fSwapsWaiting >= swapQueue()) {
   fStatLost++;
   IOLockUnlock(fSwapLock);
   return;
  }
 }
 unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;
 memcpy(fSwap[slot], swap, SWAP_SIZE);
 for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
  uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
  fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
 }
 fSwapsWaiting++;
 IOLockWakeup(fSwapLock, fSwap, true);
 IOLockUnlock(fSwapLock);
}
void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
 ((IOMobileFramebuffer *) self)->presenter();
}
void
IOMobileFramebuffer::presenter(void)
{
 static uint8_t swap[SWAP_SIZE];
 MiniIOSurface * surfaces[SWAP_LAYERS];
 for (;;) {
  IOLockLock(fSwapLock);
  while (fSwapsWaiting == 0) {
   IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
  }
  memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
  memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
  IOLockUnlock(fSwapLock);
  show(swap, surfaces);
  for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
   if (surfaces[layer] != NULL) {
    MiniIOSurfaceDrop(surfaces[layer]);
   }
  }
  MiniIOSurfaceClearKept();
 }
}
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
 uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
 uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
 uint32_t layers = 0;
 MiniIOSurface * recorded = NULL;
 bool report;
 for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
  if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
   IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
       swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
  }
 }
 fStatWaiting += mach_absolute_time() - began;
 began = ready = mach_absolute_time();
 if (gOnTheBeat) {
  uint64_t period, aim, next;
  nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
  aim = period * 3 / 8;
  next = fReady;
  fReady = began;
  if (fBeat == 0 || began - next > period * 3 / 2) {
   fBeat = began;
  } else {
   for (next = fBeat + period; next < began; next += period) {
   }
   if (next - began > aim) {
    next -= (next - began - aim) / 8;
   } else {
    next += (aim - (next - began)) / 8;
   }
   clock_delay_until(next);
   fBeat = next;
   fStatOnBeat++;
   fStatBeatWait += mach_absolute_time() - began;
   began = mach_absolute_time();
  }
 }
 shown = began;
 IOLockLock(fSwapLock);
 fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
 fSwapsWaiting--;
 IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
 IOLockUnlock(fSwapLock);
 IOLockLock(fLock);
 if (fLastFrame != 0) {
  absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
  nanos /= NSEC_PER_MSEC;
  if (nanos >= FRAME_PAUSE) {
   fFramePauses++;
  } else {
   fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
   fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
  }
 }
 fLastFrame = began;
 report = fSwapsShown < 8;
 if (report) {
  IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
      swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
      swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
 }
 for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
  uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
  int32_t src[4], dst[4];
  MiniIOSurface * s;
  layers |= sid != 0 ? 1u << layer : 0;
  for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
   if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
    fStatSurfaces[i] = sid;
    break;
   }
  }
  if (sid == 0) {
   if (layer == 0 && (set & 1) && fGraphics) {
    for (uint32_t y = 0; y < fPanelHeight; y++) {
     bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
    }
   }
   continue;
  }
  memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
  memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
  s = surfaces[layer];
  if (report) {
   IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
       "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
       s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
       s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
       s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
       src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
  }
  if (s == NULL) {
   continue;
  }
  if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
   if (!fGraphics) {
    fGraphics = true;
    PE_initialize_console(NULL, kPEGraphicsMode);
   }
   drawLayer(s, src, dst);
   if (layer == 0) {
    recorded = s;
   }
  }
 }
 drawn = mach_absolute_time();
 if (recorded != NULL) {
  record(recorded);
 }
 fSwapsShown++;
 if (gTouchLat) {
  redwood_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
      micros(drawn));
 }
 if (layers != fLayersShown) {
  IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
      fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
      swapWord(swap, SWAP_SURFACE(3)));
  fLayersShown = layers;
 }
 fStatFrames++;
 fStatDrawing += mach_absolute_time() - began;
 if (fStatSince == 0) {
  fStatSince = began;
 }
 absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
 if (nanos >= 5 * NSEC_PER_SEC) {
  uint64_t drawing, waiting, beat;
  unsigned different;
  absolutetime_to_nanoseconds(fStatDrawing, &drawing);
  absolutetime_to_nanoseconds(fStatWaiting, &waiting);
  absolutetime_to_nanoseconds(fStatBeatWait, &beat);
  IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
      "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
      "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
      fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
      waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
      fStatHeld, fStatLost, fStatSwapsLate);
  {
   char line[96];
   snprintf(line, sizeof(line), "fps: %llu (%u frames in %llu ms), %llu us each in the driver\n",
       fStatFrames * NSEC_PER_SEC / nanos, fStatFrames, nanos / NSEC_PER_MSEC, drawing / fStatFrames / NSEC_PER_USEC);
   for (char *c = line; *c; c++) {
    loader_log_putc(*c);
   }
  }
  fStatSwapsLate = 0;
  for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
  }
  if (different == 8 && fStatFrames > 8) {
   uint32_t made, freed, again;
   uint64_t making, freeing;
   MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
   IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
       "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
       "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
       fStatSurfaces[4], made, making, again, freed, freeing);
  }
  bzero(fStatSurfaces, sizeof(fStatSurfaces));
  fStatHeld = fStatLost = fStatOnBeat = 0;
  fStatWaiting = fStatBeatWait = 0;
  reportFrameTimes();
  if (fDiff) {
   IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
       fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
   fDiffPixels = 0;
   fDiffLeft = fDiffTop = ~0u;
   fDiffRight = fDiffBottom = 0;
  }
  fStatFrames = 0;
  fStatDrawing = 0;
  fStatSince = 0;
 }
 IOLockUnlock(fLock);
}
class IOMobileFramebufferUserClient : public IOUserClient
{
 OSDeclareDefaultStructors(IOMobileFramebufferUserClient);
 task_t fTask;
 IOMobileFramebuffer * fOwner;
 IOLock * fLock;
 mach_port_t fPorts[kFBNotifyTypes];
 mach_vm_address_t fCallbacks[kFBNotifyTypes];
 io_user_reference_t fRefcons[kFBNotifyTypes];
 thread_call_t fVSyncCall;
 uint64_t fVSyncCount;
 uint64_t fVSyncTime;
 bool fVSyncRunning;
 bool fClosed;
 static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
 void vsync(void);
 void armVSync(void);
public:
 virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
 virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
 virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
 virtual void free(void) APPLE_KEXT_OVERRIDE;
 virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
 virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
     IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);
bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
 if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
  return false;
 }
 fTask = owningTask;
 return true;
}
bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
 fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
 if (fOwner == NULL || !IOUserClient::start(provider)) {
  return false;
 }
 fLock = IOLockAlloc();
 fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
 return fLock != NULL && fVSyncCall != NULL;
}
IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
 if (fLock != NULL) {
  IOLockLock(fLock);
  fClosed = true;
  IOLockUnlock(fLock);
 }
 if (fVSyncCall != NULL) {
  thread_call_cancel_wait(fVSyncCall);
 }
 if (!isInactive()) {
  terminate();
 }
 return kIOReturnSuccess;
}
void
IOMobileFramebufferUserClient::free(void)
{
 if (fVSyncCall != NULL) {
  thread_call_cancel_wait(fVSyncCall);
  thread_call_free(fVSyncCall);
  fVSyncCall = NULL;
 }
 for (unsigned i = 0; i < kFBNotifyTypes; i++) {
  if (fPorts[i] != NULL) {
   releaseNotificationPort(fPorts[i]);
   fPorts[i] = NULL;
  }
 }
 if (fLock != NULL) {
  IOLockFree(fLock);
  fLock = NULL;
 }
 IOUserClient::free();
}
IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
 mach_port_t old;
 IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
 if (type >= kFBNotifyTypes) {
  return kIOReturnBadArgument;
 }
 IOLockLock(fLock);
 old = fPorts[type];
 fPorts[type] = port;
 IOLockUnlock(fLock);
 if (old != NULL) {
  releaseNotificationPort(old);
 }
 return kIOReturnSuccess;
}
void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
 ((IOMobileFramebufferUserClient *) self)->vsync();
}
void
IOMobileFramebufferUserClient::armVSync(void)
{
 uint64_t period, now = mach_absolute_time();
 nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
 fVSyncTime += period;
 if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
  fVSyncTime = now + period;
 }
 thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}
void
IOMobileFramebufferUserClient::vsync(void)
{
 OSAsyncReference64 ref;
 io_user_reference_t args[6];
 uint64_t now;
 bool send = false;
 IOLockLock(fLock);
 now = fVSyncTime;
 if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
  fVSyncRunning = false;
 } else {
  fVSyncCount++;
  setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
  args[0] = (uint32_t) now;
  args[1] = now >> 32;
  args[2] = (uint32_t) now;
  args[3] = now >> 32;
  args[4] = (uint32_t) fVSyncCount;
  args[5] = fVSyncCount >> 32;
  send = true;
  armVSync();
 }
 IOLockUnlock(fLock);
 if (send) {
  sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
 }
}
IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
 const uint64_t * in = args->scalarInput;
 uint64_t * out = args->scalarOutput;
 MiniIOSurface * s;
 if (selector < 128 && gFBCalls[selector] < gFBTrace) {
  char procname[32] = "?";
  gFBCalls[selector]++;
  proc_selfname(procname, sizeof(procname));
  IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
      "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
      args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
      args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
      args->structureInputSize, args->structureOutputSize);
 }
 switch (selector) {
 case kFBMethodGetDisplaySize:
  if (args->scalarOutputCount < 2) {
   return kIOReturnBadArgument;
  }
  out[0] = fOwner->fWidth;
  out[1] = fOwner->fHeight;
  args->scalarOutputCount = 2;
  return kIOReturnSuccess;
 case kFBMethodGetID:
  if (args->scalarOutputCount < 1) {
   return kIOReturnBadArgument;
  }
  out[0] = 1;
  args->scalarOutputCount = 1;
  return kIOReturnSuccess;
 case kFBMethodGetDotPitch:
  if (args->scalarOutputCount < 1) {
   return kIOReturnBadArgument;
  }
  out[0] = fOwner->fWidth == kFBMacanWidth ? kFBMacanDotsPerInch : kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
  args->scalarOutputCount = 1;
  return kIOReturnSuccess;
 case kFBMethodGetLayerDefaultSurface:
  if (args->scalarOutputCount < 1) {
   return kIOReturnBadArgument;
  }
  s = fOwner->defaultSurface();
  if (s == NULL) {
   return kIOReturnNoMemory;
  }
  out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
  args->scalarOutputCount = 1;
  return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;
 case kFBMethodSwapBegin:
  if (args->scalarOutputCount < 1) {
   return kIOReturnBadArgument;
  }
  out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
  args->scalarOutputCount = 1;
  return kIOReturnSuccess;
 case kFBMethodSwapEnd:
  if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
   IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
   return kIOReturnBadArgument;
  }
  fOwner->present((const uint8_t *) args->structureInput);
  return kIOReturnSuccess;
 case kFBMethodSwapWait:
  if (gTouchLat) {
   redwood_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
       args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
  }
  return kIOReturnSuccess;
 case kFBMethodSwapSignal:
 case kFBMethodSwapCancel:
 case kFBMethodSwapCancelAll:
 case kFBMethodRequestPowerChange:
 case kFBMethodChangeFrameInfo:
  return kIOReturnSuccess;
 case kFBMethodNotifications:
  if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
   return kIOReturnBadArgument;
  }
  IOLockLock(fLock);
  fCallbacks[in[2]] = in[0];
  fRefcons[in[2]] = in[1];
  if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
   fVSyncRunning = true;
   armVSync();
  }
  IOLockUnlock(fLock);
  return kIOReturnSuccess;
 case kFBMethodSupportedFrameInfo:
  if (args->scalarOutputCount >= 1) {
   out[0] = 0;
   args->scalarOutputCount = 1;
  }
  if (args->structureOutput != NULL) {
   bzero(args->structureOutput, args->structureOutputSize);
  }
  return kIOReturnSuccess;
 case kFBMethodGetBlock:
  if (args->structureOutput == NULL) {
   return kIOReturnBadArgument;
  }
  bzero(args->structureOutput, args->structureOutputSize);
  if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
   uint32_t ids[2] = { kFBColorID, kFBTimingID };
   memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
  }
  return kIOReturnSuccess;
 case kFBMethodGetCurrentAbsoluteTime:
  if (args->scalarOutputCount < 1) {
   return kIOReturnBadArgument;
  }
  out[0] = mach_absolute_time();
  args->scalarOutputCount = 1;
  return kIOReturnSuccess;
 default:
  break;
 }
 if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
  gFBLogged[selector / 64] |= 1ULL << (selector % 64);
  IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
      "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
      args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
      args->structureInputSize, args->structureOutputSize);
 }
 if (args->scalarOutputCount != 0) {
  bzero(out, args->scalarOutputCount * sizeof(out[0]));
 }
 if (args->structureOutput != NULL && args->structureOutputSize != 0) {
  bzero(args->structureOutput, args->structureOutputSize);
 }
 return kIOReturnSuccess;
}
static void
publishDisplayNode(void)
{
 IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
 IORegistryEntry * node;
 OSDictionary * props;
 OSData * data;
 if (armio == NULL) {
  IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
  return;
 }
 node = OSTypeAlloc(IORegistryEntry);
 props = OSDictionary::withCapacity(4);
 if (node != NULL && props != NULL) {
  data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
  props->setObject("device_type", data);
  OSSafeReleaseNULL(data);
  data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
  props->setObject("compatible", data);
  OSSafeReleaseNULL(data);
  data = OSData::withBytes("disp0", sizeof("disp0"));
  props->setObject("name", data);
  OSSafeReleaseNULL(data);
  if (node->init(props)) {
   node->setName("disp0");
   node->attachToParent(armio, gIODTPlane);
  }
 }
 OSSafeReleaseNULL(props);
 OSSafeReleaseNULL(node);
 armio->release();
}
class IOAcceleratorES : public IOService
{
 OSDeclareDefaultStructors(IOAcceleratorES);
};
OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);
static void
publishAccelerator(IOService * provider)
{
 IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
 if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
  accelerator->setName("sgx");
  accelerator->registerService();
 }
 OSSafeReleaseNULL(accelerator);
}
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
 uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
 uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;
 if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
  return;
 }
 for (uint32_t y = 0; y < rows; y++) {
  memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
      (size_t) columns * 4);
 }
}
void
MiniIOMobileFramebufferStart(IOService * provider)
{
 IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);
 if (fb == NULL || !fb->init() || !fb->attach(provider)) {
  OSSafeReleaseNULL(fb);
  return;
 }
 if (fb->start(provider)) {
  publishDisplayNode();
  publishAccelerator(provider);
 } else {
  fb->detach(provider);
 }
 fb->release();
}
