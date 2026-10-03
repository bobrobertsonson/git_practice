#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace sawblade {

// Lock-free handoff of heap objects from one producer thread (message/background) to one
// consumer thread (audio). Exactly one thread may call publish()/collectGarbage() (the
// producer) and exactly one other thread may call current() (the consumer). No mutex; only
// std::atomic. The audio thread never allocates or frees.
//
// Mechanism: publish() wraps the object in a heap Node (producer thread) and exchanges it into
// `pending_`. current() takes `pending_` (if any), makes it the active node, and pushes the node
// it replaced onto the lock-free `retired_` stack (the node is already allocated: no
// allocation on the audio thread). collectGarbage() takes the whole retired stack and deletes
// it on the producer thread.
//
// Memory orders:
//   * pending_.exchange in publish(): acq_rel. release publishes the object's contents to the
//     consumer; acquire pairs with nothing the consumer writes but lets us safely delete a
//     never-consumed predecessor it replaces.
//   * pending_.load/exchange in current(): acquire, pairing with publish()'s release, so the
//     consumer sees a fully constructed/prepared object.
//   * retired_ push (CAS, release on success) and retired_.exchange in collectGarbage()
//     (acquire): the consumer's last uses of the retired object happen-before its deletion.
//     Only the consumer pushes and only the producer pops (taking the entire list), so there
//     is no ABA hazard.
//   * cur_ is touched only by the consumer, and by the destructor after both threads stopped.
//
// Contract: the consumer must not hold the pointer returned by current() across the next
// current() call. T needs no particular properties other than being deletable.
template <class T>
class SwapSlot {
 public:
  SwapSlot() = default;
  SwapSlot(const SwapSlot&) = delete;
  SwapSlot& operator=(const SwapSlot&) = delete;

  ~SwapSlot() {
    delete cur_;
    delete pending_.load(std::memory_order_relaxed);
    deleteList(retired_.load(std::memory_order_relaxed));
  }

  // Producer thread. `obj` must already be fully prepared. If a previously published object
  // has not been picked up yet, it is replaced (and deleted here, since the consumer never saw it).
  void publish(std::unique_ptr<T> obj) {
    Node* n = new Node{std::move(obj), nullptr};
    Node* old = pending_.exchange(n, std::memory_order_acq_rel);
    delete old;
  }

  // Consumer thread, once at the start of each block. Wait-free, allocation-free.
  T* current() noexcept {
    if (pending_.load(std::memory_order_acquire) != nullptr) {
      Node* n = pending_.exchange(nullptr, std::memory_order_acquire);
      if (n != nullptr) {
        if (cur_ != nullptr) retire(cur_);
        cur_ = n;
      }
    }
    return cur_ != nullptr ? cur_->obj.get() : nullptr;
  }

  // Producer thread: destroys objects the consumer has replaced.
  void collectGarbage() noexcept {
    deleteList(retired_.exchange(nullptr, std::memory_order_acquire));
  }

 private:
  struct Node {
    std::unique_ptr<T> obj;
    Node* next;
  };

  void retire(Node* n) noexcept {
    Node* head = retired_.load(std::memory_order_relaxed);
    do {
      n->next = head;
    } while (!retired_.compare_exchange_weak(head, n, std::memory_order_release, std::memory_order_relaxed));
  }

  static void deleteList(Node* n) noexcept {
    while (n != nullptr) {
      Node* next = n->next;
      delete n;
      n = next;
    }
  }

  std::atomic<Node*> pending_{nullptr};
  std::atomic<Node*> retired_{nullptr};
  Node* cur_ = nullptr;  // consumer-only
};

}  // namespace sawblade
