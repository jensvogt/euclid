#include <FtpSession.h>

// C++ includes
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>

namespace Euclid::FTP {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    namespace {

        std::string Trim(const std::string &s) {
            const auto begin = s.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return {};
            const auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(begin, end - begin + 1);
        }

        std::string ToUpper(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }

        std::pair<std::string, std::string> SplitVerbArg(const std::string &line) {
            const auto pos = line.find(' ');
            if (pos == std::string::npos) return {line, {}};
            return {line.substr(0, pos), Trim(line.substr(pos + 1))};
        }

        std::tm ToUtcTm(const std::filesystem::file_time_type &ftime) {
            const auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
            const std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
            std::tm tmBuf{};
#ifdef _WIN32
            gmtime_s(&tmBuf, &tt);
#else
            gmtime_r(&tt, &tmBuf);
#endif
            return tmBuf;
        }

        // "Mon DD HH:MM" - close enough to `ls -l` for every FTP client this is meant to
        // support; the real thing's "switch to year instead of time after ~6 months"
        // heuristic isn't worth the extra code for a small server.
        std::string FormatListTimestamp(const std::filesystem::file_time_type &ftime) {
            const std::tm tmBuf = ToUtcTm(ftime);
            std::ostringstream oss;
            oss << std::put_time(&tmBuf, "%b %e %H:%M");
            return oss.str();
        }

        // MDTM's wire format: YYYYMMDDHHMMSS, always UTC.
        std::string FormatMdtmTimestamp(const std::filesystem::file_time_type &ftime) {
            const std::tm tmBuf = ToUtcTm(ftime);
            std::ostringstream oss;
            oss << std::put_time(&tmBuf, "%Y%m%d%H%M%S");
            return oss.str();
        }

        // One `ls -l`-style line. Owner/group/permissions are fixed placeholders - this
        // server has no concept of Unix uid/gid/mode, and clients only really parse the
        // leading type character, size and name out of a LIST line.
        std::string BuildListLine(const std::filesystem::path &path, const std::string &name) {
            std::error_code fsEc;
            const bool isDir = std::filesystem::is_directory(path, fsEc);
            const auto size = isDir ? 0 : std::filesystem::file_size(path, fsEc);
            const auto ftime = std::filesystem::last_write_time(path, fsEc);
            const std::string timestamp = fsEc ? "Jan  1 00:00" : FormatListTimestamp(ftime);

            std::ostringstream oss;
            oss << (isDir ? 'd' : '-') << "rw-r--r-- 1 ftp ftp " << std::setw(10) << size << " " << timestamp << " " << name;
            return oss.str();
        }

    }// namespace

    FtpSession::FtpSession(tcp::socket socket, FtpServerConfig config) : _control(std::move(socket)), _config(std::move(config)) {}

    void FtpSession::Run() {
        try {
            sendReply(220, "euclid FTP service ready");
            while (!_quitRequested) {
                const auto line = readLine();
                if (!line) break;
                const std::string trimmed = Trim(*line);
                if (trimmed.empty()) continue;
                const auto [verb, arg] = SplitVerbArg(trimmed);
                handleCommand(ToUpper(verb), arg);
            }
        } catch (const std::exception &ex) {
            log_debug << "FTP session for '" << (_username.empty() ? "<anonymous>" : _username) << "' ended: " << ex.what();
        }
        boost::system::error_code ec;
        _control.close(ec);
    }

    void FtpSession::sendReply(const std::string &line) {
        boost::system::error_code ec;
        asio::write(_control, asio::buffer(line + "\r\n"), ec);
        if (ec) {
            log_debug << "FTP write failed: " << ec.message();
        }
    }

    void FtpSession::sendReply(const int code, const std::string &message) {
        sendReply(std::to_string(code) + " " + message);
    }

    std::optional<std::string> FtpSession::readLine() {
        boost::system::error_code ec;
        asio::read_until(_control, _inputBuffer, '\n', ec);
        if (ec) return std::nullopt;

        std::istream is(&_inputBuffer);
        std::string line;
        std::getline(is, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }

    void FtpSession::handleCommand(const std::string &verb, const std::string &arg) {
        // Allowed before authentication.
        if (verb == "USER") return cmdUser(arg);
        if (verb == "PASS") return cmdPass(arg);
        if (verb == "QUIT") return cmdQuit();
        if (verb == "NOOP") return cmdNoop();
        if (verb == "SYST") return cmdSyst();
        if (verb == "FEAT") return cmdFeat();

        if (!_authenticated) {
            sendReply(530, "Not logged in");
            return;
        }

        if (verb == "TYPE") return cmdType(arg);
        if (verb == "PWD" || verb == "XPWD") return cmdPwd();
        if (verb == "CWD" || verb == "XCWD") return cmdCwd(arg);
        if (verb == "CDUP" || verb == "XCUP") return cmdCdup();
        if (verb == "PASV") return cmdPasv();
        if (verb == "PORT") return cmdPort(arg);
        if (verb == "MKD" || verb == "XMKD") return cmdMkd(arg);
        if (verb == "RMD" || verb == "XRMD") return cmdRmd(arg);
        if (verb == "LIST" || verb == "NLST") return cmdList(arg);
        if (verb == "RETR") return cmdRetr(arg);
        if (verb == "STOR") return cmdStor(arg);
        if (verb == "DELE") return cmdDele(arg);
        if (verb == "SIZE") return cmdSize(arg);
        if (verb == "MDTM") return cmdMdtm(arg);

        sendReply(500, "Unknown command");
    }

    void FtpSession::cmdUser(const std::string &arg) {
        _pendingUser = arg;
        _authenticated = false;
        sendReply(331, "Password required for " + arg);
    }

    void FtpSession::cmdPass(const std::string &arg) {
        if (_pendingUser.empty()) {
            sendReply(503, "Login with USER first");
            return;
        }

        const auto it = _config.users.find(_pendingUser);
        if (it == _config.users.end() || it->second.password != arg) {
            sendReply(530, "Login incorrect");
            _pendingUser.clear();
            return;
        }

        std::error_code fsEc;
        auto home = std::filesystem::weakly_canonical(_config.rootDir / it->second.home, fsEc);
        if (fsEc) {
            home = _config.rootDir / it->second.home;
        }

        _username = _pendingUser;
        _homeDir = home;
        _cwd = "/";
        _authenticated = true;
        _pendingUser.clear();

        log_info << "FTP login: user=" << _username;
        sendReply(230, "Login successful");
    }

    void FtpSession::cmdQuit() {
        sendReply(221, "Goodbye");
        _quitRequested = true;
    }

    void FtpSession::cmdSyst() {
        sendReply(215, "UNIX Type: L8");
    }

    void FtpSession::cmdFeat() {
        sendReply("211-Features:");
        sendReply(" PASV");
        sendReply(" SIZE");
        sendReply(" MDTM");
        sendReply("211 End");
    }

    void FtpSession::cmdNoop() {
        sendReply(200, "NOOP ok");
    }

    void FtpSession::cmdType(const std::string &arg) {
        const std::string mode = ToUpper(Trim(arg));
        if (mode == "A") {
            _binaryType = false;
            sendReply(200, "Type set to A");
        } else if (mode == "I") {
            _binaryType = true;
            sendReply(200, "Type set to I");
        } else {
            sendReply(504, "Type not supported");
        }
    }

    void FtpSession::cmdPwd() {
        sendReply("257 \"" + _cwd + "\" is the current directory");
    }

    void FtpSession::cmdCwd(const std::string &arg) {
        const auto resolved = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::is_directory(resolved.physicalPath, fsEc)) {
            sendReply(550, "No such directory");
            return;
        }
        _cwd = resolved.virtualPath;
        sendReply(250, "Directory changed to " + _cwd);
    }

    void FtpSession::cmdCdup() {
        cmdCwd("..");
    }

    void FtpSession::cmdMkd(const std::string &arg) {
        if (arg.empty()) {
            sendReply(501, "Syntax error in parameters");
            return;
        }

        const auto resolved = resolve(arg);
        std::error_code fsEc;
        if (std::filesystem::exists(resolved.physicalPath, fsEc)) {
            sendReply(550, "Directory already exists");
            return;
        }

        // Single-level create, like STOR's parent-must-already-exist behavior: MKD is not
        // expected to create intermediate directories on the client's behalf.
        if (std::filesystem::create_directory(resolved.physicalPath, fsEc)) {
            sendReply("257 \"" + resolved.virtualPath + "\" directory created");
        } else {
            sendReply(550, "Could not create directory: " + fsEc.message());
        }
    }

    void FtpSession::cmdRmd(const std::string &arg) {
        if (arg.empty()) {
            sendReply(501, "Syntax error in parameters");
            return;
        }

        const auto resolved = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::is_directory(resolved.physicalPath, fsEc)) {
            sendReply(550, "No such directory");
            return;
        }

        // Refuse to remove the user's own home root - RMD is meant to remove a
        // subdirectory, not strand the session with no valid home directory underneath it.
        if (std::filesystem::equivalent(resolved.physicalPath, _homeDir, fsEc)) {
            sendReply(550, "Cannot remove home directory");
            return;
        }

        if (!std::filesystem::is_empty(resolved.physicalPath, fsEc)) {
            sendReply(550, "Directory not empty");
            return;
        }

        if (std::filesystem::remove(resolved.physicalPath, fsEc)) {
            sendReply(250, "Directory removed");
        } else {
            sendReply(550, "Could not remove directory: " + fsEc.message());
        }
    }

    void FtpSession::cmdPasv() {
        _portTarget.reset();
        _pasvAcceptor.reset();

        for (unsigned short port = _config.pasvMin; port <= _config.pasvMax; ++port) {
            boost::system::error_code ec;
            tcp::acceptor acceptor(_control.get_executor());
            const tcp::endpoint endpoint(tcp::v4(), port);
            acceptor.open(endpoint.protocol(), ec);
            if (!ec) acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
            if (!ec) acceptor.bind(endpoint, ec);
            if (!ec) acceptor.listen(1, ec);
            if (!ec) {
                _pasvAcceptor = std::move(acceptor);
                break;
            }
        }

        if (!_pasvAcceptor) {
            sendReply(425, "Cannot open passive data connection - no free port in configured range");
            return;
        }

        boost::system::error_code ec;
        const std::string address = _config.advertisedAddress.empty() ? _control.local_endpoint(ec).address().to_string() : _config.advertisedAddress;
        const unsigned short port = _pasvAcceptor->local_endpoint().port();

        const auto v4 = asio::ip::make_address_v4(address, ec);
        if (ec) {
            sendReply(425, "Cannot determine address for passive mode");
            _pasvAcceptor.reset();
            return;
        }
        const auto bytes = v4.to_bytes();

        std::ostringstream oss;
        oss << "Entering Passive Mode (" << static_cast<int>(bytes[0]) << "," << static_cast<int>(bytes[1]) << ","
            << static_cast<int>(bytes[2]) << "," << static_cast<int>(bytes[3]) << "," << (port >> 8) << "," << (port & 0xFF) << ")";
        sendReply(227, oss.str());
    }

    void FtpSession::cmdPort(const std::string &arg) {
        _pasvAcceptor.reset();
        _portTarget.reset();

        std::vector<int> parts;
        std::istringstream iss(arg);
        std::string token;
        while (std::getline(iss, token, ',')) {
            try {
                parts.push_back(std::stoi(Trim(token)));
            } catch (...) {
                parts.clear();
                break;
            }
        }

        if (parts.size() != 6 || std::any_of(parts.begin(), parts.end(), [](const int v) { return v < 0 || v > 255; })) {
            sendReply(501, "Syntax error in PORT command");
            return;
        }

        std::ostringstream addr;
        addr << parts[0] << "." << parts[1] << "." << parts[2] << "." << parts[3];
        const auto port = static_cast<unsigned short>((parts[4] << 8) | parts[5]);

        _portTarget = {addr.str(), port};
        sendReply(200, "PORT command successful");
    }

    std::optional<tcp::socket> FtpSession::openDataConnection() {
        boost::system::error_code ec;

        if (_pasvAcceptor) {
            tcp::socket socket(_control.get_executor());
            _pasvAcceptor->accept(socket, ec);
            _pasvAcceptor.reset();
            if (ec) {
                sendReply(425, "Failed to establish passive data connection");
                return std::nullopt;
            }
            return socket;
        }

        if (_portTarget) {
            tcp::socket socket(_control.get_executor());
            const auto [address, port] = *_portTarget;
            _portTarget.reset();
            socket.connect(tcp::endpoint(asio::ip::make_address(address, ec), port), ec);
            if (ec) {
                sendReply(425, "Failed to establish active data connection: " + ec.message());
                return std::nullopt;
            }
            return socket;
        }

        sendReply(425, "Use PASV or PORT first");
        return std::nullopt;
    }

    void FtpSession::cmdList(const std::string &arg) {
        const auto resolved = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::exists(resolved.physicalPath, fsEc)) {
            sendReply(450, "No such file or directory");
            return;
        }

        sendReply(150, "Opening data connection for directory listing");
        auto data = openDataConnection();
        if (!data) return;

        std::ostringstream out;
        if (std::filesystem::is_directory(resolved.physicalPath, fsEc)) {
            for (const auto &entry: std::filesystem::directory_iterator(resolved.physicalPath, fsEc)) {
                out << BuildListLine(entry.path(), entry.path().filename().string()) << "\r\n";
            }
        } else {
            out << BuildListLine(resolved.physicalPath, resolved.physicalPath.filename().string()) << "\r\n";
        }

        boost::system::error_code ec;
        asio::write(*data, asio::buffer(out.str()), ec);
        data->shutdown(tcp::socket::shutdown_both, ec);
        data->close(ec);

        sendReply(226, "Transfer complete");
    }

    void FtpSession::cmdRetr(const std::string &arg) {
        const auto resolved = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::is_regular_file(resolved.physicalPath, fsEc)) {
            sendReply(550, "File not found");
            return;
        }

        std::ifstream in(resolved.physicalPath, std::ios::binary);
        if (!in.is_open()) {
            sendReply(550, "Could not open file");
            return;
        }

        const auto size = std::filesystem::file_size(resolved.physicalPath, fsEc);
        sendReply(150, "Opening BINARY mode data connection for " + resolved.physicalPath.filename().string() + " (" + std::to_string(size) + " bytes)");

        auto data = openDataConnection();
        if (!data) return;

        std::array<char, 65536> buffer{};
        boost::system::error_code ec;
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto n = in.gcount();
            if (n <= 0) break;
            asio::write(*data, asio::buffer(buffer.data(), static_cast<std::size_t>(n)), ec);
            if (ec) break;
        }

        data->shutdown(tcp::socket::shutdown_both, ec);
        data->close(ec);

        if (in.bad()) {
            sendReply(451, "Local error reading file");
        } else {
            sendReply(226, "Transfer complete");
        }
    }

    void FtpSession::cmdStor(const std::string &arg) {
        const auto resolved = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::is_directory(resolved.physicalPath.parent_path(), fsEc)) {
            sendReply(550, "Destination directory does not exist");
            return;
        }

        std::ofstream out(resolved.physicalPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            sendReply(550, "Could not create file");
            return;
        }

        sendReply(150, "Ok to send data");
        auto data = openDataConnection();
        if (!data) return;

        std::array<char, 65536> buffer{};
        boost::system::error_code ec;
        while (true) {
            const std::size_t n = data->read_some(asio::buffer(buffer), ec);
            if (ec == asio::error::eof) {
                ec.clear();
                break;
            }
            if (ec) break;
            out.write(buffer.data(), static_cast<std::streamsize>(n));
        }

        out.close();
        boost::system::error_code closeEc;
        data->close(closeEc);

        if (ec) {
            sendReply(426, "Connection closed; transfer aborted");
        } else {
            sendReply(226, "Transfer complete");
        }
    }

    void FtpSession::cmdDele(const std::string &arg) {
        const auto resolved = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::is_regular_file(resolved.physicalPath, fsEc)) {
            sendReply(550, "File not found");
            return;
        }
        if (std::filesystem::remove(resolved.physicalPath, fsEc)) {
            sendReply(250, "Delete successful");
        } else {
            sendReply(550, "Delete failed: " + fsEc.message());
        }
    }

    void FtpSession::cmdSize(const std::string &arg) {
        const auto resolved = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::is_regular_file(resolved.physicalPath, fsEc)) {
            sendReply(550, "Could not get file size");
            return;
        }
        const auto size = std::filesystem::file_size(resolved.physicalPath, fsEc);
        if (fsEc) {
            sendReply(550, "Could not get file size");
            return;
        }
        sendReply(213, std::to_string(size));
    }

    void FtpSession::cmdMdtm(const std::string &arg) {
        const auto resolved = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::exists(resolved.physicalPath, fsEc)) {
            sendReply(550, "Could not get modification time");
            return;
        }
        const auto ftime = std::filesystem::last_write_time(resolved.physicalPath, fsEc);
        if (fsEc) {
            sendReply(550, "Could not get modification time");
            return;
        }
        sendReply(213, FormatMdtmTimestamp(ftime));
    }

    FtpSession::ResolvedPath FtpSession::resolve(const std::string &arg) const {
        std::string target = arg;
        if (target.empty()) {
            target = _cwd;
        } else if (target.front() != '/') {
            target = (_cwd == "/" ? "/" : _cwd + "/") + target;
        }

        // ".." is clamped at the virtual root instead of rejected: popping an already-empty
        // `parts` is simply a no-op, so the loop below can never walk `physical` above
        // _homeDir - there is no path string this can ever produce that isn't itself
        // rooted at (or below) _homeDir. Caveat: this is confinement by construction of
        // the path, not a defense against a symlink under the home directory pointing
        // back out of it - not a concern for the small, single-purpose deployments this
        // server targets, but worth knowing if that assumption ever changes.
        std::vector<std::string> parts;
        std::istringstream iss(target);
        std::string segment;
        while (std::getline(iss, segment, '/')) {
            if (segment.empty() || segment == ".") continue;
            if (segment == "..") {
                if (!parts.empty()) parts.pop_back();
                continue;
            }
            parts.push_back(segment);
        }

        std::string virtualPath = "/";
        std::filesystem::path physical = _homeDir;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            virtualPath += parts[i];
            if (i + 1 < parts.size()) virtualPath += "/";
            physical /= parts[i];
        }

        return {virtualPath, physical};
    }

}// namespace Euclid::FTP
