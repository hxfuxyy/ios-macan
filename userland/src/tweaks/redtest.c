#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <objc/message.h>
#include <objc/runtime.h>
extern void MSHookMessageEx(Class cls, SEL sel, IMP imp, IMP *result);
static void say(const char *s) {
 int fd = open("/dev/console", O_WRONLY);
 if (fd >= 0) { (void)write(fd, s, strlen(s)); close(fd); }
}
static void (*orig_setIcon)(id, SEL, id);
static int hit;
static void my_setIcon(id self, SEL _cmd, id icon) {
 if (!hit) { hit = 1; say("redwood: tweak hook fired: -[SBIconView setIcon:]\n"); }
 orig_setIcon(self, _cmd, icon);
}
__attribute__((constructor))
static void redtest_init(void) {
 say("redwood: test tweak loaded\n");
 Class c = objc_getClass("SBIconView");
 SEL s = sel_registerName("setIcon:");
 if (c && class_getInstanceMethod(c, s)) {
  MSHookMessageEx(c, s, (IMP)my_setIcon, (IMP *)&orig_setIcon);
  say("redwood: tweak hooked -[SBIconView setIcon:]\n");
 } else {
  say("redwood: tweak: no SBIconView setIcon:\n");
 }
}
