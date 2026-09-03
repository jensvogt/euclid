//
// Created by vogje01 on 5/24/26.
//
#pragma once

// C++ includes
#include <algorithm>
#include <mutex>
#include <ranges>
#include <unordered_map>

// Euclid includes
#include <euclid/database/repository/emm/IEmmRepository.h>

namespace Euclid::Database {

    /**
     * @brief Module memory database.
     *
     * Controls all the AwsMock modules.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryEmmRepository final : public IEmmRepository {

    public:
        /**
         * @brief Singleton instance
         */
        static MemoryEmmRepository &instance() {
            static MemoryEmmRepository moduleDatabase;
            return moduleDatabase;
        }

        void upsertInstance(const Entity::Module &module, const Entity::ModuleInstance &instance) override {
            std::lock_guard lock(_mutex);
            auto &stored = _store[module.name];
            const auto now = std::chrono::system_clock::now();
            if (stored.name.empty()) {
                stored.name = module.name;
                stored.created = now;
            }
            stored.executable = module.executable;
            stored.socketPath = module.socketPath;
            stored.active = module.active;
            stored.autoRestart = module.autoRestart;
            stored.maxRestarts = module.maxRestarts;
            stored.minInstances = module.minInstances;
            stored.maxInstances = module.maxInstances;
            stored.args = module.args;
            stored.modified = now;

            // See MongoEmmRepository::upsertInstance for why this is gated on "no other instance
            // is already RUNNING" rather than unconditionally stamped on every RUNNING transition.
            if (instance.state == Entity::ModuleState::RUNNING) {
                const bool anyOtherRunning = std::ranges::any_of(stored.instances, [&](const auto &i) {
                    return i.instanceId != instance.instanceId && i.state == Entity::ModuleState::RUNNING;
                });
                if (!anyOtherRunning) stored.lastStartTime = now;
            }

            Entity::ModuleInstance toStore = instance;
            const auto it = std::ranges::find_if(stored.instances, [&](const auto &i) { return i.instanceId == instance.instanceId; });
            if (it != stored.instances.end()) {
                toStore.created = it->created;
                *it = toStore;
            } else {
                toStore.created = now;
                stored.instances.push_back(toStore);
            }
        }

        void removeInstance(const std::string &moduleName, const std::string &instanceId) override {
            std::lock_guard lock(_mutex);
            const auto it = _store.find(moduleName);
            if (it == _store.end()) return;
            std::erase_if(it->second.instances, [&](const auto &i) { return i.instanceId == instanceId; });
            it->second.modified = std::chrono::system_clock::now();
        }

        bool setDesiredInstances(const std::string &name, const int minInstances, const int maxInstances) override {
            std::lock_guard lock(_mutex);
            const auto it = _store.find(name);
            if (it == _store.end()) return false;
            if (minInstances >= 0) it->second.desiredMinInstances = minInstances;
            if (maxInstances >= 0) it->second.desiredMaxInstances = maxInstances;
            it->second.modified = std::chrono::system_clock::now();
            return true;
        }

        bool setDesiredThreads(const std::string &name, const int threads) override {
            std::lock_guard lock(_mutex);
            const auto it = _store.find(name);
            if (it == _store.end()) return false;
            it->second.desiredThreads = threads;
            it->second.modified = std::chrono::system_clock::now();
            return true;
        }

        bool setDesiredStopped(const std::string &name, const bool stopped) override {
            std::lock_guard lock(_mutex);
            const auto it = _store.find(name);
            if (it == _store.end()) return false;
            it->second.desiredStopped = stopped;
            it->second.modified = std::chrono::system_clock::now();
            return true;
        }

        bool requestRestart(const std::string &name) override {
            std::lock_guard lock(_mutex);
            const auto it = _store.find(name);
            if (it == _store.end()) return false;
            it->second.restartRequestedAt = std::chrono::system_clock::now();
            it->second.modified = it->second.restartRequestedAt;
            return true;
        }

        void remove(const std::string &name) override {
            std::lock_guard lock(_mutex);
            _store.erase(name);
        }

        std::optional<Entity::Module> findByName(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            const auto it = _store.find(name);
            if (it == _store.end()) return std::nullopt;
            return it->second;
        }

        std::optional<Entity::Module> findById(const std::string &id) const override {
            std::lock_guard lock(_mutex);
            for (const auto &m: _store | std::views::values) {
                if (m.oid == id) return m;
            }
            return std::nullopt;
        }

        std::vector<Entity::Module> findAll() const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::Module> result;
            result.reserve(_store.size());
            for (const auto &m: _store | std::views::values) result.push_back(m);
            return result;
        }

        bool exists(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            return _store.contains(name);
        }

        long count() const override {
            std::lock_guard lock(_mutex);
            return _store.size();
        }

        void clear() override {
            std::lock_guard lock(_mutex);
            _store.clear();
        }

    private:
        mutable std::mutex _mutex;
        std::unordered_map<std::string, Entity::Module> _store;
    };

} // namespace Euclid::Database