#include <mach/kmod.h>
#include <kern/startup.h>
#include <kern/debug.h>
extern kern_return_t pthread_start(kmod_info_t *ki, void *d);
__startup_func
static void
pthread_kext_static_start(void)
{
 kern_return_t kr = pthread_start(NULL, NULL);
 if (kr != KERN_SUCCESS) {
  panic("static pthread kext failed to start: %d", kr);
 }
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, pthread_kext_static_start);
