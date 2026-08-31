#pragma once

// C++ includes
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

// Euclid includes
#include <FtpServer.h>
#include <TransferAuthenticator.h>
#include <TransferStorage.h>
#include <euclid/core/LogStream.h>

namespace Euclid::FTP {

    /**
     * @brief Handles one client's FTP control connection, plus whatever data connections
     * (PASV/PORT) it opens along the way.
     *
     * Runs entirely on the thread that constructs it (see FtpServer::acceptLoop()), using
     * blocking Asio calls throughout - there is no async state machine here, which keeps a
     * "small" FTP server's command handling readable at the cost of one OS thread per
     * connected client.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class FtpSession {

    public:

        FtpSession(boost::asio::ip::tcp::socket socket, FtpServerConfig config);

        /**
         * @brief Sends the greeting, then reads and dispatches commands until the client
         * disconnects, sends QUIT, or a protocol error occurs. Never throws.
         */
        void Run();

    private:

        /**
         * @brief What a virtual FTP path (e.g. "..", "reports/2026-08.csv") resolves to:
         * both the normalized virtual path (for PWD/CWD replies) and the real filesystem
         * path it maps to under the user's home directory.
         */
        struct ResolvedPath {
            std::string virtualPath;
            std::filesystem::path physicalPath;
        };

        // ── Connection I/O ──────────────────────────────────────────────
        void sendReply(const std::string &line);
        void sendReply(int code, const std::string &message);
        [[nodiscard]] std::optional<std::string> readLine();

        // ── Command dispatch ────────────────────────────────────────────
        void handleCommand(const std::string &verb, const std::string &arg);

        void cmdUser(const std::string &arg);
        void cmdPass(const std::string &arg);
        void cmdQuit();
        void cmdSyst();
        void cmdFeat();
        void cmdNoop();
        void cmdType(const std::string &arg);
        void cmdPwd();
        void cmdCwd(const std::string &arg);
        void cmdCdup();
        void cmdPasv();
        void cmdPort(const std::string &arg);
        void cmdMkd(const std::string &arg);
        void cmdRmd(const std::string &arg);
        void cmdList(const std::string &arg);
        void cmdRetr(const std::string &arg);
        void cmdStor(const std::string &arg);
        void cmdDele(const std::string &arg);
        void cmdSize(const std::string &arg);
        void cmdMdtm(const std::string &arg);

        // ── Helpers ─────────────────────────────────────────────────────

        /**
         * @brief Resolves a virtual FTP path (absolute or relative to the current
         * directory) to a physical path under the user's home directory.
         *
         * ".." components are clamped at the virtual root rather than rejected, so the
         * physical path can never be constructed outside the home directory in the first
         * place - see the .cpp for the caveat this doesn't cover (symlinks).
         */
        [[nodiscard]] ResolvedPath resolve(const std::string &arg) const;

        /**
         * @brief Opens the data connection set up by the preceding PASV or PORT command:
         * accepts on the PASV listener, or connects out for PORT. Sends "425" and returns
         * std::nullopt if neither was set up, or the connection attempt fails.
         */
        [[nodiscard]] std::optional<boost::asio::ip::tcp::socket> openDataConnection();

        /**
         * @brief Maps a virtual FTP path to the bucket key it addresses, i.e. the same path
         * without its leading slash. Transfer mode only.
         */
        [[nodiscard]] static std::string keyOf(const std::string &virtualPath);

        /**
         * @brief Local scratch file backing one in-flight transfer in transfer mode.
         */
        [[nodiscard]] std::filesystem::path spoolPath() const;

        boost::asio::ip::tcp::socket _control;
        FtpServerConfig _config;

        /**
         * @brief Bucket-backed storage for the logged-in user, present only in transfer mode.
         *
         * Constructed at login rather than at startup because it carries that user's own bearer
         * token: every ESM call is made as the user who logged in, not as the server.
         */
        std::optional<Transfer::TransferStorage> _storage;
        boost::asio::streambuf _inputBuffer;

        bool _authenticated{false};
        bool _quitRequested{false};
        std::string _pendingUser;
        std::string _username;
        std::filesystem::path _homeDir;
        std::string _cwd{"/"};
        bool _binaryType{true};

        std::optional<boost::asio::ip::tcp::acceptor> _pasvAcceptor;
        std::optional<std::pair<std::string, unsigned short> > _portTarget;
    };

}// namespace Euclid::FTP
