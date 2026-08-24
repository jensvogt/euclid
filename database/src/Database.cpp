//
// Created by vogje01 on 5/24/26.
//

#include <euclid/database/Database.h>

namespace Euclid::Database {

    void Database::initialize() {

        const auto host = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.host", "localhost");
        const auto port = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.port", "27017");
        const auto user = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.user", "root");
        const auto password = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.password", "password");
        const int poolSize = Core::Configuration::instance().getOr<int>("euclid.mongodb.pool-size", 10);
        _databaseName = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.name", "euclid");

        try {

            // Embed pool size in the URI — works for all mongocxx versions
            mongocxx::uri _uri("mongodb://" + user + ":" + password + "@" + host + ":" + port + "/?maxPoolSize=" + std::to_string(poolSize));

            // options::pool exists but has no size method in newer versions
            _pool = std::make_unique<mongocxx::pool>(_uri);

            log_info << "MongoDB pool initialized" << " uri=" << _uri.to_string() << " database=" << _databaseName << " poolSize=" << poolSize;

            ping();

        } catch (const mongocxx::exception &e) {
            log_error << "MongoDB initialization failed: " << e.what();
            throw std::runtime_error(std::string("MongoDB initialization failed: ") + e.what());
        }
    }

    mongocxx::pool::entry Database::client() const {
        if (!_pool) {
            throw std::runtime_error("MongoDB not initialized — call initialize() first");
        }
        return _pool->acquire();
    }

    void Database::ping() const {
        try {
            const auto entry = _pool->acquire();
            auto db = (*entry)[_databaseName];

            const auto cmd = bsoncxx::builder::stream::document{} << "ping" << 1 << bsoncxx::builder::stream::finalize;

            db.run_command(cmd.view());
            log_trace << "MongoDB ping successful";

        } catch (const mongocxx::exception &e) {
            log_error << "MongoDB ping failed: " << e.what();
            throw std::runtime_error(std::string("MongoDB ping failed: ") + e.what());
        } catch (const std::exception &e) {
            log_error << "MongoDB ping failed: " << e.what();
            throw;
        }
    }

}// namespace Euclid::Core