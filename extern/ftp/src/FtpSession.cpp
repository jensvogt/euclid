#include <FtpSession.h>

// Euclid includes
#include <TransferMetrics.h>
#include <euclid/core/UuidUtils.h>

namespace Euclid::FTP {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    namespace {

        // Deletes a transfer's spool file however the command it belongs to returns - including
        // the several early-return error paths below, which would otherwise leak a full copy of
        // the transferred file into the spool directory on every failure.
        struct SpoolGuard {
            std::filesystem::path path;
            ~SpoolGuard() {
                if (path.empty()) return;
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        };


        std::string Trim(const std::string &s) {
            const auto begin = s.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return {};
            const auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(begin, end - begin + 1);
        }

        std::string ToUpper(std::string s) {
            std::ranges::transform(s, s.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }

        std::pair<std::string, std::string> SplitVerbArg(const std::string &line) {
            const auto pos = line.find(' ');
            if (pos == std::string::npos) return {line, {}};
            return {line.substr(0, pos), Trim(line.substr(pos + 1))};
        }

        std::tm ToUtcTm(const std::filesystem::file_time_type &ftime) {
            // std::chrono::clock_cast() (and file_clock's own to_sys()/from_sys()) would be the
            // "correct" C++20 way to do this, but Apple's libc++ doesn't implement either as of
            // this writing - diffing against each clock's own now() is the portable workaround
            // that's worked everywhere (libstdc++, MSVC STL, libc++) since long before C++20.
            const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
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
        std::ignore=_control.close(ec);
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

        // EAM is the only credential source: the transfer server definition decides which of
        // its users may log in, and success also decides which bucket this session sees and
        // under whose identity it reaches it.
        const Transfer::TransferAuthenticator authenticator(_config.transferServer);
        const auto identity = authenticator.Authenticate(_pendingUser, arg);
        if (!identity.has_value()) {
            sendReply(530, "Login incorrect");
            _pendingUser.clear();
            return;
        }

        _username = identity->userId;
        _homeDir = _config.rootDir;
        _storage.emplace(_config.transferServer.bucketErn, identity->token, _config.transferServer.region, _config.transferServer.accountId,
                         _config.transferServer.serverId, identity->userId);
        _cwd = "/";
        _authenticated = true;
        _pendingUser.clear();

        log_info << "FTP login: user=" << _username << ", server=" << _config.transferServer.serverId
                << ", bucket=" << _config.transferServer.bucketName;
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
        if (const std::string mode = ToUpper(Trim(arg)); mode == "A") {
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
        const auto [virtualPath, physicalPath] = resolve(arg);
        if (_storage) {
            // The bucket root always exists; anything else has to resolve to a directory.
            if (const auto entry = _storage->Stat(keyOf(virtualPath)); virtualPath != "/" && (!entry.has_value() || !entry->isDirectory)) {
                sendReply(550, "No such directory");
                return;
            }
        } else if (std::error_code fsEc; !std::filesystem::is_directory(physicalPath, fsEc)) {
            sendReply(550, "No such directory");
            return;
        }
        _cwd = virtualPath;
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

        const auto [virtualPath, physicalPath] = resolve(arg);
        if (_storage) {
            if (_storage->MakeDirectory(keyOf(virtualPath))) {
                sendReply("257 \"" + virtualPath + "\" directory created");
            } else {
                sendReply(550, "Could not create directory");
            }
            return;
        }

        std::error_code fsEc;
        if (std::filesystem::exists(physicalPath, fsEc)) {
            sendReply(550, "Directory already exists");
            return;
        }

        // Single-level create, like STOR's parent-must-already-exist behavior: MKD is not
        // expected to create intermediate directories on the client's behalf.
        if (std::filesystem::create_directory(physicalPath, fsEc)) {
            sendReply("257 \"" + virtualPath + "\" directory created");
        } else {
            sendReply(550, "Could not create directory: " + fsEc.message());
        }
    }

    void FtpSession::cmdRmd(const std::string &arg) {
        if (arg.empty()) {
            sendReply(501, "Syntax error in parameters");
            return;
        }

        const auto [virtualPath, physicalPath] = resolve(arg);
        if (_storage) {
            if (_storage->DeleteDirectory(keyOf(virtualPath))) {
                sendReply(250, "Directory removed");
            } else {
                sendReply(550, "Could not remove directory");
            }
            return;
        }

        std::error_code fsEc;
        if (!std::filesystem::is_directory(physicalPath, fsEc)) {
            sendReply(550, "No such directory");
            return;
        }

        // Refuse to remove the user's own home root - RMD is meant to remove a
        // subdirectory, not strand the session with no valid home directory underneath it.
        if (std::filesystem::equivalent(physicalPath, _homeDir, fsEc)) {
            sendReply(550, "Cannot remove home directory");
            return;
        }

        if (!std::filesystem::is_empty(physicalPath, fsEc)) {
            sendReply(550, "Directory not empty");
            return;
        }

        if (std::filesystem::remove(physicalPath, fsEc)) {
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
            std::ignore=acceptor.open(endpoint.protocol(), ec);
            if (!ec) std::ignore=acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
            if (!ec) std::ignore=acceptor.bind(endpoint, ec);
            if (!ec) std::ignore=acceptor.listen(1, ec);
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

        if (parts.size() != 6 || std::ranges::any_of(parts, [](const int v) { return v < 0 || v > 255; })) {
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
            std::ignore=_pasvAcceptor->accept(socket, ec);
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
            std::ignore=socket.connect(tcp::endpoint(asio::ip::make_address(address, ec), port), ec);
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
        const auto [virtualPath, physicalPath] = resolve(arg);
        std::error_code fsEc;
        if (!_storage && !std::filesystem::exists(physicalPath, fsEc)) {
            sendReply(450, "No such file or directory");
            return;
        }

        sendReply(150, "Opening data connection for directory listing");
        auto data = openDataConnection();
        if (!data) return;

        std::ostringstream out;
        if (_storage) {
            for (const auto &entry: _storage->List(keyOf(virtualPath))) {
                out << (entry.isDirectory ? 'd' : '-') << "rw-r--r-- 1 ftp ftp " << std::setw(10) << entry.size
                    << " Jan  1 00:00 " << entry.name << "\r\n";
            }
        } else if (std::filesystem::is_directory(physicalPath, fsEc)) {
            for (const auto &entry: std::filesystem::directory_iterator(physicalPath, fsEc)) {
                out << BuildListLine(entry.path(), entry.path().filename().string()) << "\r\n";
            }
        } else {
            out << BuildListLine(physicalPath, physicalPath.filename().string()) << "\r\n";
        }

        boost::system::error_code ec;
        asio::write(*data, asio::buffer(out.str()), ec);
        std::ignore=data->shutdown(tcp::socket::shutdown_both, ec);
        std::ignore=data->close(ec);

        sendReply(226, "Transfer complete");
    }

    void FtpSession::cmdRetr(const std::string &arg) {
        const auto [virtualPath, physicalPath] = resolve(arg);
        std::error_code fsEc;

        // In transfer mode the object is pulled into a spool file first, so the send loop below
        // is identical in both modes.
        std::filesystem::path sourcePath = physicalPath;
        if (_storage) {
            sourcePath = spoolPath();
            if (!_storage->Download(keyOf(virtualPath), sourcePath)) {
                sendReply(550, "File not found");
                return;
            }
        } else if (!std::filesystem::is_regular_file(physicalPath, fsEc)) {
            sendReply(550, "File not found");
            return;
        }
        const SpoolGuard guard{_storage.has_value() ? sourcePath : std::filesystem::path{}};

        std::ifstream in(sourcePath, std::ios::binary);
        if (!in.is_open()) {
            sendReply(550, "Could not open file");
            return;
        }

        const auto size = std::filesystem::file_size(sourcePath, fsEc);
        sendReply(150, "Opening BINARY mode data connection for " + physicalPath.filename().string() + " (" + std::to_string(size) + " bytes)");

        auto data = openDataConnection();
        if (!data) return;

        std::array<char, 65536> buffer{};
        boost::system::error_code ec;
        long sent = 0;
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto n = in.gcount();
            if (n <= 0) break;
            const auto written = asio::write(*data, asio::buffer(buffer.data(), static_cast<std::size_t>(n)), ec);
            sent += static_cast<long>(written);
            if (ec) break;
        }

        std::ignore=data->shutdown(tcp::socket::shutdown_both, ec);
        std::ignore=data->close(ec);

        // Recorded even for a transfer that broke off part-way: those bytes did leave the server,
        // and a download that stops half-way is exactly what a byte count is there to show. The
        // file itself only counts once it was served in full.
        Transfer::Metrics::BytesSent(_config.transferServer.serverId, sent);

        if (in.bad()) {
            sendReply(451, "Local error reading file");
        } else {
            Transfer::Metrics::FileSent(_config.transferServer.serverId);
            sendReply(226, "Transfer complete");
        }
    }

    void FtpSession::cmdStor(const std::string &arg) {
        const auto [virtualPath, physicalPath] = resolve(arg);

        // In transfer mode the bytes land in a spool file first and are stored as an object once
        // the client closes the data connection - the bucket takes whole objects, so there is
        // nothing to store until the transfer is actually complete.
        std::filesystem::path targetPath = physicalPath;
        if (_storage) {
            targetPath = spoolPath();
        } else if (std::error_code fsEc; !std::filesystem::is_directory(physicalPath.parent_path(), fsEc)) {
            sendReply(550, "Destination directory does not exist");
            return;
        }
        const SpoolGuard guard{_storage.has_value() ? targetPath : std::filesystem::path{}};

        std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            sendReply(550, "Could not create file");
            return;
        }

        sendReply(150, "Ok to send data");
        auto data = openDataConnection();
        if (!data) return;

        std::array<char, 65536> buffer{};
        boost::system::error_code ec;
        long received = 0;
        while (true) {
            const std::size_t n = data->read_some(asio::buffer(buffer), ec);
            if (ec == asio::error::eof) {
                ec.clear();
                break;
            }
            if (ec) break;
            out.write(buffer.data(), static_cast<std::streamsize>(n));
            received += static_cast<long>(n);
        }

        out.close();
        boost::system::error_code closeEc;
        std::ignore=data->close(closeEc);

        // Bytes are what arrived, whether or not the object is stored below; the file only counts
        // once the bucket actually holds it.
        Transfer::Metrics::BytesReceived(_config.transferServer.serverId, received);

        if (ec) {
            sendReply(426, "Connection closed; transfer aborted");
            return;
        }

        // A failed store has to be reported as a failed transfer: the client has sent every byte
        // and would otherwise take a 226 as confirmation that the file is safely stored.
        if (_storage && !_storage->Upload(keyOf(virtualPath), targetPath)) {
            log_error << "FTP upload to bucket failed, key: " << keyOf(virtualPath);
            sendReply(552, "Could not store file");
            return;
        }

        Transfer::Metrics::FileReceived(_config.transferServer.serverId);
        sendReply(226, "Transfer complete");
    }

    void FtpSession::cmdDele(const std::string &arg) {
        const auto [virtualPath, physicalPath] = resolve(arg);
        if (_storage) {
            if (_storage->Remove(keyOf(virtualPath))) {
                sendReply(250, "Delete successful");
            } else {
                sendReply(550, "File not found");
            }
            return;
        }

        std::error_code fsEc;
        if (!std::filesystem::is_regular_file(physicalPath, fsEc)) {
            sendReply(550, "File not found");
            return;
        }
        if (std::filesystem::remove(physicalPath, fsEc)) {
            sendReply(250, "Delete successful");
        } else {
            sendReply(550, "Delete failed: " + fsEc.message());
        }
    }

    void FtpSession::cmdSize(const std::string &arg) {
        const auto [virtualPath, physicalPath] = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::is_regular_file(physicalPath, fsEc)) {
            sendReply(550, "Could not get file size");
            return;
        }
        const auto size = std::filesystem::file_size(physicalPath, fsEc);
        if (fsEc) {
            sendReply(550, "Could not get file size");
            return;
        }
        sendReply(213, std::to_string(size));
    }

    void FtpSession::cmdMdtm(const std::string &arg) {
        const auto [virtualPath, physicalPath] = resolve(arg);
        std::error_code fsEc;
        if (!std::filesystem::exists(physicalPath, fsEc)) {
            sendReply(550, "Could not get modification time");
            return;
        }
        const auto ftime = std::filesystem::last_write_time(physicalPath, fsEc);
        if (fsEc) {
            sendReply(550, "Could not get modification time");
            return;
        }
        sendReply(213, FormatMdtmTimestamp(ftime));
    }

    std::filesystem::path FtpSession::spoolPath() const {
        // A UUID rather than the key: two sessions may transfer the same object at once, and a
        // key contains slashes that would otherwise have to be flattened into a filename.
        return _config.rootDir / ("spool-" + Core::UuidUtils::CreateRandomUuid());
    }

    std::string FtpSession::keyOf(const std::string &virtualPath) {
        std::string key = virtualPath;
        while (!key.empty() && key.front() == '/') key.erase(key.begin());
        return key;
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
            if (i + 1 < parts.size()) virtualPath += '/';
            physical /= parts[i];
        }

        return {virtualPath, physical};
    }

}// namespace Euclid::FTP
