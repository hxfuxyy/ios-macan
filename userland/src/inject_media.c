#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void
say(const char *format, ...)
{
 char line[256];
 va_list ap;
 int n;
 va_start(ap, format);
 n = vsnprintf(line, sizeof(line), format, ap);
 va_end(ap);
 if (n > (int)sizeof(line) - 1) {
  n = sizeof(line) - 1;
 }
 write(2, line, (size_t)n);
}
extern const void *_dyld_get_shared_cache_range(size_t *length);
#define CACHE_BASE 0x180000000UL
static uintptr_t slide;
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}
static int torch = -1;
static bool camera;
extern int redwood_camera_device_create(const void *allocator, const void *options, void **out);
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static unsigned torch_said;
static const struct { uintptr_t from, to; } flashlight_code[] = {
 { 0x1a1fb0830UL, 0x1a1fb0c5cUL }, { 0x1a1eee5c0UL, 0x1a1eee6e8UL },
};
static id
shared_vendor(id self, SEL selector)
{
 uintptr_t caller = (uintptr_t)__builtin_return_address(0) - slide;
 id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
 static id ours, camera_vendor;
 static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
 if (vendor != NULL) {
  return vendor;
 }
 if (!camera && access("/dev/redwood-camera", F_OK) == 0) {
  camera = true;
  say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera)\n");
 }
 if (camera) {
  pthread_mutex_lock(&lock);
  if (camera_vendor == NULL) {
   id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));
   camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
       (void *)redwood_camera_device_create);
   say("redwood: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
       "CANNOT BE MADE");
  }
  pthread_mutex_unlock(&lock);
  return camera_vendor;
 }
 for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
  if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
   if (ours == NULL) {
    ours = class_createInstance(vendor_class, 0);
    say("redwood: mediaserverd: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
   }
   return ours;
  }
 }
 return NULL;
}
static void
flashlight_set(uint32_t level)
{
 ssize_t written = write(torch, &level, sizeof(level));
 if (written != (ssize_t)sizeof(level) || torch_said < 40) {
  torch_said++;
  say("redwood: mediaserverd: the flashlight at %u of 1000%s\n", level, written == (ssize_t)sizeof(level) ? "" :
      " (NOT TAKEN by /dev/redwood-torch)");
 }
}
static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description, __unused int priority,
    __unused BOOL steal, __unused BOOL share, __unused id handler)
{
 return 1;
}
static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
 if (error != NULL) {
  *error = 0;
 }
 return class_createInstance(device_class, 0);
}
static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
 return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}
static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
 flashlight_set(0);
}
static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
 return device == NULL || object_getClass(device) == device_class;
}
static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
 static CFDictionaryRef none;
 if (none == NULL) {
  none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
 }
 return (id)none;
}
static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
 float level = 0;
 if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
  if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
   CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
  }
  level = level < 0 ? 0 : level > 1 ? 1 : level;
  flashlight_set((uint32_t)(level * 1000 + 0.5f));
 }
 return 0;
}
static int
video_stabilization_method(id self, SEL selector)
{
 (void)self;
 (void)selector;
 return 0;
}
static int
video_stabilization_type(id self, SEL selector)
{
 (void)self;
 (void)selector;
 return 0;
}
#define ERROR_NO_SUCH_THING 1852797029
static IMP original_audio_source_start;
static BOOL
audio_source_start(id self, SEL selector, id *error)
{
 BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
 id made = error != NULL ? *error : NULL;
 static bool said;
 if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
  return started;
 }
 *error = NULL;
 if (!said) {
  said = true;
  say("redwood: mediaserverd: there is no capture audio on this phone: the session's audio source gives nothing, "
      "and the camera runs without it\n");
 }
 return true;
}
static IMP original_quality_prioritization;
static int
quality_prioritization(id self, SEL selector)
{
 int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
 static bool said;
 if (asked > 1 && !said) {
  said = true;
  say("redwood: mediaserverd: a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket\n",
      asked);
 }
 return asked > 1 ? 1 : asked;
}
static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
 id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
 static bool said;
 (void)selector, (void)input;
 if (!said) {
  said = true;
  say("redwood: mediaserverd: a photograph goes past the noise reduction, which has nothing to do it with here\n");
 }
 if (output != nil) {
  ((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
 }
}
static bool
is_node(id node, const char *name)
{
 return node != nil && strcmp(object_getClassName(node), name) == 0;
}
static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
 CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
 static uint64_t began;
 static mach_timebase_info_data_t timebase;
 id connection, input;
 if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
  return;
 }
 if (is_node(((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node")), "BWPhotoEncoderNode")) {
  if (began != 0) {
   if (timebase.denom == 0) {
    mach_timebase_info(&timebase);
   }
   say("redwood: mediaserverd: a photograph of %zux%zu encoded in %.0f ms\n", CVPixelBufferGetWidth(image),
       CVPixelBufferGetHeight(image), (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
  }
  began = 0;
  return;
 }
 connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
 input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
 if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
  began = mach_absolute_time();
 }
}
static IMP original_emit_sample_buffer;
static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
 static bool said;
 if (sample_buffer == NULL) {
  if (!said) {
   said = true;
   say("redwood: mediaserverd: a node's output was given no sample buffer (the silent audio source's): not passed on\n");
  }
  return;
 }
 time_photo_encoder(self, sample_buffer);
 ((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}
static void
register_hevc_encoder(void)
{
 void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
 void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
 void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
 int (*register_encoder)(uint32_t, CFDictionaryRef, void *) = vt != NULL ? dlsym(vt, "VTRegisterVideoEncoderWithInfo") : NULL;
 CFStringRef *id_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderID") : NULL;
 CFStringRef *name_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderName") : NULL;
 CFStringRef *hardware_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated") : NULL;
 int status;
 if (create == NULL || register_encoder == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
  say("redwood: mediaserverd: no software HEVC encoder to register (%s)\n", codec == NULL ? "no VCPHEVC.videocodec" :
      create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without VTRegisterVideoEncoderWithInfo");
  return;
 }
 const void *keys[] = { *id_key, *name_key, *hardware_key };
 const void *values[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
  kCFBooleanFalse };
 CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
     &kCFTypeDictionaryValueCallBacks);
 status = register_encoder('hvc1', info, create);
 CFRelease(info);
 say("redwood: mediaserverd: the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)\n", status);
}
struct method_spec {
 const char *selector, *types;
 IMP imp;
};
static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
 Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);
 for (size_t i = 0; i < count; i++) {
  class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
 }
 objc_registerClassPair(cls);
 return cls;
}
static void
flashlight_classes(void)
{
 static const struct method_spec vendor[] = {
  { "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
    "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:", "i@:i@iBB@", (IMP)flashlight_register },
  { "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", "@@:iB^i", (IMP)flashlight_copy_device },
  { "copyStreamForFlashlightWithPosition:deviceType:forDevice:", "@@:ii@", (IMP)flashlight_copy_stream },
  { "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:", "v@:@iB", (IMP)flashlight_take_back },
  { "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
 }, stream[] = {
  { "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
  { "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
 };
 vendor_class = make_class("RedwoodFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
 device_class = make_class("RedwoodFlashlightDevice", NULL, 0);
 stream_class = make_class("RedwoodFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}
static struct replacement {
 const char *class_name, *selector;
 uintptr_t expected;
 IMP replacement, *original;
 bool instance;
 bool done;
} replacements[] = {
 { "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
   (IMP)prewarm_portrait_filter, NULL, false, false },
 { "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0x1a1ef0d04UL,
   (IMP)shared_vendor, &original_shared_vendor, false, false },
 { "BWAudioSourceNode", "start:", 0x1a1eefa18UL, (IMP)audio_source_start, &original_audio_source_start, true, false },
 { "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", 0x1a1e99ef8UL,
   (IMP)video_stabilization_method, NULL, true, false },
 { "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", 0x1a20d8f48UL,
   (IMP)video_stabilization_type, NULL, true, false },
 { "FigCaptureStillImageSettings", "qualityPrioritization", 0x1a1f533dcUL,
   (IMP)quality_prioritization, &original_quality_prioritization, true, false },
 { "BWGNRNode", "renderSampleBuffer:forInput:", 0x1a1ec83b8UL, (IMP)gnr_render, NULL, true, false },
 { "BWNodeOutput", "emitSampleBuffer:", 0x1a1ea37e0UL, (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false },
};
static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
 for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
  struct replacement *r = &replacements[i];
  Class cls;
  Method method;
  IMP original;
  if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
   continue;
  }
  r->done = true;
  method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
      class_getClassMethod(cls, sel_registerName(r->selector));
  original = method ? method_getImplementation(method) : NULL;
  if (original == NULL || (uintptr_t)original != r->expected + slide) {
   say("redwood: %c[%s %s]: found %#lx, expected %#lx; left alone\n", r->instance ? '-' : '+', r->class_name,
       r->selector,
       original ? (uintptr_t)original - slide : 0, r->expected);
   continue;
  }
  if (r->original != NULL) {
   *r->original = original;
  }
  method_setImplementation(method, r->replacement);
  say("redwood: mediaserverd: %c[%s %s] replaced\n", r->instance ? '-' : '+', r->class_name, r->selector);
 }
}
__attribute__((constructor))
static void
redwood_media_init(void)
{
 const char *name = getprogname();
 size_t length = 0;
 if (name == NULL || strcmp(name, "mediaserverd") != 0) {
  return;
 }
 slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
 torch = open("/dev/redwood-torch", O_WRONLY);
 camera = access("/dev/redwood-camera", F_OK) == 0;
 if (torch >= 0) {
  flashlight_classes();
  say("redwood: mediaserverd: the kernel has a flashlight (/dev/redwood-torch)\n");
 }
 if (camera) {
  say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera)\n");
  register_hevc_encoder();
 }
 _dyld_register_func_for_add_image(image_added);
}
