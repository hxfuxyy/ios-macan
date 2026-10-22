#include "mini.h"
struct device_mount_args {
 const char *fspec;
};
int
main(int argc, char **argv, char **envp)
{
 struct device_mount_args args = { .fspec = "/dev/md1" };
 char *init_argv[] = { "/redwood/sbin/init", NULL };
 int fd = open("/dev/console", O_RDWR, 0);
 (void)argc, (void)argv;
 dup2(fd, 0);
 dup2(fd, 1);
 dup2(fd, 2);
 if (fd > 2) {
  close(fd);
 }
 if (mount("packfs", "/redwood", MNT_RDONLY, &args) != 0) {
  printf("stage0: cannot mount /dev/md1 on /redwood: %s\n", strerror(errno));
 } else {
  execve(init_argv[0], init_argv, envp);
  printf("stage0: cannot run %s: %s\n", init_argv[0], strerror(errno));
 }
 reboot(0x08 );
 return 1;
}
