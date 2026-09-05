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

// libssh includes
#include <libssh/libssh.h>
#include <libssh/server.h>

// Euclid includes
#include <TransferStorage.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/database/entity/ets/TransferServer.h>

namespace Euclid::SFTP {

    /**
     * @brief Static configuration for the SFTP listener, shared (by value) with every
     * SftpSession.
     *
     * @par
     * Assembled from two places: the defaults under euclid.modules.ets.* (spool directory,
     * fallback host key) and the ETS transfer server definition this process was started for,
     * which supplies everything that actually distinguishes one server from another.
     */
    struct SftpServerConfig {
        std::string bindAddress{"0.0.0.0"};
        unsigned short port{2222};

        /**
         * @brief Private host key identifying this server to clients. Generated on first
         * start if the file does not exist - see SftpServer::ensureHostKey().
         */
        std::filesystem::path hostKey;

        /**
         * @brief Scratch area for in-flight transfers. Not a file store: the files themselves
         * live in the bucket, and nothing here outlives the transfer that created it.
         */
        std::filesystem::path rootDir;

        /**
         * @brief The ETS definition this process serves.
         *
         * @par
         * Always present - a transfer server has no meaning without one. It supplies the address
         * and port to listen on, the ESM bucket that backs every file operation, and the EAM
         * users and groups permitted to log in. There is deliberately no second, config-file
         * credential source: EAM is the only way in.
         */
        Database::Entity::ETS::TransferServer transferServer;
    };

    /**
     * @brief Small SFTP server module.
     *
     * @par
     * Structured exactly like the FTP module next to it: the Unix domain socket it inherits
     * from Core::HttpActionServer exists purely so euclid-mgr's ServiceController can
     * supervise the process (readiness probe, "get-metrics"), and real SFTP traffic never
     * goes through it or through the gateway. The constructor opens a second, ordinary TCP
     * acceptor on the port its ETS transfer server definition names, and serves SSH there,
     * one thread per connected client (see SftpSession).
     *
     * @par
     * SFTP is the SSH File Transfer Protocol, so despite the name it shares no wire format
     * with FTP: there is no control/data connection split and no PASV port range, only a
     * single SSH connection carrying a "sftp" subsystem channel. libssh provides the SSH
     * transport and the SFTP packet coding; this module supplies authentication, the
     * per-user directory sandbox, and the filesystem operations.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class SftpServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server: starts the Unix-socket health/metrics endpoint,
         * loads the SFTP configuration, prepares the SSH host key, and starts listening for
         * SFTP clients.
         *
         * @param socketPath Unix domain socket path to listen on (health/metrics only).
         * @param threads    number of io_context worker threads for the Unix socket.
         * @param transferServerId ETS transfer server to run as; empty runs the standalone,
         * config-driven server instead.
         *
         * @throws std::runtime_error if the host key cannot be loaded or generated, if
         * the port is already in use, or if transferServerId names no known server.
         */
        explicit SftpServer(std::string socketPath, int threads = 2, const std::string &transferServerId = "");

        /**
         * @brief Stops the SFTP TCP acceptor, joins its thread and releases the ssh_bind.
         */
        ~SftpServer() override;

        SftpServer(const SftpServer &) = delete;
        SftpServer &operator=(const SftpServer &) = delete;

    protected:

        /**
         * @brief Handles a request on the Unix domain socket. Only "get-metrics" is
         * implemented; every other action is 404, since SFTP traffic never arrives here.
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
         * @brief Generates an ed25519 host key at _config.hostKey if that file does not yet
         * exist, so a fresh install starts without an out-of-band key generation step.
         *
         * @par
         * The key is written with owner-only permissions. An existing file is never
         * touched: regenerating it would change the server's identity and make every client
         * that has already pinned it refuse to connect.
         *
         * @throws std::runtime_error if the key can be neither read nor generated.
         */
        void ensureHostKey() const;

        /**
         * @brief Accept loop for the SFTP port; runs on _acceptThread until stopped. Spawns
         * a detached thread running SftpSession::Run() per accepted connection, for the same
         * reasons the FTP module does.
         */
        void acceptLoop();

        SftpServerConfig _config;

        /**
         * @brief The ssh_bind holds the loaded host key and turns an accepted TCP socket
         * into an ssh_session. Ownership of the listening socket itself stays with
         * _acceptor rather than the bind, so shutdown works the same way as in the FTP
         * module: closing the acceptor unblocks accept().
         */
        ssh_bind _sshBind{nullptr};

        /**
         * @brief io_context backing the SFTP TCP acceptor. Only ever used for synchronous
         * (blocking) operations, so nothing ever calls run() on it - it exists purely as the
         * executor Asio's acceptor object requires.
         */
        boost::asio::io_context _sftpIoc;

        boost::asio::ip::tcp::acceptor _acceptor;
        std::thread _acceptThread;
        std::atomic<bool> _running{false};
    };

}// namespace Euclid::SFTP
