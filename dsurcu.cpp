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
// futex syscall
static int futex(int *uaddr, int futex_op, int val, const struct timespec *timeout, int *uaddr2, int val3) {
  return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

static uint64_t unoptimizable_zero() noexcept {
  uint64_t zero = 0;
  asm("":"+rm"(zero));
  return zero;
}

namespace dsurcu {
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
  void LocalEpoch::queue(std::function<void()> f) {
    Registry& registry = LocalEpoch::registry;
    Node* tail = registry.tasks.load(std::memory_order_acquire);
    Node* head = new Node{tail, std::move(f)};
    // push front the new task (lock-free)
    while (!registry.tasks.compare_exchange_weak(tail, head, std::memory_order_acq_rel)) {
      head->next = tail;
      _mm_pause();
    }

    // if there was no task in list, we wake the rcu thread
    if (tail == nullptr) {
      futex((int*)(&registry.tasks), FUTEX_WAKE, 1, 0, 0, 0);
    }
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
      futex((int*)(epochs[0].ptr), FUTEX_WAIT, epochs[0].t, &deadline, 0, 0);

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
      tasks = tasks->next;
      task->callback();
      delete task;
    } while (tasks);
  }

  void LocalEpoch::process_queue_worker() {
    Registry& registry = LocalEpoch::registry;

    // wait for a task
    futex((int*)(&registry.tasks), FUTEX_WAIT, 0, 0, 0, 0);

    // keep going as long as there are tasks
    while (registry.tasks.load(std::memory_order_relaxed)) {
      process_queue();
    }
  }
  void LocalEpoch::wake_worker() {
    Registry& registry = LocalEpoch::registry;
    futex((int*)(&registry.tasks), FUTEX_WAKE, 1, 0, 0, 0);
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
  void LocalEpochWeak::queue(std::function<void()> f) {
    Registry& registry = LocalEpochWeak::registry;
    Node* tail = registry.tasks.load(std::memory_order_acquire);
    Node* head = new Node{tail, std::move(f)};
    // push front the new task (lock-free)
    while (!registry.tasks.compare_exchange_weak(tail, head, std::memory_order_acq_rel)) {
      head->next = tail;
      _mm_pause();
    }

    // if there was no task in list, we wake the rcu thread
    if (tail == nullptr) {
      futex((int*)(&registry.tasks), FUTEX_WAKE, 1, 0, 0, 0);
    }
  }
  void LocalEpochWeak::synchronize() {
    Registry& registry = LocalEpochWeak::registry;
    struct pair {
      std::atomic<uint64_t>* ptr;
      uint64_t t;
    };
    std::vector<pair> epochs, intersection;

    // Delay current thread (should be enough for writer values to propagate to readers)
    std::atomic_thread_fence(std::memory_order_seq_cst);
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
      futex((int*)(epochs[0].ptr), FUTEX_WAIT, epochs[0].t, &deadline, 0, 0);

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
      tasks = tasks->next;
      task->callback();
      delete task;
    } while (tasks);
  }

  void LocalEpochWeak::process_queue_worker() {
    Registry& registry = LocalEpochWeak::registry;

    // wait for a task
    futex((int*)(&registry.tasks), FUTEX_WAIT, 0, 0, 0, 0);

    // keep going as long as there are tasks
    while (registry.tasks.load(std::memory_order_relaxed)) {
      process_queue();
    }
  }
  void LocalEpochWeak::wake_worker() {
    Registry& registry = LocalEpochWeak::registry;
    futex((int*)(&registry.tasks), FUTEX_WAKE, 1, 0, 0, 0);
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
  void GlobalEpoch::queue(std::function<void()> f) {
    Registry& registry = GlobalEpoch::registry;
    Node* tail = registry.tasks.load(std::memory_order_acquire);
    Node* head = new Node{tail, std::move(f)};
    // push front the new task (lock-free)
    while (!registry.tasks.compare_exchange_weak(tail, head, std::memory_order_acq_rel)) {
      head->next = tail;
      _mm_pause();
    }

    // if there was no task in list, we wake the rcu thread
    if (tail == nullptr) {
      futex((int*)(&registry.tasks), FUTEX_WAKE, 1, 0, 0, 0);
    }
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
      futex((int*)(epochs[0].ptr), FUTEX_WAIT, epochs[0].t, &deadline, 0, 0);

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
      tasks = tasks->next;
      task->callback();
      delete task;
    } while (tasks);
  }

  void GlobalEpoch::process_queue_worker() {
    Registry& registry = GlobalEpoch::registry;

    // wait for a task
    futex((int*)(&registry.tasks), FUTEX_WAIT, 0, 0, 0, 0);

    // keep going as long as there are tasks
    while (registry.tasks.load(std::memory_order_relaxed)) {
      process_queue();
    }
  }
  void GlobalEpoch::wake_worker() {
    Registry& registry = GlobalEpoch::registry;
    futex((int*)(&registry.tasks), FUTEX_WAKE, 1, 0, 0, 0);
  }

  Delay<GlobalEpoch::Registry> GlobalEpoch::registry;
}
