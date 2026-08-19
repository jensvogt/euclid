//
// Created by vogje01 on 17/08/2026.
//

#include <euclid/core/Scheduler.h>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/core/UuidUtils.h>

namespace Euclid::Core {

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
        if (_worker.joinable()) _worker.join();
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
            std::thread([entry] {
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
