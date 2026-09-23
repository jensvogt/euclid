// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 17/08/2026.
//

#include <euclid/core/Scheduler.h>

// C++ includes
#include <utility>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/core/UuidUtils.h>

namespace Euclid::Core {

    namespace {

        // How long Stop() waits for tasks that are already running.
        //
        // Under the manager's five seconds, with room left for UnixSocketServer::stop() to join
        // the io_context workers afterwards. A task that outlasts this is one that blocks
        // indefinitely - a socket read with no timeout, a backup halfway through a large zip - and
        // holding the process open for it only trades a race for a SIGKILL.
        constexpr auto kStopGracePeriod = std::chrono::milliseconds(2000);

    }// namespace

    Scheduler::~Scheduler() {
        Stop();
    }

    void Scheduler::Start() {
        if (_running.exchange(true)) return;
        _worker = std::thread(&Scheduler::run, this);
        log_debug << "Scheduler started";
    }

    void Scheduler::Stop() {
        if (!_running.exchange(false)) return;
        _cv.notify_all();

        // The timer thread first, so nothing new is dispatched while we wait below. Once it has
        // returned, every task that will ever run has already been counted into _inFlight.
        if (_worker.joinable()) _worker.join();

        std::unique_lock lock(_inFlightMutex);
        if (!_inFlightCv.wait_for(lock, kStopGracePeriod, [this] { return _inFlight.empty(); })) {

            std::string running;
            for (const auto &[name, count]: _inFlight) {
                if (!running.empty()) running += ", ";
                running += name;
                if (count > 1) running += " (x" + std::to_string(count) + ")";
            }

            // Not an error: the process is going away regardless, and the task may be doing
            // exactly what it should with a slow resource. It is a warning because whatever those
            // tasks touch is about to be destroyed underneath them.
            log_warning << "Scheduler stopped with tasks still running, waited: " << kStopGracePeriod.count()
                        << "ms, running: " << running;
            return;
        }

        log_debug << "Scheduler stopped";
    }

    std::string Scheduler::ScheduleOnce(const std::string &name, std::function<void()> task, const milliseconds delay) {
        return ScheduleAt(name, std::move(task), system_clock::now() + delay);
    }

    std::string Scheduler::ScheduleAt(const std::string &name, std::function<void()> task, const system_clock::time_point when) {
        auto entry = std::make_shared<Entry>();
        entry->id = UuidUtils::CreateRandomUuid();
        entry->name = name;
        entry->type = TaskType::Once;
        entry->function = std::move(task);
        entry->nextRun = when;
        return schedule(entry);
    }

    std::string Scheduler::SchedulePeriodic(const std::string &name, std::function<void()> task, const milliseconds interval, const std::optional<milliseconds> initialDelay) {
        auto entry = std::make_shared<Entry>();
        entry->id = UuidUtils::CreateRandomUuid();
        entry->name = name;
        entry->type = TaskType::Periodic;
        entry->function = std::move(task);
        entry->interval = interval;
        entry->nextRun = system_clock::now() + initialDelay.value_or(interval);
        return schedule(entry);
    }

    std::string Scheduler::ScheduleCron(const std::string &name, std::function<void()> task, const std::string &cronExpression) {
        CronExpression cron(cronExpression);
        auto entry = std::make_shared<Entry>();
        entry->id = UuidUtils::CreateRandomUuid();
        entry->name = name;
        entry->type = TaskType::Cron;
        entry->function = std::move(task);
        entry->nextRun = cron.Next(system_clock::now());
        entry->cron = std::move(cron);
        return schedule(entry);
    }

    std::string Scheduler::schedule(const std::shared_ptr<Entry> &entry) {
        {
            std::lock_guard lock(_mutex);
            _tasks[entry->id] = entry;
            _queue.emplace(entry->nextRun, entry);
        }
        _cv.notify_all();
        log_debug << "Scheduler task scheduled, name: " << entry->name << ", id: " << entry->id;
        return entry->id;
    }

    bool Scheduler::Cancel(const std::string &taskId) {
        std::lock_guard lock(_mutex);
        const auto it = _tasks.find(taskId);
        if (it == _tasks.end()) return false;
        it->second->cancelled = true;
        _tasks.erase(it);
        _cv.notify_all();
        return true;
    }

    void Scheduler::run() {
        std::unique_lock lock(_mutex);
        while (_running) {

            if (_queue.empty()) {
                _cv.wait(lock, [this] { return !_running || !_queue.empty(); });
                continue;
            }

            const auto nextRun = _queue.begin()->first;
            if (nextRun > system_clock::now()) {
                _cv.wait_until(lock, nextRun, [this, nextRun] { return !_running || _queue.empty() || _queue.begin()->first < nextRun; });
                continue;
            }

            const auto entry = _queue.begin()->second;
            _queue.erase(_queue.begin());

            if (entry->cancelled) continue;

            lock.unlock();

            // Counted before the thread exists, not inside it. Registering from the new thread
            // would leave a window in which the task is dispatched but invisible, and Stop() could
            // pass through that window and let the process tear down around a task about to start.
            {
                std::lock_guard inFlight(_inFlightMutex);
                ++_inFlight[entry->name];
            }

            std::thread([this, entry] {
                // Whatever happens, the count comes back down - a task that throws past the
                // handlers below would otherwise hold every future Stop() for the full grace
                // period.
                struct Done {
                    Scheduler *scheduler;
                    const std::string &name;
                    ~Done() {
                        {
                            std::lock_guard lock(scheduler->_inFlightMutex);
                            if (const auto it = scheduler->_inFlight.find(name); it != scheduler->_inFlight.end() && --it->second <= 0) {
                                scheduler->_inFlight.erase(it);
                            }
                        }
                        scheduler->_inFlightCv.notify_all();
                    }
                } done{this, entry->name};

                // Cancelled between being taken off the queue and getting here. Cheap, and the
                // difference between a clean shutdown and one last firing of a task whose owner
                // has already been destroyed - MetricsPusher's destructor cancels, and it runs
                // while the module is stopping.
                if (entry->cancelled) return;

                try {
                    entry->function();
                } catch (const std::exception &ex) {
                    log_error << "Scheduler task failed, name: " << entry->name << ", id: " << entry->id << ", error: " << ex.what();
                } catch (...) {
                    log_error << "Scheduler task failed, name: " << entry->name << ", id: " << entry->id << ", error: unknown exception";
                }
            }).detach();
            lock.lock();

            if (entry->cancelled) continue;

            if (entry->type == TaskType::Once) {
                _tasks.erase(entry->id);
                continue;
            }

            const auto now = system_clock::now();
            if (entry->type == TaskType::Periodic) {
                entry->nextRun += entry->interval;
                while (entry->nextRun <= now) entry->nextRun += entry->interval;
            } else {
                entry->nextRun = entry->cron->Next(now);
            }
            _queue.emplace(entry->nextRun, entry);
        }
    }

}// namespace Euclid::Core
