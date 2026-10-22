#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define PERSONA_SYSTEM 4
#define PERSONA_INFO_V2 2
#define SILEO_PERSONA 99
struct kpersona_info {
 uint32_t persona_info_version;
 uid_t persona_id;
 int persona_type;
 gid_t persona_gid;
 uint32_t persona_ngroups;
 gid_t persona_groups[16];
 uid_t persona_gmuid;
 char persona_name[256];
 uid_t persona_uid;
} __attribute__((packed));
_Static_assert(sizeof(struct kpersona_info) == 348, "the kernel's struct kpersona_info");
extern int kpersona_alloc(struct kpersona_info *info, uid_t *id);
extern int kpersona_get(uid_t *id);
extern int posix_spawnattr_set_persona_np(posix_spawnattr_t *attr, uid_t persona, uint32_t flags);
extern int posix_spawnattr_set_persona_uid_np(posix_spawnattr_t *attr, uid_t uid);
extern int posix_spawnattr_set_persona_gid_np(posix_spawnattr_t *attr, gid_t gid);
extern char **environ;
int
main(void)
{
 int fd = open("/dev/console", O_WRONLY);
 if (fd >= 0) {
  dup2(fd, 1);
  dup2(fd, 2);
 }
 struct kpersona_info info;
 memset(&info, 0, sizeof(info));
 info.persona_info_version = PERSONA_INFO_V2;
 info.persona_id = SILEO_PERSONA;
 info.persona_type = PERSONA_SYSTEM;
 info.persona_gid = 0;
 info.persona_uid = 0;
 strlcpy(info.persona_name, "_hackios_system", sizeof(info.persona_name));
 uid_t id = SILEO_PERSONA;
 int ret = kpersona_alloc(&info, &id);
 printf("persona-init: persona %u (system): %s\n", (unsigned)id, ret == 0 ? "made" : errno == EEXIST ? "already there" : strerror(errno));
 posix_spawnattr_t attr;
 posix_spawnattr_init(&attr);
 posix_spawnattr_set_persona_np(&attr, SILEO_PERSONA, 1);
 posix_spawnattr_set_persona_uid_np(&attr, 0);
 posix_spawnattr_set_persona_gid_np(&attr, 0);
 char *args[] = { "true", NULL };
 pid_t pid;
 int spawned = posix_spawn(&pid, "/var/jb/usr/bin/true", NULL, &attr, args, environ);
 int status = 0;
 if (spawned == 0)
  waitpid(pid, &status, 0);
 printf("persona-init: a program spawned into persona %u as root, as Sileo does: %s, exit %d\n", SILEO_PERSONA,
     spawned == 0 ? "started" : strerror(spawned), spawned == 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
 posix_spawnattr_destroy(&attr);
 return 0;
}
