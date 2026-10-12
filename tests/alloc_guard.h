#pragma once

// Test-harness allocation counter. alloc_guard.cpp replaces the global operator new/delete
// family in the test executable; allocations are counted only while a guard is armed on the
// current thread.
namespace sawblade::test {

class AllocGuard {
 public:
  AllocGuard() noexcept;   // arms counting on this thread (resets the count)
  ~AllocGuard();           // disarms
  AllocGuard(const AllocGuard&) = delete;
  AllocGuard& operator=(const AllocGuard&) = delete;

  // Number of allocations on this thread since this guard was armed.
  long count() const noexcept;
  // Number of deallocations (non-null operator delete calls) on this thread since armed.
  long frees() const noexcept;
};

}  // namespace sawblade::test
