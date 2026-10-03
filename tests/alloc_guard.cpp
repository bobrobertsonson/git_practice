#include "alloc_guard.h"

#include <cstdlib>
#include <new>

namespace {
thread_local bool g_armed = false;
thread_local long g_count = 0;

inline void note() noexcept {
  if (g_armed) ++g_count;
}

void* allocate(std::size_t n) {
  note();
  if (n == 0) n = 1;
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}

void* allocateNoThrow(std::size_t n) noexcept {
  note();
  if (n == 0) n = 1;
  return std::malloc(n);
}

void* allocateAligned(std::size_t n, std::align_val_t al) {
  note();
  if (n == 0) n = 1;
  void* p = nullptr;
  std::size_t a = static_cast<std::size_t>(al);
  if (a < sizeof(void*)) a = sizeof(void*);
  if (posix_memalign(&p, a, n) != 0) throw std::bad_alloc();
  return p;
}

void* allocateAlignedNoThrow(std::size_t n, std::align_val_t al) noexcept {
  note();
  if (n == 0) n = 1;
  void* p = nullptr;
  std::size_t a = static_cast<std::size_t>(al);
  if (a < sizeof(void*)) a = sizeof(void*);
  return posix_memalign(&p, a, n) == 0 ? p : nullptr;
}
}  // namespace

namespace sawblade::test {
AllocGuard::AllocGuard() noexcept {
  g_count = 0;
  g_armed = true;
}
AllocGuard::~AllocGuard() { g_armed = false; }
long AllocGuard::count() const noexcept { return g_count; }
}  // namespace sawblade::test

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocateNoThrow(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocateNoThrow(n); }
void* operator new(std::size_t n, std::align_val_t a) { return allocateAligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocateAligned(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return allocateAlignedNoThrow(n, a);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return allocateAlignedNoThrow(n, a);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
