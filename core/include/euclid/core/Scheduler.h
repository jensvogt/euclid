// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 17/08/2026.
//

#pragma once

// C++ standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

// Euclid includes
#include <euclid/core/CronExpression.h>

namespace Euclid::Core {

    using std::chrono::milliseconds;
    using std::chrono::system_clock;

    /**
     * @brief Background task scheduler.
     *
     * @par
     * Runs a single lightweight timer thread that tracks when the next task is due; each due
     * task is executed on its own detached thread so a slow task never delays other tasks.
     * Supports one-time tasks (run once, after a delay or at a fixed point in time), periodic
     * tasks (run every fixed interval) and cron tasks (run on a cron schedule, see CronExpression).
     *
     * @par Usage
     * @code
     * auto &scheduler = Scheduler::instance();
     * scheduler.Start();
     * scheduler.ScheduleOnce("cleanup", [] { CleanUp(); }, std::chrono::seconds(30));
     * scheduler.SchedulePeriodic("heartbeat", [] { SendHeartbeat(); }, std::chrono::seconds(10));
     * scheduler.ScheduleCron("nightly-backup", [] { RunBackup(); }, "0 2 * * *");
     * @endcode
     *
     * @author jensvogt47\@gmail.com
     */
    class Scheduler {

    public:

        /**
         * @brief Provides a singleton instance of the Scheduler class.
         *
         * @return Reference to the singleton Scheduler instance
         */
        static Scheduler &instance() {
            static Scheduler inst;
            return inst;
        }

        Scheduler(const Scheduler &) = delete;
        Scheduler &operator=(const Scheduler &) = delete;

        /**
         * @brief Starts the scheduler thread. No-op if already running.
         */
        void Start();

        /**
         * @brief Stops the scheduler thread, and waits for the tasks already running to finish.
         *
         * @par
         * A barrier, not just a flag. Task threads are detached, so nothing else in the process
         * knows they exist - and at shutdown that meant a task could still be running while the
         * singletons it reads were being destroyed underneath it. Seen in production as
         * "metrics-push-eap ... string too large": MonitoringCollector's map had already been
         * destroyed, the samples read back as garbage, and boost::json refused a string two
         * billion bytes long. That refusal was the lucky outcome; the same race is a segfault in a
         * process that was otherwise exiting cleanly, on a shutdown path nobody would think to
         * look at.
         *
         * @par
         * Bounded, because a wedged task must not hold the process open: the manager SIGKILLs an
         * instance that has not stopped in five seconds, and a shutdown that always ends in
         * SIGKILL is not a shutdown. If the grace period runs out the still-running tasks are
         * named in the log and the process carries on exiting, which is where it was before this
         * existed - but now it says so.
         *
         * @par
         * Already-running tasks are not interrupted; there is no portable way to do that to a
         * task that is mid-call. Future firings are cancelled.
         */
        void Stop();

        /**
         * @brief Returns whether the scheduler thread is currently running.
         *
         * @return true if started
         */
        [[nodiscard]]
        bool IsRunning() const { return _running; }

        /**
         * @brief Schedules a task to run exactly once, after the given delay.
         *
         * @param name descriptive name, used in log messages
         * @param task function to run in the background
         * @param delay delay before the task fires
         * @return task id, usable with Cancel()
         */
        std::string ScheduleOnce(const std::string &name, std::function<void()> task, milliseconds delay);

        /**
         * @brief Schedules a task to run exactly once, at the given point in time.
         *
         * @param name descriptive name, used in log messages
         * @param task function to run in the background
         * @param when point in time the task should fire; if already in the past, it fires immediately
         * @return task id, usable with Cancel()
         */
        std::string ScheduleAt(const std::string &name, std::function<void()> task, system_clock::time_point when);

        /**
         * @brief Schedules a task to run repeatedly at a fixed interval.
         *
         * @param name descriptive name, used in log messages
         * @param task function to run in the background on every firing
         * @param interval time between two firings
         * @param initialDelay delay before the first firing, defaults to one interval
         * @return task id, usable with Cancel()
         */
        std::string SchedulePeriodic(const std::string &name, std::function<void()> task, milliseconds interval, std::optional<milliseconds> initialDelay = std::nullopt);

        /**
         * @brief Schedules a task to run repeatedly according to a cron expression.
         *
         * @param name descriptive name, used in log messages
         * @param task function to run in the background on every firing
         * @param cronExpression standard 5-field cron expression, see CronExpression
         * @return task id, usable with Cancel()
         * @throws std::invalid_argument if the cron expression cannot be parsed
         */
        std::string ScheduleCron(const std::string &name, std::function<void()> task, const std::string &cronExpression);

        /**
         * @brief Cancels a previously scheduled task. A task already firing is not interrupted.
         *
         * @param taskId id returned by one of the Schedule* methods
         * @return true if the task was found and cancelled
         */
        bool Cancel(const std::string &taskId);

    private:

        enum class TaskType { Once,
                               Periodic,
                               Cron };

        struct Entry {
            std::string id;
            std::string name;
            TaskType type;
            std::function<void()> function;
            system_clock::time_point nextRun;
            milliseconds interval{0};
            std::optional<CronExpression> cron;
            std::atomic<bool> cancelled{false};
        };

        Scheduler() = default;
        ~Scheduler();

        /**
         * @brief Registers a new entry and wakes up the scheduler thread.
         *
         * @param entry entry to register
         * @return the entry's id
         */
        std::string schedule(const std::shared_ptr<Entry> &entry);

        /**
         * @brief Scheduler thread main loop; waits until the next entry is due, runs it on its
         * own detached thread, and reschedules periodic/cron entries.
         */
        void run();

        /**
         * @brief Scheduler timer thread.
         */
        std::thread _worker;

        /**
         * @brief Whether the scheduler thread is running.
         */
        std::atomic<bool> _running{false};

        /**
         * @brief Guards _queue and _tasks.
         */
        std::mutex _mutex;

        /**
         * @brief Signalled whenever an entry is added, cancelled, or the scheduler is stopped.
         */
        std::condition_variable _cv;

        /**
         * @brief Entries ordered by their next firing time.
         */
        std::multimap<system_clock::time_point, std::shared_ptr<Entry> > _queue;

        /**
         * @brief Entries by id, for Cancel().
         */
        std::unordered_map<std::string, std::shared_ptr<Entry> > _tasks;

        /**
         * @brief Guards _inFlight. Separate from _mutex so a finishing task never waits on the
         * timer thread, and so Stop() can wait for tasks without holding the queue lock a task
         * might itself want - scheduling from inside a task is allowed and must not deadlock.
         */
        std::mutex _inFlightMutex;

        /**
         * @brief Signalled every time a task thread finishes.
         */
        std::condition_variable _inFlightCv;

        /**
         * @brief How many threads are inside a task right now, by task name.
         *
         * @par
         * Counted by name rather than by id because the name is what a log line has to say to be
         * of any use - "waited 2000ms, still running: nightly-backup" names the thing to look at.
         * Several firings of one periodic task can overlap, hence a count per name rather than a
         * set.
         */
        std::map<std::string, long> _inFlight;
    };

}// namespace Euclid::Core
