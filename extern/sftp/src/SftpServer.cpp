#include <SftpServer.h>
#include <SftpSession.h>

// Euclid includes
#include <TransferContext.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>

namespace Euclid::SFTP {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    using namespace boost::beast::http;

    namespace {

#ifdef _WIN32
        constexpr std::string_view kDefaultDataDir = "C:\\Program Files\\euclid\\data\\sftp";
        constexpr std::string_view kDefaultHostKey = "C:\\Program Files\\euclid\\etc\\ssh_host_key";
#else
        constexpr std::string_view kDefaultDataDir = "/usr/local/euclid/data/sftp";
        constexpr std::string_view kDefaultHostKey = "/usr/local/euclid/etc/ssh_host_key";
#endif

    }// namespace

    SftpServer::SftpServer(std::string socketPath, const int threads, const std::string &transferServerId)
        : HttpActionServer("SFTP", std::move(socketPath), threads), _acceptor(_sftpIoc) {

        // A transfer server is the only thing this process can be. There is no config-file
        // credential source to fall back on, so without a definition there would be no bucket to
        // serve and nobody who could log in - failing here is clearer than listening on a port
        // that rejects everyone.
        if (transferServerId.empty()) {
            throw std::runtime_error("--transfer-server is required: define a server with 'euclid-cli ets create-server' and start it with 'euclid-cli ets start-server'");
        }

        loadConfig();

        const auto context = Transfer::TransferContext::Load(transferServerId);
        if (!context.has_value()) {
            throw std::runtime_error("transfer server '" + transferServerId + "' is not defined");
        }
        const auto &server = context->Server();
        if (server.bucketErn.empty()) {
            throw std::runtime_error("transfer server '" + transferServerId + "' has no bucket");
        }

        // The ETS definition wins over anything in the config file: it is the record euclid-mgr
        // started this process from, and the one the ETS module edits, so letting a stale config
        // key override it would make the two disagree.
        _config.transferServer = server;
        _config.bindAddress = server.address;
        _config.port = static_cast<unsigned short>(server.port);
        if (!server.hostKey.empty()) _config.hostKey = server.hostKey;
        _config.rootDir = std::filesystem::path(_config.rootDir) / ("transfer-" + server.serverId);

        std::error_code spoolEc;
        std::filesystem::create_directories(_config.rootDir, spoolEc);
        if (spoolEc) {
            throw std::runtime_error("could not create spool directory for transfer server '" + transferServerId + "': " + spoolEc.message());
        }

        log_info << "SFTP running as transfer server '" << server.serverId << "', bucket: " << server.bucketName
                << ", users: " << server.userIds.size() << ", groups: " << server.userGroups.size();

        ensureHostKey();

        _sshBind = ssh_bind_new();
        if (_sshBind == nullptr) {
            throw std::runtime_error("could not create ssh bind for SFTP");
        }

        const auto hostKey = _config.hostKey.string();
        if (ssh_bind_options_set(_sshBind, SSH_BIND_OPTIONS_HOSTKEY, hostKey.c_str()) != SSH_OK) {
            const std::string error = ssh_get_error(_sshBind);
            ssh_bind_free(_sshBind);
            _sshBind = nullptr;
            throw std::runtime_error("could not load SFTP host key '" + hostKey + "': " + error);
        }

        boost::system::error_code ec;
        const auto address = asio::ip::make_address(_config.bindAddress, ec);
        if (ec) {
            throw std::runtime_error("transfer server '" + transferServerId + "' has an invalid address '" + _config.bindAddress + "': " + ec.message());
        }
        const tcp::endpoint endpoint(address, _config.port);

        _acceptor.open(endpoint.protocol(), ec);
        if (!ec) _acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        if (!ec) _acceptor.bind(endpoint, ec);
        if (!ec) _acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("transfer server '" + transferServerId + "' could not listen on " + _config.bindAddress + ":" + std::to_string(_config.port) + ": " + ec.message());
        }

        log_info << "SFTP port listening on " << _config.bindAddress << ":" << _config.port << ", host key " << hostKey << ", root " << _config.rootDir.string();

        _running.store(true);
        _acceptThread = std::thread(&SftpServer::acceptLoop, this);
    }

    SftpServer::~SftpServer() {
        _running.store(false);
        boost::system::error_code ec;
        _acceptor.close(ec);
        if (_acceptThread.joinable()) {
            _acceptThread.join();
        }
        if (_sshBind != nullptr) {
            ssh_bind_free(_sshBind);
            _sshBind = nullptr;
        }
    }

    void SftpServer::loadConfig() {
        auto &cfg = Core::Configuration::instance();

        // Only installation-wide defaults live here. Everything that distinguishes one transfer
        // server from another - address, port, bucket, who may log in - comes from its ETS
        // definition, and is applied over these by the constructor.
        _config.hostKey = cfg.getOr<std::string>("euclid.modules.sftp.host-key", std::string(kDefaultHostKey));
        _config.rootDir = cfg.getOr<std::string>("euclid.modules.sftp.data-dir", std::string(kDefaultDataDir));

        std::error_code fsEc;
        std::filesystem::create_directories(_config.rootDir, fsEc);
        if (fsEc) {
            log_warning << "could not create SFTP spool directory '" << _config.rootDir.string() << "': " << fsEc.message();
        }
    }

    void SftpServer::ensureHostKey() const {

        if (std::filesystem::exists(_config.hostKey)) {
            return;
        }

        std::error_code fsEc;
        if (const auto parent = _config.hostKey.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent, fsEc);
            if (fsEc) {
                throw std::runtime_error("could not create directory for SFTP host key '" + _config.hostKey.string() + "': " + fsEc.message());
            }
        }

        // ed25519 rather than RSA: fixed-size keys, no parameter to get wrong, and accepted
        // by every client that speaks a modern SSH.
        ssh_key key = nullptr;
        if (ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &key) != SSH_OK || key == nullptr) {
            throw std::runtime_error("could not generate SFTP host key '" + _config.hostKey.string() + "'");
        }

        const auto path = _config.hostKey.string();
        const auto rc = ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, path.c_str());
        ssh_key_free(key);

        if (rc != SSH_OK) {
            throw std::runtime_error("could not write SFTP host key '" + path + "'");
        }

        // A world-readable private host key would let any local user impersonate this server.
        std::filesystem::permissions(_config.hostKey,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, fsEc);
        if (fsEc) {
            log_warning << "could not restrict permissions on SFTP host key '" << path << "': " << fsEc.message();
        }

        log_info << "SFTP host key generated: " << path;
    }

    void SftpServer::acceptLoop() {
        while (_running.load()) {
            boost::system::error_code ec;
            tcp::socket socket(_sftpIoc);
            _acceptor.accept(socket, ec);
            if (ec) {
                // Expected once on shutdown: the destructor closes _acceptor to unblock this
                // accept() call, which is how the loop is asked to stop.
                if (_running.load()) {
                    log_warning << "SFTP accept failed: " << ec.message();
                }
                continue;
            }

            ssh_session session = ssh_new();
            if (session == nullptr) {
                log_error << "SFTP could not allocate ssh session, dropping connection";
                continue;
            }

            // release() hands the accepted descriptor to libssh, which closes it when the
            // session is freed. Without it the fd would be closed twice - once by Asio when
            // `socket` goes out of scope here, once by ssh_free() on the session thread.
            const auto fd = socket.release(ec);
            if (ec) {
                log_warning << "SFTP could not release accepted socket: " << ec.message();
                ssh_free(session);
                continue;
            }

            if (ssh_bind_accept_fd(_sshBind, session, static_cast<socket_t>(fd)) != SSH_OK) {
                log_warning << "SFTP handshake setup failed: " << ssh_get_error(_sshBind);
                ssh_free(session);
                continue;
            }

            // Detached, one thread per connection, for the same reason the FTP module does
            // it: this is a small, synchronous-I/O server, and the process exiting - which
            // only happens after the acceptor above has stopped taking new connections - is
            // what ends any sessions still in flight.
            const SftpServerConfig config = _config;
            std::thread([session, config]() mutable {
                SftpSession sftpSession(session, config);
                sftpSession.Run();
            }).detach();
        }
    }

    response<string_body> SftpServer::Dispatch(const request<string_body> &req) {
        const auto action = std::string(req["x-euclid-action"]);
        if (action == "get-metrics") {
            return MetricsResponse(req);
        }
        return ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

}// namespace Euclid::SFTP
