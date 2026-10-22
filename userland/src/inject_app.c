#include <dispatch/dispatch.h>
#include <spawn.h>
#include <sys/stat.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern const void *_dyld_get_shared_cache_range(size_t *length);
#define CACHE_BASE 0x180000000UL
static uintptr_t slide;
static int console = -1;
static const char *who = "?";
static void
say(const char *text, size_t length)
{
 if (console < 0) {
  console = open("/dev/console", O_WRONLY);
 }
 (void)write(console >= 0 ? console : 2, text, length);
}
static void
killed(int number, siginfo_t *info, void *context)
{
 void *frames[32];
 char line[900];
 int n = backtrace(frames, 32), k;
 (void)context;
 k = snprintf(line, sizeof(line), "redwood: %s: SIGNAL %d at %p (the cache's slide %#lx), the stack:", who, number,
     info != NULL ? info->si_addr : NULL, (unsigned long)slide);
 for (int i = 0; i < n && k < (int)sizeof(line) - 24; i++) {
  k += snprintf(line + k, sizeof(line) - (size_t)k, " %#lx", (unsigned long)((uintptr_t)frames[i] - slide));
 }
 line[k++] = '\n';
 say(line, (size_t)k);
 signal(number, SIG_DFL);
 raise(number);
}
static unsigned long (*begin_task)(id, SEL, id);
static void (*resume_task)(id, SEL);
static unsigned long
watched_begin_task(id self, SEL cmd, id handler)
{
 unsigned long task;
 char line[160];
 int n;
 n = snprintf(line, sizeof(line), "redwood: %s: asking UIKit for a background task\n", who);
 say(line, (size_t)(n > 0 ? n : 0));
 task = begin_task(self, cmd, handler);
 n = snprintf(line, sizeof(line), "redwood: %s: the background task is %lu\n", who, task);
 say(line, (size_t)(n > 0 ? n : 0));
 return task;
}
static void
watched_resume_task(id self, SEL cmd)
{
 const char *(*string)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
 id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
 const char *url = "?";
 char line[700];
 id request, address, text;
 int n;
 request = get(self, sel_registerName("originalRequest"));
 address = request != NULL ? get(request, sel_registerName("URL")) : NULL;
 text = address != NULL ? get(address, sel_registerName("absoluteString")) : NULL;
 if (text != NULL) {
  url = string(text, sel_registerName("UTF8String"));
 }
 n = snprintf(line, sizeof(line), "redwood: %s: starting a session task for %s\n", who,
     url != NULL ? url : "?");
 say(line, (size_t)(n > 0 ? n : 0));
 resume_task(self, cmd);
}
static void
keep_to_the_repositories(void)
{
 id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
 id (*from_text)(id, SEL, const char *) = (id (*)(id, SEL, const char *))objc_msgSend;
 void (*set_bool)(id, SEL, signed char, id) = (void (*)(id, SEL, signed char, id))objc_msgSend;
 id defaults = (id)objc_getClass("NSUserDefaults");
 id key = from_text((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "ShowProvisional");
 if (defaults == NULL || key == NULL) {
  return;
 }
 defaults = get(defaults, sel_registerName("standardUserDefaults"));
 if (defaults != NULL) {
  set_bool(defaults, sel_registerName("setBool:forKey:"), 0, key);
  say("redwood: Sileo: its Canister search is turned off (it is not reachable here)\n", 73);
 }
}
static int
apt_install(char *const argv[])
{
 int install = 0;
 if (argv == NULL || argv[0] == NULL || strcmp(argv[0], "apt-get") != 0) {
  return 0;
 }
 for (int i = 1; argv[i] != NULL; i++) {
  if (argv[i][0] == '-' && argv[i][1] == 's') {
   return 0;
  }
  install |= strcmp(argv[i], "install") == 0;
 }
 return install;
}
static int
spawn_apt_said(pid_t *pid, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
    char *const argv[], char *const envp[])
{
 static char script[] =
     "exec 7>/dev/console; J=/var/jb\n"
     "echo \"redwood: apt-get runs as $($J/usr/bin/id), in $(pwd)\" >&7\n"
     "$J/bin/ls -ln $J/var/cache/apt/archives $J/var/lib/apt/sileolists >&7 2>&1\n"
     "for a in \"$@\"; do case $a in -*|install) ;; *) p=${a%%=*}\n"
     "  echo \"redwood: $p in sileolists:\" >&7\n"
     "  $J/usr/bin/grep -h -A30 \"^Package: $p\\$\" $J/var/lib/apt/sileolists/*Packages 2>&1 | $J/usr/bin/head -40 >&7 ;; esac; done\n"
     "$J/usr/bin/apt-get \"$@\"\n"
     "s=$?; echo \"redwood: apt-get exits $s; its archives after:\" >&7\n"
     "$J/bin/ls -ln $J/var/cache/apt/archives >&7 2>&1; exit $s\n";
 char *wrapped[96];
 int k = 0;
 for (int i = 0; envp != NULL && envp[i] != NULL; i++) {
  char line[600];
  int n = snprintf(line, sizeof(line), "redwood: %s: apt-get's environment: %s\n", who, envp[i]);
  say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
 }
 wrapped[k++] = "sh";
 wrapped[k++] = "-c";
 wrapped[k++] = script;
 wrapped[k++] = "apt-get";
 for (int i = 1; argv[i] != NULL && k < 95; i++) {
  wrapped[k++] = argv[i];
 }
 wrapped[k] = NULL;
 return posix_spawn(pid, "/var/jb/bin/sh", actions, attr, wrapped, envp);
}
static int
watched_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
    const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
 char line[1200];
 int n = snprintf(line, sizeof(line), "redwood: %s: (uid %d) spawns %s", who, (int)getuid(),
     path != NULL ? path : "?");
 for (int i = 0; argv != NULL && argv[i] != NULL && n < (int)sizeof(line) - 40; i++) {
  n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", argv[i]);
 }
 if (argv != NULL && argv[0] != NULL && strcmp(argv[0], "mv") == 0 && argv[1] != NULL) {
  struct stat st;
  if (stat(argv[1], &st) == 0) {
   n += snprintf(line + n, sizeof(line) - (size_t)n, " [%lld bytes]", (long long)st.st_size);
  } else {
   n += snprintf(line + n, sizeof(line) - (size_t)n, " [no such file]");
  }
 }
 n += snprintf(line + n, sizeof(line) - (size_t)n, "\n");
 say(line, (size_t)n);
 if (apt_install(argv)) {
  return spawn_apt_said(pid, actions, attr, argv, envp);
 }
 n = posix_spawn(pid, path, actions, attr, argv, envp);
 if (n != 0) {
  char said[300];
  int k = snprintf(said, sizeof(said), "redwood: %s: ... and that spawn failed: %s\n", who, strerror(n));
  say(said, (size_t)(k < (int)sizeof(said) ? k : (int)sizeof(said) - 1));
 }
 return n;
}
static const char *
text_of(id object)
{
 id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
 const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
 id text;
 if (object == NULL) {
  return "(nil)";
 }
 text = (class_respondsToSelector(object_getClass(object), sel_registerName("path")) ?
     get(object, sel_registerName("path")) : get(object, sel_registerName("description")));
 return text != NULL ? utf8(text, sel_registerName("UTF8String")) : "?";
}
static signed char (*move_url)(id, SEL, id, id, id *), (*copy_url)(id, SEL, id, id, id *);
static signed char (*move_path)(id, SEL, id, id, id *), (*copy_path)(id, SEL, id, id, id *);
static signed char
file_said(const char *what, signed char ok, id from, id to, id error)
{
 if (!ok) {
  char line[1400];
  int n = snprintf(line, sizeof(line), "redwood: %s: %s %s -> %s failed: %s\n", who, what, text_of(from),
      text_of(to), error != NULL ? text_of(error) : "(no error given)");
  say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
 }
 return ok;
}
static signed char
watched_move_url(id self, SEL cmd, id from, id to, id *error)
{
 id mine = NULL;
 signed char ok = move_url(self, cmd, from, to, error != NULL ? error : &mine);
 return file_said("moveItemAtURL", ok, from, to, error != NULL ? *error : mine);
}
static signed char
watched_copy_url(id self, SEL cmd, id from, id to, id *error)
{
 id mine = NULL;
 signed char ok = copy_url(self, cmd, from, to, error != NULL ? error : &mine);
 return file_said("copyItemAtURL", ok, from, to, error != NULL ? *error : mine);
}
static signed char
watched_move_path(id self, SEL cmd, id from, id to, id *error)
{
 id mine = NULL;
 signed char ok = move_path(self, cmd, from, to, error != NULL ? error : &mine);
 return file_said("moveItemAtPath", ok, from, to, error != NULL ? *error : mine);
}
static signed char
watched_copy_path(id self, SEL cmd, id from, id to, id *error)
{
 id mine = NULL;
 signed char ok = copy_path(self, cmd, from, to, error != NULL ? error : &mine);
 return file_said("copyItemAtPath", ok, from, to, error != NULL ? *error : mine);
}
__attribute__((used)) static struct {
 const void *ours, *theirs;
} spawn_interpose __attribute__((section("__DATA,__interpose"))) = {
 (const void *)watched_spawn, (const void *)posix_spawn
};
static void
watch(const char *name, const char *selector, IMP ours, void *theirs)
{
 Class class = objc_getClass(name);
 Method method = class != NULL ? class_getInstanceMethod(class, sel_registerName(selector)) : NULL;
 if (method != NULL && method_getImplementation(method) != ours) {
  *(IMP *)theirs = method_setImplementation(method, ours);
 }
}
__attribute__((constructor))
static void
redwood_app_init(void)
{
 static const int signals[] = { SIGTRAP, SIGABRT, SIGILL, SIGSEGV, SIGBUS, SIGSYS };
 struct sigaction action = { 0 };
 const char *name = getprogname();
 char line[200];
 size_t length = 0;
 int n;
 if (name == NULL) {
  return;
 }
 who = name;
 slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
 console = open("/dev/console", O_WRONLY);
 if (console >= 0) {
  dup2(console, 2);
 }
 action.sa_sigaction = killed;
 action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
 sigemptyset(&action.sa_mask);
 for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
  sigaction(signals[i], &action, NULL);
 }
 if (strcmp(name, "Sileo") == 0) {
  dispatch_async(dispatch_get_main_queue(), ^{ keep_to_the_repositories(); });
 }
 watch("UIApplication", "beginBackgroundTaskWithExpirationHandler:", (IMP)watched_begin_task, &begin_task);
 watch("NSURLSessionTask", "resume", (IMP)watched_resume_task, &resume_task);
 watch("NSFileManager", "moveItemAtURL:toURL:error:", (IMP)watched_move_url, &move_url);
 watch("NSFileManager", "copyItemAtURL:toURL:error:", (IMP)watched_copy_url, &copy_url);
 watch("NSFileManager", "moveItemAtPath:toPath:error:", (IMP)watched_move_path, &move_path);
 watch("NSFileManager", "copyItemAtPath:toPath:error:", (IMP)watched_copy_path, &copy_path);
 n = snprintf(line, sizeof(line), "redwood: %s: started as uid %d (effective %d), watching for what kills it\n",
     name, (int)getuid(), (int)geteuid());
 say(line, (size_t)(n > 0 ? n : 0));
}
