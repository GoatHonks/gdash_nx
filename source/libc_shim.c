/* libc_shim.c -- bionic-compatible libc wrappers for libcocos2dcpp.so/libfmod.so
 *
 * Both game libs are linked against bionic. Where the bionic and newlib ABIs
 * differ (struct layouts, flag values, missing functions) we provide
 * converting wrappers here; everything that matches is passed straight
 * through from imports.c.
 *
 * Adapted from lbbg_nx (fgsfds / gm666q lineage); the data01/data02 VFS and
 * the GL-ownership parking are gone, /dev/urandom + mmap emulation and the
 * Geometry Dash path remaps are new.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

// no <sys/uio.h> in devkitA64 newlib; bionic's iovec layout
struct iovec {
  void *iov_base;
  size_t iov_len;
};

#include "config.h"
#include "paths.h"
#include "so_util.h"
#include "libc_shim.h"
#include "pthr.h"
#include "net_shim.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memcpy(dst, src, n);
}

void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memmove(dst, src, n);
}

void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) {
  (void)dstlen;
  return memset(dst, c, n);
}

char *__strrchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strrchr(s, c);
}

long __read_chk_fake(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read_fake(fd, buf, count);
}

char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcat(dst, src);
}

char *__strchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strchr(s, c);
}

char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcpy(dst, src);
}

size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen;
  return strlen(s);
}

char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncat(dst, src, n);
}

char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncpy(dst, src, n);
}

int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsnprintf(s, maxlen, fmt, va);
}

int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsprintf(s, fmt, va);
}

// fortified FD_SET (libfmod's select loop); fd_set is bit-per-fd on both ABIs
void __FD_SET_chk_fake(int fd, void *set, size_t setlen) {
  (void)setlen;
  if (fd >= 0 && fd < (int)(8 * sizeof(fd_set)))
    FD_SET(fd, (fd_set *)set);
}

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  (void)name;
  value[0] = '\0';
  return 0;
}

// 0 = "no hwcaps": the game's embedded OpenSSL takes its portable C paths
// instead of probing for ARMv8 crypto extensions.
unsigned long getauxval_fake(unsigned long type) {
  (void)type;
  return 0;
}

int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID:
      return gettid_fake();
  }
  errno = ENOSYS;
  return -1;
}

void sincos_fake(double x, double *s, double *c) {
  *s = sin(x);
  *c = cos(x);
}

void sincosf_fake(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

void android_set_abort_message_fake(const char *msg) {
  (void)msg;
}

size_t __ctype_get_mb_cur_max_fake(void) {
  return 1;
}

int pthread_setname_np_fake(void *thread, const char *name) {
  (void)thread; (void)name;
  return 0;
}

// bionic sysconf constants
#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:
      return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN:
      return 3;
    case BIONIC_SC_PHYS_PAGES:
      return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default:
      return -1;
  }
}

// High-resolution monotonic clocks use the 19.2 MHz system tick. Real-time
// IDs must use the console RTC: OpenSSL uses wall-clock time when validating
// certificate validity periods. Bionic's IDs differ from newlib's (Android
// CLOCK_REALTIME is 0; newlib CLOCK_REALTIME is 1).
#define FAKE_EPOCH_BASE 1700000000ull // ~2023-11, seconds

int clock_gettime_fake(int clk_id, struct timespec *tp) {
  if (!tp)
    return -1;
  // Android CLOCK_REALTIME / REALTIME_COARSE / REALTIME_ALARM.
  if (clk_id == 0 || clk_id == 5 || clk_id == 8)
    return clock_gettime(CLOCK_REALTIME, tp);
  static u64 freq = 0;
  if (!freq)
    freq = armGetSystemTickFreq(); // 19200000 on the Switch
  const u64 tick = armGetSystemTick();
  tp->tv_sec = (time_t)(FAKE_EPOCH_BASE + tick / freq);
  tp->tv_nsec = (long)(((tick % freq) * 1000000000ull) / freq);
  return 0;
}

// bionic (BSD-style) fpclassify values
#define BIONIC_FP_INFINITE  0x01
#define BIONIC_FP_NAN       0x02
#define BIONIC_FP_NORMAL    0x04
#define BIONIC_FP_SUBNORMAL 0x08
#define BIONIC_FP_ZERO      0x10

int __isnanf_fake(float f) {
  return isnan(f);
}

int __fpclassifyd_fake(double d) {
  switch (fpclassify(d)) {
    case FP_INFINITE:  return BIONIC_FP_INFINITE;
    case FP_NAN:       return BIONIC_FP_NAN;
    case FP_SUBNORMAL: return BIONIC_FP_SUBNORMAL;
    case FP_ZERO:      return BIONIC_FP_ZERO;
    default:           return BIONIC_FP_NORMAL;
  }
}

// bionic basename(3): never modifies its argument
char *basename_fake(const char *path) {
  if (!path || !*path)
    return (char *)".";
  const char *slash = strrchr(path, '/');
  return (char *)(slash ? slash + 1 : path);
}

void *memrchr_fake(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s + n;
  while (n--) {
    if (*--p == (unsigned char)c)
      return (void *)p;
  }
  return NULL;
}

void __assert2_fake(const char *file, int line, const char *func, const char *expr) {
  (void)file; (void)line; (void)func; (void)expr;
  abort();
}

int __android_log_write_fake(int prio, const char *tag, const char *msg) {
  (void)prio; (void)tag; (void)msg;
  return 0;
}

int setpriority_fake(int which, int who, int prio) {
  (void)which; (void)who; (void)prio;
  return 0;
}

// ---------------------------------------------------------------------------
// process/signal stubs: curl and the game's crash guards import these; none
// of it can work on Horizon, and all callers tolerate failure
// ---------------------------------------------------------------------------

int getpid_fake(void) { return 1000; }
int fork_fake(void) { errno = ENOSYS; return -1; }
int execl_fake(const char *path, const char *arg, ...) {
  (void)path; (void)arg;
  errno = ENOSYS;
  return -1;
}
int waitpid_fake(int pid, int *status, int options) {
  (void)pid; (void)options;
  if (status) *status = 0;
  errno = ECHILD;
  return -1;
}
int kill_fake(int pid, int sig) { (void)pid; (void)sig; return 0; }
unsigned alarm_fake(unsigned sec) { (void)sec; return 0; }
void *signal_fake(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
int sigaction_fake(int sig, const void *act, void *oact) {
  (void)sig; (void)act; (void)oact;
  return 0;
}
int sigprocmask_fake(int how, const void *set, void *oset) {
  (void)how; (void)set; (void)oset;
  return 0;
}

// ---------------------------------------------------------------------------
// path remapping: every GD variant hardcodes a different Android package dir
// in a few places (song/save paths built before JNI's writable path is queried)
// ---------------------------------------------------------------------------

const char *fix_path(const char *path) {
  static char bufs[4][1024];
  static unsigned rr = 0;
  if (!path)
    return path;

  // OpenSSL 1.1.0c in the Android library was built on a developer Mac and
  // retained that machine's absolute OPENSSLDIR. net_init() exports the
  // Switch firmware's current trusted public roots here instead.
  static const char *ca_suffixes[] = {
    "/ssl/cert.pem",
    "/ssl/certs/ca-certificates.crt",
    "/etc/ssl/cert.pem",
    "/etc/ssl/certs/ca-certificates.crt",
  };
  const size_t path_len = strlen(path);
  for (unsigned i = 0; i < sizeof(ca_suffixes) / sizeof(*ca_suffixes); i++) {
    const size_t suffix_len = strlen(ca_suffixes[i]);
    if (path_len >= suffix_len &&
        strcmp(path + path_len - suffix_len, ca_suffixes[i]) == 0)
      return path_ca_bundle();
  }

  const char *suffix = path_android_private_suffix(path);
  if (suffix) {
    char *out = bufs[rr++ & 3];
    if (suffix[0])
      snprintf(out, sizeof(bufs[0]), "%s/%s", path_save(), suffix);
    else
      snprintf(out, sizeof(bufs[0]), "%s", path_save());
    return out;
  }
  return path;
}

// ---------------------------------------------------------------------------
// /dev/urandom emulation: the embedded OpenSSL seeds its RNG from it (open/
// read/fstat/close on a raw fd). Served from Horizon's csrng.
// ---------------------------------------------------------------------------

#define URANDOM_FD_BASE 0x7f000000

int is_urandom_fd_fake(int fd) {
  return fd >= URANDOM_FD_BASE && fd < URANDOM_FD_BASE + 16;
}

static int is_urandom_path(const char *path) {
  return strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0 ||
         strcmp(path, "/dev/srandom") == 0;
}

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000

static int convert_open_flags(int flags) {
  int out = flags & 3; // O_RDONLY/O_WRONLY/O_RDWR match
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  if (flags & LINUX_O_NONBLOCK) out |= O_NONBLOCK;
  return out;
}

int open_fake(const char *path, int flags, ...) {
  if (is_urandom_path(path))
    return URANDOM_FD_BASE;
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  const char *p = fix_path(path);
  return open(p, convert_open_flags(flags), mode);
}

// bionic's fortified open with no variadic mode (read/existing files)
int open2_fake(const char *path, int flags) {
  if (is_urandom_path(path))
    return URANDOM_FD_BASE;
  const char *p = fix_path(path);
  return open(p, convert_open_flags(flags), 0666);
}

long read_fake(int fd, void *buf, size_t count) {
  if (is_urandom_fd_fake(fd)) {
    randomGet(buf, count); // kernel-entropy chacha, no service needed
    return (long)count;
  }
  long result = read(fd, buf, count);
  if (result < 0)
    errno = net_errno_to_linux(errno);
  return result;
}

long write_fake(int fd, const void *buf, size_t count) {
  long result = write(fd, buf, count);
  if (result < 0)
    errno = net_errno_to_linux(errno);
  return result;
}

int close_fake(int fd) {
  if (is_urandom_fd_fake(fd))
    return 0;
  int result = close(fd);
  if (result < 0)
    errno = net_errno_to_linux(errno);
  return result;
}

int access_fake(const char *path, int mode) {
  (void)mode;
  struct stat st;
  return stat(fix_path(path), &st);
}

int chmod_fake(const char *path, unsigned mode) {
  (void)path; (void)mode;
  return 0;
}

int mkdir_fake(const char *path, unsigned int mode) {
  const char *p = fix_path(path);
  // Recursive: create each component, ignoring "exists".
  char tmp[1024];
  size_t n = strnlen(p, sizeof(tmp) - 1);
  memcpy(tmp, p, n);
  tmp[n] = '\0';
  for (char *s = tmp + 1; *s; s++) {
    if (*s == '/') {
      *s = '\0';
      mkdir(tmp, mode); // ignore EEXIST and intermediate failures
      *s = '/';
    }
  }
  return mkdir(tmp, mode);
}

int remove_fake(const char *path) {
  return remove(fix_path(path));
}

int rename_fake(const char *from, const char *to) {
  // both sides need remapping; fix_path uses rotating buffers so a single
  // call to each is safe before we hand them to rename()
  const char *f = fix_path(from);
  const char *t = fix_path(to);
  // Horizon's filesystem RenameFile FAILS if the destination already exists,
  // whereas POSIX rename() atomically replaces it. The game saves via
  // write-then-rename; without clearing the target every save after the
  // first would silently fail. remove() gives POSIX semantics.
  remove(t);
  return rename(f, t);
}

int chdir_fake(const char *path) {
  return chdir(fix_path(path));
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  struct stat real;
  const int ret = stat(fix_path(path), &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

int fstat_fake(int fd, struct bionic_stat *st) {
  if (is_urandom_fd_fake(fd)) {
    // OpenSSL checks S_ISCHR on the random device
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0444;
    st->st_nlink = 1;
    return 0;
  }
  struct stat real;
  const int ret = fstat(fd, &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

int lstat_fake(const char *path, struct bionic_stat *st) {
  return stat_fake(path, st);
}

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

void *opendir_fake(const char *path) {
  return opendir(fix_path(path));
}

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // NOTE: not thread-safe
  struct dirent *e = readdir((DIR *)dirp);
  if (!e)
    return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// ---------------------------------------------------------------------------
// mmap emulation: anonymous mappings (and read-only file windows) over malloc
// ---------------------------------------------------------------------------

#define LINUX_MAP_PRIVATE   0x02
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FAILED    ((void *)-1)

void *mmap_fake(void *addr, size_t len, int prot, int flags, int fd, long off) {
  (void)addr; (void)prot;
  if (len == 0)
    return LINUX_MAP_FAILED;
  void *p = memalign(0x1000, len);
  if (!p)
    return LINUX_MAP_FAILED;
  if (flags & LINUX_MAP_ANONYMOUS) {
    memset(p, 0, len);
    return p;
  }
  // file-backed: snapshot the window (private copy semantics)
  if (fd < 0) {
    free(p);
    return LINUX_MAP_FAILED;
  }
  const off_t old = lseek(fd, 0, SEEK_CUR);
  lseek(fd, off, SEEK_SET);
  ssize_t got = read(fd, p, len);
  lseek(fd, old, SEEK_SET);
  if (got < 0) {
    free(p);
    return LINUX_MAP_FAILED;
  }
  if ((size_t)got < len)
    memset((char *)p + got, 0, len - got);
  return p;
}

int munmap_fake(void *addr, size_t len) {
  (void)len;
  if (addr && addr != LINUX_MAP_FAILED)
    free(addr);
  return 0;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C locale versions
// ---------------------------------------------------------------------------

struct lconv *localeconv_fake(void) {
  return localeconv();
}

void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base;
  return (void *)1;
}

void freelocale_fake(void *loc) {
  (void)loc;
}

void *uselocale_fake(void *loc) {
  (void)loc;
  return (void *)1;
}

long double strtold_l_fake(const char *s, char **end, void *loc) {
  (void)loc;
  return strtold(s, end);
}

long long strtoll_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoll(s, end, base);
}

unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoull(s, end, base);
}

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  // ascii-ish naive conversion
  size_t i = 0;
  const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (unsigned char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0;
  const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

char *realpath_fake(const char *path, char *resolved) {
  if (!resolved)
    resolved = malloc(0x1000);
  strcpy(resolved, fix_path(path));
  return resolved;
}

int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

long writev_fake(int fd, const struct iovec *iov, int iovcnt) {
  long total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0)
      continue;
    long w = write_fake(fd, iov[i].iov_base, iov[i].iov_len);
    if (w < 0)
      return total > 0 ? total : -1;
    total += w;
    if ((size_t)w < iov[i].iov_len)
      break;
  }
  return total;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr): the game's libc++
// and its own logging write to &__sF[1]/&__sF[2]; these wrappers absorb
// accesses to those fake FILEs and forward the rest
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c too

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

// /proc emulation: the embedded OpenSSL reads /proc/cpuinfo when getauxval
// yields nothing; serve a plausible file from memory.
static const char fake_cpuinfo[] =
    "processor\t: 0\n"
    "BogoMIPS\t: 38.40\n"
    "Features\t: fp asimd\n"
    "CPU implementer\t: 0x41\n"
    "CPU architecture: 8\n"
    "CPU variant\t: 0x1\n"
    "CPU part\t: 0xd07\n"
    "CPU revision\t: 1\n";
static const char fake_meminfo[] =
    "MemTotal:        3276800 kB\n"
    "MemFree:         1048576 kB\n"
    "MemAvailable:    2097152 kB\n";

FILE *fopen_fake(const char *path, const char *mode) {
  if (strcmp(path, "/proc/cpuinfo") == 0)
    return fmemopen((void *)fake_cpuinfo, sizeof(fake_cpuinfo) - 1, "r");
  if (strcmp(path, "/proc/meminfo") == 0)
    return fmemopen((void *)fake_meminfo, sizeof(fake_meminfo) - 1, "r");
  if (is_urandom_path(path))
    path = "/dev/urandom"; // no FILE-level RNG consumer known; fall through
  const char *p = fix_path(path);
  FILE *f = fopen(p, mode);
  if (f && strchr(mode, 'r')) {
    // the .apk archive is streamed with many small reads through the game's
    // minizip; buffer it hard. Same for the big level/save .dat files.
    const char *ext = strrchr(p, '.');
    if (ext && (strcasecmp(ext, ".apk") == 0 || strcasecmp(ext, ".dat") == 0))
      setvbuf(f, NULL, _IOFBF, 256 * 1024);
  }
  return f;
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f))
    return n; // stdout/stderr sink
  return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return c;
  return fputc(c, f);
}

int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f))
    return 0; // stdout/stderr sink
  return fputs(s, f);
}

int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL)
    return 0;
  return fflush(f);
}

int fclose_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fclose(f);
}

int ferror_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return ferror(f);
}

int feof_fake(FILE *f) {
  if (is_fake_file(f))
    return 1;
  return feof(f);
}

int fileno_fake(FILE *f) {
  if (is_fake_file(f))
    return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  return fileno(f);
}

long ftell_fake(FILE *f) {
  if (is_fake_file(f))
    return -1;
  return ftell(f);
}

int fseek_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f))
    return -1;
  return fseek(f, off, whence);
}

int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f))
    return 0; // stdout/stderr sink
  va_list va;
  va_start(va, fmt);
  int ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}

int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f))
    return 0; // stdout/stderr sink
  return vfprintf(f, fmt, va);
}

int getc_fake(FILE *f) {
  if (is_fake_file(f))
    return -1; // EOF
  return getc(f);
}

int ungetc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return -1;
  return ungetc(c, f);
}

char *fgets_fake(char *s, int size, FILE *f) {
  if (is_fake_file(f))
    return NULL;
  return fgets(s, size, f);
}

void setbuf_fake(FILE *f, char *buf) {
  if (is_fake_file(f))
    return;
  setbuf(f, buf);
}

int setvbuf_fake(FILE *f, char *buf, int mode, size_t size) {
  if (is_fake_file(f))
    return 0;
  return setvbuf(f, buf, mode, size);
}

// wide-char stdio: the game's C++ runtime binds std::wcout/wcerr to the fake
// __sF slots; absorb those like the narrow functions above
wint_t getwc_fake(FILE *f) {
  if (is_fake_file(f))
    return WEOF;
  return getwc(f);
}

wint_t putwc_fake(wchar_t c, FILE *f) {
  if (is_fake_file(f))
    return (wint_t)c;
  return putwc(c, f);
}

wint_t ungetwc_fake(wint_t c, FILE *f) {
  if (is_fake_file(f))
    return WEOF;
  return ungetwc(c, f);
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks and semaphores via pointer indirection
// (bionic types are plain structs the game allocates; we stash a pointer
// to the real object in their first bytes, like the mutex fakes)
// ---------------------------------------------------------------------------

typedef struct {
  RwLock lock;
} FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) {
    FakeRwLock *l = calloc(1, sizeof(*l));
    rwlockInit(&l->lock);
    *storage = l;
  }
  return *storage;
}

int pthread_rwlock_init_fake(void **rw, const void *attr) {
  (void)attr;
  get_rwlock(rw);
  return 0;
}

int pthread_rwlock_destroy_fake(void **rw) {
  if (rw && *rw) {
    free(*rw);
    *rw = NULL;
  }
  return 0;
}

int pthread_rwlock_rdlock_fake(void **rw) {
  rwlockReadLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_wrlock_fake(void **rw) {
  rwlockWriteLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  // libnx needs to know which way it was locked
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock))
    rwlockWriteUnlock(&l->lock);
  else
    rwlockReadUnlock(&l->lock);
  return 0;
}

typedef struct {
  Semaphore sem;
} FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}

int sem_destroy_fake(void **s) {
  if (s && *s) {
    free(*s);
    *s = NULL;
  }
  return 0;
}

int sem_post_fake(void **s) {
  if (s && *s)
    semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_wait_fake(void **s) {
  if (s && *s)
    semaphoreWait(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem))
    return 0;
  errno = EAGAIN;
  return -1;
}
