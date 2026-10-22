#include <fcntl.h>
#include <string.h>
#include <unistd.h>
__attribute__((constructor))
static void redbare_init(void) {
 int fd = open("/dev/console", O_WRONLY);
 const char *s = "redwood: bare tweak loaded (no libsubstrate)\n";
 if (fd >= 0) { (void)write(fd, s, strlen(s)); close(fd); }
}
