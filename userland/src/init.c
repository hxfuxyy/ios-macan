#include "mini.h"
#define PATTERN_SIZE 300000
#define RB_HALT 0x08
static int checks, failures;
static const char *base = "";
static const char *
in_image(const char *path)
{
 static char buf[256];
 strcpy(buf, base);
 strcpy(buf + strlen(buf), path);
 return buf;
}
static int flash_root_mode;
static void check(int ok, const char *what);
static const char *
join(const char *a, const char *b, const char *c)
{
 static char buf[1200];
 size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
 if (la + lb + lc >= sizeof(buf)) {
  buf[0] = 0;
  return buf;
 }
 memcpy(buf, a, la);
 memcpy(buf + la, b, lb);
 memcpy(buf + la + lb, c, lc + 1);
 return buf;
}
static const char *
boot_arg(const char *name)
{
 static char args[1024], value[128];
 size_t len = sizeof(args) - 1, n = strlen(name);
 if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
  return NULL;
 }
 args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
 for (char *p = args; *p;) {
  while (*p == ' ') {
   p++;
  }
  if (strncmp(p, name, n) == 0 && p[n] == '=') {
   size_t i = 0;
   for (p += n + 1; *p && *p != ' ' && i < sizeof(value) - 1; p++) {
    value[i++] = *p;
   }
   value[i] = 0;
   return value;
  }
  while (*p && *p != ' ') {
   p++;
  }
 }
 return NULL;
}
static int
bind_dir(const char *from, const char *to)
{
 int ok = mount("bindfs", to, MNT_RDONLY, (void *)from) == 0;
 if (!ok) {
  printf("  bindfs %s on %s: %s\n", from, to, strerror(errno));
 }
 return ok;
}
static int
synthetic_dir(const char *path)
{
 int ok = sysctlbyname("kern.apfs_synthetic", NULL, NULL, (void *)path, strlen(path)) == 0;
 if (!ok) {
  printf("  synthetic directory %s: %s\n", path, strerror(errno));
 }
 return ok;
}
#ifndef EINVAL
#define EINVAL 22
#endif
static void
mount_cryptex(const char *name, const char *container, const char *flashdev)
{
 char at[64], spec[128], what[200];
 int err;
 strcpy(at, join("/private/preboot/Cryptexes/", name, ""));
 strcpy(spec, join("/dev/md0", container, ""));
 if (mount("apfs", at, MNT_RDONLY, (void *)spec) != 0) {
  err = errno;
  if (err != EINVAL) {
   printf("  the %s cryptex from %s: %s\n", name, spec, strerror(err));
  }
  strcpy(spec, join(flashdev, container, ""));
  if (mount("apfs", at, MNT_RDONLY, (void *)spec) != 0) {
   printf("  the %s cryptex from %s: %s\n", name, spec, strerror(errno));
   strcpy(what, join("the ", name, " cryptex: in neither md0 nor the flash"));
   check(0, what);
   return;
  }
 }
 strcpy(what, join("the ", name, " cryptex ("));
 strcpy(what, join(what, spec, ") on "));
 strcpy(what, join(what, at, ""));
 check(1, what);
}
static void
flash_root(const char *dev)
{
 static char buf[8192];
 char devpath[96], name[256], from[800], to[400];
 struct stat fw_st;
 off_t pos = 0;
 ssize_t n;
 int fd, ok, apps = 0, bound = 0;
 printf("\n-- the iOS root volume on /dev/%s as the root --\n", dev);
 strcpy(devpath, join("/dev/", dev, ""));
 ok = mount("apfs", "/flash", MNT_RDONLY, devpath) == 0;
 if (!ok) {
  printf("  mount %s: %s\n", devpath, strerror(errno));
 }
 check(ok, "mount the iOS volume read only");
 if (!ok) {
  return;
 }
 check(synthetic_dir("/flash/redwood"), "a synthetic /redwood on it, for this image");
 check(bind_dir(in_image("/LaunchDaemons"), "/flash/System/Library/LaunchDaemons") &&
     bind_dir(in_image("/empty"), "/flash/System/Library/NanoLaunchDaemons") &&
     bind_dir(in_image("/empty"), "/flash/System/Library/LaunchAngels") &&
     bind_dir(in_image("/xpc"), "/flash/System/Library/xpc"), "our launchd jobs, not iOS's");
 check(bind_dir(in_image("/hal"), "/flash/System/Library/Audio/Plug-Ins/HAL"),
     "our audio device, not Apple's plug-ins");
 if (stat("/fw", &fw_st) == 0) {
  check(synthetic_dir("/flash/fw") && bind_dir("/fw", "/flash/fw"), "the firmware on /fw (adsp=1, sound=N)");
 }
 if (stat("/jb-bin", &fw_st) == 0) {
  ok = mount("bindfs", "/flash/bin", MNT_RDONLY | MNT_UNION, (void *)"/jb-bin") == 0;
  if (!ok) {
   printf("  bindfs /jb-bin on /flash/bin, union: %s\n", strerror(errno));
  }
  check(ok, "Sileo's programs in /bin, over the volume's own");
 }
 fd = open("/flash/private/var/staged_system_apps", O_RDONLY | O_DIRECTORY, 0);
 while (fd >= 0 && (n = getdirentries(fd, buf, sizeof(buf), &pos)) > 0) {
  for (char *p = buf; p < buf + n;) {
   struct dirent *de = (struct dirent *)(void *)p;
   size_t len = de->d_namlen;
   if (de->d_reclen == 0) {
    break;
   }
   p += de->d_reclen;
   if (len < 5 || len >= sizeof(name) || strncmp(de->d_name + len - 4, ".app", 4) != 0) {
    continue;
   }
   memcpy(name, de->d_name, len);
   name[len] = 0;
   apps++;
   strcpy(to, join("/flash/Applications/", name, "="));
   strcpy(from, join(to, "/flash/private/var/staged_system_apps/", name));
   if (synthetic_dir(from)) {
    bound++;
   }
  }
 }
 if (fd >= 0) {
  close(fd);
 }
 printf("  %d applications waiting to be installed, %d of them in /Applications\n", apps, bound);
 check(apps > 0 && bound == apps, "the first boot's applications in /Applications");
 ok = sysctlbyname("kern.redwood_switch_root", NULL, NULL, "/flash private/preboot", 22) == 0;
 if (!ok) {
  printf("  switching the root: %s\n", strerror(errno));
 }
 check(ok, "the iOS volume is the root, the RAM part of the boot on /private/preboot");
 if (!ok) {
  return;
 }
 chdir("/");
 mount_cryptex("OS", ",c=1", devpath);
 mount_cryptex("App", ",c=2", devpath);
 check(bind_dir("/private/preboot/redwood", "/redwood"), "this image back on /redwood");
 flash_root_mode = 1;
}
static void
check(int ok, const char *what)
{
 checks++;
 if (!ok) {
  failures++;
 }
 printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
}
static unsigned char
pattern(unsigned long i)
{
 return (unsigned char)(i * 31 + (i >> 8));
}
static void
show_system(void)
{
 char type[32] = "?", release[32] = "?", machine[32] = "?", version[256] = "?";
 unsigned long long memsize = 0, pagesize = 0;
 int ncpu = 0;
 size_t len;
 len = sizeof(type), sysctlbyname("kern.ostype", type, &len, NULL, 0);
 len = sizeof(release), sysctlbyname("kern.osrelease", release, &len, NULL, 0);
 len = sizeof(version), sysctlbyname("kern.version", version, &len, NULL, 0);
 len = sizeof(machine), sysctlbyname("hw.machine", machine, &len, NULL, 0);
 len = sizeof(memsize), sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
 len = sizeof(pagesize), sysctlbyname("hw.pagesize", &pagesize, &len, NULL, 0);
 len = sizeof(ncpu), sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0);
 printf("%s %s %s\n%s\n", type, release, machine, version);
 printf("memory %llu MB, page size %llu, %d cpu\n\n", memsize >> 20, pagesize, ncpu);
 check(strcmp(type, "Darwin") == 0 && memsize != 0, "sysctl: kernel identifies itself");
}
static void
mode_string(unsigned mode, char *out)
{
 static const char bits[] = "rwxrwxrwx";
 out[0] = (mode & S_IFMT) == S_IFDIR ? 'd' : (mode & S_IFMT) == S_IFLNK ? 'l' :
     (mode & S_IFMT) == S_IFCHR ? 'c' : (mode & S_IFMT) == S_IFBLK ? 'b' : (mode & S_IFMT) == S_IFSOCK ? 's' : '-';
 for (int i = 0; i < 9; i++) {
  out[1 + i] = (mode & (0400 >> i)) ? bits[i] : '-';
 }
 out[10] = 0;
}
static int
list_tree(const char *path, int depth)
{
 static char buf[8192];
 char names[32][64];
 int count = 0, subdirs = 0, total = 0;
 off_t position = 0;
 ssize_t n;
 int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
 if (fd < 0) {
  printf("  %s: %s\n", path, strerror(errno));
  return -1;
 }
 while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
  for (ssize_t at = 0; at < n;) {
   struct dirent *de = (struct dirent *)(buf + at);
   char full[256], mode[12], target[128];
   struct stat st;
   at += de->d_reclen;
   if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
    continue;
   }
   strcpy(full, path);
   if (full[strlen(full) - 1] != '/') {
    strcpy(full + strlen(full), "/");
   }
   strcpy(full + strlen(full), de->d_name);
   if (lstat(full, &st) != 0) {
    printf("  %s: %s\n", full, strerror(errno));
    close(fd);
    return -1;
   }
   mode_string(st.st_mode, mode);
   printf("  %s %2u %8lld %s", mode, st.st_nlink, st.st_size, full);
   if ((st.st_mode & S_IFMT) == S_IFLNK) {
    ssize_t t = readlink(full, target, sizeof(target) - 1);
    target[t > 0 ? t : 0] = 0;
    printf(" -> %s", target);
   }
   printf("\n");
   count++;
   if (de->d_type == DT_DIR && depth > 0 && subdirs < 32 && strlen(full) < 64) {
    strcpy(names[subdirs++], full);
   }
  }
 }
 close(fd);
 if (n < 0) {
  return -1;
 }
 total = count;
 for (int i = 0; i < subdirs; i++) {
  int sub = list_tree(names[i], depth - 1);
  if (sub < 0) {
   return -1;
  }
  total += sub;
 }
 return total;
}
static void
test_files(void)
{
 static unsigned char buf[65536];
 char text[128];
 struct stat st;
 int fd, good;
 ssize_t n;
 unsigned long done;
 unsigned char *map;
 printf("\n-- read-only filesystem --\n");
 check(list_tree(*base ? base : "/", 3) >= 8, "walk our image (readdir, lstat, readlink)");
 check(stat("/usr/lib/dyld", &st) != 0 || st.st_size > 100000, "the system image has the dynamic linker");
 fd = open(in_image("/etc/motd"), O_RDONLY, 0);
 n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;
 text[n > 0 ? n : 0] = 0;
 close(fd);
 printf("  etc/motd: %s", text);
 check(n > 0 && strncmp(text, "Darwin", 6) == 0, "read a small file");
 check(stat("/tmp", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR, "follow a symlink to a directory");
 check(open("/nonexistent", O_RDONLY, 0) < 0 && errno == 2, "missing file gives ENOENT");
 check(open(in_image("/etc/newfile"), O_WRONLY | O_CREAT, 0644) < 0 && errno == 30, "creating a file gives EROFS");
 fd = open(in_image("/share/pattern.bin"), O_RDONLY, 0);
 good = fd >= 0 && fstat(fd, &st) == 0 && st.st_size == PATTERN_SIZE;
 for (done = 0; good && done < PATTERN_SIZE; done += (unsigned long)n) {
  n = read(fd, buf, sizeof(buf));
  if (n <= 0) {
   good = 0;
   break;
  }
  for (ssize_t i = 0; i < n; i++) {
   if (buf[i] != pattern(done + (unsigned long)i)) {
    good = 0;
   }
  }
 }
 check(good && done == PATTERN_SIZE, "read a 300 KB file and verify every byte");
 good = fd >= 0 && lseek(fd, 123457, SEEK_SET) == 123457 && read(fd, buf, 1000) == 1000;
 for (int i = 0; good && i < 1000; i++) {
  good = buf[i] == pattern(123457 + (unsigned long)i);
 }
 check(good, "seek into the middle and read");
 map = fd >= 0 ? mmap(NULL, PATTERN_SIZE, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
 good = map != MAP_FAILED;
 for (unsigned long i = 0; good && i < PATTERN_SIZE; i++) {
  good = map[i] == pattern(i);
 }
 check(good, "mmap the file and verify every byte (page-in)");
 if (map != MAP_FAILED) {
  munmap(map, PATTERN_SIZE);
 }
 close(fd);
}
static void
test_memory(void)
{
 const size_t size = 64u << 20, step = 4096;
 unsigned long *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
 int good = mem != MAP_FAILED;
 printf("\n-- memory --\n");
 for (size_t off = 0; good && off < size; off += step) {
  mem[off / sizeof(*mem)] = off ^ 0x5a5a5a5a5a5a5a5aUL;
 }
 for (size_t off = 0; good && off < size; off += step) {
  good = mem[off / sizeof(*mem)] == (off ^ 0x5a5a5a5a5a5a5a5aUL);
 }
 check(good, "64 MB of anonymous memory: touch every page, read it back");
 check(good && munmap(mem, size) == 0, "unmap it");
}
static void
test_processes(void)
{
 char program[128];
 char *argv[] = { program, "one", "two", NULL };
 char *envp[] = { "GREETING=hello from init", NULL };
 int status = 0, fds[2];
 char text[16] = "";
 pid_t pid;
 strcpy(program, in_image("/bin/child"));
 printf("\n-- processes --\n");
 pid = fork();
 if (pid == 0) {
  execve(argv[0], argv, envp);
  printf("  execve: %s\n", strerror(errno));
  _exit(127);
 }
 check(pid > 1, "fork");
 check(pid > 0 && wait4(pid, &status, 0, NULL) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42,
     "exec a second program from the image, collect its exit status");
 if (pipe(fds) == 0) {
  pid = fork();
  if (pid == 0) {
   close(fds[0]);
   write(fds[1], "ping", 4);
   _exit(0);
  }
  close(fds[1]);
  read(fds[0], text, sizeof(text) - 1);
  close(fds[0]);
  wait4(pid, &status, 0, NULL);
 }
 check(strcmp(text, "ping") == 0, "pipe from a child process");
}
static void
test_dynamic(void)
{
 char program[128];
 char *argv[] = { program, "from", "init", NULL };
 char *envp[] = { "PATH=/bin:/usr/bin", "HOME=/", NULL };
 struct stat st;
 int status = 0;
 pid_t pid;
 strcpy(program, in_image("/bin/hello_dyn"));
 if (stat(argv[0], &st) != 0 || stat("/usr/lib/dyld", &st) != 0) {
  return;
 }
 printf("\n-- dynamic linking --\n");
 pid = fork();
 if (pid == 0) {
  execve(argv[0], argv, envp);
  printf("  execve: %s\n", strerror(errno));
  _exit(127);
 }
 if (pid > 0 && wait4(pid, &status, 0, NULL) == pid) {
  if (WIFEXITED(status)) {
   printf("  %s exited with status %d\n", argv[0], WEXITSTATUS(status));
  } else {
   printf("  %s was killed by signal %d\n", argv[0], WTERMSIG(status));
  }
 }
 check(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 7, "run a program through dyld and the shared cache");
}
static int
split_tabs(char *line, char **fields, int max)
{
 int n = 0;
 while (n < max) {
  fields[n++] = line;
  line = strchr(line, '\t');
  if (line == NULL) {
   break;
  }
  *line++ = 0;
 }
 return n;
}
static long
parse_number(const char *text, int radix)
{
 long value = 0;
 for (; *text >= '0' && *text < '0' + radix; text++) {
  value = value * radix + (*text - '0');
 }
 return value;
}
static int
copy_file(const char *from, const char *to, int mode, int uid, int gid)
{
 static char buf[4096];
 int in = open(from, O_RDONLY, 0), out, ok = 0;
 ssize_t n;
 if (in < 0) {
  return 0;
 }
 out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
 if (out >= 0) {
  ok = 1;
  while ((n = read(in, buf, sizeof(buf))) > 0) {
   ok = ok && write(out, buf, (size_t)n) == n;
  }
  close(out);
  ok = ok && n == 0 && chmod(to, mode) == 0 && chown(to, uid, gid) == 0;
 }
 close(in);
 return ok;
}
static int
own_tree(const char *path, int uid)
{
 char buf[4096], child[512];
 struct stat st;
 off_t position = 0;
 ssize_t n;
 int ok, fd;
 if (lstat(path, &st) != 0 || (st.st_mode & S_IFMT) == S_IFLNK) {
  return 1;
 }
 ok = chown(path, uid, uid) == 0;
 if ((st.st_mode & S_IFMT) != S_IFDIR) {
  return ok;
 }
 if ((fd = open(path, O_RDONLY | O_DIRECTORY, 0)) < 0) {
  return ok;
 }
 while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
  for (ssize_t at = 0; at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
   const char *name = ((struct dirent *)(buf + at))->d_name;
   if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    continue;
   }
   if (strlen(path) + strlen(name) + 2 > sizeof(child)) {
    continue;
   }
   strcpy(child, path);
   strcpy(child + strlen(child), "/");
   strcpy(child + strlen(child), name);
   ok = own_tree(child, uid) && ok;
  }
 }
 close(fd);
 return ok;
}
static int
copy_tree(const char *from, const char *to, int *files)
{
 char buf[4096], f[512], t[512], target[512];
 struct stat st;
 off_t position = 0;
 ssize_t n;
 int ok = 1, fd, mode;
 if (lstat(from, &st) != 0) {
  return 0;
 }
 mode = st.st_mode & 07777;
 if (mkdir(to, mode) != 0 || chown(to, 0, 0) != 0) {
  return 0;
 }
 if ((fd = open(from, O_RDONLY | O_DIRECTORY, 0)) < 0) {
  return 0;
 }
 while (ok && (n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
  for (ssize_t at = 0; ok && at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
   const char *name = ((struct dirent *)(buf + at))->d_name;
   if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    continue;
   }
   if (strlen(from) + strlen(name) + 2 > sizeof(f) || strlen(to) + strlen(name) + 2 > sizeof(t)) {
    printf("  %s/%s: the path is too long\n", from, name);
    ok = 0;
    break;
   }
   strcpy(f, from);
   strcpy(f + strlen(f), "/");
   strcpy(f + strlen(f), name);
   strcpy(t, to);
   strcpy(t + strlen(t), "/");
   strcpy(t + strlen(t), name);
   if (lstat(f, &st) != 0) {
    ok = 0;
   } else if ((st.st_mode & S_IFMT) == S_IFDIR) {
    ok = copy_tree(f, t, files);
   } else if ((st.st_mode & S_IFMT) == S_IFLNK) {
    ssize_t length = readlink(f, target, sizeof(target) - 1);
    target[length > 0 ? length : 0] = 0;
    ok = length > 0 && symlink(target, t) == 0;
    (*files)++;
   } else {
    ok = copy_file(f, t, st.st_mode & 07777, 0, 0);
    (*files)++;
   }
   if (!ok) {
    printf("  %s: %s\n", f, strerror(errno));
   }
  }
 }
 close(fd);
 return ok && chmod(to, mode) == 0;
}
static const char *const graft_copy[] = { "Library", "etc", "var", "tmp" };
static int
graft_tree(const char *from, const char *to, int top, int *made, int *linked)
{
 char buf[4096], f[512], t[512];
 struct stat st;
 off_t position = 0;
 ssize_t n;
 int ok = 1, fd, mode;
 if (lstat(from, &st) != 0) {
  return 0;
 }
 mode = st.st_mode & 07777;
 if (mkdir(to, mode) != 0 || chown(to, 0, 0) != 0) {
  return 0;
 }
 (*made)++;
 if ((fd = open(from, O_RDONLY | O_DIRECTORY, 0)) < 0) {
  return 0;
 }
 while (ok && (n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
  for (ssize_t at = 0; ok && at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
   const char *name = ((struct dirent *)(buf + at))->d_name;
   int copy_it = 0;
   if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    continue;
   }
   if (strlen(from) + strlen(name) + 2 > sizeof(f) || strlen(to) + strlen(name) + 2 > sizeof(t)) {
    printf("  %s/%s: the path is too long\n", from, name);
    ok = 0;
    break;
   }
   strcpy(f, from);
   strcpy(f + strlen(f), "/");
   strcpy(f + strlen(f), name);
   strcpy(t, to);
   strcpy(t + strlen(t), "/");
   strcpy(t + strlen(t), name);
   for (unsigned i = 0; top && i < sizeof(graft_copy) / sizeof(graft_copy[0]); i++) {
    copy_it = copy_it || strcmp(name, graft_copy[i]) == 0;
   }
   if (lstat(f, &st) != 0) {
    ok = 0;
   } else if (copy_it) {
    ok = copy_tree(f, t, linked);
   } else if ((st.st_mode & S_IFMT) == S_IFDIR) {
    ok = graft_tree(f, t, 0, made, linked);
   } else {
    ok = symlink(f, t) == 0;
    (*linked)++;
   }
   if (!ok) {
    printf("  %s: %s\n", f, strerror(errno));
   }
  }
 }
 close(fd);
 return ok && chmod(to, mode) == 0;
}
static ssize_t
read_whole(const char *path, char *buf, size_t size)
{
 int fd = open(path, O_RDONLY, 0);
 size_t got = 0;
 ssize_t n;
 char over;
 if (fd < 0) {
  return -1;
 }
 while (got < size - 1 && (n = read(fd, buf + got, size - 1 - got)) > 0) {
  got += (size_t)n;
 }
 if (got == size - 1 && read(fd, &over, 1) > 0) {
  printf("init: %s does not fit in %zu bytes: the rest of it is not used\n", path, size - 1);
 }
 close(fd);
 buf[got] = 0;
 return (ssize_t)got;
}
static void
mount_writable(void)
{
 static char skeleton[16384];
 static const char *const markers[] = { "/private/var/db/diagnostics/.migrated",
                                 "/private/var/db/diagnostics/timesync/.migrated", "/private/var/db/uuidtext/.migrated" };
 int ok, fd, made = 0, files = 0;
 const char *jb;
 struct stat st;
 printf("\n-- writable filesystem --\n");
 ok = mount("ramfs", "/private/var", 0, NULL) == 0;
 if (!ok) {
  printf("  mount: %s\n", strerror(errno));
 }
 check(ok, "mount a memory filesystem on /private/var");
 read_whole(in_image("/etc/var.skel"), skeleton, sizeof(skeleton));
 for (char *line = skeleton; ok && *line;) {
  char *end = strchr(line, '\n'), *f[6];
  int count;
  if (end) {
   *end = 0;
  }
  count = split_tabs(line, f, 6);
  if (count == 5 && f[0][0] == 'd') {
   int mode = (int)parse_number(f[1], 8);
   ok = mkdir(f[4], mode) == 0 && chmod(f[4], mode) == 0 &&
       chown(f[4], (int)parse_number(f[2], 10), (int)parse_number(f[3], 10)) == 0;
   if (!ok) {
    printf("  %s: %s\n", f[4], strerror(errno));
   }
   made++;
  } else if (count == 3 && f[0][0] == 'l') {
   symlink(f[1], f[2]);
  } else if (count == 6 && f[0][0] == 'f') {
   if (!copy_file(in_image(f[5]), f[4], (int)parse_number(f[1], 8), (int)parse_number(f[2], 10),
       (int)parse_number(f[3], 10))) {
    printf("  %s: %s\n", f[4], strerror(errno));
   }
   files++;
  }
  if (!end) {
   break;
  }
  line = end + 1;
 }
 for (unsigned i = 0; ok && i < sizeof(markers) / sizeof(markers[0]); i++) {
  fd = open(markers[i], O_WRONLY | O_CREAT, 0644);
  ok = fd >= 0 && chown(markers[i], 272, 272) == 0;
  close(fd);
 }
 printf("  %d directories, %d files\n", made, files);
 check(ok && made > 50, "create the directories of the data volume");
 jb = stat("/jb", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR ? "/jb" :
     stat("/private/preboot/jb", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR ? "/private/preboot/jb" : NULL;
 if (ok && jb != NULL) {
  const char *to = "/private/var/jb";
  int dirs = 0, entries = 0, grafted, users = 0;
  if (strcmp(jb, "/private/preboot/jb") == 0 && stat("/private/preboot/jb-rw", &st) == 0) {
   if (mount("ramfs", "/private/preboot/jb-rw", 0, NULL) == 0 &&
       symlink("/private/preboot/jb-rw/procursus", "/private/var/jb") == 0) {
    to = "/private/preboot/jb-rw/procursus";
   } else {
    printf("  the prefix on /private/preboot/jb-rw: %s\n", strerror(errno));
   }
  }
  grafted = graft_tree(jb, to, 1, &dirs, &entries);
  static const char *const mobiles[] = { "/private/var/jb/var/mobile", "/private/var/jb/var/mobile/Library",
                                  "/private/var/jb/var/mobile/Library/Caches",
                                  "/private/var/jb/var/mobile/Library/Preferences",
                                  "/private/var/jb/var/mobile/Documents",
                                  "/private/var/jb/var/lib/apt/sileolists",
                                  "/private/var/jb/var/lib/apt/sileolists/partial" };
  for (unsigned i = 0; i < sizeof(mobiles) / sizeof(mobiles[0]); i++) {
   mkdir(mobiles[i], 0755);
   grafted = chown(mobiles[i], 501, 501) == 0 && grafted;
  }
  grafted = own_tree("/private/var/jb/var/mobile", 501) && grafted;
  if (stat("/private/var/jb/usr/sbin/pwd_mkdb", &st) == 0) {
   char *argv[] = { "/var/jb/usr/sbin/pwd_mkdb", "-p", "/var/jb/etc/master.passwd", NULL };
   char *envp[] = { "PATH=/var/jb/usr/sbin:/var/jb/usr/bin:/usr/bin:/bin", NULL };
   int status = 0;
   pid_t pid = fork();
   if (pid == 0) {
    execve(argv[0], argv, envp);
    _exit(127);
   }
   if (pid > 0 && wait4(pid, &status, 0, NULL) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    users = 1;
   }
  }
  printf("  /var/jb from %s, in %s: %d directories, %d files and links%s\n", jb, to, dirs, entries,
      users ? ", users resolvable" : "");
  check(grafted, "graft the package manager's prefix onto the data volume");
 }
}
static void
restore_var(void)
{
 const char *wanted = boot_arg("var_store");
 char *argv[3];
 int status = 0;
 pid_t pid;
 if (wanted == NULL || strcmp(wanted, "1") != 0) {
  return;
 }
 printf("\n-- the data volume of the last boot --\n");
 argv[0] = (char *)in_image("/bin/var_store");
 argv[1] = (char *)"load";
 argv[2] = NULL;
 pid = fork();
 if (pid == 0) {
  execve(argv[0], argv, environ);
  printf("  %s: %s\n", argv[0], strerror(errno));
  _exit(127);
 }
 if (pid < 0 || wait4(pid, &status, 0, NULL) != pid || !WIFEXITED(status)) {
  printf("  var_store did not run\n");
 }
}
static int
count_entries(const char *path)
{
 static char buf[4096];
 off_t position = 0;
 ssize_t n;
 int count = 0, fd = open(path, O_RDONLY | O_DIRECTORY, 0);
 if (fd < 0) {
  return -1;
 }
 while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
  for (ssize_t at = 0; at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
   const char *name = ((struct dirent *)(buf + at))->d_name;
   count += strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
  }
 }
 close(fd);
 return n < 0 ? -1 : count;
}
static void
test_writable(void)
{
 static unsigned char buf[65536];
 const unsigned long total = 100000;
 struct sockaddr_un sun = { .sun_len = sizeof(sun), .sun_family = AF_UNIX, .sun_path = "/tmp/t/socket" };
 struct stat st;
 char text[64];
 unsigned char *map;
 unsigned long done;
 int fd, fd2, good, s;
 ssize_t n;
 if (mkdir("/tmp/t", 0755) != 0) {
  return;
 }
 printf("\n-- writing files --\n");
 fd = open("/tmp/t/file", O_RDWR | O_CREAT | O_EXCL, 0644);
 good = fd >= 0;
 for (done = 0; good && done < total; done += (unsigned long)n) {
  unsigned long piece = total - done < 7777 ? total - done : 7777;
  for (unsigned long i = 0; i < piece; i++) {
   buf[i] = pattern(done + i);
  }
  n = write(fd, buf, piece);
  good = n == (ssize_t)piece;
 }
 good = good && fstat(fd, &st) == 0 && st.st_size == (off_t)total && lseek(fd, 0, SEEK_SET) == 0;
 for (done = 0; good && done < total; done += (unsigned long)n) {
  n = read(fd, buf, sizeof(buf));
  good = n > 0;
  for (ssize_t i = 0; good && i < n; i++) {
   good = buf[i] == pattern(done + (unsigned long)i);
  }
 }
 check(good && done == total, "create a file, write 100 KB, read it back");
 good = pwrite(fd, "0123456789", 10, 50000) == 10 && pread(fd, text, 12, 49999) == 12 &&
     (unsigned char)text[0] == pattern(49999) && memcmp(text + 1, "0123456789", 10) == 0 &&
     (unsigned char)text[11] == pattern(50010);
 check(good, "overwrite in the middle");
 fd2 = open("/tmp/t/file", O_WRONLY | O_APPEND, 0);
 good = fd2 >= 0 && write(fd2, "tail!", 5) == 5 && fstat(fd, &st) == 0 && st.st_size == (off_t)total + 5 &&
     pread(fd, text, 5, (off_t)total) == 5 && memcmp(text, "tail!", 5) == 0;
 close(fd2);
 check(good, "append");
 good = ftruncate(fd, 1000) == 0 && fstat(fd, &st) == 0 && st.st_size == 1000 && ftruncate(fd, 40000) == 0 &&
     pread(fd, buf, 2000, 0) == 2000;
 for (int i = 0; good && i < 2000; i++) {
  good = buf[i] == (i < 1000 ? pattern((unsigned long)i) : 0);
 }
 good = good && pread(fd, buf, 100, 30000) == 100 && buf[0] == 0 && buf[99] == 0 && pread(fd, buf, 10, 40000) == 0;
 check(good, "truncate, then extend: the new part reads as zeros");
 good = rename("/tmp/t/file", "/tmp/t/renamed") == 0 && stat("/tmp/t/file", &st) != 0 && errno == 2 &&
     stat("/tmp/t/renamed", &st) == 0 && st.st_size == 40000;
 check(good, "rename");
 good = link("/tmp/t/renamed", "/tmp/t/hard") == 0 && fstat(fd, &st) == 0 && st.st_nlink == 2 &&
     unlink("/tmp/t/renamed") == 0 && fstat(fd, &st) == 0 && st.st_nlink == 1;
 check(good, "hard link, remove the first name");
 good = unlink("/tmp/t/hard") == 0 && stat("/tmp/t/hard", &st) != 0 && fstat(fd, &st) == 0 && st.st_nlink == 0 &&
     pread(fd, buf, 100, 0) == 100 && buf[5] == pattern(5) && pwrite(fd, "x", 1, 0) == 1;
 close(fd);
 check(good, "a removed file stays usable while it is open");
 n = symlink("some/where else", "/tmp/t/symlink") == 0 ? readlink("/tmp/t/symlink", text, sizeof(text) - 1) : -1;
 text[n > 0 ? n : 0] = 0;
 check(n == 15 && strcmp(text, "some/where else") == 0 && lstat("/tmp/t/symlink", &st) == 0 &&
     (st.st_mode & S_IFMT) == S_IFLNK, "symbolic link");
 good = mkdir("/tmp/t/a", 0700) == 0 && mkdir("/tmp/t/a/b", 0700) == 0 && rmdir("/tmp/t/a") != 0 && errno == 66 &&
     rename("/tmp/t/a", "/tmp/t/c") == 0 && stat("/tmp/t/c/b", &st) == 0 && stat("/tmp/t/c/b/..", &st) == 0 &&
     rename("/tmp/t/c", "/tmp/t/c/b/inside") != 0 && rmdir("/tmp/t/c/b") == 0 && rmdir("/tmp/t/c") == 0 &&
     stat("/tmp/t/c", &st) != 0;
 check(good, "directories: nest, refuse to remove a full one, rename, remove");
 fd = open("/tmp/t/mapped", O_RDWR | O_CREAT, 0600);
 map = fd >= 0 && ftruncate(fd, 65536) == 0 ? mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
 good = map != MAP_FAILED;
 if (good) {
  memset(map + 1000, 'M', 100);
  good = pread(fd, buf, 102, 999) == 102 && buf[0] == 0 && buf[1] == 'M' && buf[100] == 'M' && buf[101] == 0;
 }
 check(good, "shared mapping: what is stored through memory is seen by read()");
 good = good && pwrite(fd, "WWWW", 4, 40000) == 4 && map[40000] == 'W' && map[40003] == 'W' && map[1000] == 'M';
 check(good, "and what write() stores is seen through the mapping");
 if (map != MAP_FAILED) {
  munmap(map, 65536);
 }
 close(fd);
 s = socket(AF_UNIX, SOCK_STREAM, 0);
 good = s >= 0 && bind(s, &sun, sizeof(sun)) == 0 && stat(sun.sun_path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFSOCK;
 close(s);
 check(good, "bind a local socket to a name");
 good = mkdir("/tmp/t/many", 0755) == 0;
 for (int i = 0; good && i < 300; i++) {
  char name[32] = "/tmp/t/many/f000";
  name[13] = (char)('0' + i / 100), name[14] = (char)('0' + i / 10 % 10), name[15] = (char)('0' + i % 10);
  fd = open(name, O_WRONLY | O_CREAT, 0644);
  good = fd >= 0 && write(fd, name, 16) == 16;
  close(fd);
 }
 good = good && count_entries("/tmp/t/many") == 300;
 for (int i = 0; good && i < 300; i++) {
  char name[32] = "/tmp/t/many/f000";
  name[13] = (char)('0' + i / 100), name[14] = (char)('0' + i / 10 % 10), name[15] = (char)('0' + i % 10);
  good = unlink(name) == 0;
 }
 check(good && count_entries("/tmp/t/many") == 0 && rmdir("/tmp/t/many") == 0, "300 files in one directory: create, list, remove");
 list_tree("/tmp/t", 1);
}
static void
run_command(char *line)
{
 char *argv[64];
 char *envp[] = { "PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/bin:/sbin:/usr/bin:/usr/sbin",
           "HOME=/var/mobile", "TERM=dumb", NULL };
 const char *append_to = NULL;
 int argc = 0, status = 0, background = 0;
 struct stat st;
 pid_t pid;
 for (char *p = line; *p && argc < 63;) {
  while (*p == ' ' || *p == '\t') {
   *p++ = 0;
  }
  if (!*p) {
   break;
  }
  argv[argc++] = p;
  while (*p && *p != ' ' && *p != '\t') {
   p++;
  }
 }
 argv[argc] = NULL;
 if (argc == 0) {
  return;
 }
 printf("\n$");
 for (int i = 0; i < argc; i++) {
  printf(" %s", argv[i]);
 }
 printf("\n");
 if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) {
  background = 1;
  argv[--argc] = NULL;
 }
 if (argc > 2 && strcmp(argv[argc - 2], ">>") == 0) {
  append_to = argv[argc - 1];
  argc -= 2;
  argv[argc] = NULL;
 }
 if (strcmp(argv[0], "sleep") == 0 && argc > 1) {
  int seconds = 0;
  for (const char *d = argv[1]; *d >= '0' && *d <= '9'; d++) {
   seconds = seconds * 10 + (*d - '0');
  }
  for (int left = seconds * 5; left > 0; left--) {
   usleep(200000);
  }
  return;
 }
 if (strcmp(argv[0], "exec") == 0 && argc > 1) {
  if (stat(in_image("/etc/quiet"), &st) == 0) {
   int null = open("/dev/null", O_WRONLY, 0);
   if (null >= 0) {
    dup2(null, 2);
    close(null);
   }
  }
  if (flash_root_mode && strcmp(argv[1], "/sbin/launchd.ios") == 0) {
   argv[1] = "/sbin/launchd";
  }
  execve(argv[1], argv + 1, envp);
  printf("%s: %s\n", argv[1], strerror(errno));
  return;
 }
 pid = fork();
 if (pid == 0) {
  if (append_to != NULL) {
   int fd = open(append_to, O_WRONLY | O_CREAT | O_APPEND, 0644);
   if (fd >= 0) {
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
   }
  }
  execve(argv[0], argv, envp);
  printf("%s: %s\n", argv[0], strerror(errno));
  _exit(127);
 }
 if (background && pid > 0) {
  return;
 }
 if (pid < 0 || wait4(pid, &status, 0, NULL) != pid) {
  printf("[could not run: %s]\n", strerror(errno));
 } else if (!WIFEXITED(status)) {
  printf("[killed by signal %d]\n", WTERMSIG(status));
 } else if (WEXITSTATUS(status) != 0) {
  printf("[exit status %d]\n", WEXITSTATUS(status));
 }
}
static void
run_rc(const char *path)
{
 static char script[65536];
 if (read_whole(path, script, sizeof(script)) < 0) {
  return;
 }
 printf("\n=== %s ===\n", path);
 for (char *line = script; *line;) {
  char *end = strchr(line, '\n');
  if (end) {
   *end = 0;
  }
  if (*line != '#') {
   run_command(line);
  }
  if (!end) {
   break;
  }
  line = end + 1;
 }
}
static void
test_devices(void)
{
 unsigned char buf[16];
 unsigned sum = 0;
 int fd, good;
 printf("\n-- devices --\n");
 check(list_tree("/dev", 0) > 3, "devfs is mounted on /dev");
 fd = open("/dev/zero", O_RDONLY, 0);
 memset(buf, 0xff, sizeof(buf));
 good = fd >= 0 && read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
 for (unsigned i = 0; i < sizeof(buf); i++) {
  sum += buf[i];
 }
 close(fd);
 check(good && sum == 0, "/dev/zero");
 fd = open("/dev/null", O_WRONLY, 0);
 check(fd >= 0 && write(fd, "x", 1) == 1, "/dev/null");
 close(fd);
 fd = open("/dev/random", O_RDONLY, 0);
 memset(buf, 0, sizeof(buf));
 good = fd >= 0 && read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
 close(fd);
 printf("  /dev/random:");
 for (unsigned i = 0; i < sizeof(buf); i++) {
  printf(" %02x", buf[i]);
  sum += buf[i];
 }
 printf("\n");
 check(good && sum != 0, "/dev/random");
}
static void
test_time(void)
{
 struct timeval before = { 0 }, after = { 0 };
 long elapsed;
 printf("\n-- time --\n");
 gettimeofday(&before, NULL);
 usleep(200000);
 gettimeofday(&after, NULL);
 elapsed = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_usec - before.tv_usec) / 1000;
 printf("  slept 200 ms, the clock advanced %ld ms\n", elapsed);
 check(elapsed >= 190 && elapsed < 1000, "sleep and wall clock agree");
}
static void
configure_inject(void)
{
 char conf[4096];
 long n = (long)read_whole(in_image("/etc/inject.conf"), conf, sizeof(conf));
 if (n < 0) {
  return;
 }
 while (n > 0 && (conf[n - 1] == '\n' || conf[n - 1] == ' ')) {
  n--;
 }
 if (n <= 0) {
  return;
 }
 conf[n] = 0;
 for (long i = 0; i < n; i++) {
  if (conf[i] == '\n') {
   conf[i] = ';';
  }
 }
 if (sysctlbyname("kern.exec_inject", NULL, NULL, conf, (size_t)n + 1) == 0) {
  printf("init: library insertion: %s\n", conf);
 } else {
  printf("init: kern.exec_inject: error %d\n", errno);
 }
}
#define MAX_VNODES 32768
static void
configure_vnodes(void)
{
 unsigned long long memsize = 0;
 size_t len = sizeof(memsize);
 int vnodes;
 if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) != 0) {
  return;
 }
 vnodes = memsize / 65536 + 1024 > MAX_VNODES ? MAX_VNODES : (int)(memsize / 65536) + 1024;
 if (sysctlbyname("kern.maxvnodes", NULL, NULL, &vnodes, sizeof(vnodes)) == 0) {
  printf("init: up to %d vnodes\n", vnodes);
 } else {
  printf("init: kern.maxvnodes: error %d\n", errno);
 }
}
int
main(int argc, char **argv, char **envp)
{
 int fd = open("/dev/console", O_RDWR, 0);
 dup2(fd, 0);
 dup2(fd, 1);
 dup2(fd, 2);
 if (fd > 2) {
  close(fd);
 }
 (void)envp;
 if (argc > 0 && strncmp(argv[0], "/redwood/", 9) == 0) {
  base = "/redwood";
 }
 if (getpid() != 1) {
  if (argc > 1) {
   run_rc(argv[1]);
  }
  return 0;
 }
 printf("\n=== init: process %d, started as %s ===\n", getpid(), argc > 0 ? argv[0] : "?");
 {
  const char *dev = boot_arg("root_apfs");
  if (dev != NULL) {
   flash_root(dev);
  }
 }
 show_system();
 mount_writable();
 restore_var();
 test_files();
 test_writable();
 test_memory();
 test_processes();
 test_dynamic();
 test_devices();
 test_time();
 printf("\n=== %d checks, %d failed ===\n", checks, failures);
 chmod("/dev/console", 0666);
 configure_inject();
 configure_vnodes();
 run_rc(in_image("/etc/rc"));
 printf("\n=== init: done ===\n");
 reboot(RB_HALT);
 return failures != 0;
}
