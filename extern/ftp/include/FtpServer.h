#pragma once

// C++ includes
#include <atomic>
#include <optional>
#include <filesystem>
#include <string>
#include <thread>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <TransferStorage.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/database/entity/ets/TransferServer.h>

namespace Euclid::FTP {

    /**
     * @brief Static configuration for the FTP control/data listeners, read once at startup
     * from euclid.modules.ets.* and shared (by value) with every FtpSession.
     */
    struct FtpServerConfig {
        std::string bindAddress{"0.0.0.0"};
        unsigned short port{2121};
        unsigned short pasvMin{6000};
        unsigned short pasvMax{6100};

        /**
         * @brief Address advertised to clients in the PASV reply. Empty means "use the
         * control connection's own local address", which is correct unless the server is
         * behind NAT and needs to advertise a different externally-reachable address.
         */
        std::string advertisedAddress;

        /**
         * @brief Scratch area for in-flight transfers. Not a file store: the files themselves
         * live in the bucket, and nothing here outlives the transfer that created it.
         */
        std::filesystem::path rootDir;

        /**
         * @brief The ETS definition this process serves.
         *
         * @par
         * Always present - a transfer server has no meaning without one. It supplies the address,
         * port and passive range to listen on, the ESM bucket that backs every file operation, and
         * the EAM users and groups permitted to log in. There is deliberately no second,
         * config-file credential source: EAM is the only way in.
         */
        Database::Entity::ETS::TransferServer transferServer;
    };

    /**
     * @brief Small FTP server module.
     *
     * Like every other euclid module it exposes a Unix domain socket
     * (Core::HttpActionServer) purely so euclid-mgr's ServiceController can supervise it
     * (readiness probe, "get-metrics") - real FTP traffic never goes through that socket
     * or through the gateway. Instead, the constructor opens a second, ordinary TCP
     * acceptor on the port its ETS transfer server definition names, and serves the actual
     * FTP control protocol there, one thread per client connection (see FtpSession).
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class FtpServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server: starts the Unix-socket health/metrics endpoint,
         * loads the FTP configuration, and starts listening for FTP clients.
         *
         * @param socketPath Unix domain socket path to listen on (health/metrics only).
         * @param threads    number of io_context worker threads for the Unix socket.
         *
         * @throws std::runtime_error if transferServerId is empty or names no known server,
         * or if the port that server defines is already in use.
         */
        explicit FtpServer(std::string socketPath, int threads = 2, const std::string &transferServerId = "");

        /**
         * @brief Stops the FTP TCP acceptor and joins its thread.
         */
        ~FtpServer() override;

        FtpServer(const FtpServer &) = delete;
        FtpServer &operator=(const FtpServer &) = delete;

    protected:

        /**
         * @brief Handles a request on the Unix domain socket. Only "get-metrics" is
         * implemented; every other action is 404, since FTP traffic never arrives here.
         */
        [[nodiscard]]
        boost::beast::http::response<boost::beast::http::string_body>
        Dispatch(const boost::beast::http::request<boost::beast::http::string_body> &req) override;

    private:

        /**
         * @brief Reads euclid.modules.ets.* into _config, creating each configured user's
         * home directory under rootDir if it doesn't already exist.
         */
        void loadConfig();

        /**
         * @brief Accept loop for the FTP control port; runs on _acceptThread until
         * stopped. Spawns a detached thread running FtpSession::Run() per accepted
         * connection - see the .cpp for why detached is an acceptable tradeoff here.
         */
        void acceptLoop();

        FtpServerConfig _config;

        /**
         * @brief io_context backing the FTP TCP acceptor and every session's sockets.
         * Only ever used for synchronous (blocking) operations, so nothing ever calls
         * run() on it - it exists purely as the executor Asio's socket/acceptor objects
         * require.
         */
        boost::asio::io_context _ftpIoc;

        boost::asio::ip::tcp::acceptor _acceptor;
        std::thread _acceptThread;
        std::atomic<bool> _running{false};
    };

}// namespace Euclid::FTP
