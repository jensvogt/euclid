#include <FtpServer.h>
#include <FtpSession.h>

// Euclid includes
#include <TransferContext.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>

namespace Euclid::FTP {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    using namespace boost::beast::http;

    namespace {

#ifdef _WIN32
        constexpr std::string_view kDefaultDataDir = "C:\\Program Files\\euclid\\data\\ftp";
#else
        constexpr std::string_view kDefaultDataDir = "/usr/local/euclid/data/ftp";
#endif

    }// namespace

    FtpServer::FtpServer(std::string socketPath, const int threads, const std::string &transferServerId)
        : HttpActionServer("FTP", std::move(socketPath), threads), _acceptor(_ftpIoc) {

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
        if (server.pasvMin > 0) _config.pasvMin = static_cast<unsigned short>(server.pasvMin);
        if (server.pasvMax > 0) _config.pasvMax = static_cast<unsigned short>(server.pasvMax);
        _config.rootDir = std::filesystem::path(_config.rootDir) / ("transfer-" + server.serverId);

        std::error_code spoolEc;
        std::filesystem::create_directories(_config.rootDir, spoolEc);
        if (spoolEc) {
            throw std::runtime_error("could not create spool directory for transfer server '" + transferServerId + "': " + spoolEc.message());
        }

        log_info << "FTP running as transfer server '" << server.serverId << "', bucket: " << server.bucketName
                << ", users: " << server.userIds.size() << ", groups: " << server.userGroups.size();

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

        log_info << "FTP control port listening on " << _config.bindAddress << ":" << _config.port << ", data ports " << _config.pasvMin << "-" << _config.pasvMax << ", root " << _config.rootDir.string();

        _running.store(true);
        _acceptThread = std::thread(&FtpServer::acceptLoop, this);
    }

    FtpServer::~FtpServer() {
        _running.store(false);
        boost::system::error_code ec;
        _acceptor.close(ec);
        if (_acceptThread.joinable()) {
            _acceptThread.join();
        }
    }

    void FtpServer::loadConfig() {
        auto &cfg = Core::Configuration::instance();

        // Only installation-wide defaults live here. Everything that distinguishes one transfer
        // server from another - address, port, passive range, bucket, who may log in - comes from
        // its ETS definition, and is applied over these by the constructor. advertisedAddress is
        // the exception: it describes the host's own NAT situation, not the server's, so it stays
        // a per-installation setting.
        _config.advertisedAddress = cfg.getOr<std::string>("euclid.modules.ftp.advertised-address", "");
        _config.rootDir = cfg.getOr<std::string>("euclid.modules.ftp.data-dir", std::string(kDefaultDataDir));

        std::error_code fsEc;
        std::filesystem::create_directories(_config.rootDir, fsEc);
        if (fsEc) {
            log_warning << "could not create FTP spool directory '" << _config.rootDir.string() << "': " << fsEc.message();
        }

    }

    void FtpServer::acceptLoop() {
        while (_running.load()) {
            boost::system::error_code ec;
            tcp::socket socket(_ftpIoc);
            _acceptor.accept(socket, ec);
            if (ec) {
                // Expected once on shutdown: the destructor closes _acceptor to unblock this
                // accept() call, which is how the loop is asked to stop.
                if (_running.load()) {
                    log_warning << "FTP accept failed: " << ec.message();
                }
                continue;
            }

            // Detached, one thread per connection: this is a small, synchronous-I/O FTP
            // server, not one built for thousands of concurrent clients. Sessions are cut
            // loose here rather than tracked/joined because there is no use case for
            // waiting on an individual client to disconnect - the process exiting (which
            // only happens after the acceptor above has already stopped taking new
            // connections) is what ends any sessions still in flight.
            const FtpServerConfig config = _config;
            std::thread([sock = std::move(socket), config]() mutable {
                FtpSession session(std::move(sock), config);
                session.Run();
            }).detach();
        }
    }

    response<string_body> FtpServer::Dispatch(const request<string_body> &req) {
        const auto action = std::string(req["x-euclid-action"]);
        if (action == "get-metrics") {
            return MetricsResponse(req);
        }
        return ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

}// namespace Euclid::FTP
