#include <FtpServer.h>
#include <FtpSession.h>

// Euclid includes
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

    FtpServer::FtpServer(std::string socketPath, const int threads) : HttpActionServer("FTP", std::move(socketPath), threads), _acceptor(_ftpIoc) {

        loadConfig();

        boost::system::error_code ec;
        const auto address = asio::ip::make_address(_config.bindAddress, ec);
        if (ec) {
            throw std::runtime_error("invalid euclid.modules.ftp.address '" + _config.bindAddress + "': " + ec.message());
        }
        const tcp::endpoint endpoint(address, _config.port);

        _acceptor.open(endpoint.protocol(), ec);
        if (!ec) _acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        if (!ec) _acceptor.bind(endpoint, ec);
        if (!ec) _acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("could not listen on " + _config.bindAddress + ":" + std::to_string(_config.port) + " for FTP: " + ec.message());
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

        _config.bindAddress = cfg.getOr<std::string>("euclid.modules.ftp.address", "0.0.0.0");
        _config.port = static_cast<unsigned short>(cfg.getOr<long>("euclid.modules.ftp.port", 2121L));
        _config.pasvMin = static_cast<unsigned short>(cfg.getOr<long>("euclid.modules.ftp.pasv-min", 6000L));
        _config.pasvMax = static_cast<unsigned short>(cfg.getOr<long>("euclid.modules.ftp.pasv-max", 6100L));
        _config.advertisedAddress = cfg.getOr<std::string>("euclid.modules.ftp.advertised-address", "");
        _config.rootDir = cfg.getOr<std::string>("euclid.modules.ftp.data-dir", std::string(kDefaultDataDir));

        std::error_code fsEc;
        std::filesystem::create_directories(_config.rootDir, fsEc);
        if (fsEc) {
            log_warning << "could not create FTP root directory '" << _config.rootDir.string() << "': " << fsEc.message();
        }

        if (cfg.has("euclid.modules.ftp.users")) {
            for (const auto &[name, props]: cfg.getObjects("euclid.modules.ftp.users")) {
                FtpUser user;
                user.password = props.contains("password") ? std::get<std::string>(props.at("password")) : std::string();
                user.home = props.contains("home") ? std::get<std::string>(props.at("home")) : name;
                _config.users.emplace(name, user);

                std::filesystem::create_directories(_config.rootDir / user.home, fsEc);
                if (fsEc) {
                    log_warning << "could not create FTP home directory for user '" << name << "': " << fsEc.message();
                }
            }
        }

        if (_config.users.empty()) {
            log_warning << "no users configured under euclid.modules.ftp.users - nobody will be able to log in";
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
