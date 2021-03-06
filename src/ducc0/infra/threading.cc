/*
 *  This file is part of the MR utility library.
 *
 *  This code is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This code is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this code; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Copyright (C) 2019-2020 Peter Bell, Max-Planck-Society
   Authors: Peter Bell, Martin Reinecke */

#include "ducc0/infra/threading.h"

#ifndef DUCC0_NO_THREADING
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <atomic>
#include <vector>
#include <exception>
#if __has_include(<pthread.h>)
#include <pthread.h>
#endif
#endif

namespace ducc0 {

namespace detail_threading {

#ifndef DUCC0_NO_THREADING

static const size_t max_threads_ = std::max<size_t>(1, std::thread::hardware_concurrency());

std::atomic<size_t> default_nthreads_(max_threads_);

size_t get_default_nthreads()
  { return default_nthreads_; }

void set_default_nthreads(size_t new_default_nthreads)
  { default_nthreads_ = std::max<size_t>(1, new_default_nthreads); }

size_t max_threads() { return max_threads_; }

class latch
  {
    std::atomic<size_t> num_left_;
    std::mutex mut_;
    std::condition_variable completed_;
    using lock_t = std::unique_lock<std::mutex>;

  public:
    latch(size_t n): num_left_(n) {}

    void count_down()
      {
      lock_t lock(mut_);
      if (--num_left_)
        return;
      completed_.notify_all();
      }

    void wait()
      {
      lock_t lock(mut_);
      completed_.wait(lock, [this]{ return is_ready(); });
      }
    bool is_ready() { return num_left_ == 0; }
  };

template <typename T> class concurrent_queue
  {
    std::queue<T> q_;
    std::mutex mut_;
    std::atomic<size_t> size_;
    using lock_t = std::lock_guard<std::mutex>;

  public:
    void push(T val)
      {
      ++size_;
      lock_t lock(mut_);
      q_.push(std::move(val));
      }

    bool try_pop(T &val)
      {
      if (size_==0) return false;
      lock_t lock(mut_);
      // Queue might have been emptied while we acquired the lock
      if (q_.empty()) return false;

      --size_;
      val = std::move(q_.front());
      q_.pop();
      return true;
      }


    // bool try_pop(T &val)
    //   {
    //   while (size_ > 0)
    //     {
    //     lock_t lock(mut_);
    //     // Item might not have been added yet
    //     if (q_.empty()) continue;

    //     --size_;
    //     val = std::move(q_.front());
    //     q_.pop();
    //     return true;
    //     }
    //   return false;
    //   }
  };

class thread_pool
  {
  private:
    // A reasonable guess, probably close enough for most hardware
    static constexpr size_t cache_line_size = 64;
    struct alignas(cache_line_size) worker
      {
      std::thread thread;
      std::condition_variable work_ready;
      std::mutex mut;
      std::atomic_flag busy_flag = ATOMIC_FLAG_INIT;
      std::function<void()> work;

      void worker_main(std::atomic<bool> & shutdown_flag,
        concurrent_queue<std::function<void()>> &overflow_work)
        {
        using lock_t = std::unique_lock<std::mutex>;
        while (!shutdown_flag)
          {
          std::function<void()> local_work;
          {
          lock_t lock(mut);
          // Wait to be woken by the thread pool with a piece of work
          work_ready.wait(lock, [&]{ return (work || shutdown_flag); });
          local_work.swap(work);
          }

          if (local_work) local_work();

          // Execute any work which queued up while we were busy
          while (overflow_work.try_pop(local_work)) local_work();

          // Mark ourself as available before going back to sleep
          busy_flag.clear();
          }
        }
      };

    concurrent_queue<std::function<void()>> overflow_work_;
    std::mutex mut_;
    std::vector<worker> workers_;
    std::atomic<bool> shutdown_;
    using lock_t = std::lock_guard<std::mutex>;

    void create_threads()
      {
      lock_t lock(mut_);
      size_t nthreads=workers_.size();
      for (size_t i=0; i<nthreads; ++i)
        {
        try
          {
          auto *worker = &workers_[i];
          worker->busy_flag.clear();
          worker->work = nullptr;
          worker->thread = std::thread(
            [worker, this]{ worker->worker_main(shutdown_, overflow_work_); });
          }
        catch (...)
          {
          shutdown_locked();
          throw;
          }
        }
      }

    void shutdown_locked()
      {
      shutdown_ = true;
      for (auto &worker : workers_)
        worker.work_ready.notify_all();

      for (auto &worker : workers_)
        if (worker.thread.joinable())
          worker.thread.join();
      }

  public:
    explicit thread_pool(size_t nthreads):
      workers_(nthreads)
      { create_threads(); }

    thread_pool(): thread_pool(max_threads_) {}

    ~thread_pool() { shutdown(); }

    void submit(std::function<void()> work)
      {
      lock_t lock(mut_);
      if (shutdown_)
        throw std::runtime_error("Work item submitted after shutdown");

      auto submit_to_idle = [&](std::function<void()> &work) -> bool
        {
        for (auto &worker : workers_)
          if (!worker.busy_flag.test_and_set())
            {
            {
            lock_t lock(worker.mut);
            worker.work = std::move(work);
            }
            worker.work_ready.notify_one();
            return true;
            }
        return false;
        };

      // First check for any idle workers and wake those
      if (submit_to_idle(work)) return;
      // If no workers were idle, push onto the overflow queue for later
      overflow_work_.push(std::move(work));

      // Possible race: All workers might have gone idle between the first
      // submit attempt and pushing the work item into the queue. So, there
      // could be no active workers to check the queue.
      // Resolve with another check for idle workers.
      std::function<void()> dummy_work = []{};
      submit_to_idle(dummy_work);
      }

    void shutdown()
      {
      lock_t lock(mut_);
      shutdown_locked();
      }

    void restart()
      {
      shutdown_ = false;
      create_threads();
      }
  };

inline thread_pool &get_pool()
  {
  static thread_pool pool;
#if __has_include(<pthread.h>)
  static std::once_flag f;
  call_once(f,
    []{
    pthread_atfork(
      +[]{ get_pool().shutdown(); },  // prepare
      +[]{ get_pool().restart(); },   // parent
      +[]{ get_pool().restart(); }    // child
      );
    });
#endif

  return pool;
  }

class Distribution
  {
  private:
    size_t nthreads_;
    std::mutex mut_;
    size_t nwork_;
    size_t cur_;
    size_t chunksize_;
    double fact_max_;
    std::vector<size_t> nextstart;
    enum SchedMode { SINGLE, STATIC, DYNAMIC };
    SchedMode mode;
    bool single_done;

    void thread_map(std::function<void(Scheduler &)> f);

  public:
    size_t nthreads() const { return nthreads_; }

    void execSingle(size_t nwork, std::function<void(Scheduler &)> f)
      {
      mode = SINGLE;
      single_done = false;
      nwork_ = nwork;
      nthreads_ = 1;
      thread_map(move(f));
      }
    void execStatic(size_t nwork, size_t nthreads, size_t chunksize,
      std::function<void(Scheduler &)> f)
      {
      mode = STATIC;
      nthreads_ = (nthreads==0) ? get_default_nthreads() : nthreads;
      nwork_ = nwork;
      chunksize_ = (chunksize<1) ? (nwork_+nthreads_-1)/nthreads_
                                 : chunksize;
      if (chunksize_>=nwork_) return execSingle(nwork_, move(f));
      nextstart.resize(nthreads_);
      for (size_t i=0; i<nextstart.size(); ++i)
        nextstart[i] = i*chunksize_;
      thread_map(move(f));
      }
    void execDynamic(size_t nwork, size_t nthreads, size_t chunksize_min,
      double fact_max, std::function<void(Scheduler &)> f)
      {
      mode = DYNAMIC;
      nthreads_ = (nthreads==0) ? get_default_nthreads() : nthreads;
      nwork_ = nwork;
      chunksize_ = (chunksize_min<1) ? 1 : chunksize_min;
      if (chunksize_*nthreads_>=nwork_)
        return execStatic(nwork, nthreads, 0, move(f));
      fact_max_ = fact_max;
      cur_ = 0;
      thread_map(move(f));
      }
    void execParallel(size_t nthreads, std::function<void(Scheduler &)> f)
      {
      mode = STATIC;
      nthreads_ = (nthreads==0) ? get_default_nthreads() : nthreads;
      nwork_ = nthreads_;
      chunksize_ = 1;
      thread_map(move(f));
      }
    Range getNext(size_t thread_id)
      {
      switch (mode)
        {
        case SINGLE:
          {
          if (single_done) return Range();
          single_done=true;
          return Range(0, nwork_);
          }
        case STATIC:
          {
          if (nextstart[thread_id]>=nwork_) return Range();
          size_t lo=nextstart[thread_id];
          size_t hi=std::min(lo+chunksize_,nwork_);
          nextstart[thread_id] += nthreads_*chunksize_;
          return Range(lo, hi);
          }
        case DYNAMIC:
          {
          std::unique_lock<std::mutex> lck(mut_);
          if (cur_>=nwork_) return Range();
          auto rem = nwork_-cur_;
          size_t tmp = size_t((fact_max_*double(rem))/double(nthreads_));
          auto sz = std::min(rem, std::max(chunksize_, tmp));
          size_t lo=cur_;
          cur_+=sz;
          size_t hi=cur_;
          return Range(lo, hi);
          }
        }
      return Range();
      }
  };

class MyScheduler: public Scheduler
  {
  private:
    Distribution &dist_;
    size_t ithread_;

  public:
    MyScheduler(Distribution &dist, size_t ithread)
      : dist_(dist), ithread_(ithread) {}
    virtual size_t num_threads() const { return dist_.nthreads(); }
    virtual size_t thread_num() const { return ithread_; }
    virtual Range getNext() { return dist_.getNext(ithread_); }
  };

void Distribution::thread_map(std::function<void(Scheduler &)> f)
  {
  if (nthreads_ == 1)
    {
    MyScheduler sched(*this, 0);
    f(sched);
    return;
    }

  auto & pool = get_pool();
  latch counter(nthreads_);
  std::exception_ptr ex;
  std::mutex ex_mut;
  for (size_t i=0; i<nthreads_; ++i)
    {
    pool.submit(
      [this, &f, i, &counter, &ex, &ex_mut] {
      try
        {
        MyScheduler sched(*this, i);
        f(sched);
        }
      catch (...)
        {
        std::lock_guard<std::mutex> lock(ex_mut);
        ex = std::current_exception();
        }
      counter.count_down();
      });
    }
  counter.wait();
  if (ex)
    std::rethrow_exception(ex);
  }

void execSingle(size_t nwork, std::function<void(Scheduler &)> func)
  {
  Distribution dist;
  dist.execSingle(nwork, move(func));
  }
void execStatic(size_t nwork, size_t nthreads, size_t chunksize,
  std::function<void(Scheduler &)> func)
  {
  Distribution dist;
  dist.execStatic(nwork, nthreads, chunksize, move(func));
  }
void execDynamic(size_t nwork, size_t nthreads, size_t chunksize_min,
  std::function<void(Scheduler &)> func)
  {
  Distribution dist;
  dist.execDynamic(nwork, nthreads, chunksize_min, 0., move(func));
  }
void execGuided(size_t nwork, size_t nthreads, size_t chunksize_min,
  double fact_max, std::function<void(Scheduler &)> func)
  {
  Distribution dist;
  dist.execDynamic(nwork, nthreads, chunksize_min, fact_max, move(func));
  }
void execParallel(size_t nthreads, std::function<void(Scheduler &)> func)
  {
  Distribution dist;
  dist.execParallel(nthreads, move(func));
  }

#else

size_t get_default_nthreads() { return 1; }
void set_default_nthreads(size_t /* new_default_nthreads */) {}
size_t max_threads() { return 1; }

class MyScheduler: public Scheduler
  {
  private:
    size_t nwork_;

  public:
    MyScheduler(size_t nwork) : nwork_(nwork) {}
    virtual size_t num_threads() const { return 1; }
    virtual size_t thread_num() const { return 0; }
    virtual Range getNext()
      {
      Range res(0, nwork_);
      nwork_=0;
      return res;
      }
  };

void execSingle(size_t nwork, std::function<void(Scheduler &)> func)
  {
  MyScheduler sched(nwork);
  func(sched);
  }
void execStatic(size_t nwork, size_t, size_t,
  std::function<void(Scheduler &)> func)
  {
  MyScheduler sched(nwork);
  func(sched);
  }
void execDynamic(size_t nwork, size_t, size_t,
  std::function<void(Scheduler &)> func)
  {
  MyScheduler sched(nwork);
  func(sched);
  }
void execGuided(size_t nwork, size_t, size_t, double,
  std::function<void(Scheduler &)> func)
  {
  MyScheduler sched(nwork);
  func(sched);
  }
void execParallel(size_t, std::function<void(Scheduler &)> func)
  {
  MyScheduler sched(1);
  func(sched);
  }

#endif

}}
