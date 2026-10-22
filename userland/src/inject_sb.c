#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
static int say_fd = 2;
static void
say(const char *format, ...)
{
 char line[1280];
 va_list ap;
 int n;
 va_start(ap, format);
 n = vsnprintf(line, sizeof(line), format, ap);
 va_end(ap);
 if (n > (int)sizeof(line) - 1) {
  n = sizeof(line) - 1;
 }
 write(say_fd, line, (size_t)n);
}
#define CA_COMMIT_ORDER 2000000
static uint64_t main_woke, commit_began;
static mach_timebase_info_data_t timebase;
static uint64_t
now_us(void)
{
 return mach_absolute_time() * timebase.numer / timebase.denom / 1000;
}
static void
main_loop_turn(__unused CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
 if (activity == kCFRunLoopAfterWaiting) {
  main_woke = now_us();
 } else if (activity == kCFRunLoopBeforeWaiting && info == NULL) {
  commit_began = now_us();
 } else if (activity == kCFRunLoopBeforeWaiting && main_woke != 0) {
  say("tl: a %llu %llu %llu\n", main_woke, commit_began, now_us());
  main_woke = 0;
 }
}
static void
watch_main_loop(void)
{
 char args[1024] = "";
 size_t size = sizeof(args) - 1;
 const char *arg;
 if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (arg = strstr(args, "touch_lat=1")) == NULL ||
     (arg != args && arg[-1] != ' ')) {
  return;
 }
 mach_timebase_info(&timebase);
 CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting, true, LONG_MIN,
     main_loop_turn, NULL), kCFRunLoopCommonModes);
 CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, CA_COMMIT_ORDER - 1,
     main_loop_turn, NULL), kCFRunLoopCommonModes);
 {
  CFRunLoopObserverContext after = { .info = (void *)1 };
  CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true,
      CA_COMMIT_ORDER + 1, main_loop_turn, &after), kCFRunLoopCommonModes);
 }
}
extern const void *_dyld_get_shared_cache_range(size_t *length);
#define CACHE_BASE 0x180000000UL
static uintptr_t slide;
static IMP
replace_method_of_kind(char kind, const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
 Class cls = objc_getClass(class_name);
 Method method = cls == NULL ? NULL : kind == '+' ? class_getClassMethod(cls, sel_registerName(selector)) :
     class_getInstanceMethod(cls, sel_registerName(selector));
 IMP original = method ? method_getImplementation(method) : NULL;
 if (original == NULL || (uintptr_t)original != expected + slide) {
  say("redwood: %c[%s %s]: found %#lx, expected %#lx; left alone\n", kind, class_name, selector,
      original ? (uintptr_t)original - slide : 0, expected);
  return NULL;
 }
 method_setImplementation(method, replacement);
 return original;
}
static IMP
replace_method(const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
 return replace_method_of_kind('-', class_name, selector, expected, replacement);
}
#define LOCKDOWN_STATE(manager) (*(int *)((char *)(manager) + 8))
#define LOCKDOWN_ACTIVATED 2
static void
setup_activation_state(id self, __unused SEL selector)
{
 LOCKDOWN_STATE(self) = LOCKDOWN_ACTIVATED;
}
static void (*original_set_setup_required_reason)(id self, SEL selector, long reason);
static void
set_setup_required_reason(id self, SEL selector, long reason)
{
 if (reason != 0) {
  say("redwood: SpringBoard wants the setup assistant (reason %ld); not here\n", reason);
 }
 original_set_setup_required_reason(self, selector, 0);
}
#define SNAPSHOT_FORMAT_PNG 0
#define SNAPSHOT_FORMAT_JPEG 3
static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
 SEL opaque = sel_registerName("isImageOpaque");
 if (snapshot != NULL && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
     ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
  return SNAPSHOT_FORMAT_JPEG;
 }
 return SNAPSHOT_FORMAT_PNG;
}
static float (*original_battery_level)(id self, SEL sel);
static long (*original_battery_state)(id self, SEL sel);
static BOOL (*original_battery_view_shows_percentage)(id self, SEL sel);
static void (*original_battery_view_set_shows_percentage)(id self, SEL sel, BOOL shows);
static BOOL (*original_battery_view_should_show_bolt)(id self, SEL sel);
static double (*original_battery_view_charge_percent)(id self, SEL sel);
static void (*original_battery_view_set_charge_percent)(id self, SEL sel, double p);
static double (*original_ring_charging_level)(id self, SEL sel);
static int
sysctl_int(const char *name)
{
 int value = -1;
 size_t size = sizeof(value);
 sysctlbyname(name, &value, &size, NULL, 0);
 return value;
}
static bool
battery_power_source(void)
{
 static bool found;
 io_service_t service;
 if (!found && (service = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodBattery"))) != 0) {
  IOObjectRelease(service);
  found = true;
 }
 return found;
}
static float
battery_level(id self, SEL sel)
{
 int soc = sysctl_int("kern.redwood.battery_soc");
 if (soc < 0 || soc > 100 || battery_power_source()) {
  return original_battery_level ? original_battery_level(self, sel) : -1.0f;
 }
 return (float)soc / 100.0f;
}
static long
battery_state(id self, SEL sel)
{
 int charging = sysctl_int("kern.redwood.battery_charging");
 int present = sysctl_int("kern.redwood.battery_present");
 int plugged = sysctl_int("kern.redwood.battery_charger_present");
 int soc = sysctl_int("kern.redwood.battery_soc");
 if (charging == -1 || present == -1 || plugged == -1 || soc < 0 || soc > 100 || battery_power_source()) {
  return original_battery_state ? original_battery_state(self, sel) : 0;
 }
 if (!present) return 0;
 if (plugged && soc >= 100) return 3;
 return charging ? 2 : 1;
}
static BOOL
battery_view_shows_percentage(id self, SEL sel)
{
 int soc = sysctl_int("kern.redwood.battery_soc");
 if (soc >= 0 && soc <= 100 && !battery_power_source()) return YES;
 return original_battery_view_shows_percentage ? original_battery_view_shows_percentage(self, sel) : NO;
}
static void
battery_view_set_shows_percentage(id self, SEL sel, BOOL shows)
{
 int soc = sysctl_int("kern.redwood.battery_soc");
 original_battery_view_set_shows_percentage(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? YES : shows);
}
static BOOL
battery_view_should_show_bolt(id self, SEL sel)
{
 int charging = sysctl_int("kern.redwood.battery_charging");
 int present = sysctl_int("kern.redwood.battery_present");
 int plugged = sysctl_int("kern.redwood.battery_charger_present");
 if (charging != -1 && present != -1 && plugged != -1 && !battery_power_source()) return charging && present && plugged;
 return original_battery_view_should_show_bolt ? original_battery_view_should_show_bolt(self, sel) : NO;
}
static double
battery_view_charge_percent(id self, SEL sel)
{
 int soc = sysctl_int("kern.redwood.battery_soc");
 if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
 return original_battery_view_charge_percent ? original_battery_view_charge_percent(self, sel) : 0.0;
}
static void
battery_view_set_charge_percent(id self, SEL sel, double p)
{
 int soc = sysctl_int("kern.redwood.battery_soc");
 original_battery_view_set_charge_percent(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? (double)soc / 100.0 : p);
}
static double
ring_charging_level(id self, SEL sel)
{
 int soc = sysctl_int("kern.redwood.battery_soc");
 if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
 return original_ring_charging_level ? original_ring_charging_level(self, sel) : 0.0;
}
static void
battery_post_notifications(void)
{
 Class uidev = objc_getClass("UIDevice");
 Class notification_center = objc_getClass("NSNotificationCenter");
 id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, sel_registerName("currentDevice")) : nil;
 id center = notification_center ? ((id (*)(Class, SEL))objc_msgSend)(notification_center,
     sel_registerName("defaultCenter")) : nil;
 if (center != nil && !battery_power_source()) {
  SEL post = sel_registerName("postNotificationName:object:");
  ((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
      (id)CFSTR("UIDeviceBatteryLevelDidChangeNotification"), device);
  ((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
      (id)CFSTR("UIDeviceBatteryStateDidChangeNotification"), device);
 }
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
  battery_post_notifications();
 });
}
#define REDWOOD_GL "/redwood/lib/libredwood-gl.dylib"
static bool
gpu_draws(void)
{
 char arguments[1024] = "";
 size_t size = sizeof(arguments) - 1;
 const char *argument;
 io_service_t gpu;
 if (access(REDWOOD_GL, R_OK) != 0) {
  return false;
 }
 if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
  for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
   if (argument == arguments || argument[-1] == ' ') {
    return argument[3] != '0';
   }
  }
 }
 gpu = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));
 if (gpu != 0) {
  IOObjectRelease(gpu);
 }
 return gpu != 0;
}
static void
plain_vibrancy(void)
{
 if (gpu_draws()) {
  return;
 }
 CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
 say("redwood: SpringBoard: no GPU draws, vibrant views are plain (the lock screen's clock is white)\n");
}
#define GESTURE_BEGAN 1
#define GESTURE_ENDED 3
static void (*original_lock_long_press)(id self, SEL selector, id recognizer);
static bool
lock_button_held(void)
{
 static int token = NOTIFY_TOKEN_INVALID;
 uint64_t state = 0;
 if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.redwood.button.lock", &token) != NOTIFY_STATUS_OK) {
  token = NOTIFY_TOKEN_INVALID;
  return true;
 }
 return notify_get_state(token, &state) != NOTIFY_STATUS_OK || state != 1;
}
static void
lock_long_press(id self, SEL selector, id recognizer)
{
 static bool dropping;
 SEL state_selector = sel_registerName("state");
 long state = recognizer != NULL && class_getInstanceMethod(object_getClass(recognizer), state_selector) != NULL ?
     ((long (*)(id, SEL))objc_msgSend)(recognizer, state_selector) : 0;
 if (state == GESTURE_BEGAN) {
  dropping = !lock_button_held();
  if (dropping) {
   say("redwood: SpringBoard: a long press of the side button, which was released long ago: dropped\n");
  }
 }
 if (dropping) {
  if (state >= GESTURE_ENDED) {
   dropping = false;
  }
  return;
 }
 original_lock_long_press(self, selector, recognizer);
}
#define UNLOCK_SOURCE_COVER_SHEET 24
static void
unlock(void)
{
 Class cls = objc_getClass("SBLockScreenManager");
 SEL shared = sel_registerName("sharedInstance"), locked = sel_registerName("isUILocked");
 SEL unlock_ui = sel_registerName("unlockUIFromSource:withOptions:");
 id manager;
 if (cls == NULL || class_getClassMethod(cls, shared) == NULL || class_getInstanceMethod(cls, locked) == NULL ||
     class_getInstanceMethod(cls, unlock_ui) == NULL) {
  say("redwood: unlock: SBLockScreenManager is not what it was\n");
  return;
 }
 manager = ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
 if (manager == NULL) {
  say("redwood: unlock: no lock screen manager yet\n");
 } else if (!((BOOL (*)(id, SEL))objc_msgSend)(manager, locked)) {
  say("redwood: unlock: not locked\n");
 } else {
  BOOL done = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, NULL);
  say("redwood: unlock: asked, answer %d\n", done);
 }
}
static BOOL
poster_switcher_allowed(__unused id self, __unused SEL selector, id *explanation)
{
 if (explanation != NULL) {
  *explanation = (id)CFSTR("the poster switcher is not shown here (see inject_sb.c)");
 }
 return NO;
}
#define SEND(type,object,name) ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))
#define SEND1(type,object,name,argument_type,argument) \
 ((type (*)(id, SEL, argument_type))objc_msgSend)((id)(object), sel_registerName(name), argument)
static const char *layers_of = "";
static const char *
text_of(id object)
{
 id text = object ? SEND(id, object, "description") : NULL;
 return text ? SEND(const char *, text, "UTF8String") : "-";
}
static void (*original_switcher_wallpaper_overlay)(id self, SEL selector);
static void
switcher_wallpaper_overlay(id self, SEL selector)
{
 static int wanted = -1;
 static unsigned told;
 if (wanted < 0) {
  char args[1024] = "";
  size_t size = sizeof(args) - 1;
  const char *at;
  wanted = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
      (at = strstr(args, "switcher_wallpaper=1")) != NULL && (at == args || at[-1] == ' ');
 }
 if (told++ == 0) {
  say("redwood: the switcher's wash of wallpaper is %s\n", wanted ?
      "drawn as iOS asks (switcher_wallpaper=1)" : "left out: there is no picture of the wallpaper for it");
 }
 if (wanted) {
  original_switcher_wallpaper_overlay(self, selector);
 }
}
static void
layer_filter(id filter, const char *what, int depth)
{
 static const char *const inputs[] = { "inputColorMatrix", "inputRadius", "inputAmount", "inputColor", "inputAngle",
                                "inputPremultipliedValues", "inputNormalizeEdges", "inputReversed" };
 char line[1024];
 size_t n;
 if (!class_respondsToSelector(object_getClass(filter), sel_registerName("type"))) {
  say("redwood: layers%s: %*s  %s %s\n", layers_of, 2 * depth, "", what, text_of(filter));
  return;
 }
 n = (size_t)snprintf(line, sizeof(line), "%s %s \"%s\"%s", what, text_of(SEND(id, filter, "type")),
     text_of(SEND(id, filter, "name")), SEND(BOOL, filter, "isEnabled") ? "" : " (disabled)");
 for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]) && n < sizeof(line); i++) {
  id key = SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, inputs[i]);
  id value = SEND1(id, filter, "valueForKey:", id, key);
  float m[20];
  if (value == NULL) {
   continue;
  }
  if (i == 0 && class_respondsToSelector(object_getClass(value), sel_registerName("CAColorMatrixValue"))) {
   SEND1(void, value, "getValue:", void *, m);
   n += (size_t)snprintf(line + n, sizeof(line) - n, " %s [%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g | "
       "%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g]", inputs[i], m[0], m[1], m[2], m[3], m[4], m[5], m[6],
       m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19]);
  } else {
   n += (size_t)snprintf(line + n, sizeof(line) - n, " %s %s", inputs[i], text_of(value));
  }
 }
 say("redwood: layers%s: %*s  %s\n", layers_of, 2 * depth, "", line);
}
static void
layer_line(id layer, const char *role, int depth)
{
 struct { double x, y, w, h; } frame = SEND(typeof(frame), layer, "frame");
 const char *class_name = object_getClassName(layer);
 id contents = SEND(id, layer, "contents"), filters = SEND(id, layer, "filters"), mask = SEND(id, layer, "mask");
 id compositing = SEND(id, layer, "compositingFilter"), sublayers = SEND(id, layer, "sublayers");
 id background = SEND(id, layer, "backgroundFilters");
 void *colour = SEND(void *, layer, "backgroundColor");
 char more[1024] = "";
 size_t n = 0;
 if (SEND(BOOL, layer, "isHidden")) {
  n += (size_t)snprintf(more + n, sizeof(more) - n, " HIDDEN");
 }
 if (contents != NULL) {
  id text = (id)CFCopyDescription((CFTypeRef)contents);
  const char *from = text_of(text);
  char flat[300];
  size_t k = 0;
  while (from != NULL && *from != 0 && k + 1 < sizeof(flat)) {
   if (*from == ' ' || *from == '\n' || *from == '\t' || *from == '\r') {
    if (k > 0 && flat[k - 1] != ' ') {
     flat[k++] = ' ';
    }
    from++;
    continue;
   }
   flat[k++] = *from++;
  }
  flat[k] = 0;
  n += (size_t)snprintf(more + n, sizeof(more) - n, " contents %s", flat);
  CFRelease((CFTypeRef)text);
 }
 if (contents != NULL && class_respondsToSelector(object_getClass(layer), sel_registerName("contentsMultiplyColor")) &&
     SEND(void *, layer, "contentsMultiplyColor") != NULL) {
  id text = (id)CFCopyDescription(SEND(void *, layer, "contentsMultiplyColor"));
  const char *numbers = strstr(text_of(text), "( ");
  n += (size_t)snprintf(more + n, sizeof(more) - n, " times %.40s", numbers ? numbers : "?");
  CFRelease((CFTypeRef)text);
 }
 if (colour != NULL) {
  id text = (id)CFCopyDescription(colour);
  const char *numbers = strstr(text_of(text), "( ");
  n += (size_t)snprintf(more + n, sizeof(more) - n, " colour %.40s", numbers ? numbers : "?");
  CFRelease((CFTypeRef)text);
 }
 if (strcmp(class_name, "CAPortalLayer") == 0) {
  n += (size_t)snprintf(more + n, sizeof(more) - n, " PORTAL of %p%s%s", (void *)SEND(id, layer, "sourceLayer"),
      SEND(BOOL, layer, "hidesSourceLayer") ? " (hides it)" : "", SEND(BOOL, layer, "matchesOpacity") ? " (its opacity)" : "");
 } else if (strcmp(class_name, "CABackdropLayer") == 0) {
  n += (size_t)snprintf(more + n, sizeof(more) - n, " BACKDROP group \"%s\" scale %g%s", text_of(SEND(id, layer, "groupName")),
      SEND(double, layer, "scale"), SEND(BOOL, layer, "captureOnly") ? " (capture only)" : "");
 } else if (strcmp(class_name, "CAGradientLayer") == 0) {
  struct { double x, y; } from = SEND(typeof(from), layer, "startPoint"), to = SEND(typeof(to), layer, "endPoint");
  id colours = SEND(id, layer, "colors");
  n += (size_t)snprintf(more + n, sizeof(more) - n, " GRADIENT %s from %g,%g to %g,%g at %.40s:", text_of(SEND(id, layer, "type")),
      from.x, from.y, to.x, to.y, text_of(SEND(id, layer, "locations")));
  for (long i = 0; colours != NULL && i < SEND(long, colours, "count") && n < sizeof(more); i++) {
   id text = (id)CFCopyDescription((CFTypeRef)SEND1(id, colours, "objectAtIndex:", long, i));
   const char *numbers = strstr(text_of(text), "( ");
   n += (size_t)snprintf(more + n, sizeof(more) - n, " %.30s", numbers ? numbers : "?");
   CFRelease((CFTypeRef)text);
  }
 } else if (strcmp(class_name, "CAShapeLayer") == 0) {
  void *stroke = SEND(void *, layer, "strokeColor"), *fill = SEND(void *, layer, "fillColor");
  id text_stroke = stroke ? (id)CFCopyDescription(stroke) : NULL, text_fill = fill ? (id)CFCopyDescription(fill) : NULL;
  const char *ns = text_stroke ? strstr(text_of(text_stroke), "( ") : NULL, *nf = text_fill ? strstr(text_of(text_fill), "( ") : NULL;
  n += (size_t)snprintf(more + n, sizeof(more) - n, " SHAPE line %g stroke %.30s fill %.30s ends %g-%g", SEND(double, layer,
      "lineWidth"), ns ? ns : "-", nf ? nf : "-", SEND(double, layer, "strokeStart"), SEND(double, layer, "strokeEnd"));
  if (text_stroke != NULL) {
   CFRelease((CFTypeRef)text_stroke);
  }
  if (text_fill != NULL) {
   CFRelease((CFTypeRef)text_fill);
  }
 } else if (strcmp(class_name, "CALayerHost") == 0) {
  n += (size_t)snprintf(more + n, sizeof(more) - n, " HOST of context %#x%s%s", SEND(unsigned, layer, "contextId"),
      SEND(BOOL, layer, "inheritsSecurity") ? " (inherits security)" : "",
      SEND(BOOL, layer, "rendersAsynchronously") ? " (asynchronously)" : "");
 }
 {
  id delegate = SEND(id, layer, "delegate");
  if (delegate != NULL) {
   n += (size_t)snprintf(more + n, sizeof(more) - n, " of %s", object_getClassName(delegate));
  }
 }
 {
  id keys = SEND(id, layer, "animationKeys");
  if (SEND(BOOL, layer, "shouldRasterize")) {
   n += (size_t)snprintf(more + n, sizeof(more) - n, " RASTERIZED");
  }
  for (long i = 0; keys != NULL && i < SEND(long, keys, "count") && n < sizeof(more); i++) {
   n += (size_t)snprintf(more + n, sizeof(more) - n, "%s%s", i == 0 ? " ANIMATIONS " : ",",
       text_of(SEND1(id, keys, "objectAtIndex:", long, i)));
  }
 }
 say("redwood: layers%s: %*s%s%s %p \"%s\" %g,%g %gx%g opacity %g%s\n", layers_of, 2 * depth, "", role, class_name, (void *)layer,
     text_of(SEND(id, layer, "name")), frame.x, frame.y, frame.w, frame.h, (double)SEND(float, layer, "opacity"), more);
 for (long i = 0; filters != NULL && i < SEND(long, filters, "count"); i++) {
  layer_filter(SEND1(id, filters, "objectAtIndex:", long, i), "filter", depth);
 }
 for (long i = 0; background != NULL && i < SEND(long, background, "count"); i++) {
  layer_filter(SEND1(id, background, "objectAtIndex:", long, i), "background filter", depth);
 }
 if (compositing != NULL) {
  layer_filter(compositing, "compositing filter", depth);
 }
 if (mask != NULL) {
  layer_line(mask, "MASK ", depth + 1);
 }
 for (long i = 0; sublayers != NULL && i < SEND(long, sublayers, "count"); i++) {
  layer_line(SEND1(id, sublayers, "objectAtIndex:", long, i), "", depth + 1);
 }
}
static void
web_views(id view, int depth)
{
 Class web = objc_getClass("WKWebView");
 id subviews;
 bool is_web = false;
 if (view == NULL || depth > 40) {
  return;
 }
 for (Class c = object_getClass(view); web != NULL && c != NULL && !is_web; c = class_getSuperclass(c)) {
  is_web = c == web;
 }
 if (is_web) {
  SEL background = sel_registerName("_isBackground"), pid = sel_registerName("_webProcessIdentifier");
  SEL responsive = sel_registerName("_webProcessIsResponsive");
  say("redwood: layers%s: a WKWebView %p: in the background %s, web process %d, responsive %s, in a window %s, hidden %s\n",
      layers_of, (void *)view, class_respondsToSelector(object_getClass(view), background) ?
      (SEND(BOOL, view, "_isBackground") ? "YES" : "no") : "?", class_respondsToSelector(object_getClass(view), pid) ?
      SEND(int, view, "_webProcessIdentifier") : -1, class_respondsToSelector(object_getClass(view), responsive) ?
      (SEND(BOOL, view, "_webProcessIsResponsive") ? "yes" : "NO") : "?", SEND(id, view, "window") ? "yes" : "NO",
      SEND(BOOL, view, "isHidden") ? "YES" : "no");
 }
 subviews = SEND(id, view, "subviews");
 for (long i = 0; subviews != NULL && i < SEND(long, subviews, "count"); i++) {
  web_views(SEND1(id, subviews, "objectAtIndex:", long, i), depth + 1);
 }
}
static void
layers(void)
{
 Class window = objc_getClass("UIWindow");
 SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
 id windows = window && class_getClassMethod(window, all) ? ((id (*)(Class, SEL, BOOL, BOOL))objc_msgSend)(window, all, 1, 1) : NULL;
 id application = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
 id authentication = application && class_respondsToSelector(object_getClass(application),
     sel_registerName("authenticationController")) ? SEND(id, application, "authenticationController") : NULL;
 say("redwood: layers%s: %ld windows\n", layers_of, windows ? SEND(long, windows, "count") : 0L);
 if (authentication != NULL) {
  say("redwood: layers: the user is %sauthenticated, %s passcode, the display %sin secure mode\n",
      SEND(BOOL, authentication, "isAuthenticated") ? "" : "NOT ", SEND(BOOL, authentication, "hasPasscodeSet") ? "a" : "no",
      SEND(BOOL, authentication, "inSecureDisplayMode") ? "" : "not ");
 }
 for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
  id w = SEND1(id, windows, "objectAtIndex:", long, i);
  id scene = class_respondsToSelector(object_getClass(w), sel_registerName("windowScene")) ? SEND(id, w, "windowScene") : NULL;
  id context = class_respondsToSelector(object_getClass(w), sel_registerName("_boundContext")) ? SEND(id, w, "_boundContext") : NULL;
  say("redwood: layers%s: window %s level %g, context %#x (%ssecure, %u commits), scene %s (activation state %ld)\n",
      layers_of, object_getClassName(w), SEND(double, w, "windowLevel"), context ? SEND(unsigned, context, "contextId") : 0,
      context && SEND(BOOL, context, "isSecure") ? "" : "not ", context ? (unsigned)SEND(unsigned long long, context, "commitId") : 0,
      scene ? text_of(SEND(id, scene, "_persistenceIdentifier")) : "none", scene ? SEND(long, scene, "activationState") : -1L);
  layer_line(SEND(id, w, "layer"), "", 1);
 }
 if (application != NULL) {
  say("redwood: layers%s: the application's state %ld (0 active, 1 inactive, 2 in the background)\n", layers_of,
      SEND(long, application, "applicationState"));
 }
 for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
  web_views(SEND1(id, windows, "objectAtIndex:", long, i), 0);
 }
 say("redwood: layers%s: end\n", layers_of);
}
static void
switcher(void)
{
 Class coordinator = objc_getClass("SBMainSwitcherControllerCoordinator");
 SEL activate = sel_registerName("activateMainSwitcherNoninteractivelyWithSource:animated:");
 id shared = coordinator != NULL ? SEND(id, coordinator, "sharedInstance") : NULL;
 Method method = shared != NULL ? class_getInstanceMethod(object_getClass(shared), activate) : NULL;
 if (method == NULL) {
  say("redwood: the switcher: %s has no %s\n",
      coordinator != NULL ? "SBMainSwitcherControllerCoordinator" : "nothing", sel_getName(activate));
  return;
 }
 say("redwood: the switcher: opening it (%s, %s)\n", object_getClassName(shared),
     method_getTypeEncoding(method) != NULL ? method_getTypeEncoding(method) : "?");
 for (long source = 0; source < 10; source++) {
  BOOL taken = ((BOOL (*)(id, SEL, long, BOOL))objc_msgSend)(shared, activate, source, NO);
  say("redwood: the switcher: from source %ld: %s\n", source, taken ? "taken" : "refused");
  if (taken) {
   return;
  }
 }
}
#define CC_FADE_MS 0
static void (*original_add_animation_cc)(id self, SEL selector, id animation, id key);
static double cc_fade_s = CC_FADE_MS / 1000.0;
static unsigned cc_fades;
static bool
in_control_center(id layer)
{
 for (int depth = 0; layer != NULL && depth < 64; depth++) {
  id delegate = SEND(id, layer, "delegate");
  if (delegate != NULL && strcmp(object_getClassName(delegate), "SBControlCenterWindow") == 0) {
   return true;
  }
  layer = SEND(id, layer, "superlayer");
 }
 return false;
}
static void
add_animation_cc(id self, SEL selector, id animation, id key)
{
 Class basic = objc_getClass("CABasicAnimation");
 id path = animation != NULL && class_respondsToSelector(object_getClass(animation), sel_registerName("keyPath")) ?
     SEND(id, animation, "keyPath") : NULL;
 if (path != NULL && strcmp(text_of(path), "opacity") == 0 &&
     class_respondsToSelector(object_getClass(animation), sel_registerName("fromValue")) && in_control_center(self)) {
  id fade = SEND1(id, basic, "animationWithKeyPath:", id, path);
  id ease = SEND1(id, objc_getClass("CAMediaTimingFunction"), "functionWithName:", id,
      SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, "easeOut"));
  SEND1(void, fade, "setFromValue:", id, SEND(id, animation, "fromValue"));
  SEND1(void, fade, "setToValue:", id, SEND(id, animation, "toValue"));
  SEND1(void, fade, "setByValue:", id, SEND(id, animation, "byValue"));
  SEND1(void, fade, "setAdditive:", BOOL, SEND(BOOL, animation, "isAdditive"));
  SEND1(void, fade, "setBeginTime:", double, SEND(double, animation, "beginTime"));
  SEND1(void, fade, "setFillMode:", id, SEND(id, animation, "fillMode"));
  SEND1(void, fade, "setRemovedOnCompletion:", BOOL, SEND(BOOL, animation, "isRemovedOnCompletion"));
  SEND1(void, fade, "setDelegate:", id, SEND(id, animation, "delegate"));
  SEND1(void, fade, "setDuration:", double, cc_fade_s);
  SEND1(void, fade, "setTimingFunction:", id, ease);
  if (cc_fades++ < 3) {
   say("redwood: Control Center: an opacity animation of %.3f s (%s) made a fade of %.3f s\n",
       SEND(double, animation, "duration"), object_getClassName(animation), cc_fade_s);
  }
  animation = fade;
 }
 original_add_animation_cc(self, selector, animation, key);
}
static void
control_center_fades(const char *arguments)
{
 const char *setting = strstr(arguments, "cc_fade=");
 Method add = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("addAnimation:forKey:"));
 if (setting != NULL) {
  cc_fade_s = atof(setting + 8) / 1000.0;
 }
 if (cc_fade_s <= 0 || add == NULL) {
  return;
 }
 original_add_animation_cc = (void (*)(id, SEL, id, id))method_getImplementation(add);
 method_setImplementation(add, (IMP)add_animation_cc);
}
#define REDWOOD_TWEAK_LOADER "/redwood/lib/libredwood-tweaks.dylib"
static void
load_tweaks(void)
{
 struct stat st;
 if (stat("/var/jb/usr/lib/TweakInject", &st) != 0) {
  return;
 }
 if (dlopen(REDWOOD_TWEAK_LOADER, RTLD_NOW | RTLD_GLOBAL) == NULL) {
  say("redwood: the tweak loader %s: %s\n", REDWOOD_TWEAK_LOADER, dlerror());
 } else {
  say("redwood: tweaks: %s opened\n", REDWOOD_TWEAK_LOADER);
 }
}
extern void redwood_island_init(void);
__attribute__((constructor))
static void
redwood_springboard_init(void)
{
 const char *name = getprogname();
 size_t length = 0;
 IMP activation, snapshot_format, battery_lvl, battery_st, poster_switcher;
 int token;
 if (name == NULL) {
  return;
 }
 if (strcmp(name, "SpringBoard") != 0) {
  static char who[64];
  snprintf(who, sizeof(who), " of %s", name);
  layers_of = who;
  if ((say_fd = open("/dev/console", O_WRONLY | O_NOCTTY)) < 0) {
   say_fd = 2;
  }
  say("redwood: in %s[%d]: its layers are said on the notification com.redwood.layers\n", name, getpid());
  notify_register_dispatch("com.redwood.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
   layers();
  });
  return;
 }
 redwood_island_init();
 load_tweaks();
 slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
 activation = replace_method("SBLockdownManager", "_setupActivationState", 0x1c4bc8dfcUL, (IMP)setup_activation_state);
 original_set_setup_required_reason = (void (*)(id, SEL, long))replace_method("SBSetupManager",
     "_setSetupRequiredReason:", 0x1c4cdd3a4UL, (IMP)set_setup_required_reason);
 snapshot_format = replace_method_of_kind('+', "XBApplicationSnapshotManifestImpl", "_outputFormatForSnapshot:",
     0x1f1c91480UL, (IMP)snapshot_output_format);
 plain_vibrancy();
 original_lock_long_press = (void (*)(id, SEL, id))replace_method("SBLockHardwareButton", "longPress:", 0x1c4f6ab44UL,
     (IMP)lock_long_press);
 poster_switcher = replace_method("SBMainDisplayPolicyAggregator", "_allowsCapabilityPosterSwitcherWithExplanation:",
     0x1c507e0fcUL, (IMP)poster_switcher_allowed);
 original_switcher_wallpaper_overlay = (void (*)(id, SEL))replace_method("SBAppSwitcherPageView",
     "_createOrDestroyWallpaperOverlayView", 0x1c4af7860UL, (IMP)switcher_wallpaper_overlay);
 original_battery_level = (float (*)(id, SEL))replace_method("UIDevice", "batteryLevel", 0x1897021b0UL, (IMP)battery_level);
 original_battery_state = (long (*)(id, SEL))replace_method("UIDevice", "batteryState", 0x189209794UL, (IMP)battery_state);
 original_battery_view_shows_percentage = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "showsPercentage",
     0x189a9370cUL, (IMP)battery_view_shows_percentage);
 original_battery_view_set_shows_percentage = (void (*)(id, SEL, BOOL))replace_method("_UIBatteryView", "setShowsPercentage:",
     0x18926e150UL, (IMP)battery_view_set_shows_percentage);
 original_battery_view_should_show_bolt = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "_shouldShowBolt",
     0x189203e28UL, (IMP)battery_view_should_show_bolt);
 original_battery_view_charge_percent = (double (*)(id, SEL))replace_method("_UIBatteryView", "chargePercent",
     0x189a935d8UL, (IMP)battery_view_charge_percent);
 original_battery_view_set_charge_percent = (void (*)(id, SEL, double))replace_method("_UIBatteryView", "setChargePercent:",
     0x18928d310UL, (IMP)battery_view_set_charge_percent);
 original_ring_charging_level = (double (*)(id, SEL))replace_method("CSBatteryChargingRingView", "batteryChargingLevel",
     0x1c48c2cf4UL, (IMP)ring_charging_level);
 battery_lvl = (IMP)original_battery_level;
 battery_st = (IMP)original_battery_state;
 say("redwood: SpringBoard: activation %s, setup assistant %s, application snapshots %s, the side button's long press %s, "
     "battery level %s, battery state %s, poster switcher %s\n",
     activation ? "answered here" : "NOT replaced", original_set_setup_required_reason ? "refused" : "NOT replaced",
     snapshot_format ? "as JPEG" : "NOT changed", original_lock_long_press ? "checked" : "NOT checked",
     battery_lvl ? "from QG" : "NOT replaced", battery_st ? "from QG" : "NOT replaced",
     poster_switcher ? "not offered" : "NOT withheld");
 say("redwood: battery UI: shows%% %s, set-shows%% %s, bolt %s, chargePercent %s, setChargePercent %s, lockscreen ring %s\n",
     original_battery_view_shows_percentage ? "ok" : "NO",
     original_battery_view_set_shows_percentage ? "ok" : "NO",
     original_battery_view_should_show_bolt ? "ok" : "NO",
     original_battery_view_charge_percent ? "ok" : "NO",
     original_battery_view_set_charge_percent ? "ok" : "NO",
     original_ring_charging_level ? "ok" : "NO");
 {
  char args[1024] = "";
  size_t size = sizeof(args) - 1;
  const char *delay;
  if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (delay = strstr(args, "sb_delay=")) != NULL &&
      (delay == args || delay[-1] == ' ')) {
   usleep((useconds_t)(atof(delay + 9) * 1e6));
  }
 }
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
  Class uidev = objc_getClass("UIDevice");
  SEL current = sel_registerName("currentDevice");
  SEL monitoring = sel_registerName("setBatteryMonitoringEnabled:");
  id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, current) : nil;
  if (device && class_getInstanceMethod(object_getClass(device), monitoring)) {
   ((void (*)(id, SEL, BOOL))objc_msgSend)(device, monitoring, YES);
  }
  watch_main_loop();
  {
   char arguments[1024] = "";
   size_t size = sizeof(arguments) - 1;
   if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
    control_center_fades(arguments);
   }
  }
  say("redwood: battery: SOC=%d%% battery=%d charger=%d charging=%d\n",
      sysctl_int("kern.redwood.battery_soc"), sysctl_int("kern.redwood.battery_present"),
      sysctl_int("kern.redwood.battery_charger_present"), sysctl_int("kern.redwood.battery_charging"));
  battery_post_notifications();
 });
 notify_register_dispatch("com.redwood.unlock", &token, dispatch_get_main_queue(), ^(__unused int t) {
  unlock();
 });
 notify_register_dispatch("com.redwood.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
  layers();
 });
 notify_register_dispatch("com.redwood.switcher", &token, dispatch_get_main_queue(), ^(__unused int t) {
  switcher();
 });
 {
  char args[1024] = "";
  size_t size = sizeof(args) - 1;
  const char *arg;
  if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (arg = strstr(args, "autounlock=1")) == NULL ||
      (arg != args && arg[-1] != ' ')) {
   say("redwood: the lock screen stays until it is swiped away (autounlock=1 lifts it by itself)\n");
   return;
  }
 }
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
  unlock();
 });
}
