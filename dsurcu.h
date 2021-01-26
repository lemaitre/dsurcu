#pragma once

#include <memory>
#include <functional>
#include <atomic>

namespace dsurcu {
  // Node for task queueing
  struct Node {
    Node* next = nullptr;
    std::function<void()> callback{};
  };

  template <class T>
  struct Delay {
    alignas(64) char dummy0[64];
    alignas(alignof(T)) char mem[sizeof(T)];
    alignas(64) char dummy1[64];

    T      &  get()      &  noexcept { return reinterpret_cast<T      & >(mem); }
    T      && get()      && noexcept { return reinterpret_cast<T      &&>(mem); }
    T const&  get() const&  noexcept { return reinterpret_cast<T const& >(mem); }
    T const&& get() const&& noexcept { return reinterpret_cast<T const&&>(mem); }
    operator T      &  ()      &  noexcept { return reinterpret_cast<T      & >(mem); }
    operator T      && ()      && noexcept { return reinterpret_cast<T      &&>(mem); }
    operator T const&  () const&  noexcept { return reinterpret_cast<T const& >(mem); }
    operator T const&& () const&& noexcept { return reinterpret_cast<T const&&>(mem); }

    constexpr Delay() noexcept = default;

    template <class... Args>
    void construct(Args&&... args) noexcept(noexcept(T(static_cast<Args&&>(args)...))) {
      new (mem) T(static_cast<Args&&>(args)...);
    }

    void destruct() noexcept(noexcept(get().~T())) {
      get().~T();
    }
  };

  struct Noop {
    static void init() noexcept {}
    static void thread_init() noexcept {}
    static void thread_fini() noexcept {}
    static void fini() noexcept {}

    static void read_lock() noexcept {
      std::atomic_thread_fence(std::memory_order_acquire);
    }
    static void read_unlock() noexcept {
      std::atomic_thread_fence(std::memory_order_release);
    }
    static void quiescent() noexcept {}

    template <class F> // F shall be convertible to std::function<void()>
    static void queue(F const&) noexcept {}
    static void synchronize() noexcept {}
    static void process_queue() noexcept {}
    static void process_queue_wait() noexcept;
    static void process_queue_busy() noexcept {}
    static void wake() noexcept;
  };

  template <class RCU>
  struct Recursive : public RCU {
    inline static thread_local int depth;

    static void thread_init() noexcept(noexcept(RCU::thread_init())) {
      // force symbol emission
      asm(""::"m"(read_lock), "m"(read_unlock), "m"(quiescent));
      depth = 0;
      RCU::thread_init();
    }

    static void read_lock() noexcept(noexcept(RCU::read_lock())) {
      if (depth == 0) {
        RCU::read_lock();
      }
      depth += 1;
    }
    static void read_unlock() noexcept(noexcept(RCU::read_unlock())) {
      depth -= 1;
      if (depth == 0) {
        RCU::read_unlock();
      }
    }
    static void quiescent() noexcept(noexcept(RCU::quiescent())) {
      if (depth == 0) {
        RCU::quiescent();
      }
    }
  };

  template <>
  struct Recursive<Noop> : public Noop {
  };

  struct LocalEpoch {
    inline static thread_local Delay<std::atomic<uint64_t>> local_epoch;

    struct Registry;
    static Delay<Registry> registry;

    static void init();
    static void thread_init();
    static void thread_fini();
    static void fini();

    static void read_lock() noexcept {
      std::atomic<uint64_t>& epoch = local_epoch;
      epoch.fetch_add(1, std::memory_order_acquire);

      //uint64_t e = epoch.load(std::memory_order_acquire);
      //e += 1;
      //epoch.store(e, std::memory_order_relaxed);
      //// Wrong: resource read can be reordered before the write of epoch
    }
    static void read_unlock() noexcept {
      std::atomic<uint64_t>& epoch = local_epoch;
      // There is no need here for an atomic RMW
      uint64_t e = epoch.load(std::memory_order_relaxed);
      e += 1;
      epoch.store(e, std::memory_order_release);
    }
    static void quiescent() noexcept {}

    static void queue(std::function<void()>);
    static void synchronize();
    static void process_queue();
    static void process_queue_wait();
    static void process_queue_busy();
    static void wake();
  };

  struct LocalEpochWeak {
    inline static thread_local Delay<std::atomic<uint64_t>> local_epoch;

    struct Registry;
    static Delay<Registry> registry;

    static void init();
    static void thread_init();
    static void thread_fini();
    static void fini();

    static void read_lock() noexcept {
      std::atomic<uint64_t>& epoch = local_epoch;
      uint64_t e = epoch.load(std::memory_order_acquire);
      e += 1;
      epoch.store(e, std::memory_order_relaxed);
    }
    static void read_unlock() noexcept {
      std::atomic<uint64_t>& epoch = local_epoch;
      uint64_t e = epoch.load(std::memory_order_relaxed);
      e += 1;
      epoch.store(e, std::memory_order_release);
    }
    static void quiescent() noexcept {}

    static void queue(std::function<void()>);
    static void synchronize();
    static void process_queue();
    static void process_queue_wait();
    static void process_queue_busy();
    static void wake();
  };

  struct GlobalEpoch {
    inline static Delay<std::atomic<uint64_t>> global_epoch;
    inline static thread_local Delay<std::atomic<uint64_t>> local_epoch;

    struct Registry;
    static Delay<Registry> registry;

    static void init();
    static void thread_init();
    static void thread_fini();
    static void fini();

    static void read_lock() noexcept {
      quiescent();
    }
    static void read_unlock() noexcept {
      quiescent();
    }
    static void quiescent() noexcept {
      std::atomic<uint64_t>& global = global_epoch;
      std::atomic<uint64_t>& local = local_epoch;
      uint64_t e = global.load(std::memory_order_acquire);
      local.store(e, std::memory_order_release);
    }

    static void queue(std::function<void()>);
    static void synchronize();
    static void process_queue();
    static void process_queue_wait();
    static void process_queue_busy();
    static void wake();
  };
}
