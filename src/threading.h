#ifndef _THREADING_WORKER_THREAD_H_
#define _THREADING_WORKER_THREAD_H_

#include <stdint.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <queue>

/*******************************************************************************************************************
    BRIEF DESCRIPTION OF THE CONTENTS OF THIS HEADER

    classes:
        Sync:
            description:
                Synchronization class that can wait on a thread and can be waken up from another
                Similar to a binary semaphore
        Worker:
            description:
                Can start a constantly running thread waiting for jobs to execute
            functions:
                void push(const std::function<void()>& job)
                bool start(bool wait_to_start = false)
                void stop()
    functions:
        wait_for_thread_to_start:
            description:
                Starts an std::thread and waits for it to start
            footprint:
                std::shared_ptr<std::thread> wait_for_thread_to_start(const std::function<void()>& thread_function)

********************************************************************************************************************/

namespace threading 
{
    class Sync 
    {
    public:
        Sync() : mutex(), cond_var(), cond(false) {}

        void wait()
        {
            std::unique_lock<std::mutex> lock(mutex);
            cond_var.wait(lock, [this]()->bool { return cond.load();  });
            cond.store(false);
        }

        void wake() 
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                cond.store(true);
            }
            cond_var.notify_one();
        }
    private:
        std::mutex                          mutex;
        std::condition_variable             cond_var;
        std::atomic_bool                    cond;
    };

    class SyncPair 
    {
    public:
        SyncPair() : first(), second() {}

        void waitForFirst() 
        {
            second.wake();
            first.wait();
        }

        void waitForSecond() 
        {
            first.wake();
            second.wait();
        }
    private:
        Sync first;
        Sync second;
    };

    class Worker
    {
    public:
        /**
         * Pushes the given job to the end of the queue
         * @param job Function to call on the working thread
         */
        void push(const std::function<void()>& job)
        {
            {
                std::lock_guard<std::mutex> guard(jobs_mutex);
                jobs.emplace(job);
                ++counter;
            }
            cond_var.notify_one();
        }
        /**
         * Starts the worker thread if it is not started yet
         * @param wait_to_start If true is given then the function will wait for the working thread to start
         * @return True is returned on success, othewise false if it is already started
         */
        bool start(bool wait_to_start = false) 
        {
            std::unique_lock<std::mutex> lock(thread_mutex, std::try_to_lock);
            if (lock) 
            {
                if (thread == nullptr) 
                {
                    thread.reset(new std::thread([this, wait_to_start]()
                    {
                        if (wait_to_start) 
                        {
                            start_sync.wake();
                        }
                        std::queue<std::function<void()>> jobs_cpy;
                        while (!stop_request.load())
                        {
                            std::unique_lock<std::mutex> lock(jobs_mutex);
                            cond_var.wait(lock, [this]() -> bool { return ((counter.load() > 0) || stop_request.load()); });
                            jobs_cpy.swap(jobs);
                            counter.store(0);
                            lock.unlock();
                            while (!jobs_cpy.empty()) 
                            {
                                auto& job = jobs_cpy.front();
                                if (job) { job(); }
                                jobs_cpy.pop();
                            }
                        }

                    }));

                    if (wait_to_start)
                    {
                        start_sync.wait();
                    }
                }
            }
            return false;
        }
        /**
         * Stops the worker thread if it is running
         * Also waits while the thread is finishing it's execution
         */
        void stop() 
        {
            std::unique_lock<std::mutex> guard(thread_mutex);
            if (thread) 
            {
                stop_request.store(true);
                cond_var.notify_one();
                if (thread->joinable()) { thread->join(); }
                thread.reset();
                counter.store(0);
                while (!jobs.empty()) { jobs.pop(); }
                stop_request.store(false);
            }
        }
        /**
         * Returns the inside shared std::thread object as a weak pointer for explicit use
         */
        std::weak_ptr<std::thread> native() const 
        {
            std::lock_guard<std::mutex> guard(thread_mutex);
            return thread;
        }

        Worker() : thread(nullptr), thread_mutex(), cond_var(), counter(0), stop_request(false), jobs(), jobs_mutex() {}
        ~Worker() { stop(); }
    private:
        std::shared_ptr<std::thread>        thread;
        mutable std::mutex                  thread_mutex;
        std::condition_variable             cond_var;
        std::atomic_uint32_t                counter;
        std::atomic_bool                    stop_request;
        std::queue<std::function<void()>>   jobs;
        std::mutex                          jobs_mutex;
        Sync                                start_sync;
    };

    /**
     * Starts an std::thread and waits for it to start
     * @return A shared_ptr is returned for the started std::thread
     */
    inline std::shared_ptr<std::thread> wait_for_thread_to_start(const std::function<void()>& thread_function)
    {
        std::mutex                          mutex;
        std::condition_variable             cond_var;
        std::atomic_bool                    cond(false);
        auto th = std::make_shared<std::thread>([thread_function, &mutex, &cond_var, &cond]()
        {
            {
                std::unique_lock<std::mutex> lock(mutex);
                cond.store(true);
            }
            cond_var.notify_one();
            if (thread_function) { thread_function(); }
        });
        std::unique_lock<std::mutex> lock(mutex);
        cond_var.wait(lock, [&cond]() -> bool { return cond.load(); });
        return th;
    }
}

#endif