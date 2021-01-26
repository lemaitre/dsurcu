/*
Copyright 2020 Florian Lemaitre

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <mutex>
#include <thread>
#include <random>
#include <cstdint>
#include <cstdio>
#include <x86intrin.h>

#include "dsurcu.h"

template <class rcu, class Derived>
struct Worker {
  using RCU = rcu;
  std::atomic<std::atomic<uint64_t>*>* time;
  std::vector<uint64_t>* writer_data;
  long niter;
  double read_ratio;

  Worker(std::atomic<std::atomic<uint64_t>*>& time, std::vector<uint64_t>& writer_data, long niter, double read_ratio) : time(&time), writer_data(&writer_data), niter(niter), read_ratio(read_ratio) {}
  void consume(std::atomic<uint64_t>*) const = delete;

  static void reclaim(std::atomic<uint64_t>*, std::vector<uint64_t>*) {}

  __attribute__((noinline))
  void operator()() const {
    double reads = 0., accesses = 1.;

    for (long i = 0; i < niter; i += 1) {
      std::atomic<uint64_t>* p;
      if (reads <= accesses * read_ratio) {
        RCU::read_lock();
        p = time->load(std::memory_order_relaxed);
        static_cast<Derived const*>(this)->consume(p);
        RCU::read_unlock();
        reads += 1.;
      } else {
        std::atomic<uint64_t> *p;
        p = new std::atomic<uint64_t>(_rdtsc());
        p = time->exchange(p, std::memory_order_acq_rel);
        std::vector<uint64_t>* writer_data = this->writer_data;
        RCU::queue([p,writer_data]() {
          Derived::reclaim(p, writer_data);
          delete p;
        });
      }
      //RCU::quiescent();
      accesses += 1.;
    }
  }
};

template <class RCU>
struct WorkerRead : public Worker<RCU, WorkerRead<RCU>> {
  using Worker<RCU, WorkerRead<RCU>>::Worker;
  using Worker<RCU, WorkerRead<RCU>>::operator=;

  void consume(std::atomic<uint64_t>* p) const {
    uint64_t dummy = p->load(std::memory_order_relaxed);
    asm (""::"r"(dummy));
  }
};

template <class RCU>
struct WorkerWrite : public Worker<RCU, WorkerWrite<RCU>> {
  using Worker<RCU, WorkerWrite<RCU>>::Worker;
  using Worker<RCU, WorkerWrite<RCU>>::operator=;

  void consume(std::atomic<uint64_t>* p) const {
    uint64_t zero = 0;
    asm("":"+rm"(zero));
    p->store(zero, std::memory_order_relaxed);
  }
};

template <class RCU>
struct WorkerRDTSC : public Worker<RCU, WorkerRDTSC<RCU>> {
  using Worker<RCU, WorkerRDTSC<RCU>>::Worker;
  using Worker<RCU, WorkerRDTSC<RCU>>::operator=;

  void consume(std::atomic<uint64_t>* p) const {
    p->store(_rdtsc(), std::memory_order_relaxed);
  }

  static void reclaim(std::atomic<uint64_t>* p, std::vector<uint64_t>* writer_data) {
    uint64_t t1 = _rdtsc();
    uint64_t t0 = p->load(std::memory_order_acquire);
    writer_data->emplace_back(t1 - t0);
  }
};

template <class Worker>
void bench(int nthreads, long niter, double read_ratio) {
  using RCU = typename Worker::RCU;
  RCU::init();

  uint64_t rcu_wait = 0, rcu_busy = 0, rcu_end = 0, rcu_iter = 0;

  // RCU thread: when there is some tasks to complete, wait for readers to finish their critical sections
  std::atomic<bool> stop{false};
  std::thread rcu([&]() {
    do {
      uint64_t t0 = _rdtsc();
      RCU::process_queue_wait();
      uint64_t t1 = _rdtsc();
      RCU::process_queue_busy();
      uint64_t t2 = _rdtsc();
      rcu_wait += t1 - t0;
      rcu_busy += t2 - t1;
      rcu_iter += 1;
    // stop is required
    } while (!stop.load());
    rcu_end = _rdtsc();
  });


  std::vector<uint64_t> writer_data;
  writer_data.reserve(niter * nthreads * std::max(0., 1. - read_ratio) + nthreads + 1);

  std::vector<std::thread> workers;
  std::vector<uint64_t> worker_data(nthreads);
  workers.reserve(nthreads);

  std::atomic<std::atomic<uint64_t>*> time;
  time.store(new std::atomic<uint64_t>(_rdtsc()));

  std::atomic<int> barrier{nthreads};

  for (int t = 0; t < nthreads; ++t) {
    workers.emplace_back([&, t]() {
      RCU::thread_init();

      Worker work(time, writer_data, niter, read_ratio);


      barrier.fetch_sub(1, std::memory_order_release);
      int b;
      do {
        _mm_pause();
        b = barrier.load(std::memory_order_relaxed);
      } while (b > 0);
      std::atomic_thread_fence(std::memory_order_acquire);

      uint64_t t0 = _rdtsc();

      work();

      uint64_t t1 = _rdtsc();

      RCU::thread_fini();

      worker_data[t] = t1 - t0;
    });
  }

  // wait for all workers to finish
  for (auto& w : workers) {
    w.join();
  }

  // stop the RCU thread
  stop.store(true);
  RCU::wake();
  rcu.join();

  RCU::fini();

  std::atomic<uint64_t> *p = time.load(std::memory_order_acquire);
  uint64_t t0 = p->load(std::memory_order_relaxed);
  uint64_t t1 = rcu_end;
  delete p;

  double lateness = t1 - t0;
  printf("    RCU: %lu times (wait: % 6lg\tbusy: % 6lg\t% 6lg%%)\n", rcu_iter, double(rcu_wait) / double(rcu_iter), double(rcu_busy) / double(rcu_iter), 100. * double(rcu_busy) / double(rcu_busy + rcu_wait));


  if (worker_data.size() > 0) {
    int n = worker_data.size();
    std::sort(worker_data.begin(), worker_data.end());
    double min = double(worker_data.front()) / double(niter);
    double max = double(worker_data.back()) / double(niter);
    double med = double(worker_data[0.5*n]) / double(niter);
    uint64_t s1 = 0;
    for (uint64_t delay : worker_data) {
      s1 += delay;
    }
    double avg = double(s1) / double(n * niter);

    printf("    processing time: % 6lg\t(min: % 6lg\tmed: % 6lg\tmax: % 6lg)\n", avg, min, med, max);
  }
  if (writer_data.size() > 2) {
    long n = writer_data.size();
    std::sort(writer_data.begin(), writer_data.end());
    double min = writer_data.front();
    double max = writer_data.back();
    double p50 = writer_data[0.50*n];
    double p90 = writer_data[0.90*n];
    double p99 = writer_data[0.99*n];
    uint64_t s1 = 0, s2 = 0;
    for (uint64_t delay : writer_data) {
      s1 += delay;
      s2 += delay*delay;
    }
    double avg = double(s1) / double(n);
    double var = (double(s2) - double(s1*s1) / double(n)) / double(n);
    double err = std::sqrt(var);

    printf("    Delay stat (%ld writes):\n", n);
    printf("    avg: % 6lg +- % 6lg\n", avg, err);
    printf("    min: % 6lg\n", min);
    printf("    50%%: % 6lg\n", p50);
    printf("    90%%: % 6lg\n", p90);
    printf("    99%%: % 6lg\n", p99);
    printf("    max: % 6lg\n", max);
    printf("    lateness: % 6lg\n", lateness);
  }
}

template <class RCU>
void bench_rcu(int nthreads, long niter, float ratio) {
  printf("  Read:\n");
  bench<WorkerRead<RCU>>(nthreads, niter, ratio);
  printf("  Write:\n");
  bench<WorkerWrite<RCU>>(nthreads, niter, ratio);
  printf("  RDTSC:\n");
  bench<WorkerRDTSC<RCU>>(nthreads, niter, ratio);
}


int main() {
  int nthreads = 10;
  long niter = 1e7;
  double ratio = 0.999;
  printf("Noop:\n");
  bench_rcu<dsurcu::Noop>(nthreads, niter, ratio);
  printf("\nLocalEpochWeak:\n");
  bench_rcu<dsurcu::LocalEpochWeak>(nthreads, niter, ratio);
  printf("\nGlobalEpoch:\n");
  bench_rcu<dsurcu::GlobalEpoch>(nthreads, niter, ratio);
  printf("\nLocalEpoch:\n");
  bench_rcu<dsurcu::LocalEpoch>(nthreads, niter, ratio);
}
