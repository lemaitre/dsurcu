#pragma once

#include <memory>
#include <functional>
#include <atomic>

namespace dsurcu {
  // Node for task queueing
  struct Node {
    void* allocd;
    void (*reclaim)(Node*);
    int n;
    Node* next;

    static Node* get(void* p) noexcept {
      return (Node*)((char*)p - sizeof(Node));
    }
    void* data() noexcept {
      return (char*)this + sizeof(Node);
    }
    template <class T>
    T* data() noexcept {
      return (T*)(data());
    }
    static void del(Node* node) {
      delete[]((char*)node->allocd);
    }
    template <class T>
    static void del(Node* node) {
      T* p = node->data<T>();
      for (int i = 0; i < node->n; ++i) {
        p[i].~T();
      }
      del(node);
    }
  };

  inline void* rcu_allocate(size_t size, size_t align) {
    align &= -align;
    if (align < alignof(Node)) align = alignof(Node);
    char* allocd = new char[size + sizeof(Node) + align];
    char* p = (char*)(((uintptr_t) allocd + sizeof(Node) + align-1) & -align);
    Node* node = Node::get(p);
    node->allocd = allocd;
    node->n = size;
    node->next = nullptr;
    node->reclaim = Node::del;
    return p;
  };

  inline void rcu_deallocate(void* p) {
    Node* node = Node::get(p);
    node->reclaim(node);
  }

  template <class T>
  T* rcu_allocate(size_t n = 1, size_t align = alignof(T)) {
    void* p = rcu_allocate(n * sizeof(T), align);
    Node* node = Node::get(p);
    node->reclaim = Node::del<T>;
    node->n = n;
    for (int i = 0; i < n; ++i) {
      new ((char*)p + i * sizeof(T)) T;
    }
    return (T*)p;
  }

  template <class T>
  struct Delay {
    alignas(alignof(T)) char mem[sizeof(T)];

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

    static void queue(void*) noexcept {}
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

    static void queue(void*);
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

    static void queue(void*);
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

    static void queue(void*);
    static void synchronize();
    static void process_queue();
    static void process_queue_wait();
    static void process_queue_busy();
    static void wake();
  };
}
