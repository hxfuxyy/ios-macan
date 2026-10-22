#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "camera_isp.h"
void redwood_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));
void
redwood_camera_say(const char *format, ...)
{
 char line[400];
 va_list ap;
 int n = snprintf(line, sizeof(line), "redwood: camera: ");
 va_start(ap, format);
 n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
 va_end(ap);
 if (n > (int)sizeof(line) - 2) {
  n = sizeof(line) - 2;
 }
 line[n++] = '\n';
 write(2, line, (size_t)n);
}
#define say redwood_camera_say
typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef const void *CMBaseClassID;
struct base_class {
 uint64_t version;
 uint64_t storage_size;
 Boolean (*equal)(CMBaseObjectRef, CMBaseObjectRef);
 OSStatus (*invalidate)(CMBaseObjectRef);
 void (*finalize)(CMBaseObjectRef);
 CFStringRef (*copy_debug_description)(CMBaseObjectRef);
 OSStatus (*copy_property)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
 OSStatus (*set_property)(CMBaseObjectRef, CFStringRef, CFTypeRef);
 OSStatus (*notification_barrier)(CMBaseObjectRef);
 const void * protocols;
 const void * reserved;
};
struct device_class {
 uint64_t version;
 void * processing_session_create;
 OSStatus (*request_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
 OSStatus (*relinquish_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
 void * pearl_calibration;
};
struct stream_class {
 uint64_t version;
 OSStatus (*start)(CMBaseObjectRef);
 OSStatus (*stop)(CMBaseObjectRef);
 OSStatus (*add_attachments)(CMBaseObjectRef, CMSampleBufferRef, CFDictionaryRef);
};
struct vtable {
 const void * reserved;
 const struct base_class *base;
 const void * derived;
};
#define ERR_PARAM (-12780)
#define ERR_NOT_AVAILABLE (-12783)
#define ERR_UNSUPPORTED (-12782)
static OSStatus (*derived_create)(CFAllocatorRef, const struct vtable *, CMBaseClassID, CMBaseObjectRef *);
static void *(*derived_storage)(CMBaseObjectRef);
static CMBaseClassID (*device_class_id)(void);
static CMBaseClassID (*stream_class_id)(void);
static CFStringRef port_back;
static CFStringRef port_front;
enum {
 MD_PORT, MD_PTS, MD_EXPOSURE, MD_AGC, MD_ISP_DGAIN, MD_SENSOR_DGAIN, MD_ISO_BASE, MD_FRAME_RATE, MD_LUX, MD_SNR,
 MD_AF_STATUS, MD_FOCUSING, MD_AE_STABLE, MD_AE_LOCKED, MD_AWB_STABLE, MD_AWB_LOCKED, MD_AF_LOCKED, MD_ZOOM, MD_KEYS
};
static const char *const md_names[MD_KEYS] = {
 "kFigCaptureStreamMetadata_PortType", "kFigCaptureSampleBufferAttachmentKey_OriginalPresentationTimeStamp",
 "kFigCaptureStreamMetadata_ExposureTime", "kFigCaptureStreamMetadata_AGC", "kFigCaptureStreamMetadata_ispDGain",
 "kFigCaptureStreamMetadata_sensorDGain", "kFigCaptureStreamMetadata_ISOBase", "kFigCaptureStreamMetadata_CurrentFrameRate",
 "kFigCaptureStreamMetadata_LuxLevel", "kFigCaptureStreamMetadata_NormalizedSNR", "kFigCaptureStreamMetadata_AFStatus",
 "kFigCaptureStreamMetadata_FocusingMethod", "kFigCaptureStreamMetadata_AEStable", "kFigCaptureStreamMetadata_AELocked",
 "kFigCaptureStreamMetadata_AWBStable", "kFigCaptureStreamMetadata_AWBLocked", "kFigCaptureStreamMetadata_AFLocked",
 "kFigCaptureStreamMetadata_ZoomFactor",
};
static CFStringRef md_key[MD_KEYS];
static CFStringRef md_attachment;
static bool
find_functions(void)
{
 CFStringRef *port;
 derived_create = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
 derived_storage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
 device_class_id = dlsym(RTLD_DEFAULT, "FigCaptureDeviceGetClassID");
 stream_class_id = dlsym(RTLD_DEFAULT, "FigCaptureStreamGetClassID");
 port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_BackFacingCamera");
 port_back = port != NULL ? *port : NULL;
 port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_FrontFacingCamera");
 port_front = port != NULL ? *port : NULL;
 port = dlsym(RTLD_DEFAULT, "kFigCaptureSampleBufferAttachmentKey_MetadataDictionary");
 md_attachment = port != NULL ? *port : NULL;
 for (int i = 0; i < MD_KEYS; i++) {
  port = dlsym(RTLD_DEFAULT, md_names[i]);
  md_key[i] = port != NULL ? *port : NULL;
  if (md_key[i] == NULL) {
   say("%s is not there: the frames' metadata go without it", md_names[i]);
  }
 }
 if (md_attachment == NULL || md_key[MD_PORT] == NULL) {
  say("kFigCaptureSampleBufferAttachmentKey_MetadataDictionary or the port's key is not there: frames without "
      "metadata, and the shutter will wait for ever");
  md_attachment = NULL;
 }
 if (derived_create == NULL || derived_storage == NULL || device_class_id == NULL || stream_class_id == NULL || port_back == NULL ||
     port_front == NULL) {
  say("CoreMedia's or CMCapture's functions are not there: no camera");
  return false;
 }
 return true;
}
static CFMutableSetRef said_names;
static pthread_mutex_t said_lock = PTHREAD_MUTEX_INITIALIZER;
static void
say_value(const char *what, CFStringRef key, CFTypeRef value)
{
 char name[160] = "?", text[300] = "";
 CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
 if (value != NULL) {
  CFStringRef d = CFCopyDescription(value);
  CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
  CFRelease(d);
  for (char *c = text; *c; c++) {
   *c = *c == '\n' ? ' ' : *c;
  }
 }
 say("%s %s%s%s", what, name, value ? " = " : "", text);
}
static void
say_once(const char *what, CFStringRef key, CFTypeRef value)
{
 CFStringRef tag = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s %@"), what, key);
 pthread_mutex_lock(&said_lock);
 if (said_names == NULL) {
  said_names = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
 }
 bool first = !CFSetContainsValue(said_names, tag);
 CFSetAddValue(said_names, tag);
 pthread_mutex_unlock(&said_lock);
 CFRelease(tag);
 if (first) {
  say_value(what, key, value);
 }
}
static CFNumberRef
number_i(int64_t v)
{
 return CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
}
static CFNumberRef
number_f(double v)
{
 return CFNumberCreate(NULL, kCFNumberDoubleType, &v);
}
static void
put(CFMutableDictionaryRef d, const char *key, CFTypeRef value)
{
 CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
 CFDictionarySetValue(d, k, value);
 CFRelease(k);
 CFRelease(value);
}
static CFDictionaryRef
new_dict(void)
{
 return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}
static bool
key_is(CFStringRef key, const char *name)
{
 char buffer[128];
 return CFStringGetCString(key, buffer, sizeof(buffer), kCFStringEncodingUTF8) && strcmp(buffer, name) == 0;
}
struct prop {
 const char *name, *type, *access;
};
static const struct prop stream_props[] = {
 { "PortType", "String", "ReadOnly" }, { "UniqueID", "String", "ReadOnly" }, { "NonLocalizedName", "String", "ReadOnly" },
 { "SupportedFormatsArray", "Collection", "ReadOnly" }, { "FocalLength", "Number", "ReadOnly" },
 { "LensFNumber", "Number", "ReadOnly" }, { "PixelSize", "Number", "ReadOnly" },
 { "MaxZoomFactor", "Number", "ReadOnly" }, { "SupportedColorSpaces", "Collection", "ReadOnly" },
 { "HasAutoFocusPositionSensor", "Boolean", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" },
 { "FormatIndex", "Number", "ReadWrite" }, { "MinimumFrameRate", "Number", "ReadWrite" }, { "MaximumFrameRate", "Number", "ReadWrite" },
 { "MaximumAllowedFrameRate", "Number", "ReadWrite" }, { "VideoOutputConfigurations", "Collection", "ReadWrite" },
 { "VideoOutputsEnabled", "Collection", "ReadWrite" }, { "VideoOutputHandlers", "Collection", "WriteOnly" },
 { "DeferAdditionOfAttachments", "Boolean", "ReadWrite" }, { "TorchLevel", "Number", "ReadWrite" },
 { "HighCurrentTorchEnabled", "Boolean", "ReadWrite" }, { "ColorSpace", "Number", "ReadWrite" }, { "ZoomFactor", "Number", "ReadWrite" },
 { "ExposureBias", "Number", "ReadWrite" }, { "DeviceOrientation", "Number", "ReadWrite" }, { "FlashMode", "Number", "ReadWrite" },
 { "StillImageCaptureNow", "Collection", "WriteOnly" }, { "LockAENow", "Boolean", "WriteOnly" }, { "UnlockAENow", "Boolean", "WriteOnly" },
 { "LockAWBNow", "Boolean", "WriteOnly" }, { "UnlockAWBNow", "Boolean", "WriteOnly" }, { "MaxIntegrationTime", "Number", "ReadWrite" },
 { "AEMaxGain", "Number", "ReadWrite" }, { "AEMinGain", "Number", "ReadWrite" }, { "FaceDetectionEnabled", "Boolean", "ReadWrite" },
 { "VideoRecordingInProgress", "Boolean", "ReadWrite" }, { "MemoryPoolID", "Number", "ReadWrite" },
 { "ChromaNoiseReductionEnabled", "Boolean", "ReadWrite" }, { "TemporalNoiseReductionEnabled", "Boolean", "ReadWrite" },
 { "MultipleOutputSupportEnabled", "Boolean", "ReadWrite" }, { "VideoOutputsColorInfo", "Collection", "ReadWrite" },
 { "RegionOfInterestForCameraControls", "Collection", "ReadWrite" }, { "AEMeteringMode", "Number", "ReadWrite" },
 { "MaxFrameCoalescingDuration", "Number", "ReadWrite" }, { "LimitAERange", "Boolean", "ReadWrite" },
 { "TimeMachineSuspendNow", "Boolean", "WriteOnly" }, { "TimeMachineResumeNow", "Boolean", "WriteOnly" },
 { "AutoFocusNow", "Collection", "WriteOnly" }, { "ContinuousAutoFocusNow", "Collection", "WriteOnly" },
 { "FocusNow", "Collection", "WriteOnly" }, { "PeakTrackingFocusNow", "Collection", "WriteOnly" },
 { "LockAFNow", "Boolean", "WriteOnly" }, { "ManualFocusNow", "Collection", "WriteOnly" },
 { "AFWindowParams", "Collection", "ReadWrite" }, { "AutoFocusMode", "Number", "ReadWrite" },
}, device_props[] = {
 { "StreamArray", "Collection", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" }, { "ClientIsActive", "Boolean", "ReadWrite" },
 { "SupportedSynchronizedStreamsGroups", "Collection", "ReadOnly" }, { "MemoryPoolID", "Number", "ReadWrite" },
};
static CFDictionaryRef
supported(const struct prop *p, size_t n)
{
 CFMutableDictionaryRef all = (CFMutableDictionaryRef)new_dict();
 for (size_t i = 0; i < n; i++) {
  CFMutableDictionaryRef one = (CFMutableDictionaryRef)new_dict();
  put(one, "PropertyType", CFStringCreateWithCString(NULL, p[i].type, kCFStringEncodingUTF8));
  put(one, "ReadWriteStatus", CFStringCreateWithCString(NULL, p[i].access, kCFStringEncodingUTF8));
  put(all, p[i].name, one);
 }
 return all;
}
#define PF_420F 0x34323066
#define PF_420V 0x34323076
#define PF_BGRA 0x42475241
static CFDictionaryRef
output_info(int width, int height)
{
 CFMutableDictionaryRef o = (CFMutableDictionaryRef)new_dict();
 CFNumberRef pf[3] = { number_i(PF_420F), number_i(PF_420V), number_i(PF_BGRA) };
 CFArrayRef pfs = CFArrayCreate(NULL, (const void **)pf, 3, &kCFTypeArrayCallBacks);
 for (int i = 0; i < 3; i++) {
  CFRelease(pf[i]);
 }
 put(o, "SupportedPixelFormats", pfs);
 put(o, "NativeWidth", number_i(width));
 put(o, "NativeHeight", number_i(height));
 put(o, "MaxDownscaleFactor", number_f(32));
 put(o, "MaxScaleFactor", number_f(1));
 put(o, "MinScaleFactor", number_f(1.0 / 32));
 put(o, "OutputWidthAlignmentRequirement", number_i(16));
 return o;
}
static CFDictionaryRef
format(int sensor_width, int sensor_height, int max_fps, bool binned, double fov)
{
 CFMutableDictionaryRef f = (CFMutableDictionaryRef)new_dict(), outputs = (CFMutableDictionaryRef)new_dict();
 CMVideoFormatDescriptionRef fd = NULL;
 CMVideoFormatDescriptionCreate(NULL, PF_420F, sensor_width, sensor_height, NULL, &fd);
 if (fd != NULL) {
  put(f, "FormatDescription", fd);
 }
 put(f, "VideoMinFrameRate", number_f(2));
 put(f, "VideoMaxFrameRate", number_f(max_fps));
 put(f, "VideoIsBinned", CFRetain(binned ? kCFBooleanTrue : kCFBooleanFalse));
 put(f, "VideoBinningFactorHorizontal", number_i(binned ? 2 : 1));
 put(f, "VideoBinningFactorVertical", number_i(binned ? 2 : 1));
 put(f, "VideoFieldOfView", number_f(fov));
 put(f, "VideoMinIntegrationTime", number_i(100));
 put(f, "VideoMaxIntegrationTime", number_i(1000000 / 15));
 put(f, "VideoRawBitDepth", number_i(10));
 put(f, "VideoMaxWidth", number_i(sensor_width));
 put(f, "VideoMaxHeight", number_i(sensor_height));
 put(f, "Experimental", CFRetain(kCFBooleanFalse));
 put(f, "SensorWidth", number_i(sensor_width));
 put(f, "SensorHeight", number_i(sensor_height));
 put(f, "ISOBase", number_f(32));
 put(f, "DefaultAEMaxGain", number_f(64));
 put(f, "SensorCropWidth", number_i(sensor_width));
 put(f, "SensorCropHeight", number_i(sensor_height));
 put(f, "PreGDCSensorCropWidth", number_i(sensor_width));
 put(f, "PreGDCSensorCropHeight", number_i(sensor_height));
 put(f, "PostGDCCropWidth", number_i(sensor_width));
 put(f, "PostGDCCropHeight", number_i(sensor_height));
 put(f, "GeometricDistortionCorrectedNativeWidth", number_i(sensor_width));
 put(f, "GeometricDistortionCorrectedNativeHeight", number_i(sensor_height));
 put(outputs, "PrimaryScaler", output_info(sensor_width, sensor_height));
 put(outputs, "SecondaryScaler", output_info(sensor_width, sensor_height));
 put(outputs, "StillImage", output_info(sensor_width, sensor_height));
 put(f, "SupportedOutputs", outputs);
 return f;
}
#define HELD 3
struct output {
 const char * name;
 CFDictionaryRef config;
 bool enabled;
 void * handler;
 bool said_shape;
 uint64_t delivered, refused;
 CVPixelBufferRef held[HELD];
 unsigned held_next;
 bool said_pool;
};
static void let_go(struct output *out);
#define OUTPUTS 3
static const char *const output_names[OUTPUTS] = { "PrimaryScaler", "SecondaryScaler", "StillImage" };
#define MAX_ZOOM 16
static const struct camera {
 uint32_t sensor;
 const char *name, *unique_id;
 bool turned;
 double focal_length, f_number, pixel_size, fov;
 int formats[2][3];
} cameras[2] = {
 { 1, "Back Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:0", false, 4.0, 1.8, 1.22, 65.0,
   { { 4032, 3024, 30 }, { 4224, 2376, 60 } } },
 { 2, "Front Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:1", true, 2.87, 2.2, 1.0, 70.0,
   { { 3088, 2320, 30 }, { 3840, 2160, 30 } } },
};
struct stream {
 CFArrayRef formats;
 CFDictionaryRef supported;
 CFDictionaryRef colour_info;
 CFDictionaryRef metadata;
 pthread_mutex_t metadata_lock;
 unsigned stills_wanted;
 int format_index;
 double min_fps, max_fps;
 float zoom;
 const struct camera *camera;
 CFStringRef port;
 bool af_now;
 bool af_locked;
 float af_window[4];
 struct output out[OUTPUTS];
 pthread_mutex_t lock;
 pthread_t worker;
 volatile bool running;
 volatile bool graph_running;
 bool worker_started;
 int torch;
 uint64_t frames, stills_given;
};
static struct stream *
stream_of(CMBaseObjectRef o)
{
 return derived_storage(o);
}
static int
output_index(CFStringRef name)
{
 for (int i = 0; i < OUTPUTS; i++) {
  if (key_is(name, output_names[i])) {
   return i;
  }
 }
 return -1;
}
static OSStatus
stream_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
 struct stream *s = stream_of(o);
 CFTypeRef value = NULL;
 (void)allocator;
 if (key_is(key, "PortType")) {
  value = CFRetain(s->port);
 } else if (key_is(key, "UniqueID") || key_is(key, "ModelID")) {
  value = CFStringCreateWithCString(NULL, s->camera->unique_id, kCFStringEncodingUTF8);
 } else if (key_is(key, "NonLocalizedName") || key_is(key, "LocalizedName")) {
  value = CFStringCreateWithCString(NULL, s->camera->name, kCFStringEncodingUTF8);
 } else if (key_is(key, "Manufacturer")) {
  value = CFRetain(CFSTR("Apple Inc."));
 } else if (key_is(key, "SupportedFormatsArray")) {
  value = CFRetain(s->formats);
 } else if (key_is(key, "FormatIndex")) {
  value = number_i(s->format_index);
 } else if (key_is(key, "MinimumFrameRate")) {
  value = number_f(s->min_fps);
 } else if (key_is(key, "MaximumFrameRate")) {
  value = number_f(s->max_fps);
 } else if (key_is(key, "Clock")) {
  value = CFRetain(CMClockGetHostTimeClock());
 } else if (key_is(key, "TorchLevel") || key_is(key, "ExposureBias")) {
  value = number_f(0);
 } else if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled")) {
  bool configs = key_is(key, "VideoOutputConfigurations");
  CFMutableDictionaryRef d = (CFMutableDictionaryRef)new_dict();
  pthread_mutex_lock(&s->lock);
  for (int i = 0; i < OUTPUTS; i++) {
   CFStringRef name = CFStringCreateWithCString(NULL, output_names[i], kCFStringEncodingUTF8);
   if (configs) {
    if (s->out[i].config != NULL) {
     CFDictionarySetValue(d, name, s->out[i].config);
    }
   } else {
    CFDictionarySetValue(d, name, s->out[i].enabled ? kCFBooleanTrue : kCFBooleanFalse);
   }
   CFRelease(name);
  }
  pthread_mutex_unlock(&s->lock);
  value = d;
 } else if (key_is(key, "VideoOutputsColorInfo")) {
  pthread_mutex_lock(&s->lock);
  value = s->colour_info != NULL ? CFRetain(s->colour_info) : new_dict();
  pthread_mutex_unlock(&s->lock);
 } else if (key_is(key, "SupportedPropertiesDictionary")) {
  value = CFRetain(s->supported);
 } else if (key_is(key, "FocalLength")) {
  value = number_f(s->camera->focal_length);
 } else if (key_is(key, "LensFNumber")) {
  value = number_f(s->camera->f_number);
 } else if (key_is(key, "PixelSize")) {
  value = number_f(s->camera->pixel_size);
 } else if (key_is(key, "MaxZoomFactor")) {
  value = number_f(MAX_ZOOM);
 } else if (key_is(key, "ZoomFactor")) {
  value = number_f(s->zoom);
 } else if (key_is(key, "SupportedColorSpaces")) {
  CFNumberRef srgb = number_i(0);
  value = CFArrayCreate(NULL, (const void **)&srgb, 1, &kCFTypeArrayCallBacks);
  CFRelease(srgb);
 } else if (key_is(key, "ColorSpace") || key_is(key, "DeviceOrientation") || key_is(key, "FlashMode")) {
  value = number_i(0);
 } else if (key_is(key, "HasAutoFocusPositionSensor") || key_is(key, "HighCurrentTorchEnabled") ||
     key_is(key, "DeferAdditionOfAttachments")) {
  value = CFRetain(kCFBooleanFalse);
 } else {
  say_once("stream: no answer for", key, NULL);
  return ERR_NOT_AVAILABLE;
 }
 *(CFTypeRef *)out = value;
 return 0;
}
static void torch_set(struct stream *s, CFTypeRef value);
static double
dict_number(CFDictionaryRef d, const char *name, double otherwise)
{
 CFStringRef k = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
 CFTypeRef v = CFDictionaryGetValue(d, k);
 double n = otherwise;
 CFRelease(k);
 if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
  CFNumberGetValue(v, kCFNumberDoubleType, &n);
 }
 return n;
}
static void
af_window(struct stream *s, CFTypeRef value)
{
 CFDictionaryRef w = NULL;
 double x0, y0, width, height;
 if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() && CFArrayGetCount(value) > 0) {
  w = CFArrayGetValueAtIndex(value, 0);
 }
 if (w == NULL || CFGetTypeID(w) != CFDictionaryGetTypeID() || (width = dict_number(w, "width", 0)) <= 0 ||
     (height = dict_number(w, "height", 0)) <= 0) {
  s->af_window[0] = s->af_window[1] = 0.3f;
  s->af_window[2] = s->af_window[3] = 0.7f;
  return;
 }
 x0 = dict_number(w, "hStart", 0) / 1000;
 y0 = dict_number(w, "vStart", 0) / 1000;
 width /= 1000;
 height /= 1000;
 if (width < 0.1) {
  x0 -= (0.1 - width) / 2;
  width = 0.1;
 }
 if (height < 0.1) {
  y0 -= (0.1 - height) / 2;
  height = 0.1;
 }
 x0 = x0 < 0 ? 0 : x0 + width > 1 ? 1 - width : x0;
 y0 = y0 < 0 ? 0 : y0 + height > 1 ? 1 - height : y0;
 s->af_window[0] = (float)x0;
 s->af_window[1] = (float)y0;
 s->af_window[2] = (float)(x0 + width);
 s->af_window[3] = (float)(y0 + height);
}
static OSStatus
stream_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
 struct stream *s = stream_of(o);
 if (key_is(key, "FormatIndex")) {
  int64_t i = 0;
  if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
   CFNumberGetValue(value, kCFNumberSInt64Type, &i);
  }
  if (i < 0 || i >= CFArrayGetCount(s->formats)) {
   return ERR_PARAM;
  }
  s->format_index = (int)i;
  say("stream: format %lld", (long long)i);
  return 0;
 }
 if (key_is(key, "ZoomFactor")) {
  double zoom = 1;
  if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
   CFNumberGetValue(value, kCFNumberDoubleType, &zoom);
  }
  zoom = zoom < 1 ? 1 : zoom > MAX_ZOOM ? MAX_ZOOM : zoom;
  if (fabs(zoom - s->zoom) >= 0.25 || (zoom == 1) != (s->zoom == 1)) {
   say("stream: zoom %.2fx", zoom);
  }
  s->zoom = (float)zoom;
  return 0;
 }
 if (key_is(key, "MinimumFrameRate") || key_is(key, "MaximumFrameRate")) {
  double fps = 0;
  if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
   CFNumberGetValue(value, kCFNumberDoubleType, &fps);
  }
  *(key_is(key, "MinimumFrameRate") ? &s->min_fps : &s->max_fps) = fps;
  say_once("stream: set", key, value);
  return 0;
 }
 if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled") || key_is(key, "VideoOutputHandlers")) {
  bool configs = key_is(key, "VideoOutputConfigurations"), enabled = key_is(key, "VideoOutputsEnabled");
  say_once("stream: set", key, value);
  if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
   return 0;
  }
  pthread_mutex_lock(&s->lock);
  CFIndex n = CFDictionaryGetCount(value);
  const void **keys = calloc((size_t)n + 1, sizeof(void *)), **values = calloc((size_t)n + 1, sizeof(void *));
  CFDictionaryGetKeysAndValues(value, keys, values);
  for (CFIndex i = 0; i < n; i++) {
   int k = CFGetTypeID(keys[i]) == CFStringGetTypeID() ? output_index(keys[i]) : -1;
   struct output *out;
   if (k < 0) {
    say_once("stream: an output not here:", keys[i], NULL);
    continue;
   }
   out = &s->out[k];
   if (configs) {
    if (out->config != NULL) {
     CFRelease(out->config);
    }
    let_go(out);
    out->config = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ? CFRetain(values[i]) : NULL;
   } else if (enabled) {
    int64_t on = 1;
    if (CFGetTypeID(values[i]) == CFBooleanGetTypeID()) {
     on = CFBooleanGetValue(values[i]);
    } else if (CFGetTypeID(values[i]) == CFNumberGetTypeID()) {
     CFNumberGetValue(values[i], kCFNumberSInt64Type, &on);
    }
    out->enabled = on != 0;
   } else {
    CFTypeRef h = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ?
        CFDictionaryGetValue(values[i], CFSTR("PixelBufferHandler")) : NULL;
    if (out->handler != NULL) {
     CFRelease(out->handler);
    }
    out->handler = h != NULL ? (void *)CFRetain(h) : NULL;
   }
  }
  free(keys);
  free(values);
  pthread_mutex_unlock(&s->lock);
  return 0;
 }
 if (key_is(key, "StillImageCaptureNow")) {
  pthread_mutex_lock(&s->lock);
  s->stills_wanted++;
  pthread_mutex_unlock(&s->lock);
  say_once("stream: a photograph asked for", key, value);
  return 0;
 }
 if (key_is(key, "VideoOutputsColorInfo")) {
  pthread_mutex_lock(&s->lock);
  if (s->colour_info != NULL) {
   CFRelease(s->colour_info);
  }
  s->colour_info = value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID() ? CFRetain(value) : NULL;
  pthread_mutex_unlock(&s->lock);
  return 0;
 }
 if (key_is(key, "TorchLevel")) {
  torch_set(s, value);
  return 0;
 }
 if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
     key_is(key, "PeakTrackingFocusNow") || key_is(key, "LockAFNow") || key_is(key, "ManualFocusNow") ||
     key_is(key, "AFWindowParams") || key_is(key, "AutoFocusMode")) {
  if (!key_is(key, "AFWindowParams") || (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() &&
      CFArrayGetCount(value) > 0)) {
   say_value("stream: focus:", key, value);
  }
  if (key_is(key, "AFWindowParams")) {
   af_window(s, value);
  } else if (key_is(key, "LockAFNow")) {
   s->af_locked = true;
  } else if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
      key_is(key, "PeakTrackingFocusNow")) {
   s->af_locked = false;
   s->af_now = true;
  }
  return 0;
 }
 say_once("stream: set (not used)", key, value);
 return 0;
}
static void
torch_set(struct stream *s, CFTypeRef value)
{
 float level = 0;
 uint32_t thousandths;
 if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
  CFNumberGetValue(value, kCFNumberFloatType, &level);
 }
 level = level < 0 ? 0 : level > 1 ? 1 : level;
 thousandths = (uint32_t)(level * 1000 + 0.5f);
 if (s->torch < 0) {
  s->torch = open("/dev/redwood-torch", O_WRONLY);
 }
 if (s->torch < 0 || write(s->torch, &thousandths, sizeof(thousandths)) != (ssize_t)sizeof(thousandths)) {
  say("the flashlight at %u of 1000 NOT TAKEN (no /dev/redwood-torch)", thousandths);
 } else {
  say("the flashlight at %u of 1000", thousandths);
 }
}
struct render_job {
 const struct isp *isp;
 const struct redwood_camera_frame *f;
 const uint8_t *raw;
 CVPixelBufferRef pb;
 uint32_t format, width, height, pieces;
 bool still;
};
static void
render_piece(void *context, size_t i)
{
 struct render_job *j = context;
 uint32_t rows = (j->height + j->pieces - 1) / j->pieces, first = (uint32_t)i * rows, end = first + rows;
 first &= ~1u;
 end = i + 1 == j->pieces ? j->height : end & ~1u;
 if (j->format == PF_BGRA) {
  isp_render_bgra(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddress(j->pb), CVPixelBufferGetBytesPerRow(j->pb), j->width,
      j->height, first, end);
 } else if (j->still) {
  isp_render_still_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0),
      CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0), CVPixelBufferGetBaseAddressOfPlane(j->pb, 1),
      CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end, j->format == PF_420V);
 } else {
  isp_render_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0),
      CVPixelBufferGetBaseAddressOfPlane(j->pb, 1), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end,
      j->format == PF_420V);
 }
}
static CVPixelBufferRef
output_buffer(struct output *out)
{
 CVPixelBufferPoolRef pool = NULL;
 CVPixelBufferRef pb = NULL;
 CFTypeRef value = out->config != NULL ? CFDictionaryGetValue(out->config, CFSTR("BufferPool")) : NULL;
 if (value != NULL && CFGetTypeID(value) == CVPixelBufferPoolGetTypeID()) {
  pool = (CVPixelBufferPoolRef)value;
 }
 if (pool == NULL) {
  if (!out->said_shape) {
   out->said_shape = true;
   say("%s: no pool of the graph's in the configuration: no pictures", out->name);
  }
  return NULL;
 }
 for (unsigned i = 0; CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess && i < HELD; i++) {
  unsigned oldest = (out->held_next + i) % HELD;
  if (out->held[oldest] != NULL) {
   CVPixelBufferRelease(out->held[oldest]);
   out->held[oldest] = NULL;
  }
  if (!out->said_pool) {
   out->said_pool = true;
   say("%s: the graph's pool will not grow by %u: fewer pictures kept", out->name, HELD);
  }
 }
 if (pb == NULL) {
  if (out->refused++ % 300 == 0) {
   say("%s: the graph's pool has no buffer to spare (%llu frames so)", out->name,
       (unsigned long long)out->refused);
  }
  return NULL;
 }
 return pb;
}
typedef void (^pixel_buffer_handler)(CVPixelBufferRef, CMTime);
static void
let_go(struct output *out)
{
 for (unsigned i = 0; i < HELD; i++) {
  if (out->held[i] != NULL) {
   CVPixelBufferRelease(out->held[i]);
   out->held[i] = NULL;
  }
 }
}
static void
md_put(CFMutableDictionaryRef m, int key, CFTypeRef value)
{
 if (md_key[key] != NULL && value != NULL) {
  CFDictionarySetValue(m, md_key[key], value);
 }
 if (value != NULL) {
  CFRelease(value);
 }
}
static CFDictionaryRef
frame_metadata(const struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *st, CFStringRef port)
{
 CFMutableDictionaryRef m;
 double exposure = f->exposure_lines * (double)f->line_ns / 1e9, analog = f->gain_unity ? (double)f->gain / f->gain_unity : 1;
 double total = analog * isp->digital, fps = f->frame_lines && f->line_ns ? 1e9 / ((double)f->frame_lines * f->line_ns) : 30;
 double lux = exposure > 0 && total > 0 ? st->mean[1] / (exposure * total) / 4 : 0;
 if (md_attachment == NULL) {
  return NULL;
 }
 m = (CFMutableDictionaryRef)new_dict();
 md_put(m, MD_PORT, CFRetain(port));
 md_put(m, MD_PTS, CMTimeCopyAsDictionary(CMClockMakeHostTimeFromSystemUnits(f->time), NULL));
 md_put(m, MD_EXPOSURE, number_f(exposure));
 md_put(m, MD_AGC, number_i((int64_t)(analog * 256 + 0.5)));
 md_put(m, MD_ISP_DGAIN, number_i((int64_t)(isp->digital * 256 + 0.5)));
 md_put(m, MD_SENSOR_DGAIN, number_i(256));
 md_put(m, MD_ISO_BASE, number_f(32));
 md_put(m, MD_FRAME_RATE, number_f(fps));
 md_put(m, MD_LUX, number_i((int64_t)(lux + 0.5)));
 md_put(m, MD_SNR, number_f(40 - 10 * log10(total < 1 ? 1 : total)));
 md_put(m, MD_AF_STATUS, number_i(0));
 md_put(m, MD_FOCUSING, number_i(0));
 md_put(m, MD_AE_STABLE, CFRetain(kCFBooleanTrue));
 md_put(m, MD_AE_LOCKED, CFRetain(kCFBooleanFalse));
 md_put(m, MD_AWB_STABLE, CFRetain(kCFBooleanTrue));
 md_put(m, MD_AWB_LOCKED, CFRetain(kCFBooleanFalse));
 md_put(m, MD_AF_LOCKED, CFRetain(kCFBooleanFalse));
 md_put(m, MD_ZOOM, number_f(isp->zoom));
 return m;
}
static void
deliver(struct stream *s, struct output *out, const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw,
    CFDictionaryRef metadata)
{
 CVPixelBufferRef pb = output_buffer(out);
 uint32_t pf, width, height;
 (void)s;
 if (pb == NULL) {
  return;
 }
 pf = CVPixelBufferGetPixelFormatType(pb);
 width = (uint32_t)CVPixelBufferGetWidth(pb);
 height = (uint32_t)CVPixelBufferGetHeight(pb);
 if (width == 0 || height == 0 || (pf != PF_420F && pf != PF_420V && pf != PF_BGRA)) {
  if (out->delivered == 0 && out->said_shape == false) {
   out->said_shape = true;
   say("%s: %ux%u %c%c%c%c buffers are not for this ISP", out->name, width, height, pf >> 24, pf >> 16 & 255,
       pf >> 8 & 255, pf & 255);
  }
  CVPixelBufferRelease(pb);
  return;
 }
 CVPixelBufferLockBaseAddress(pb, 0);
 struct render_job job = { isp, f, raw, pb, pf, width, height, 8, strcmp(out->name, "StillImage") == 0 && pf != PF_BGRA };
 uint64_t began = mach_absolute_time();
 dispatch_apply_f(job.pieces, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), &job, render_piece);
 if (job.still) {
  static mach_timebase_info_data_t tb;
  if (tb.denom == 0) {
   mach_timebase_info(&tb);
  }
  say("%s: %ux%u made of every pixel in %.0f ms", out->name, width, height,
      (double)(mach_absolute_time() - began) * tb.numer / tb.denom / 1e6);
 }
 CVPixelBufferUnlockBaseAddress(pb, 0);
 CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);
 CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
 CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
     kCVAttachmentMode_ShouldPropagate);
 if (metadata != NULL) {
  CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(NULL, 0, metadata);
  CVBufferSetAttachment(pb, md_attachment, m, kCVAttachmentMode_ShouldPropagate);
  CFRelease(m);
 }
 CMTime pts = CMClockMakeHostTimeFromSystemUnits(f->time);
 ((pixel_buffer_handler)out->handler)(pb, pts);
 if (strcmp(out->name, "StillImage") != 0) {
  if (out->held[out->held_next] != NULL) {
   CVPixelBufferRelease(out->held[out->held_next]);
  }
  out->held[out->held_next] = pb;
  out->held_next = (out->held_next + 1) % HELD;
 } else {
  CVPixelBufferRelease(pb);
 }
 if (out->delivered++ == 0) {
  say("%s: the first picture given (%ux%u %c%c%c%c)", out->name, width, height, pf >> 24, pf >> 16 & 255,
      pf >> 8 & 255, pf & 255);
 }
}
static float
lux_index(const struct isp *isp, const struct redwood_camera_frame *f)
{
 double exposure = f->exposure_lines * (double)f->line_ns / 1e9 * f->gain / f->gain_unity * isp->digital;
 return (float)(197 + 46 * log2(exposure > 1e-6 ? exposure / 0.0333 : 1e-6 / 0.0333));
}
static float
colour_temperature(const struct isp *isp)
{
 static const float ratio[3] = { 0.8f, 1.3f, 2.0f }, kelvin[3] = { 6500, 4000, 2850 };
 float r = isp->wb[0] > 0 ? isp->wb[2] / isp->wb[0] : 1, cct = isp_cct(isp);
 if (cct > 0) {
  return cct;
 }
 if (r <= ratio[0]) {
  return kelvin[0];
 }
 for (int i = 1; i < 3; i++) {
  if (r <= ratio[i]) {
   float t = logf(r / ratio[i - 1]) / logf(ratio[i] / ratio[i - 1]);
   return kelvin[i - 1] + t * (kelvin[i] - kelvin[i - 1]);
  }
 }
 return kelvin[2];
}
enum { AF_WATCH, AF_STEADY, AF_CLIMB, AF_SWEEP, AF_FINE_SEARCH, AF_SETTLE };
#define AF_NEAR 16
#define AF_FAR 768
#define AF_STEP 48
#define AF_FINE 12
struct af {
 int state;
 uint32_t at, target;
 unsigned wait;
 int way;
 bool turned;
 unsigned worse;
 uint32_t start, best, fine_to;
 float best_sharpness;
 float sharpness, level;
 float last_sharpness, last_level;
 unsigned unlike, steady, steps;
 bool now;
 bool sweep;
 float sum_sharpness, sum_level;
 unsigned measured;
};
static void
af_go(int device, struct af *af, uint32_t target)
{
 struct redwood_camera_command c = { .command = REDWOOD_CAMERA_FOCUS };
 af->target = target < AF_NEAR ? AF_NEAR : target > AF_FAR ? AF_FAR : target;
 c.a = af->at < af->target ? (af->target - af->at > AF_STEP ? af->at + AF_STEP : af->target) :
     (af->at - af->target > AF_STEP ? af->at - AF_STEP : af->target);
 af->at = c.a;
 af->wait = af->at == af->target ? 1 : 0;
 af->steps++;
 af->sum_sharpness = af->sum_level = 0;
 af->measured = 0;
 (void)write(device, &c, sizeof(c));
}
static void
af_frame(int device, struct af *af, const struct redwood_camera_frame *f, const uint8_t *raw, const float window[4], bool locked)
{
 float level, sharpness;
 unsigned needed = f->gain >= 16 * f->gain_unity ? 2 : 1;
 if (f->focus == REDWOOD_CAMERA_NO_FOCUS) {
  return;
 }
 if (af->at == 0 && af->target == 0) {
  af->at = af->target = f->focus;
 }
 if (af->wait > 0) {
  af->wait--;
  return;
 }
 if (af->at != af->target) {
  af_go(device, af, af->target);
  return;
 }
 sharpness = isp_sharpness(f, raw, window[0], window[1], window[2], window[3], &level);
 af->sum_sharpness += sharpness;
 af->sum_level += level;
 if (++af->measured < needed) {
  return;
 }
 sharpness = af->sum_sharpness / af->measured;
 level = af->sum_level / af->measured;
 af->sum_sharpness = af->sum_level = 0;
 af->measured = 0;
 switch (af->state) {
 case AF_WATCH:
  if (locked || (af->sharpness > 0 && sharpness > af->sharpness * 0.6f && level > af->level * 0.7f &&
      level < af->level * 1.4f)) {
   af->unlike = 0;
   return;
  }
  if (af->sharpness > 0 && ++af->unlike < 30) {
   return;
  }
  af->state = AF_STEADY;
  af->steady = 0;
  af->last_sharpness = sharpness;
  af->last_level = level;
  return;
 case AF_STEADY:
  if (fabsf(sharpness - af->last_sharpness) < 0.15f * af->last_sharpness && fabsf(level - af->last_level) < 0.1f * af->last_level) {
   af->steady++;
  } else {
   af->steady = 0;
  }
  af->last_sharpness = sharpness;
  af->last_level = level;
  if (af->steady < 10 && !af->now) {
   return;
  }
  af->now = false;
  if (af->sweep) {
   af->sweep = false;
   af->state = AF_SWEEP;
   af->best_sharpness = -1;
   af->worse = 0;
   af->steps = 0;
   af->way = af->at - AF_NEAR < AF_FAR - af->at ? 1 : -1;
   af_go(device, af, af->way > 0 ? AF_NEAR : AF_FAR);
   return;
  }
  af->state = AF_CLIMB;
  af->best = af->start = af->at;
  af->best_sharpness = sharpness;
  af->way = af->at + AF_STEP > AF_FAR ? -1 : 1;
  af->turned = false;
  af->worse = 0;
  af->steps = 0;
  af_go(device, af, (uint32_t)((int)af->at + af->way * AF_STEP));
  return;
 case AF_CLIMB: {
  int next = (int)af->at + af->way * AF_STEP;
  bool end = next < AF_NEAR || next > AF_FAR;
  if (sharpness > af->best_sharpness * 1.02f) {
   af->best = af->at;
   af->best_sharpness = sharpness;
   af->worse = 0;
  } else {
   af->worse++;
  }
  if (af->worse < 2 && !end) {
   af_go(device, af, (uint32_t)next);
   return;
  }
  next = (int)af->best - af->way * AF_STEP;
  if (!af->turned && af->best == af->start && next >= AF_NEAR && next <= AF_FAR) {
   af->turned = true;
   af->way = -af->way;
   af->worse = 0;
   af_go(device, af, (uint32_t)next);
   return;
  }
  af->state = AF_FINE_SEARCH;
  af->fine_to = af->best + 2 * AF_FINE > AF_FAR ? AF_FAR : af->best + 2 * AF_FINE;
  af_go(device, af, af->best > 2 * AF_FINE + AF_NEAR ? af->best - 2 * AF_FINE : AF_NEAR);
  return;
 }
 case AF_SWEEP:
  say("focus: sweep: %u %.4f", af->at, sharpness);
  if (sharpness > af->best_sharpness) {
   af->best = af->at;
   af->best_sharpness = sharpness;
   af->worse = 0;
  } else if (sharpness < af->best_sharpness * 0.85f) {
   af->worse++;
  }
  if (af->worse < 2 && (int)af->at + af->way * AF_STEP >= AF_NEAR && (int)af->at + af->way * AF_STEP <= AF_FAR) {
   af_go(device, af, (uint32_t)((int)af->at + af->way * AF_STEP));
   return;
  }
  af->state = AF_FINE_SEARCH;
  af->fine_to = af->best + 2 * AF_FINE > AF_FAR ? AF_FAR : af->best + 2 * AF_FINE;
  af_go(device, af, af->best > 2 * AF_FINE + AF_NEAR ? af->best - 2 * AF_FINE : AF_NEAR);
  return;
 case AF_FINE_SEARCH:
  if (sharpness > af->best_sharpness) {
   af->best = af->at;
   af->best_sharpness = sharpness;
  }
  if (af->at + AF_FINE <= af->fine_to) {
   af_go(device, af, af->at + AF_FINE);
   return;
  }
  af->state = AF_SETTLE;
  af_go(device, af, af->best);
  return;
 case AF_SETTLE:
  af->state = AF_WATCH;
  af->sharpness = sharpness;
  af->level = level;
  af->unlike = 0;
  say("focus: the lens at %u after %u moves (sharpness %.3f)", af->at, af->steps, sharpness);
  return;
 }
}
static void
load_tuning(struct isp *isp, uint32_t sensor)
{
 static const char *const names[] = { "ov02b10", "s5khm2sd", "ov16a1q", "s5k4h7" };
 char path[96];
 if (sensor >= sizeof(names) / sizeof(names[0])) {
  return;
 }
 snprintf(path, sizeof(path), "/redwood/fw/camera-%s.isp", names[sensor]);
 if (isp_load_tuning(isp, path)) {
  say("the %s's colour tuning from %s: %u colour matrices' lights, %u lens shadings'", names[sensor], path,
      isp->tuning->ccm.lights, isp->tuning->lsc.lights);
 } else {
  say("no colour tuning for the %s (%s): no colour matrix, no lens shading", names[sensor], path);
  isp_set_ccm(isp, (const float[9]){ 1, 0, 0, 0, 1, 0, 0, 0, 1 });
  isp->lsc.on = false;
 }
}
static void *
worker(void *context)
{
 CMBaseObjectRef o = context;
 struct stream *s = stream_of(o);
 struct redwood_camera_frame head;
 struct isp isp;
 struct isp_stats st;
 uint8_t *buffer = NULL;
 size_t bytes = 0;
 uint32_t tuned_for = ~0u;
 bool switched = false;
 float lux = -1, cct = -1;
 struct af af = { 0 };
 int device = open(REDWOOD_CAMERA_DEVICE, O_RDWR), failures = 0;
 pthread_setname_np("redwood-camera");
 if (device < 0) {
  say("no %s (%s): no frames", REDWOOD_CAMERA_DEVICE, strerror(errno));
  return NULL;
 }
 isp_init(&isp);
 while (s->running && failures < 5) {
  ssize_t n;
  if (buffer == NULL) {
   n = read(device, &head, sizeof(head));
   if (n != (ssize_t)sizeof(head)) {
    failures += n < 0 && errno == EAGAIN ? 0 : 1;
    if (n < 0 && errno == EIO) {
     say("the kernel's camera has had a fault: no frames");
     break;
    }
    continue;
   }
   if (head.sensor != s->camera->sensor) {
    if (!switched) {
     struct redwood_camera_command c = { .command = REDWOOD_CAMERA_SENSOR, .a = s->camera->sensor, .b = ~0u };
     switched = true;
     say("the kernel's frames are camera %u's: to %u, the %s", head.sensor, s->camera->sensor, s->camera->name);
     (void)write(device, &c, sizeof(c));
    }
    continue;
   }
   bytes = sizeof(head) + (size_t)head.stride * head.height;
   buffer = malloc(bytes);
   say("frames of %ux%u from the kernel (the %s)", head.width, head.height, s->camera->name);
   continue;
  }
  n = read(device, buffer, bytes);
  if (n != (ssize_t)bytes) {
   if (n < 0 && errno == EIO) {
    say("the kernel's camera has had a fault: no more frames");
    break;
   }
   failures += n < 0 && errno == EAGAIN ? 0 : 1;
   continue;
  }
  failures = 0;
  const struct redwood_camera_frame *f = (const void *)buffer;
  const uint8_t *raw = buffer + f->header_bytes;
  struct redwood_camera_command c;
  isp_measure(&isp, f, raw, &st);
  isp_awb(&isp, &st);
  if (f->sensor != tuned_for) {
   tuned_for = f->sensor;
   load_tuning(&isp, f->sensor);
  }
  lux = lux < 0 ? lux_index(&isp, f) : lux * 0.9f + lux_index(&isp, f) * 0.1f;
  cct = cct < 0 ? colour_temperature(&isp) : cct * 0.97f + colour_temperature(&isp) * 0.03f;
  isp_tune(&isp, lux, cct);
  isp.zoom = s->zoom;
  isp.turned = s->camera->turned;
  if (isp_ae(&isp, f, &st, &c)) {
   (void)write(device, &c, sizeof(c));
  }
  if (s->af_now) {
   s->af_now = false;
   af.state = AF_STEADY;
   af.now = true;
   af.sweep = true;
  }
  af_frame(device, &af, f, raw, s->af_window, s->af_locked);
  s->frames++;
  CFDictionaryRef metadata = frame_metadata(&isp, f, &st, s->port);
  if (metadata != NULL) {
   pthread_mutex_lock(&s->metadata_lock);
   if (s->metadata != NULL) {
    CFRelease(s->metadata);
   }
   s->metadata = CFRetain(metadata);
   pthread_mutex_unlock(&s->metadata_lock);
  }
  pthread_mutex_lock(&s->lock);
  for (int i = 0; s->graph_running && i < OUTPUTS; i++) {
   struct output *out = &s->out[i];
   bool still = strcmp(out->name, "StillImage") == 0;
   if (out->handler == NULL || out->config == NULL || !(out->enabled || i == 0)) {
    continue;
   }
   if (still && s->stills_wanted == 0) {
    continue;
   }
   deliver(s, out, &isp, f, raw, metadata);
   if (still) {
    s->stills_wanted--;
    say("the photograph is given (%llu so far)", (unsigned long long)++s->stills_given);
   }
  }
  pthread_mutex_unlock(&s->lock);
  if (metadata != NULL) {
   CFRelease(metadata);
  }
  if (s->frames == 1 || s->frames % 300 == 0) {
   say("frame %llu: exposure %.1f ms, gain %.1fx, digital %.2fx, green %.0f above black; white balance R %.2f "
       "B %.2f, lux index %.0f, about %.0f K", f->sequence, f->exposure_lines * (double)f->line_ns / 1e6,
       (double)f->gain / f->gain_unity, isp.digital, st.mean[1], isp.wb[0], isp.wb[2], lux, cct);
  }
 }
 free(buffer);
 close(device);
 say("the frames stopped (%llu)", s->frames);
 return NULL;
}
static CMBaseObjectRef all_streams[2];
static OSStatus stream_stop(CMBaseObjectRef o);
static OSStatus
stream_begin(CMBaseObjectRef o, bool graph)
{
 struct stream *s = stream_of(o);
 if (graph) {
  pthread_mutex_lock(&s->lock);
  s->graph_running = true;
  pthread_mutex_unlock(&s->lock);
  say("stream: the graph starts the stream (format %d)", s->format_index);
 }
 if (s->running) {
  return 0;
 }
 for (int i = 0; i < 2; i++) {
  if (all_streams[i] != NULL && all_streams[i] != o && stream_of(all_streams[i])->running) {
   say("stream: the %s's frames stop for the %s's", stream_of(all_streams[i])->camera->name, s->camera->name);
   (void)stream_stop(all_streams[i]);
  }
 }
 s->running = true;
 s->frames = 0;
 say("stream: frames begin (format %d)", s->format_index);
 if (pthread_create(&s->worker, NULL, worker, o) != 0) {
  s->running = false;
  return ERR_UNSUPPORTED;
 }
 s->worker_started = true;
 return 0;
}
static OSStatus
stream_start(CMBaseObjectRef o)
{
 return stream_begin(o, true);
}
static OSStatus
stream_stop(CMBaseObjectRef o)
{
 struct stream *s = stream_of(o);
 say("stream: stop (%llu frames, %llu pictures given)", (unsigned long long)s->frames,
     (unsigned long long)s->out[0].delivered);
 pthread_mutex_lock(&s->lock);
 s->graph_running = false;
 for (int i = 0; i < OUTPUTS; i++) {
  let_go(&s->out[i]);
 }
 pthread_mutex_unlock(&s->lock);
 s->running = false;
 if (s->worker_started) {
  pthread_join(s->worker, NULL);
  s->worker_started = false;
 }
 return 0;
}
static void
add_absent(const void *key, const void *value, void *context)
{
 if (!CFDictionaryContainsKey(context, key)) {
  CFDictionarySetValue(context, key, value);
 }
}
static OSStatus
stream_add_attachments(CMBaseObjectRef o, CMSampleBufferRef sbuf, CFDictionaryRef options)
{
 struct stream *s = stream_of(o);
 static bool said;
 CVImageBufferRef image = sbuf != NULL ? CMSampleBufferGetImageBuffer(sbuf) : NULL;
 CFTypeRef ours = image != NULL && md_attachment != NULL ? CVBufferGetAttachment(image, md_attachment, NULL) : NULL, theirs;
 CFMutableDictionaryRef m;
 if (!said) {
  said = true;
  say("stream: AddAttachmentsToSampleBuffer (options %s): the frames' metadata %s", options ? "given" : "none",
      md_attachment != NULL ? "attached" : "NOT THERE");
 }
 if (md_attachment == NULL || sbuf == NULL) {
  return 0;
 }
 if (ours != NULL && CFGetTypeID(ours) == CFDictionaryGetTypeID()) {
  CFRetain(ours);
 } else {
  pthread_mutex_lock(&s->metadata_lock);
  ours = s->metadata != NULL ? CFDictionaryCreateMutableCopy(NULL, 0, s->metadata) : NULL;
  pthread_mutex_unlock(&s->metadata_lock);
 }
 if (ours == NULL) {
  return 0;
 }
 theirs = CMGetAttachment(sbuf, md_attachment, NULL);
 if (theirs == ours) {
  CFRelease(ours);
  return 0;
 }
 if (theirs != NULL && CFGetTypeID(theirs) == CFDictionaryGetTypeID()) {
  m = CFDictionaryCreateMutableCopy(NULL, 0, theirs);
  CFDictionaryApplyFunction(ours, add_absent, m);
 } else {
  m = (CFMutableDictionaryRef)CFRetain(ours);
 }
 CMSetAttachment(sbuf, md_attachment, m, kCMAttachmentMode_ShouldPropagate);
 CFRelease(m);
 CFRelease(ours);
 return 0;
}
static OSStatus
stream_invalidate(CMBaseObjectRef o)
{
 (void)stream_stop(o);
 return 0;
}
static void
stream_finalize(CMBaseObjectRef o)
{
 struct stream *s = stream_of(o);
 (void)stream_stop(o);
 for (int i = 0; i < OUTPUTS; i++) {
  if (s->out[i].config) {
   CFRelease(s->out[i].config);
  }
  if (s->out[i].handler) {
   CFRelease(s->out[i].handler);
  }
 }
 if (s->formats) {
  CFRelease(s->formats);
 }
 if (s->supported) {
  CFRelease(s->supported);
 }
 if (s->colour_info) {
  CFRelease(s->colour_info);
 }
 if (s->metadata) {
  CFRelease(s->metadata);
 }
 if (s->torch >= 0) {
  close(s->torch);
 }
}
static CFStringRef
stream_description(CMBaseObjectRef o)
{
 (void)o;
 return CFRetain(CFSTR("<RedwoodCaptureStream: the wide camera>"));
}
static const struct base_class stream_base = {
 1, sizeof(struct stream), NULL, stream_invalidate, stream_finalize, stream_description, stream_copy_property, stream_set_property,
 NULL, NULL, NULL
};
static const struct stream_class stream_derived = { 2, stream_start, stream_stop, stream_add_attachments };
static const struct vtable stream_vtable = { NULL, &stream_base, &stream_derived };
static CMBaseObjectRef
stream_create(CFAllocatorRef allocator, const struct camera *camera, CFStringRef port)
{
 CMBaseObjectRef o = NULL;
 struct stream *s;
 if (derived_create(allocator, &stream_vtable, stream_class_id(), &o) != 0 || o == NULL) {
  say("the stream cannot be made");
  return NULL;
 }
 s = stream_of(o);
 memset(s, 0, sizeof(*s));
 pthread_mutex_init(&s->lock, NULL);
 pthread_mutex_init(&s->metadata_lock, NULL);
 s->torch = -1;
 s->min_fps = 2;
 s->zoom = 1;
 s->af_window[0] = s->af_window[1] = 0.3f;
 s->af_window[2] = s->af_window[3] = 0.7f;
 s->max_fps = 30;
 s->camera = camera;
 s->port = port;
 for (int i = 0; i < OUTPUTS; i++) {
  s->out[i].name = output_names[i];
 }
 CFDictionaryRef f[2] = { format(camera->formats[0][0], camera->formats[0][1], camera->formats[0][2], false, camera->fov),
  format(camera->formats[1][0], camera->formats[1][1], camera->formats[1][2], false, camera->fov) };
 s->formats = CFArrayCreate(NULL, (const void **)f, 2, &kCFTypeArrayCallBacks);
 s->supported = supported(stream_props, sizeof(stream_props) / sizeof(stream_props[0]));
 CFRelease(f[0]);
 CFRelease(f[1]);
 return o;
}
struct device {
 CFArrayRef streams;
 CFDictionaryRef supported;
};
static OSStatus
device_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
 struct device *d = derived_storage(o);
 CFTypeRef value = NULL;
 (void)allocator;
 if (key_is(key, "StreamArray")) {
  value = CFRetain(d->streams);
 } else if (key_is(key, "Clock")) {
  value = CFRetain(CMClockGetHostTimeClock());
 } else if (key_is(key, "SupportedPropertiesDictionary")) {
  value = CFRetain(d->supported);
 } else if (key_is(key, "SupportedSynchronizedStreamsGroups")) {
  value = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
 } else {
  say_once("device: no answer for", key, NULL);
  return ERR_NOT_AVAILABLE;
 }
 *(CFTypeRef *)out = value;
 return 0;
}
static OSStatus
device_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
 (void)o;
 say_once("device: set (not used)", key, value);
 return 0;
}
static unsigned requests, relinquishes;
static OSStatus
device_request_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
 (void)o;
 if (requests++ < 3) {
  say("device: streams asked for (%ld)%s", streams ? (long)CFArrayGetCount(streams) : 0L, options ? ", with options" : "");
  if (options != NULL) {
   say_once("device: the request's options", CFSTR("RequestStreams"), options);
  }
 }
 if (streams != NULL) {
  CFIndex n = CFArrayGetCount(streams);
  for (CFIndex i = 0; i < n; i++) {
   CMBaseObjectRef st = (CMBaseObjectRef)CFArrayGetValueAtIndex(streams, i);
   if (st != NULL) {
    (void)stream_begin(st, false);
   }
  }
 }
 return 0;
}
static OSStatus
device_relinquish_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
 (void)o;
 (void)options;
 if (relinquishes++ < 3) {
  say("device: streams given back (%ld)", streams ? (long)CFArrayGetCount(streams) : 0L);
 } else if (relinquishes % 1000 == 0) {
  say("device: streams given back %u times", relinquishes);
 }
 return 0;
}
static OSStatus
device_invalidate(CMBaseObjectRef o)
{
 (void)o;
 return 0;
}
static void
device_finalize(CMBaseObjectRef o)
{
 struct device *d = derived_storage(o);
 if (d->streams) {
  CFRelease(d->streams);
 }
 if (d->supported) {
  CFRelease(d->supported);
 }
}
static CFStringRef
device_description(CMBaseObjectRef o)
{
 (void)o;
 return CFRetain(CFSTR("<RedwoodCaptureDevice: the POCO X5 Pro's wide camera>"));
}
static const struct base_class device_base = {
 1, sizeof(struct device), NULL, device_invalidate, device_finalize, device_description, device_copy_property, device_set_property,
 NULL, NULL, NULL
};
static const struct device_class device_derived = { 3, NULL, device_request_streams, device_relinquish_streams, NULL };
static const struct vtable device_vtable = { NULL, &device_base, &device_derived };
OSStatus redwood_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out);
OSStatus
redwood_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out)
{
 static bool found, usable;
 CMBaseObjectRef device = NULL, stream[2];
 struct device *d;
 (void)options;
 if (!found) {
  found = true;
  usable = find_functions();
 }
 if (!usable || out == NULL) {
  return ERR_UNSUPPORTED;
 }
 if ((stream[0] = stream_create(allocator, &cameras[0], port_back)) == NULL) {
  return ERR_UNSUPPORTED;
 }
 if ((stream[1] = stream_create(allocator, &cameras[1], port_front)) == NULL) {
  CFRelease(stream[0]);
  return ERR_UNSUPPORTED;
 }
 if (derived_create(allocator, &device_vtable, device_class_id(), &device) != 0 || device == NULL) {
  CFRelease(stream[0]);
  CFRelease(stream[1]);
  say("the device cannot be made");
  return ERR_UNSUPPORTED;
 }
 d = derived_storage(device);
 memset(d, 0, sizeof(*d));
 d->streams = CFArrayCreate(NULL, (const void **)stream, 2, &kCFTypeArrayCallBacks);
 all_streams[0] = stream[0];
 all_streams[1] = stream[1];
 d->supported = supported(device_props, sizeof(device_props) / sizeof(device_props[0]));
 CFRelease(stream[0]);
 CFRelease(stream[1]);
 *out = device;
 say("the device made: two streams, the back camera and the front one");
 return 0;
}
