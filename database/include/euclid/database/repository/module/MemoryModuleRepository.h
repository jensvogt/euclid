//
// Created by vogje01 on 5/24/26.
//
#pragma once

// C++ includes
#include <mutex>
#include <ranges>
#include <unordered_map>

// Euclid includes
#include <euclid/database/repository/module/IModuleRepository.h>

namespace Euclid::Database {

    /**
     * @brief Module memory database.
     *
     * Controls all the AwsMock modules.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryModuleRepository final : public IModuleRepository {

    public:
        /**
         * @brief Singleton instance
         */
        static MemoryModuleRepository &instance() {
            static MemoryModuleRepository moduleDatabase;
            return moduleDatabase;
        }

        void upsert(const Entity::Module &module) override {
            std::lock_guard lock(_mutex);
            _store[module.name] = module;
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