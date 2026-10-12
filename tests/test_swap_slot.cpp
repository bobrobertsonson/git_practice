#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <thread>

#include "alloc_guard.h"
#include "sawblade/swap_slot.h"

using namespace sawblade;
using sawblade::test::AllocGuard;

namespace {

std::atomic<int> g_live{0};
constexpr std::uint64_t kMagic = 0xC0FFEE1234567890ull;

struct Obj {
  explicit Obj(std::uint64_t s) : seq(s) { ++g_live; }
  ~Obj() {
    magic = 0xDEADDEADDEADDEADull;  // poison: a use-after-free read would fail the check
    --g_live;
  }
  std::uint64_t magic = kMagic;
  std::uint64_t seq;
  std::uint64_t payload[8] = {};
};

}  // namespace

TEST_CASE("SwapSlot hands objects over and retires the old ones", "[swapslot]") {
  g_live = 0;
  {
    SwapSlot<Obj> slot;
    REQUIRE(slot.current() == nullptr);
    slot.publish(std::make_unique<Obj>(1));
    Obj* a = slot.current();
    REQUIRE(a != nullptr);
    REQUIRE(a->seq == 1);
    REQUIRE(slot.current() == a);  // stable until something new is published

    slot.publish(std::make_unique<Obj>(2));
    slot.publish(std::make_unique<Obj>(3));  // 2 never consumed: dropped by the producer
    REQUIRE(g_live == 2);                    // 1 (active) + 3 (pending)
    Obj* c = slot.current();
    REQUIRE(c->seq == 3);
    REQUIRE(g_live == 2);                    // 1 is retired, not yet deleted
    slot.collectGarbage();
    REQUIRE(g_live == 1);
  }
  REQUIRE(g_live == 0);
}

TEST_CASE("SwapSlot stress: 10000 publishes against a spinning consumer", "[swapslot][stress]") {
  g_live = 0;
  constexpr std::uint64_t kCount = 10000;
  SwapSlot<Obj> slot;
  std::atomic<bool> done{false};
  std::atomic<bool> bad{false};
  std::atomic<std::uint64_t> lastSeen{0};

  std::thread consumer([&] {
    std::uint64_t prev = 0;
    for (;;) {
      const bool finished = done.load(std::memory_order_acquire);
      Obj* o = slot.current();
      if (o != nullptr) {
        if (o->magic != kMagic || o->seq < prev || o->seq > kCount) bad = true;
        prev = o->seq;
      }
      if (finished) break;
    }
    lastSeen = prev;
  });

  for (std::uint64_t i = 1; i <= kCount; ++i) {
    slot.publish(std::make_unique<Obj>(i));
    if (i % 7 == 0) slot.collectGarbage();
  }
  done.store(true, std::memory_order_release);
  consumer.join();

  REQUIRE_FALSE(bad.load());
  REQUIRE(lastSeen.load() == kCount);  // consumer's final read (after done) sees the last publish
  Obj* last = slot.current();
  REQUIRE(last != nullptr);
  REQUIRE(last->seq == kCount);
  slot.collectGarbage();
  REQUIRE(g_live == 1);  // only the active object remains
}

TEST_CASE("SwapSlot destroys everything on destruction", "[swapslot]") {
  g_live = 0;
  {
    SwapSlot<Obj> slot;
    for (std::uint64_t i = 1; i <= 50; ++i) {
      slot.publish(std::make_unique<Obj>(i));
      slot.current();
    }
    slot.publish(std::make_unique<Obj>(51));  // pending, never consumed
  }
  REQUIRE(g_live == 0);
}

TEST_CASE("SwapSlot::current performs no allocations", "[swapslot][alloc]") {
  SwapSlot<Obj> slot;
  slot.publish(std::make_unique<Obj>(1));
  slot.current();
  slot.publish(std::make_unique<Obj>(2));
  long allocs = -1;
  {
    AllocGuard g;
    for (int i = 0; i < 100; ++i) slot.current();  // includes the pickup + retire of Obj 1
    allocs = g.count();
  }
  REQUIRE(allocs == 0);
  REQUIRE(slot.current()->seq == 2);
}
