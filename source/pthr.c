/* pthr.c -- bionic<->newlib pthread wrappers for libcocos2dcpp.so + libfmod.so
 *
 * Ported from gm666q/lswtcs-vita (reimpl/pthr.c) via lbbg_nx, adapted for
 * devkitA64/libnx; the GL-ownership parking of the Fusion ports is gone (the
 * game renders on one thread only).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <switch.h>

#include "pthr.h"
#include "util.h"

#define BIONIC_PTHREAD_MUTEX_INITIALIZER            0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER  0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER 0x8000

// ---------------------------------------------------------------------------
// fake RW TLS: AArch64 -fstack-protector loads the canary relative to
// TPIDR_EL0, which libnx leaves pointing at read-only/zero storage. Each game
// thread gets a small zeroed block installed in TPIDR_EL0 so those loads (and
// any other bionic TLS-relative reads) land on valid, writable memory. The
// block is intentionally leaked: it must stay live for the thread's lifetime.
// ---------------------------------------------------------------------------

void pthr_install_fake_tls(void) {
  uint8_t *tls = calloc(1, 0x200);
  armSetTlsRw(tls);
}

void pthr_ensure_fake_tls(void) {
  if (armGetTlsRw() == NULL)
    pthr_install_fake_tls();
}

// ---------------------------------------------------------------------------
// lazy first-use initialization of game-owned sync objects: the hot path is
// one acquire-load of the magic word; only init and destroy take init_lock
// ---------------------------------------------------------------------------

#define PTHR_MUTEX_MAGIC 0x4D58544Du // "MTXM"
#define PTHR_COND_MAGIC  0x444E434Du // "CNDM"

static Mutex init_lock;

static int attr_static_init(pthread_attr_t_bionic *attr) {
  if (attr->magic != 0x42424242) {
    attr->magic = 0x42424242;
    attr->real_ptr = malloc(sizeof(pthread_attr_t));
    return pthread_attr_init(attr->real_ptr);
  }
  return 0;
}

static int mutex_static_init(pthread_mutex_t_bionic *mutex, const pthread_mutexattr_t *attr) {
  // pairs with the release store below so real_ptr is visible once magic is
  if (__atomic_load_n(&mutex->magic, __ATOMIC_ACQUIRE) == PTHR_MUTEX_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) == PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock); // another thread won the first-use race
    return 0;
  }

  int kind = PTHREAD_MUTEX_NORMAL;
  if (attr) {
    pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &kind);
  } else {
    // the kind word of a statically initialized bionic mutex (overlaps the
    // low half of real_ptr, which we haven't written yet)
    switch (*(int *)mutex) {
      case BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER:  kind = PTHREAD_MUTEX_RECURSIVE;  break;
      case BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER: kind = PTHREAD_MUTEX_ERRORCHECK; break;
      default:                                          kind = PTHREAD_MUTEX_NORMAL;     break;
    }
  }

  pthread_mutex_t *real = malloc(sizeof(pthread_mutex_t));

  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, kind);
  int ret = pthread_mutex_init(real, &ma);
  pthread_mutexattr_destroy(&ma);

  if (ret == 0) {
    mutex->real_ptr = real;
    __atomic_store_n(&mutex->magic, PTHR_MUTEX_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);
  }
  mutexUnlock(&init_lock);
  return ret;
}

static int cond_static_init(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr) {
  if (__atomic_load_n(&cond->magic, __ATOMIC_ACQUIRE) == PTHR_COND_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) == PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }

  pthread_cond_t *real = malloc(sizeof(pthread_cond_t));
  int ret = pthread_cond_init(real, attr);

  if (ret == 0) {
    cond->real_ptr = real;
    __atomic_store_n(&cond->magic, PTHR_COND_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);
  }
  mutexUnlock(&init_lock);
  return ret;
}

// ---------------------------------------------------------------------------
// core affinity: the wrapper's render/game thread gets the last application
// core to itself; everything spawned by the game/FMOD round-robins the rest
// ---------------------------------------------------------------------------

static Mutex core_lock;
static int core_list[4];
static int core_count = 0;
static unsigned core_rr = 0;

#define HORIZON_RESERVED_CORE 3

static void core_init_once(void) {
  if (core_count)
    return;
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0)
    mask = 0x7; // fallback: cores 0,1,2 (typical homebrew allotment)

  const u64 app_mask = mask & ~(1ull << HORIZON_RESERVED_CORE);
  if (app_mask)
    mask = app_mask;

  for (int c = 0; c < 4; c++)
    if (mask & (1ull << c))
      core_list[core_count++] = c;

  if (!core_count)
    core_list[core_count++] = 0;
}

static void assign_core_bg(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int n = (core_count > 1) ? core_count - 1 : core_count; // cores 0..n-1
  const int core = core_list[core_rr++ % (unsigned)n];
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
}

// pin the caller (the wrapper's render/game thread) to the last core, so it
// never shares a CPU with FMOD's mixer or the game's HTTP workers.
void pthr_pin_render_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int core = core_list[core_count - 1];
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
}

// pin the calling thread to a background core. For threads our vendored code
// spawns directly through newlib pthread_create (the SDL audio mixer and the
// OpenSL play-callback thread) -- they never pass through the trampoline, so
// without this they'd default onto the render core and stutter it. Also raise
// their priority so the FMOD mixer never starves into an audout stall.
void pthr_pin_bg_core(void) {
  assign_core_bg();
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);
}

// vendored OpenSL (sync.c / ThreadPool.c) pins its helper threads with this
void pthr_pin_worker_core(void) {
  assign_core_bg();
}

// ---------------------------------------------------------------------------
// thread creation (installs the fake TLS before running game code)
// ---------------------------------------------------------------------------

typedef struct {
  void *(*start)(void *);
  void *arg;
} ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart s = *(ThreadStart *)p;
  free(p);
  pthr_install_fake_tls();
  assign_core_bg();
  return s.start(s.arg);
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr,
                            void *(*start)(void *), void *param) {
  ThreadStart *s = malloc(sizeof(*s));
  s->start = start;
  s->arg = param;

  pthread_attr_t a;
  pthread_attr_init(&a);
  // FMOD's mixer and the game's loaders are fine with 512k on Android, but
  // recursion depth is cheap to buy here; keep 2 MB unless the game asks big
  pthread_attr_setstacksize(&a, 2 * 1024 * 1024);
  if (attr) {
    attr_static_init((pthread_attr_t_bionic *)attr);
    size_t want = 0;
    if (attr->real_ptr && pthread_attr_getstacksize(attr->real_ptr, &want) == 0 &&
        want > 2 * 1024 * 1024)
      pthread_attr_setstacksize(&a, want);
  }

  int ret = pthread_create(thread, &a, thread_trampoline, s);
  pthread_attr_destroy(&a);
  if (ret != 0)
    free(s);
  return ret;
}

int pthread_join_soloader(pthread_t thread, void **value_ptr) {
  return pthread_join(thread, value_ptr);
}
int pthread_detach_soloader(pthread_t thread) { return pthread_detach(thread); }
pthread_t pthread_self_soloader(void) { return pthread_self(); }

int pthread_equal_soloader(pthread_t t1, pthread_t t2) {
  if (t1 == t2) return 1;
  if (!t1 || !t2) return 0;
  return pthread_equal(t1, t2);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy, struct sched_param *param) {
  // newlib on devkitA64 doesn't expose pthread_getschedparam; the game only
  // reads these to echo them back, so reporting a default schedule is fine
  (void)thread;
  if (policy) *policy = 0; // SCHED_OTHER
  if (param) param->sched_priority = 0;
  return 0;
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine)
    return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

// ---------------------------------------------------------------------------
// mutex / cond / attr
// ---------------------------------------------------------------------------

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_init(attr); }
int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type) { return pthread_mutexattr_settype(attr, type); }
int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_destroy(attr); }

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr) {
  if (!uid) return EINVAL;
  return mutex_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) != PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&mutex->magic, 0, __ATOMIC_RELEASE);
  pthread_mutex_t *real = mutex->real_ptr;
  mutex->real_ptr = NULL;
  mutexUnlock(&init_lock);
  int ret = pthread_mutex_destroy(real);
  free(real);
  return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  return pthread_mutex_lock(mutex->real_ptr);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex || !mutex->real_ptr) return EINVAL;
  return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr) {
  if (!cond) return EINVAL;
  return cond_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) != PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&cond->magic, 0, __ATOMIC_RELEASE);
  pthread_cond_t *real = cond->real_ptr;
  cond->real_ptr = NULL;
  mutexUnlock(&init_lock);
  int ret = pthread_cond_destroy(real);
  free(real);
  return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  return pthread_cond_broadcast(cond->real_ptr);
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  return pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex,
                                    struct timespec *abstime) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  return pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr) {
  if (!attr) return EINVAL;
  return attr_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr) {
  if (!attr || attr->magic != 0x42424242) return 0;
  int ret = pthread_attr_destroy(attr->real_ptr);
  free(attr->real_ptr);
  attr->magic = 0;
  return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state) {
  if (!attr) return -1;
  attr_static_init(attr);
  return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
  if (!attr) return -1;
  attr_static_init(attr);
  return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}
