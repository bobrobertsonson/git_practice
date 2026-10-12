#include "lock_guard.h"

#ifdef SAWBLADE_LOCK_WRAP
#include <pthread.h>

namespace {
thread_local bool g_armed = false;
thread_local long g_count = 0;
inline void note() noexcept {
  if (g_armed) ++g_count;
}
}  // namespace

extern "C" {
int __real_pthread_mutex_lock(pthread_mutex_t*);
int __real_pthread_mutex_trylock(pthread_mutex_t*);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t*);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t*);

int __wrap_pthread_mutex_lock(pthread_mutex_t* m) {
  note();
  return __real_pthread_mutex_lock(m);
}
int __wrap_pthread_mutex_trylock(pthread_mutex_t* m) {
  note();
  return __real_pthread_mutex_trylock(m);
}
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t* l) {
  note();
  return __real_pthread_rwlock_rdlock(l);
}
int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t* l) {
  note();
  return __real_pthread_rwlock_wrlock(l);
}
}

namespace sawblade::test {
LockGuard::LockGuard() noexcept {
  g_count = 0;
  g_armed = true;
}
LockGuard::~LockGuard() { g_armed = false; }
long LockGuard::count() const noexcept { return g_count; }
bool LockGuard::enabled() noexcept { return true; }
}  // namespace sawblade::test

#else

namespace sawblade::test {
LockGuard::LockGuard() noexcept {}
LockGuard::~LockGuard() {}
long LockGuard::count() const noexcept { return 0; }
bool LockGuard::enabled() noexcept { return false; }
}  // namespace sawblade::test

#endif
