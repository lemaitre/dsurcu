#include "dsurcu.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <functional>
#include <thread>
#include <random>
#include <cstdint>
#include <cstdio>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/time.h>
#include <x86intrin.h>

#ifndef TASK_FUTEX
#define TASK_FUTEX 0
#endif
#ifndef EPOCH_FUTEX
#define EPOCH_FUTEX 0
#endif


// futex syscall
static int futex(int *uaddr, int futex_op, int val, const struct timespec *timeout, int *uaddr2, int val3) {
  return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

static uint64_t unoptimizable_zero() noexcept {
  uint64_t zero = 0;
  asm("":"+rm"(zero));
  return zero;
}

static int noop_wait = 0;

namespace dsurcu {
  void Noop::process_queue_wait() noexcept {
    futex(&noop_wait, FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
  }
  void Noop::wake() noexcept {
    noop_wait = 1;
    futex(&noop_wait, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
  }
  /**
   * LocalEpoch
   */
  struct LocalEpoch::Registry {
    // list of thread epochs (sorted)
    std::vector<std::atomic<uint64_t>*> epochs{};
    // epoch list must be guarded
    std::mutex mutex{};
    // lock-free list of tasks
    std::atomic<Node*> tasks{nullptr};
  };

  void LocalEpoch::init() {
    // force symbol emission
    asm(""::"m"(read_lock), "m"(read_unlock), "m"(quiescent), "m"(queue));
    registry.construct();
  }
  void LocalEpoch::thread_init() {
    local_epoch.construct();
    Registry& registry = LocalEpoch::registry;
    std::atomic<uint64_t>* ptr = &local_epoch.get();

    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = std::lower_bound(registry.epochs.begin(), registry.epochs.end(), ptr);
    if (it != registry.epochs.end() && *it == ptr) return;
    registry.epochs.insert(it, ptr);
  }
  void LocalEpoch::thread_fini() {
    Registry& registry = LocalEpoch::registry;
    std::atomic<uint64_t>* ptr = &local_epoch.get();

    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = std::lower_bound(registry.epochs.begin(), registry.epochs.end(), ptr);
    if (it == registry.epochs.end() || *it != ptr) return;
    registry.epochs.erase(it);
    local_epoch.destruct();
  }
  void LocalEpoch::fini() {
    registry.destruct();
  }
  void LocalEpoch::queue(void* p) {
    Registry& registry = LocalEpoch::registry;
    Node* tail = registry.tasks.load(std::memory_order_acquire);
    Node* head = Node::get(p);
    head->next = tail;
    // push front the new task (lock-free)
    while (!registry.tasks.compare_exchange_weak(tail, head, std::memory_order_acq_rel)) {
      head->next = tail;
      _mm_pause();
    }

#if TASK_FUTEX
    // if there was no task in list, we wake the rcu thread
    if (tail == nullptr) {
      futex((int*)(&registry.tasks), FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    }
#endif
  }
  void LocalEpoch::synchronize() {
    Registry& registry = LocalEpoch::registry;
    struct pair {
      std::atomic<uint64_t>* ptr;
      uint64_t t;
    };
    std::vector<pair> epochs, intersection;

    // Release for all epoch that will be written (propagate resource values)
    std::atomic_thread_fence(std::memory_order_release);

    // find which thread is late
    { std::lock_guard<std::mutex> lock(registry.mutex);
      epochs.reserve(registry.epochs.size());
      for (std::atomic<uint64_t>* ptr : registry.epochs) {
        uint64_t t = ptr->fetch_add(unoptimizable_zero(), std::memory_order_relaxed);
        //uint64_t t = ptr->load(std::memory_order_relaxed); // doesn't work
        if (t&1) {
          epochs.emplace_back(pair{ptr, t});
        }
      }
    }
    intersection.reserve(epochs.size());

    // wait for all threads to exit their critical section
    while (!epochs.empty()) {
      struct timespec deadline;
      deadline.tv_sec = 0;
      deadline.tv_nsec = 1000000; // 1 ms
      // futex would most likely block as readers don't notify anybody
#if EPOCH_FUTEX
      futex((int*)(epochs[0].ptr), FUTEX_WAIT_PRIVATE, epochs[0].t, &deadline, 0, 0);
#else
      std::this_thread::yield();
#endif

      // remove threads that have reach current epoch
      intersection.clear();
      { std::lock_guard<std::mutex> lock(registry.mutex);
        auto it0 = registry.epochs.begin();
        auto it1 = epochs.begin();
        auto end0 = registry.epochs.end();
        auto end1 = epochs.end();
        while (it0 != end0 && it1 != end1) {
          if (*it0 < it1->ptr) {
            ++it0;
          } else if (*it0 > it1->ptr) {
            ++it1;
          } else /*if (*it0 == it1->ptr)*/ {
            // RMW is not necessary here as the release has already been done by a previous one
            uint64_t t = it1->ptr->load(std::memory_order_relaxed);
            if (t == it1->t) {
              intersection.push_back(*it1);
            }
            ++it0;
            ++it1;
          }
        }
      }
      std::swap(epochs, intersection);
    }

    // Acquire for all epochs read (ensure readers have finished)
    std::atomic_thread_fence(std::memory_order_acquire);
  }
  void LocalEpoch::process_queue() {
    Registry& registry = LocalEpoch::registry;

    // extract the whole list of tasks (lock-free)
    // acquire ensures we see the newest value of the resource linked to the task
    Node* tasks = registry.tasks.exchange(nullptr, std::memory_order_acquire);

    // no pending tasks, no need to wait
    if (!tasks) return;

    // wait for all threads
    synchronize();

    // process all the tasks and free their nodes
    do {
      Node* task = tasks;
      tasks = task->next;
      task->reclaim(task);
    } while (tasks);
  }

  void LocalEpoch::process_queue_wait() {
    // wait for a task
#if TASK_FUTEX
    Registry& registry = LocalEpoch::registry;
    futex((int*)(&registry.tasks), FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
#else
    std::this_thread::yield();
#endif
  }
  void LocalEpoch::process_queue_busy() {
    Registry& registry = LocalEpoch::registry;
    // keep going as long as there are tasks
    while (registry.tasks.load(std::memory_order_relaxed)) {
      process_queue();
    }
  }
  void LocalEpoch::wake() {
#if TASK_FUTEX
    Registry& registry = LocalEpoch::registry;
    futex((int*)(&registry.tasks), FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
#endif
  }

  Delay<LocalEpoch::Registry> LocalEpoch::registry;



  /**
   * LocalEpochWeak
   */
  struct LocalEpochWeak::Registry {
    // list of thread epochs (sorted)
    std::vector<std::atomic<uint64_t>*> epochs{};
    // epoch list must be guarded
    std::mutex mutex{};
    // lock-free list of tasks
    std::atomic<Node*> tasks{nullptr};
  };

  void LocalEpochWeak::init() {
    // force symbol emission
    asm(""::"m"(read_lock), "m"(read_unlock), "m"(quiescent), "m"(queue));
    registry.construct();
  }
  void LocalEpochWeak::thread_init() {
    local_epoch.construct();
    Registry& registry = LocalEpochWeak::registry;
    std::atomic<uint64_t>* ptr = &local_epoch.get();

    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = std::lower_bound(registry.epochs.begin(), registry.epochs.end(), ptr);
    if (it != registry.epochs.end() && *it == ptr) return;
    registry.epochs.insert(it, ptr);
  }
  void LocalEpochWeak::thread_fini() {
    Registry& registry = LocalEpochWeak::registry;
    std::atomic<uint64_t>* ptr = &local_epoch.get();

    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = std::lower_bound(registry.epochs.begin(), registry.epochs.end(), ptr);
    if (it == registry.epochs.end() || *it != ptr) return;
    registry.epochs.erase(it);
    local_epoch.destruct();
  }
  void LocalEpochWeak::fini() {
    registry.destruct();
  }
  void LocalEpochWeak::queue(void* p) {
    Registry& registry = LocalEpochWeak::registry;
    Node* tail = registry.tasks.load(std::memory_order_acquire);
    Node* head = Node::get(p);
    head->next = tail;
    // push front the new task (lock-free)
    while (!registry.tasks.compare_exchange_weak(tail, head, std::memory_order_acq_rel)) {
      head->next = tail;
      _mm_pause();
    }

#if TASK_FUTEX
    // if there was no task in list, we wake the rcu thread
    if (tail == nullptr) {
      futex((int*)(&registry.tasks), FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    }
#endif
  }
  void LocalEpochWeak::synchronize() {
    Registry& registry = LocalEpochWeak::registry;
    struct pair {
      std::atomic<uint64_t>* ptr;
      uint64_t t;
    };
    std::vector<pair> epochs, intersection;

    std::atomic_thread_fence(std::memory_order_seq_cst);

    // propagate new resource values
    { std::lock_guard<std::mutex> lock(registry.mutex);
      epochs.reserve(registry.epochs.size());
      for (std::atomic<uint64_t>* ptr : registry.epochs) {
        ptr->fetch_add(unoptimizable_zero());
      }
    }

    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Delay current thread (should be enough for writer values to propagate to readers)
    std::this_thread::yield();

    // find which thread is late
    { std::lock_guard<std::mutex> lock(registry.mutex);
      epochs.reserve(registry.epochs.size());
      for (std::atomic<uint64_t>* ptr : registry.epochs) {
        uint64_t t = ptr->load(std::memory_order_relaxed);
        if (t&1) {
          epochs.emplace_back(pair{ptr, t});
        }
      }
    }
    intersection.reserve(epochs.size());

    // wait for all threads to exit their critical section
    while (!epochs.empty()) {
      struct timespec deadline;
      deadline.tv_sec = 0;
      deadline.tv_nsec = 1000000; // 1 ms
      // futex would most likely block as readers don't notify anybody
#if EPOCH_FUTEX
      futex((int*)(epochs[0].ptr), FUTEX_WAIT_PRIVATE, epochs[0].t, &deadline, 0, 0);
#else
      std::this_thread::yield();
#endif

      // remove threads that have reach current epoch
      intersection.clear();
      { std::lock_guard<std::mutex> lock(registry.mutex);
        auto it0 = registry.epochs.begin();
        auto it1 = epochs.begin();
        auto end0 = registry.epochs.end();
        auto end1 = epochs.end();
        while (it0 != end0 && it1 != end1) {
          if (*it0 < it1->ptr) {
            ++it0;
          } else if (*it0 > it1->ptr) {
            ++it1;
          } else /*if (*it0 == it1->ptr)*/ {
            uint64_t t = it1->ptr->load(std::memory_order_relaxed);
            if (t == it1->t) {
              intersection.push_back(*it1);
            }
            ++it0;
            ++it1;
          }
        }
      }
      std::swap(epochs, intersection);
    }

    // Acquire for all epochs read (ensure readers have finished)
    std::atomic_thread_fence(std::memory_order_acquire);
  }
  void LocalEpochWeak::process_queue() {
    Registry& registry = LocalEpochWeak::registry;

    // extract the whole list of tasks (lock-free)
    // acquire ensures we see the newest value of the resource linked to the task
    Node* tasks = registry.tasks.exchange(nullptr, std::memory_order_acquire);

    // no pending tasks, no need to wait
    if (!tasks) return;

    // wait for all threads
    synchronize();

    // process all the tasks and free their nodes
    do {
      Node* task = tasks;
      tasks = task->next;
      task->reclaim(task);
    } while (tasks);
  }

  void LocalEpochWeak::process_queue_wait() {
    // wait for a task
#if TASK_FUTEX
    Registry& registry = LocalEpochWeak::registry;
    futex((int*)(&registry.tasks), FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
#else
    std::this_thread::yield();
#endif
  }
  void LocalEpochWeak::process_queue_busy() {
    Registry& registry = LocalEpochWeak::registry;
    // keep going as long as there are tasks
    while (registry.tasks.load(std::memory_order_relaxed)) {
      process_queue();
    }
  }
  void LocalEpochWeak::wake() {
#if TASK_FUTEX
    Registry& registry = LocalEpochWeak::registry;
    futex((int*)(&registry.tasks), FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
#endif
  }

  Delay<LocalEpochWeak::Registry> LocalEpochWeak::registry;


  /**
   * GlobalEpoch
   */
  struct GlobalEpoch::Registry {
    // list of thread epochs (sorted)
    std::vector<std::atomic<uint64_t>*> epochs{};
    // epoch list must be guarded
    std::mutex mutex{};
    // lock-free list of tasks
    std::atomic<Node*> tasks{nullptr};
  };

  void GlobalEpoch::init() {
    // force symbol emission
    asm(""::"m"(read_lock), "m"(read_unlock), "m"(quiescent), "m"(queue));
    global_epoch.construct(0);
    registry.construct();
  }
  void GlobalEpoch::thread_init() {
    local_epoch.construct(0);
    Registry& registry = GlobalEpoch::registry;
    std::atomic<uint64_t>* ptr = &local_epoch.get();

    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = std::lower_bound(registry.epochs.begin(), registry.epochs.end(), ptr);
    if (it != registry.epochs.end() && *it == ptr) return;
    registry.epochs.insert(it, ptr);
  }
  void GlobalEpoch::thread_fini() {
    Registry& registry = GlobalEpoch::registry;
    std::atomic<uint64_t>* ptr = &local_epoch.get();

    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = std::lower_bound(registry.epochs.begin(), registry.epochs.end(), ptr);
    if (it == registry.epochs.end() || *it != ptr) return;
    registry.epochs.erase(it);
    local_epoch.destruct();
  }
  void GlobalEpoch::fini() {
    registry.destruct();
    global_epoch.destruct();
  }
  void GlobalEpoch::queue(void* p) {
    Registry& registry = GlobalEpoch::registry;
    Node* tail = registry.tasks.load(std::memory_order_acquire);
    Node* head = Node::get(p);
    head->next = tail;
    // push front the new task (lock-free)
    while (!registry.tasks.compare_exchange_weak(tail, head, std::memory_order_acq_rel)) {
      head->next = tail;
      _mm_pause();
    }

#if TASK_FUTEX
    // if there was no task in list, we wake the rcu thread
    if (tail == nullptr) {
      futex((int*)(&registry.tasks), FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    }
#endif
  }
  void GlobalEpoch::synchronize() {
    std::atomic<uint64_t>& global = global_epoch;
    Registry& registry = GlobalEpoch::registry;
    struct pair {
      std::atomic<uint64_t>* ptr;
      uint64_t t;
    };
    std::vector<pair> epochs, intersection;

    // Release for all epoch that will be written (propagate resource values)
    std::atomic_thread_fence(std::memory_order_release);

    uint64_t epoch = global.fetch_add(1, std::memory_order_relaxed);

    // find which thread is late
    { std::lock_guard<std::mutex> lock(registry.mutex);
      epochs.reserve(registry.epochs.size());
      for (std::atomic<uint64_t>* ptr : registry.epochs) {
        uint64_t t = ptr->load(std::memory_order_relaxed);
        if (t <= epoch) {
          epochs.emplace_back(pair{ptr, t});
        }
      }
    }
    intersection.reserve(epochs.size());

    // wait for all threads to get to current epoch
    while (!epochs.empty()) {
      struct timespec deadline;
      deadline.tv_sec = 0;
      deadline.tv_nsec = 1000000; // 1 ms
      // futex would most likely block as readers don't notify anybody
#if EPOCH_FUTEX
      futex((int*)(epochs[0].ptr), FUTEX_WAIT_PRIVATE, epochs[0].t, &deadline, 0, 0);
#else
      std::this_thread::yield();
#endif

      // remove threads that have reach current epoch
      intersection.clear();
      { std::lock_guard<std::mutex> lock(registry.mutex);
        auto it0 = registry.epochs.begin();
        auto it1 = epochs.begin();
        auto end0 = registry.epochs.end();
        auto end1 = epochs.end();
        while (it0 != end0 && it1 != end1) {
          if (*it0 < it1->ptr) {
            ++it0;
          } else if (*it0 > it1->ptr) {
            ++it1;
          } else /*if (*it0 == it1->ptr)*/ {
            uint64_t t = it1->ptr->load(std::memory_order_relaxed);
            if (t <= epoch) {
              intersection.push_back(*it1);
            }
            ++it0;
            ++it1;
          }
        }
      }
      std::swap(epochs, intersection);
    }

    // Acquire for all epochs read (ensure readers have finished)
    std::atomic_thread_fence(std::memory_order_acquire);
  }
  void GlobalEpoch::process_queue() {
    Registry& registry = GlobalEpoch::registry;

    // extract the whole list of tasks (lock-free)
    // acquire ensures we see the newest value of the resource linked to the task
    Node* tasks = registry.tasks.exchange(nullptr, std::memory_order_acquire);

    // no pending tasks, no need to wait
    if (!tasks) return;

    // wait for all threads
    synchronize();

    // process all the tasks and free their nodes
    do {
      Node* task = tasks;
      tasks = task->next;
      task->reclaim(task);
    } while (tasks);
  }

  void GlobalEpoch::process_queue_wait() {
    // wait for a task
#if TASK_FUTEX
    Registry& registry = GlobalEpoch::registry;
    futex((int*)(&registry.tasks), FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
#else
    std::this_thread::yield();
#endif
  }
  void GlobalEpoch::process_queue_busy() {
    Registry& registry = GlobalEpoch::registry;
    // keep going as long as there are tasks
    while (registry.tasks.load(std::memory_order_relaxed)) {
      process_queue();
    }
  }
  void GlobalEpoch::wake() {
#if TASK_FUTEX
    Registry& registry = GlobalEpoch::registry;
    futex((int*)(&registry.tasks), FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
#endif
  }

  Delay<GlobalEpoch::Registry> GlobalEpoch::registry;
}
