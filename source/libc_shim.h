/* libc_shim.h -- bionic-compatible libc wrappers for libcocos2dcpp.so/libfmod.so
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __LIBC_SHIM_H__
#define __LIBC_SHIM_H__

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>

// fortify
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen);
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen);
char *__strchr_chk_fake(const char *s, int c, size_t slen);
char *__strrchr_chk_fake(const char *s, int c, size_t slen);
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen);
size_t __strlen_chk_fake(const char *s, size_t slen);
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va);
long __read_chk_fake(int fd, void *buf, size_t count, size_t buf_size);
void __FD_SET_chk_fake(int fd, void *set, size_t setlen);

// path remapping: rewrites the game's Android-absolute paths onto the SD
// layout. Safe on any path; returns either the input or a rotating buffer.
const char *fix_path(const char *path);

// misc bionic
int __system_property_get_fake(const char *name, char *value);
unsigned long getauxval_fake(unsigned long type);
int gettid_fake(void);
long syscall_fake(long number, ...);
void sincos_fake(double x, double *s, double *c);
void sincosf_fake(float x, float *s, float *c);
int pthread_setname_np_fake(void *thread, const char *name);
void android_set_abort_message_fake(const char *msg);
size_t __ctype_get_mb_cur_max_fake(void);
long sysconf_fake(int name);
struct timespec;
int clock_gettime_fake(int clk_id, struct timespec *tp);
int __isnanf_fake(float f);
int __fpclassifyd_fake(double d);
char *basename_fake(const char *path);
void *memrchr_fake(const void *s, int c, size_t n);
void __assert2_fake(const char *file, int line, const char *func, const char *expr);
int __android_log_write_fake(int prio, const char *tag, const char *msg);
int setpriority_fake(int which, int who, int prio);

// process/signal stubs
int getpid_fake(void);
int fork_fake(void);
int execl_fake(const char *path, const char *arg, ...);
int waitpid_fake(int pid, int *status, int options);
int kill_fake(int pid, int sig);
unsigned alarm_fake(unsigned sec);
void *signal_fake(int sig, void *handler);
int sigaction_fake(int sig, const void *act, void *oact);
int sigprocmask_fake(int how, const void *set, void *oset);

// fs (all remap android-absolute paths through fix_path)
int open_fake(const char *path, int flags, ...);
int open2_fake(const char *path, int flags); // bionic __open_2
int is_urandom_fd_fake(int fd);
long read_fake(int fd, void *buf, size_t count);
long write_fake(int fd, const void *buf, size_t count);
int close_fake(int fd);
int access_fake(const char *path, int mode);
int chmod_fake(const char *path, unsigned mode);
int mkdir_fake(const char *path, unsigned int mode);
int remove_fake(const char *path);
int rename_fake(const char *from, const char *to);
int chdir_fake(const char *path);
struct bionic_stat;
int stat_fake(const char *path, struct bionic_stat *st);
int fstat_fake(int fd, struct bionic_stat *st);
int lstat_fake(const char *path, struct bionic_stat *st);
void *opendir_fake(const char *path);
void *readdir_fake(void *dirp);
char *realpath_fake(const char *path, char *resolved);
int strerror_r_fake(int err, char *buf, size_t len);

// mmap emulation (malloc-backed)
void *mmap_fake(void *addr, size_t len, int prot, int flags, int fd, long off);
int munmap_fake(void *addr, size_t len);

// locale
struct lconv *localeconv_fake(void);
void *newlocale_fake(int mask, const char *locale, void *base);
void freelocale_fake(void *loc);
void *uselocale_fake(void *loc);
long double strtold_l_fake(const char *s, char **end, void *loc);
long long strtoll_l_fake(const char *s, char **end, int base, void *loc);
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc);
size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps);
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps);

// memory
int posix_memalign_fake(void **out, size_t align, size_t size);

// stdio over fake __sF (stdin/stdout/stderr)
extern uint8_t fake_sF[3][0x100];
size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f);
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f);
int fputc_fake(int c, FILE *f);
int fputs_fake(const char *s, FILE *f);
int fflush_fake(FILE *f);
int fclose_fake(FILE *f);
int ferror_fake(FILE *f);
int feof_fake(FILE *f);
int fileno_fake(FILE *f);
int fprintf_fake(FILE *f, const char *fmt, ...);
int vfprintf_fake(FILE *f, const char *fmt, va_list va);
int fseek_fake(FILE *f, long off, int whence);
long ftell_fake(FILE *f);
int getc_fake(FILE *f);
int ungetc_fake(int c, FILE *f);
char *fgets_fake(char *s, int size, FILE *f);
void setbuf_fake(FILE *f, char *buf);
int setvbuf_fake(FILE *f, char *buf, int mode, size_t size);
#include <wchar.h>
wint_t getwc_fake(FILE *f);
wint_t putwc_fake(wchar_t c, FILE *f);
wint_t ungetwc_fake(wint_t c, FILE *f);

// buffered fopen with path remapping (+ big buffer for the .apk archive)
FILE *fopen_fake(const char *path, const char *mode);

// io vector fallback
struct iovec;
long writev_fake(int fd, const struct iovec *iov, int iovcnt);

// pthread extras
int pthread_rwlock_init_fake(void **rw, const void *attr);
int pthread_rwlock_destroy_fake(void **rw);
int pthread_rwlock_rdlock_fake(void **rw);
int pthread_rwlock_wrlock_fake(void **rw);
int pthread_rwlock_unlock_fake(void **rw);
int sem_init_fake(void **s, int pshared, unsigned int value);
int sem_destroy_fake(void **s);
int sem_post_fake(void **s);
int sem_wait_fake(void **s);
int sem_trywait_fake(void **s);

#endif
