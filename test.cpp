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

#include "dsurcu.h"


int main() {
  using RCU = dsurcu::LocalEpochWeak;
  RCU::init();

  // RCU thread: when there is some tasks to complete, wait for readers to finish their critical sections
  std::atomic<bool> stop{false};
  std::thread rcu([&stop]() {
    do {
      RCU::process_queue_worker();
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
      RCU::thread_init();
      std::this_thread::sleep_for(std::chrono::duration<double>(2*rand()));
      RCU::read_lock();
      printf("thread #%d lock\n", i);
      std::this_thread::sleep_for(std::chrono::duration<double>(rand()));
      printf("thread #%d unlock\n", i);
      RCU::read_unlock();
      std::this_thread::sleep_for(std::chrono::duration<double>(0.5*rand()));
      printf("thread #%d task queue\n", i);
      RCU::queue([=]() {
        printf("task from thread #%d\n", i);
      });
      std::this_thread::sleep_for(std::chrono::duration<double>(rand()));
      RCU::quiescent();
      printf("thread #%d quiescent\n", i);
      std::this_thread::sleep_for(std::chrono::duration<double>(5*rand()));
      printf("thread #%d stop\n", i);
      RCU::thread_fini();
    }, i, n);
  }

  // wait for all workers to finish
  for (auto& t : workers) {
    t.join();
  }

  // stop the RCU thread
  stop.store(true);
  RCU::wake_worker();
  rcu.join();

  RCU::fini();
}
