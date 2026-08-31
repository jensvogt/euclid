#pragma once

// C++ includes
#include <optional>
#include <string>

// Euclid includes
#include <euclid/database/entity/ets/TransferServer.h>

namespace Euclid::Transfer {

    /**
     * @brief Everything a spawned euclid-ftp/euclid-sftp process needs to know about itself.
     *
     * @par
     * A transfer server process is started by euclid-mgr with nothing but a --transfer-server
     * ID; it reads its own definition back out of the ETS repository from there. That
     * indirection is what lets the ETS module change a server's bucket, ports or permitted
     * users without the manager having to pass any of it on the command line.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class TransferContext {

    public:

        /**
         * @brief Loads the transfer server definition with this ID.
         *
         * @param serverId server ID, as passed to --transfer-server.
         * @return the context, or std::nullopt if no such server is defined.
         */
        [[nodiscard]]
        static std::optional<TransferContext> Load(const std::string &serverId);

        /**
         * @brief The definition this process is running.
         */
        [[nodiscard]]
        const Database::Entity::ETS::TransferServer &Server() const { return _server; }

    private:

        explicit TransferContext(Database::Entity::ETS::TransferServer server) : _server(std::move(server)) {}

        Database::Entity::ETS::TransferServer _server;
    };

    /**
     * @brief Result of one call to a module over its Unix domain socket.
     */
    struct ModuleResponse {

        /**
         * @brief HTTP status returned by the module, or 0 if the call never got that far.
         */
        int status{};

        /**
         * @brief Response body, which for ESM's object actions is the object's raw bytes.
         */
        std::string body;

        /**
         * @brief Whether the call succeeded.
         */
        [[nodiscard]] bool ok() const { return status >= 200 && status < 300; }
    };

    /**
     * @brief Calls one action on a running instance of another module.
     *
     * @par
     * The target instance is resolved through the module repository, the same way
     * Core::Monitoring::MetricsPusher finds the monitoring module: euclid-mgr publishes each
     * live instance's socket path there, so addressing a module means asking which of its
     * processes are currently up rather than assuming the single base socket from the config.
     *
     * @param moduleName module to call, e.g. "esm".
     * @param action value for the x-euclid-action header.
     * @param token bearer token to authenticate as; a transfer server passes one minted for the
     * logged-in user, so the target module applies that user's own permissions.
     * @param headers additional request headers, e.g. x-euclid-bucket-ern.
     * @param body request body, raw.
     * @return the module's response, or a zero status if no instance could be reached.
     */
    [[nodiscard]]
    ModuleResponse CallModule(const std::string &moduleName, const std::string &action, const std::string &token,
                              const std::vector<std::pair<std::string, std::string> > &headers, const std::string &body);

}// namespace Euclid::Transfer
