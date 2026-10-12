#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <new>
#include <vector>

#include "alloc_guard.h"

using sawblade::test::AllocGuard;

namespace {
// Compiler barrier so new/delete pairs are not elided by the optimizer.
inline void keep(void* p) { asm volatile("" : : "r"(p) : "memory"); }
}  // namespace

TEST_CASE("AllocGuard counts armed allocations only", "[allocguard]") {
  long armed = 0;
  {
    AllocGuard g;
    int* p = new int(42);
    std::vector<int>* v = new std::vector<int>();
    v->reserve(16);
    auto* a = new int[4];
    auto* n = new (std::nothrow) int(7);
    struct alignas(64) Wide { char c[64]; };
    auto* w = new Wide;
    keep(p); keep(v); keep(a); keep(n); keep(w);
    armed = g.count();
    delete p; delete v; delete[] a; delete n; delete w;
  }
  REQUIRE(armed == 6);  // int, vector, vector storage, int[4], nothrow int, over-aligned
}

TEST_CASE("AllocGuard reports zero when nothing allocates", "[allocguard]") {
  AllocGuard g;
  volatile int x = 0;
  for (int i = 0; i < 100; ++i) x = x + i;
  REQUIRE(g.count() == 0);
}

TEST_CASE("AllocGuard is not armed outside its scope", "[allocguard]") {
  long inside = 0;
  { AllocGuard g; inside = g.count(); }
  auto p = std::make_unique<int>(1);  // disarmed: must not disturb a later guard
  AllocGuard g2;
  REQUIRE(inside == 0);
  REQUIRE(g2.count() == 0);
}
