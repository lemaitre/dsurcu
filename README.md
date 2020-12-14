# What is DSURCU?

DSURCU is a proof of concept for a Dead-Simple Userspace Read-Copy-Update.
The main goal is to play with the concept and to show it is possible to implement RCU in userspace in a really simple way.

If you don't know what is RCU, you should probably start here: [Wikipedia](https://en.wikipedia.org/wiki/Read-copy-update) or [What is RCU, Really?](https://lwn.net/Articles/262464/)

# What is **not** DSURCU?

DSURCU is not a production ready library.
It might become that at some point, but we are very far away for now.

It will not be a replacement for the [urcu](https://liburcu.org/) library.

# Goals

In the spirit of the kernel version of RCU, DSURCU is designed to be as efficient as it could be for readers.
This means no RMW atomic operations nor contention for reader path.
For now, the reader path (both lock and unlock) consists of 2 loads, 2 writes, 2 add, and a (almost) never taken branch.

Another goal of DSURCU is to provide writers a way to defer an operation (typically freeing the memory) untill all the readers have finished.
Such writers would push a callback into a queue that is processed by another thread.
Alternatively, writers can also wait for all readers to leave their critical sections before continuing.

# Non-goals

DSURCU is designed only for linux-x86_64.

DSURCU is not safe: it does not check its integrity.
It is user responsability to properly use DSURCU (no checks are performed).

DSURCU is not for latency critical operations as reader critical sections are not detected right away.


# How does it work?

The principle is the following: all threads have an epoch counter.
This epoch counter is incremented each time the thread enters or leaves the read critical section.
When a writer needs to wait for all readers, it will read epoch counters of all threads.
For all odd epochs, the writer must wait for those epochs to change.
This means that readers have finished their critical sections.
Epochs might still be odd when checked by the writer, but as soon as it is different from the start, the reader has left its section, and just might have started another one, with more up-to-date data.

Usually, writers don't need to actually wait for readers, and just need to defer a single operation (typically, freeing memory) after the last reader has finished.
This is accomplished with a task queue that is processed by an independant thread.
As soon as a task is queued, this thread extracts all tasks and waits for all readers to finish their current critical section (if any).
Once readers have finished their current sections (their epochs have changed) or were already outside any critical section (epoch is even), the thread executes all tasks it has extracted before waiting.
If any other tasks have been queued in the meantime, the thread starts again to wait for redears.
The task queue is lock free.

In order to wait on all threads, a global list of all threads is needed.
This list is actually a list of epoch pointers, and is changed when a thread calls `dsurcu::read_lock()` for the first time, and when a thread terminates.
This list is protected by a mutex and is read anytime a waiter checks epochs.
This mutex is crucial when waiting as a reader thread might terminate before the waiting thread had a chance to see the end of the reader critical section.
Without it, the waiter could segfault when reading epochs.
The mutex is released when the waiter sleeps, letting readers to terminates if needed.

DSURCU uses C++ `thread_local`s in order to gracefully handle thread termination without polluting user code with an explicit call to a finalize/unregister function.
This _cannot_ be done in plain C as there is no way to register a function to be called when a thread exits.

# Disclaimer

DSURCU is not ready for any production code and is very likely to contain bugs. DO NOT USE.
