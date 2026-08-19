//
// Created by vogje01 on 5/24/26.
//
#pragma once

// C++ includes
#include <string>
#include <memory>

// Mongodb includes
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/v_noabi/mongocxx/exception/exception.hpp>
#include <mongocxx/v_noabi/mongocxx/uri.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Database {

    class Database {

    public:
        static Database &instance() {
            static Database inst;
            return inst;
        }

        Database(const Database &) = delete;

        Database &operator=(const Database &) = delete;

        /**
         * @brief Initialize the MongoDB connection pool
         */
        void initialize();

        /**
         * @brief Acquire a client from the pool
         */
        mongocxx::pool::entry client() const;

        /**
         * @brief Returns the database name
         */
        [[nodiscard]]
        const std::string &databaseName() const { return _databaseName; }

        /**
         * @brief Ping the server — throws if unreachable
         */
        void ping() const;

    private:
        Database() = default;

        // mongocxx::instance must be created exactly once per process
        mongocxx::instance _instance{};
        std::unique_ptr<mongocxx::pool> _pool{};
        std::string _databaseName;
    };

} // namespace Euclid::Core