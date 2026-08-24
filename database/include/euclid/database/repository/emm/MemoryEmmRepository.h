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
            stored.args = module.args;
            stored.modified = now;

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