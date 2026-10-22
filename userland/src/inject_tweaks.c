#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *const tweak_dirs[] = {
 "/var/jb/usr/lib/TweakInject",
 "/var/jb/Library/MobileSubstrate/DynamicLibraries",
};
static void
say(const char *format, ...)
{
 char line[512];
 va_list args;
 int n, fd;
 va_start(args, format);
 n = vsnprintf(line, sizeof(line), format, args);
 va_end(args);
 if (n <= 0) {
  return;
 }
 fd = open("/dev/console", O_WRONLY | O_NOCTTY);
 if (fd >= 0) {
  (void)write(fd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
  close(fd);
 }
}
static const char *
executable_name(void)
{
 static char path[1024];
 uint32_t size = sizeof(path);
 char *slash;
 if (_NSGetExecutablePath(path, &size) != 0) {
  return NULL;
 }
 slash = strrchr(path, '/');
 return slash != NULL ? slash + 1 : path;
}
static bool
bundle_identifier(char *out, size_t size)
{
 CFBundleRef bundle = CFBundleGetMainBundle();
 CFStringRef identifier;
 if (bundle == NULL) {
  return false;
 }
 identifier = CFBundleGetIdentifier(bundle);
 return identifier != NULL && CFStringGetCString(identifier, out, (CFIndex)size, kCFStringEncodingUTF8);
}
static bool
array_has(CFArrayRef array, const char *want)
{
 CFIndex count = array != NULL ? CFArrayGetCount(array) : 0;
 for (CFIndex i = 0; i < count; i++) {
  CFStringRef string = CFArrayGetValueAtIndex(array, i);
  char text[256];
  if (CFGetTypeID(string) == CFStringGetTypeID() &&
      CFStringGetCString(string, text, sizeof(text), kCFStringEncodingUTF8) && strcmp(text, want) == 0) {
   return true;
  }
 }
 return false;
}
static bool
array_has_class(CFArrayRef array)
{
 CFIndex count = array != NULL ? CFArrayGetCount(array) : 0;
 for (CFIndex i = 0; i < count; i++) {
  CFStringRef string = CFArrayGetValueAtIndex(array, i);
  char text[256];
  if (CFGetTypeID(string) == CFStringGetTypeID() &&
      CFStringGetCString(string, text, sizeof(text), kCFStringEncodingUTF8) && objc_getClass(text) != NULL) {
   return true;
  }
 }
 return false;
}
static CFArrayRef
filter_array(CFDictionaryRef filter, const char *key)
{
 CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
 CFArrayRef array = NULL;
 if (name != NULL) {
  CFTypeRef value = CFDictionaryGetValue(filter, name);
  if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID()) {
   array = value;
  }
  CFRelease(name);
 }
 return array;
}
static bool
wanted_here(const char *path, const char *name, const char *identifier)
{
 CFDictionaryRef plist, filter;
 CFArrayRef bundles, executables, classes;
 CFStringRef mode_key, filter_key;
 CFDataRef data;
 CFTypeRef mode;
 bool all, hits[3], wanted;
 int fd, used = 0;
 uint8_t buffer[65536];
 ssize_t n;
 fd = open(path, O_RDONLY);
 n = fd >= 0 ? read(fd, buffer, sizeof(buffer)) : -1;
 if (fd >= 0) {
  close(fd);
 }
 if (n <= 0) {
  return false;
 }
 data = CFDataCreate(NULL, buffer, (CFIndex)n);
 if (data == NULL) {
  return false;
 }
 plist = (CFDictionaryRef)CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
 CFRelease(data);
 if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
  if (plist != NULL) {
   CFRelease(plist);
  }
  say("redwood: tweaks: %s is not a property list\n", path);
  return false;
 }
 filter_key = CFSTR("Filter");
 filter = CFDictionaryGetValue(plist, filter_key);
 if (filter == NULL || CFGetTypeID(filter) != CFDictionaryGetTypeID()) {
  CFRelease(plist);
  return true;
 }
 bundles = filter_array(filter, "Bundles");
 executables = filter_array(filter, "Executables");
 classes = filter_array(filter, "Classes");
 hits[0] = bundles != NULL && identifier != NULL && array_has(bundles, identifier);
 hits[1] = executables != NULL && name != NULL && array_has(executables, name);
 hits[2] = classes != NULL && array_has_class(classes);
 mode_key = CFSTR("Mode");
 mode = CFDictionaryGetValue(filter, mode_key);
 all = mode != NULL && CFGetTypeID(mode) == CFStringGetTypeID() &&
     CFStringCompare(mode, CFSTR("Any"), 0) == kCFCompareEqualTo;
 used = (bundles != NULL) + (executables != NULL) + (classes != NULL);
 if (used == 0) {
  wanted = true;
 } else if (all) {
  wanted = (bundles == NULL || hits[0]) && (executables == NULL || hits[1]) && (classes == NULL || hits[2]);
 } else {
  wanted = hits[0] || hits[1] || hits[2];
 }
 CFRelease(plist);
 return wanted;
}
static void
load_the_tweaks(void)
{
 char identifier[256];
 const char *name = executable_name();
 const char *who = bundle_identifier(identifier, sizeof(identifier)) ? identifier : NULL;
 int opened = 0, tried = 0;
 for (unsigned d = 0; d < sizeof(tweak_dirs) / sizeof(tweak_dirs[0]); d++) {
  const char *where = tweak_dirs[d];
  struct dirent *entry;
  struct stat st;
  DIR *dir;
  if (d > 0 && lstat(where, &st) == 0 && S_ISLNK(st.st_mode)) {
   continue;
  }
  dir = opendir(where);
  if (dir == NULL) {
   continue;
  }
  while ((entry = readdir(dir)) != NULL) {
   char plist[1024], dylib[1024];
   size_t length = strlen(entry->d_name);
   if (length < 7 || strcmp(entry->d_name + length - 6, ".plist") != 0) {
    continue;
   }
   snprintf(plist, sizeof(plist), "%s/%s", where, entry->d_name);
   snprintf(dylib, sizeof(dylib), "%s/%.*s.dylib", where, (int)(length - 6), entry->d_name);
   if (!wanted_here(plist, name, who)) {
    continue;
   }
   tried++;
   if (dlopen(dylib, RTLD_NOW | RTLD_GLOBAL) != NULL) {
    opened++;
    say("redwood: tweaks: %s\n", dylib);
   } else {
    say("redwood: tweaks: %s: %s\n", dylib, dlerror());
   }
  }
  closedir(dir);
 }
 if (tried > 0) {
  say("redwood: tweaks: %s (%s): %d of %d opened\n", name != NULL ? name : "?",
      who != NULL ? who : "no bundle", opened, tried);
 }
}
__attribute__((constructor))
static void
redwood_tweaks_init(void)
{
 static bool done;
 if (done) {
  return;
 }
 done = true;
 load_the_tweaks();
}
