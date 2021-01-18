/*
Copyright 2020 Florian Lemaitre

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

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
int futex(int *uaddr, int futex_op, int val, const struct timespec *timeout, int *uaddr2, int val3) {
  return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}


namespace dsurcu {
  // Node for task queueing
  struct Node {
    Node* next = nullptr;
    std::function<void()> callback{};
  };

  // Thread registry
  struct Registry {
    // list of thread epochs (sorted)
    std::vector<uint64_t*> epochs{};
    // epoch list must be guarded
    std::mutex mutex{};
    // lock-free list of tasks
    std::atomic<Node*> tasks{nullptr};

    // register a new thread
    void register_ptr(uint64_t* ptr) {
      std::lock_guard<std::mutex> lock(mutex);
      auto it = std::lower_bound(epochs.begin(), epochs.end(), ptr);
      if (it != epochs.end() && *it == ptr) return;
      epochs.insert(it, ptr);
    }

    // unregister a thread
    void unregister_ptr(uint64_t* ptr) {
      std::lock_guard<std::mutex> lock(mutex);
      auto it = std::lower_bound(epochs.begin(), epochs.end(), ptr);
      if (it == epochs.end() || *it != ptr) return;
      epochs.erase(it);
    }
  };
  Registry registry{};

  // Global Epoch
  uint64_t epoch = 1;
  // Local Epoch
  thread_local uint64_t thread_epoch;

  // automatic register and unregister of epochs
  struct EpochRAII {
    uint64_t& ref;
    EpochRAII(uint64_t& ref) : ref(ref) {
      registry.register_ptr(&ref);
    }
    ~EpochRAII() {
      registry.unregister_ptr(&ref);
    };
  };
  thread_local EpochRAII thread_epoch_raii(thread_epoch);

  // synchronize local epoch to global one
  void update_epoch() {
    uint64_t e = __atomic_load_n(&epoch, __ATOMIC_ACQUIRE);
    __atomic_store_n(&thread_epoch, e, __ATOMIC_RELEASE);
  }

  __attribute__((noinline,cold))
  static void init() {
    (void) thread_epoch_raii;
    update_epoch();
  }

  // read lock
  void read_lock() {
    if (__builtin_expect(thread_epoch == 0, 0)) {
      init();
    }
  }

  // read unlock
  void read_unlock() {
    update_epoch();
  }

  // wait for all threads to finish their current critical section
  void synchronize() {
    uint64_t e = __atomic_add_fetch(&epoch, 1, __ATOMIC_ACQ_REL);
    struct pair {
      uint64_t* ptr;
      uint64_t t;
    };
    std::vector<pair> epochs, intersection;
    epochs.reserve(registry.epochs.size());

    // find which thread is late
    { std::lock_guard<std::mutex> lock(registry.mutex);
      for (uint64_t* ptr : registry.epochs) {
        uint64_t t = __atomic_load_n(ptr, __ATOMIC_ACQUIRE); // relaxed?
        if (t < e) {
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

      // remove threads that have finished their critical section
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
            uint64_t t = __atomic_load_n(it1->ptr, __ATOMIC_ACQUIRE);
            if (t < e) {
              intersection.push_back(*it1);
            }
            ++it0;
            ++it1;
          }
        }
      }
      std::swap(epochs, intersection);
    }

    // memory fence
    std::atomic_thread_fence(std::memory_order_acquire);
  }

  // retrieve all tasks and wait for all threads
  void synchronize_all() {
    // extract the whole list of tasks (lock-free)
    Node* tasks = registry.tasks.exchange(nullptr, std::memory_order_relaxed);

    // no pending tasks, no need to wait
    if (!tasks) return;

    // wait for all threads
    synchronize();

    // process all the tasks and free their nodes
    do {
      Node* task = tasks;
      tasks = tasks->next;
      task->callback();
      free(task);
    } while (tasks);
  }

  // queue a new task to be processed once all threads have finished their current critical sections
  // (async)
  void queue(std::function<void()> f) {
    Node* tail = registry.tasks.load(std::memory_order_relaxed);
    Node* head = new Node{tail, std::move(f)};
    // push front the new task (lock-free)
    while (!registry.tasks.compare_exchange_weak(tail, head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      head->next = tail;
      _mm_pause();
    }

    // if there was no task in list, we wake the rcu thread
    if (tail == nullptr) {
      futex((int*)(&registry.tasks), FUTEX_WAKE, 1, 0, 0, 0);
    }
  }
}








//// Test

int main() {
  // RCU thread: when there is some tasks to complete, wait for readers to finish their critical sections
  std::atomic<bool> stop{false};
  std::thread RCU([&stop]() {
    do {
      // wait for a task
      futex((int*)(&dsurcu::registry.tasks), FUTEX_WAIT, 0, 0, 0, 0);

      // keep going as long as there are tasks
      while (dsurcu::registry.tasks.load(std::memory_order_relaxed)) {
        dsurcu::synchronize_all();
      }

    // stop is required
    } while (!stop.load());
  });


  // thread-safe random number generator
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<> distrib(0.0, 1.);
  std::mutex mutex;
  auto rand = [&]() {
    std::lock_guard<std::mutex> lock(mutex);
    return distrib(gen);
  };

  // spawn some worker to demonstrate RCU
  int n = 10;
  std::vector<std::thread> workers;
  workers.reserve(n);
  for (int i = 0; i < n; ++i) {
    workers.emplace_back([&rand](int i, int n) {
      printf("thread #%d start\n", i);
      std::this_thread::sleep_for(std::chrono::duration<double>(2*rand()));
      dsurcu::read_lock();
      printf("thread #%d lock\n", i);
      std::this_thread::sleep_for(std::chrono::duration<double>(rand()));
      printf("thread #%d unlock\n", i);
      dsurcu::read_unlock();
      std::this_thread::sleep_for(std::chrono::duration<double>(0.5*rand()));
      printf("thread #%d task queue\n", i);
      dsurcu::queue([=]() {
        printf("task from thread #%d\n", i);
      });
      std::this_thread::sleep_for(std::chrono::duration<double>(5*rand()));
      printf("thread #%d stop\n", i);
    }, i, n);
  }

  // wait for all workers to finish
  for (auto& t : workers) {
    t.join();
  }

  // stop the RCU thread
  stop.store(true);
  futex((int*)(&dsurcu::registry.tasks), FUTEX_WAKE, 1, 0, 0, 0);
  RCU.join();
}
