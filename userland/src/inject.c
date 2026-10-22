#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include "macan_gl.h"
#include "macan_input.h"
extern const void *_dyld_get_shared_cache_range(size_t *length);
#define CACHE_BASE 0x180000000UL
#define REDWOOD_GL "/redwood/lib/libredwood-gl.dylib"
static uintptr_t slide;
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
static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
 uintptr_t *entry = (uintptr_t *)(slot + slide);
 vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
 kern_return_t kr;
 void *original;
 if (*entry != expected + slide) {
  say("redwood: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
  return NULL;
 }
 kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
 if (kr != KERN_SUCCESS) {
  say("redwood: %s: cannot make the table writable (%d)\n", what, kr);
  return NULL;
 }
 original = (void *)*entry;
 *entry = (uintptr_t)replacement;
 vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
 return original;
}
#define VT_APPLE_CLCD_DISPLAY 0x1d5b44520UL
#define VT_EAGL_SERVER 0x1d5b45c58UL
#define VSLOT(vtable,offset) ((vtable) + 0x10 + (offset))
#define DISPLAY_FLAGS(display) (*(uint64_t *)((char *)(display) + 0x9f2))
#define DISPLAY_B233(display) (*(uint8_t *)((char *)(display) + 0x233))
static int (*orig_is_ready)(void *display);
static int (*orig_ignore_update_p)(void *display, void *update);
static uintptr_t (*orig_begin_update)(void *display, void *update);
static uintptr_t (*orig_finish_update)(void *display, void *update, unsigned flags);
static void *(*orig_renderer)(void *server);
static uintptr_t (*orig_render_update)(void *server, void *update, unsigned *flags);
static unsigned calls_ready, calls_ignore, calls_begin, calls_finish, calls_render;
static int last_ready = -1, last_ignore = -1;
static int
my_is_ready(void *display)
{
 int ready = orig_is_ready(display);
 if (ready != last_ready || calls_ready < 3) {
  say("redwood: is_ready -> %d (call %u, flags %#llx, b233 %#x)\n", ready, calls_ready,
      (unsigned long long)DISPLAY_FLAGS(display), DISPLAY_B233(display));
  last_ready = ready;
 }
 calls_ready++;
 return ready;
}
static int
my_ignore_update_p(void *display, void *update)
{
 int ignore = orig_ignore_update_p(display, update) & 1;
 if (ignore != last_ignore || calls_ignore < 3) {
  uint64_t *q = (uint64_t *)((char *)display + 0x220);
  say("redwood: ignore_update_p -> %d (call %u, flags %#llx, update flags %#x)\n", ignore, calls_ignore,
      (unsigned long long)DISPLAY_FLAGS(display), *(uint32_t *)((char *)update + 0xf0));
  say("redwood:   display +0x220: %#llx %#llx %#llx %#llx %#llx %#llx\n", (unsigned long long)q[0],
      (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
      (unsigned long long)q[5]);
  last_ignore = ignore;
 }
 calls_ignore++;
 return ignore;
}
static uintptr_t
my_begin_update(void *display, void *update)
{
 uintptr_t r = orig_begin_update(display, update);
 if (calls_begin++ < 6) {
  say("redwood: begin_update -> %#lx (flags %#llx)\n", r, (unsigned long long)DISPLAY_FLAGS(display));
 }
 return r;
}
static uintptr_t
my_finish_update(void *display, void *update, unsigned flags)
{
 uintptr_t r = orig_finish_update(display, update, flags);
 if (calls_finish++ < 6) {
  say("redwood: finish_update(%#x) -> %#lx\n", flags, r);
 }
 return r;
}
static void *
my_renderer(void *server)
{
 static int failed, said;
 void *renderer;
 if (failed) {
  return NULL;
 }
 renderer = orig_renderer(server);
 if (renderer == NULL) {
  failed = 1;
  say("redwood: no accelerated renderer; the software one will draw\n");
 } else if (!said) {
  said = 1;
  say("redwood: the server has an accelerated renderer (%p)\n", renderer);
 }
 return renderer;
}
static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
 uintptr_t r = orig_render_update(server, update, flags);
 if (calls_render++ < 6) {
  say("redwood: render_update -> %#lx (flags %#x)\n", r, flags ? *flags : 0);
 }
 return r;
}
#define VT_GLES_CONTEXT 0x1d5b3b540UL
static int gl_loaded;
static void (*gl_function_refused)(unsigned function);
static int (*orig_function_supported)(void *context);
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
 ucontext_t *context = context_in;
 uintptr_t pc = 0, lr = 0;
 if (context != NULL && context->uc_mcontext != NULL) {
  pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
  lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
 }
 char frames[8 * 20 + 1] = "";
 size_t used = 0;
 uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;
 for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
  uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
  if (ret == 0) {
   break;
  }
  used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
  fp = next > fp ? next : 0;
 }
 say("redwood: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
     info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
 signal(signal_number, SIG_DFL);
 raise(signal_number);
}
static void
watch_for_faults(void)
{
 struct sigaction action;
 memset(&action, 0, sizeof(action));
 action.sa_sigaction = say_the_fault;
 action.sa_flags = SA_SIGINFO | SA_RESETHAND;
 sigaction(SIGSEGV, &action, NULL);
 sigaction(SIGBUS, &action, NULL);
 sigaction(SIGABRT, &action, NULL);
 sigaction(SIGILL, &action, NULL);
 sigaction(SIGTRAP, &action, NULL);
}
#define CA_SHADER_TABLE 0x1d5b54790UL
static const char shadow_blur_source[] =
    "uniform lowp sampler2DRect texture0;\n"
    "varying highp vec2 texcoord0, texcoord1, texcoord2, texcoord3;\n"
    "void main () {\n"
    "  lowp vec4 q0, q1, q2, q3;\n"
    "  q0 = texture2DRect(texture0, texcoord0);\n"
    "  q1 = texture2DRect(texture0, texcoord1);\n"
    "  q2 = texture2DRect(texture0, texcoord2);\n"
    "  q3 = texture2DRect(texture0, texcoord3);\n"
    "  gl_FragData[0] = (q0 + q1 + q2 + q3) * 0.25;\n"
    "}\n";
static void
give_the_renderer_a_shader(unsigned function, const char *source)
{
 uintptr_t *entry = (uintptr_t *)(CA_SHADER_TABLE + slide + 8 * (uintptr_t)function);
 vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
 if (*entry != 0) {
  say("redwood: CoreAnimation's function %u has a shader of its own; left alone\n", function);
  return;
 }
 if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
     VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
  say("redwood: CoreAnimation's function %u: cannot make the table of shaders writable\n", function);
  return;
 }
 *entry = (uintptr_t)source;
 vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
 say("redwood: CoreAnimation's function %u (\"sb3\") is given a shader of ours\n", function);
}
static uint8_t say_yes[90];
static int compile_shaders_said;
static void
functions_said_yes(const char *arguments)
{
 static const char names[] = REDWOOD_CA_FUNCTIONS;
 const char *argument = strstr(arguments, "glno=");
 for (argument = argument != NULL ? argument + 5 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
  unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);
  if (function < 90) {
   say_yes[function] = 0;
   say("redwood: the renderer will not answer that it has CoreAnimation's function %u (\"%.3s\")\n",
       function, names + 4 * function);
  }
  argument = *argument == ',' ? argument + 1 : NULL;
 }
 argument = strstr(arguments, "glyes=");
 for (argument = argument != NULL ? argument + 6 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
  unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);
  if (function < 90) {
   say_yes[function] = 1;
   say("redwood: the renderer will answer that it has CoreAnimation's function %u (\"%.3s\")\n",
       function, names + 4 * function);
  }
  argument = *argument == ',' ? argument + 1 : NULL;
 }
}
static uintptr_t (*orig_compile_shader)(void *context, const char *source);
static uintptr_t
my_compile_shader(void *context, const char *source)
{
 static unsigned told, seen;
 if (seen++ == 0) {
  say("redwood: CoreAnimation compiles its shaders through the table (the first of them, %zu characters)\n",
      source != NULL ? strlen(source) : 0);
 }
 if (compile_shaders_said && told < 24) {
  told++;
  say("redwood: CoreAnimation compiles a shader, %zu characters:\n", source != NULL ? strlen(source) : 0);
  for (size_t at = 0, length = source != NULL ? strlen(source) : 0; at < length; at += 700) {
   say("redwood:  %.700s\n", source + at);
  }
 }
 return orig_compile_shader(context, source);
}
static int
my_function_supported(void *context)
{
 static const char names[] = REDWOOD_CA_FUNCTIONS;
 static uint8_t said[256];
 int supported = orig_function_supported(context);
 uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);
 if (!supported && function < 90 && say_yes[function] && gl_loaded) {
  if (!said[function]) {
   said[function] = 1;
   say("redwood: CoreAnimation's function %u (\"%.3s\"): the renderer is made to say it has it\n",
       function, names + 4 * function);
  }
  return 1;
 }
 if (!supported && gl_function_refused != NULL) {
  gl_function_refused(function);
 }
 if (!supported && !said[function]) {
  said[function] = 1;
  say("redwood: CoreAnimation's OpenGL ES renderer has no function %u (\"%.3s\"): what needs it is not drawn\n",
      function, function < 90 ? names + 4 * function : "?");
 }
 return supported;
}
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);
static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
 uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);
 if (say_yes[function < 90 ? function : 0]) {
  static uint8_t told[90];
  if (function < 90 && !told[function]) {
   told[function] = 1;
   say("redwood: function %u takes its numbers from %#lx %#lx %#lx\n", function,
       (uintptr_t)__builtin_return_address(0) - slide,
       (uintptr_t)__builtin_return_address(1) - slide,
       (uintptr_t)__builtin_return_address(2) - slide);
  }
 }
 if (gl_function_values != NULL) {
  gl_function_values(context, function, index, components, count, values);
 }
 orig_function_uniform(context, index, components, count, values);
}
static void
window_server_fixes(void)
{
 orig_is_ready = replace_virtual("is_ready", VSLOT(VT_APPLE_CLCD_DISPLAY, 0xb0), 0x1882f0908UL, my_is_ready);
 orig_ignore_update_p = replace_virtual("ignore_update_p", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x138), 0x1882e0c90UL,
     my_ignore_update_p);
 orig_begin_update = replace_virtual("begin_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x170), 0x1882ddda4UL,
     my_begin_update);
 orig_finish_update = replace_virtual("finish_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x178), 0x1882c8568UL,
     my_finish_update);
 orig_renderer = replace_virtual("renderer", VSLOT(VT_EAGL_SERVER, 0x1a0), 0x1882e2548UL, my_renderer);
 orig_render_update = replace_virtual("render_update", VSLOT(VT_EAGL_SERVER, 0x178), 0x18835770cUL,
     my_render_update);
 if (gl_loaded) {
  orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), 0x1884a9308UL,
      my_function_supported);
  if (gl_function_values != NULL) {
   orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), 0x1884a9350UL,
       my_function_uniform);
  }
  orig_compile_shader = (uintptr_t (*)(void *, const char *))replace_virtual("compile_shader",
      VSLOT(VT_GLES_CONTEXT, 0x060), 0x1883fc808UL, my_compile_shader);
  say("redwood: the shader compiler of CoreAnimation is %s\n", orig_compile_shader ? "watched" : "NOT watched");
  watch_for_faults();
  {
   char arguments[1024] = "";
   size_t size = sizeof(arguments) - 1;
   if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
       strstr(arguments, "glsb3=1") != NULL) {
    give_the_renderer_a_shader(40, shadow_blur_source);
   }
  }
 }
 say("redwood: window server hooks %s\n", orig_is_ready && orig_ignore_update_p && orig_begin_update &&
     orig_finish_update && orig_renderer && orig_render_update ? "installed" : "INCOMPLETE");
}
static dispatch_semaphore_t input_services_made;
static void
wait_for_als_arrival(__unused id self, __unused SEL selector)
{
 if (input_services_made != NULL &&
     dispatch_semaphore_wait(input_services_made, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
  say("redwood: input: no touch screen service after five seconds; going on without\n");
 }
}
__attribute__((unused)) static void
brightness_fixes(void)
{
 Class cls = objc_getClass("BLControl");
 Method method = cls ? class_getInstanceMethod(cls, sel_registerName("waitForALSArrival")) : NULL;
 IMP original = method ? method_getImplementation(method) : NULL;
 if (original == NULL || (uintptr_t)original != 0x1af043734UL + slide) {
  say("redwood: -[BLControl waitForALSArrival]: found %#lx, not what was expected; left alone\n",
      original ? (uintptr_t)original - slide : 0);
  return;
 }
 method_setImplementation(method, (IMP)wait_for_als_arrival);
}
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
struct virtual_service_callbacks {
 uint64_t version;
 void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
 Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
 CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
 IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
 int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
 CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};
extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);
#define HID_CLIENT_SIMPLE 4
#define HID_CLIENT_RATE_CONTROLLED 3
#define HID_PAGE_DIGITIZER 0x0d
#define HID_USAGE_TOUCH_SCREEN 0x04
#define HID_PAGE_CONSUMER 0x0c
#define HID_USAGE_CONSUMER_CONTROL 0x01
#define HID_USAGE_POWER 0x30
#define HID_USAGE_MENU 0x40
#define HID_USAGE_VOLUME_UP 0xe9
#define HID_USAGE_VOLUME_DOWN 0xea
#define DIGITIZER_HAND 3
#define DIGITIZER_RANGE 0x1
#define DIGITIZER_TOUCH 0x2
#define DIGITIZER_POSITION 0x4
#define DIGITIZER_FROM_EDGE_TIP 0x800
#define FIELD_IS_BUILT_IN 0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019
#define MAX_FINGERS 10
#define EDGE_SIDE (16.0 / 375)
#define EDGE_TOP (16.0 / 812)
#define EDGE_BOTTOM (34.0 / 812)
static struct {
 bool touching, edge;
 double x, y;
} fingers[MAX_FINGERS];
static CFNumberRef
number(int value)
{
 return CFNumberCreate(NULL, kCFNumberIntType, &value);
}
struct service {
 CFMutableDictionaryRef properties;
 volatile bool opened;
};
static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
 static CFMutableSetRef asked;
 CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);
 if (asked == NULL) {
  asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
 }
 if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
  char name[96] = "?";
  CFSetAddValue(asked, key);
  CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
  say("redwood: input: the event server asks for property \"%s\", which we do not have\n", name);
 }
 return value ? CFRetain(value) : NULL;
}
#define TOUCH_MODE_WAKE 0xfe
#define TOUCH_MODE_OFF 0xff
static volatile int touch_mode;
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
 static CFMutableSetRef told;
 char name[96] = "?", text[160] = "?";
 CFStringRef description;
 if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
     CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
  ((struct service *)refcon)->opened = true;
 }
 if (key != NULL && value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
  int mode = 0;
  CFNumberGetValue(value, kCFNumberIntType, &mode);
  if (mode != touch_mode) {
   say("redwood: input: touch detection mode %#x\n", mode);
  }
  touch_mode = mode;
 }
 if (told == NULL) {
  told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
 }
 if (key != NULL && !CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
  CFSetAddValue(told, key);
  CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
  description = value ? CFCopyDescription(value) : NULL;
  if (description != NULL) {
   CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
   CFRelease(description);
  }
  say("redwood: input: the event server sets \"%s\" = %s\n", name, text);
 }
 return true;
}
static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}
static const struct virtual_service_callbacks service_callbacks = {
 .version = 2, .notification = service_notification, .setProperty = service_set_property,
 .copyProperty = service_copy_property,
};
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
 struct service *made = calloc(1, sizeof(*made));
 IOHIDServiceClientRef service = NULL;
 CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
     &kCFTypeDictionaryValueCallBacks);
 CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
     &kCFTypeDictionaryValueCallBacks);
 CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
 CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
 CFArrayRef pairs;
 CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
 CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
 pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
 CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
 CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
 CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
 CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
 CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
 CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
 CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
 CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("redwood"));
 CFDictionarySetValue(properties, CFSTR("Product"), name);
 CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
 CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
 if (display) {
  CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
 }
 made->properties = properties;
 for (int attempt = 1; attempt <= 20; attempt++) {
  made->opened = false;
  service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
  if (service == NULL) {
   break;
  }
  for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
   usleep(100000);
  }
  if (made->opened) {
   break;
  }
  say("redwood: input: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
  IOHIDVirtualServiceClientRemove(service);
  CFRelease(service);
  service = NULL;
 }
 return service;
}
static void
send_touch(IOHIDServiceClientRef screen, const struct redwood_touch *t)
{
 uint32_t change = t->type == REDWOOD_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
 IOHIDEventRef hand, finger;
 bool any = false;
 unsigned f = t->finger;
 if (f >= MAX_FINGERS || (t->type != REDWOOD_TOUCH_DOWN && !fingers[f].touching)) {
  return;
 }
 fingers[f].touching = t->type != REDWOOD_TOUCH_UP;
 fingers[f].x = t->x / 65535.0;
 fingers[f].y = t->y / 65535.0;
 if (t->type == REDWOOD_TOUCH_DOWN) {
  fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
      fingers[f].y > 1 - EDGE_BOTTOM;
 }
 for (unsigned i = 0; i < MAX_FINGERS; i++) {
  any |= fingers[i].touching;
 }
 hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
     fingers[f].y, 0, 0, 0, 0, any, any, 0);
 IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
 IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
 for (unsigned i = 0; i < MAX_FINGERS; i++) {
  if (!fingers[i].touching && i != f) {
   continue;
  }
  finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
      (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
      fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
  IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
  IOHIDEventAppendEvent(hand, finger, 0);
  CFRelease(finger);
 }
 IOHIDVirtualServiceClientDispatchEvent(screen, hand);
 CFRelease(hand);
}
static void
publish_lock_button(bool down)
{
 static int token = NOTIFY_TOKEN_INVALID;
 if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.redwood.button.lock", &token) != NOTIFY_STATUS_OK) {
  token = NOTIFY_TOKEN_INVALID;
  return;
 }
 notify_set_state(token, down ? 2 : 1);
}
static void
send_button(IOHIDServiceClientRef buttons, const struct redwood_touch *t)
{
 uint32_t usage = t->finger == REDWOOD_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == REDWOOD_BUTTON_HOME ? HID_USAGE_MENU :
     t->finger == REDWOOD_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == REDWOOD_BUTTON_VOLUME_DOWN ?
     HID_USAGE_VOLUME_DOWN : 0;
 IOHIDEventRef event;
 if (usage == 0) {
  return;
 }
 if (t->finger == REDWOOD_BUTTON_LOCK) {
  publish_lock_button(t->type == REDWOOD_BUTTON_DOWN);
 }
 event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == REDWOOD_BUTTON_DOWN, 0);
 IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
 IOHIDVirtualServiceClientDispatchEvent(buttons, event);
 CFRelease(event);
}
static bool touch_latency;
static mach_timebase_info_data_t timebase;
static uint64_t
micros(uint64_t absolute)
{
 return absolute * timebase.numer / timebase.denom / 1000;
}
static void *
input_thread(__unused void *arg)
{
 static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
 IOHIDEventSystemClientRef client = NULL;
 IOHIDServiceClientRef screen = NULL, buttons = NULL;
 struct redwood_touch t;
 unsigned count = 0, waking = 0;
 int device;
 pthread_setname_np("redwood input");
 {
  char arguments[1024] = "";
  size_t size = sizeof(arguments) - 1;
  if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
   touch_latency = strstr(arguments, "touch_lat=1") != NULL;
  }
  mach_timebase_info(&timebase);
 }
 device = open(REDWOOD_TOUCH_DEVICE, O_RDONLY);
 if (device < 0) {
  say("redwood: input: %s: %s\n", REDWOOD_TOUCH_DEVICE, strerror(errno));
  return NULL;
 }
 sleep(3);
 for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
  client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
  if (client == NULL) {
   say("redwood: input: no HID client of type %d\n", types[i]);
   continue;
  }
  IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("redwood.input", NULL));
  screen = make_service(client, "Redwood touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
  if (screen == NULL) {
   say("redwood: input: a HID client of type %d may not create services\n", types[i]);
  }
 }
 if (screen != NULL) {
  buttons = make_service(client, "Redwood buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
 }
 say("redwood: input: touch screen service %p, buttons %p\n", (void *)screen, (void *)buttons);
 dispatch_semaphore_signal(input_services_made);
 if (screen == NULL) {
  return NULL;
 }
 while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
  uint64_t read_at = mach_absolute_time();
  if (count++ < 4) {
   say("redwood: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
  }
  if (t.type > REDWOOD_TOUCH_UP) {
   if (buttons != NULL) {
    send_button(buttons, &t);
   }
  } else if (t.type == REDWOOD_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE && buttons != NULL) {
   struct redwood_touch press = { .time = t.time, .type = REDWOOD_BUTTON_DOWN, .finger = REDWOOD_BUTTON_LOCK };
   send_button(buttons, &press);
   press.type = REDWOOD_BUTTON_UP;
   send_button(buttons, &press);
   waking |= 1u << (t.finger & 31);
  } else if (waking & (1u << (t.finger & 31))) {
   if (t.type == REDWOOD_TOUCH_UP) {
    waking &= ~(1u << (t.finger & 31));
   }
  } else {
   send_touch(screen, &t);
   if (touch_latency) {
    say("tl: i %u posted %llu read %llu sent %llu\n", t.type, micros(t.time), micros(read_at),
        micros(mach_absolute_time()));
   }
  }
 }
 say("redwood: input: %s: %s\n", REDWOOD_TOUCH_DEVICE, strerror(errno));
 return NULL;
}
static void
input_start(void)
{
 pthread_t thread;
 input_services_made = dispatch_semaphore_create(0);
 if (pthread_create(&thread, NULL, input_thread, NULL) == 0) {
  pthread_detach(thread);
 }
}
static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
 return NO;
}
static void
container_manager_fixes(void)
{
 Class cls = objc_getClass("MCMUserIdentityCache");
 Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
 IMP original = method ? method_getImplementation(method) : NULL;
 if (original == NULL || (uintptr_t)original != 0x1c3ea6298UL + slide) {
  say("redwood: +[MCMUserIdentityCache personasAreSupported]: found %#lx, not what was expected; left alone\n",
      original ? (uintptr_t)original - slide : 0);
  return;
 }
 method_setImplementation(method, (IMP)personas_are_supported);
 say("redwood: container manager: no personas\n");
}
static void
opengl_es(void)
{
 char arguments[1024] = "";
 size_t size = sizeof(arguments) - 1;
 const char *argument;
 io_service_t gpu;
 void *library;
 int wanted = -1, told = 0;
 if (access(REDWOOD_GL, R_OK) != 0) {
  return;
 }
 if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
  functions_said_yes(arguments);
  compile_shaders_said = strstr(arguments, "glcompile=1") != NULL;
  for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
   if (argument == arguments || argument[-1] == ' ') {
    wanted = argument[3] != '0';
    told = 1;
   }
  }
 }
 if (wanted < 0) {
  gpu = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));
  wanted = gpu != 0;
  if (gpu != 0) {
   IOObjectRelease(gpu);
  }
 }
 if (!wanted) {
  say("redwood: %s: CoreAnimation keeps its software renderer\n", told ? "gl=0" : "no GPU to draw with");
  return;
 }
 if ((library = dlopen(REDWOOD_GL, RTLD_NOW)) == NULL) {
  say("redwood: %s: %s\n", REDWOOD_GL, dlerror());
  return;
 }
 gl_loaded = 1;
 gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(library,
     "redwood_gl_function_values");
 gl_function_refused = (void (*)(unsigned))dlsym(library, "redwood_gl_function_refused");
}
__attribute__((constructor))
static void
redwood_init(void)
{
 const char *name = getprogname();
 size_t length = 0;
 const void *cache = _dyld_get_shared_cache_range(&length);
 if (name == NULL || cache == NULL) {
  return;
 }
 slide = (uintptr_t)cache - CACHE_BASE;
 if (strcmp(name, "backboardd") == 0) {
  char arguments[1024] = "";
  size_t size = sizeof(arguments) - 1;
  if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "ca_vbl=1") != NULL) {
   setenv("CA_FORCE_TIMER", "1", 1);
   say("redwood: ca_vbl=1: CoreAnimation is to take the display driver's refresh notifications\n");
  }
  for (const char *argument = arguments; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
   char setting[128];
   size_t length = strcspn(argument + 6, " ");
   char *equals;
   if ((argument != arguments && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
    continue;
   }
   memcpy(setting, argument + 6, length);
   setting[length] = 0;
   if ((equals = strchr(setting, '=')) != NULL) {
    *equals = 0;
    setenv(setting, equals + 1, 1);
    say("redwood: caenv: %s=%s for the window server\n", setting, equals + 1);
   }
  }
  opengl_es();
  window_server_fixes();
  input_start();
 } else if (strncmp(name, "containermanagerd", 17) == 0) {
  container_manager_fixes();
 }
}
