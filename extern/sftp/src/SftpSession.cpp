#include <SftpSession.h>

// C++ includes
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

// Euclid includes
#include <TransferAuthenticator.h>
#include <TransferMetrics.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/UuidUtils.h>

namespace Euclid::SFTP {

    namespace {

        // A single READ is answered from one heap buffer, so the client's requested length is
        // capped rather than trusted. 256 KiB is comfortably above what any client asks for in
        // practice (OpenSSH uses 32 KiB), and bounds what a malformed request can allocate.
        constexpr std::uint32_t kMaxReadLength = 256 * 1024;

        // POSIX mode type bits, which SFTP version 3 expects to be part of "permissions".
        // Spelled out here rather than taken from <sys/stat.h> so the module still compiles on
        // Windows, where those macros do not exist.
        constexpr std::uint32_t kModeDirectory = 0040000;
        constexpr std::uint32_t kModeRegular = 0100000;
        constexpr std::uint32_t kModeSymlink = 0120000;

        // std::filesystem::file_time_type has its own epoch. std::chrono::clock_cast (C++20) and
        // file_clock::to_sys are the "correct" conversions, but Apple's libc++ implements
        // neither, so the FTP module next door diffs against each clock's own now() instead -
        // the same workaround is used here, for the same reason.
        std::time_t toTimeT(const std::filesystem::file_time_type &ftime) {
            const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(sctp);
        }

        std::string formatListTimestamp(const std::filesystem::file_time_type &ftime) {
            const std::time_t tt = toTimeT(ftime);
            std::tm tmBuf{};
#ifdef _WIN32
            gmtime_s(&tmBuf, &tt);
#else
            gmtime_r(&tt, &tmBuf);
#endif
            std::ostringstream oss;
            oss << std::put_time(&tmBuf, "%b %e %H:%M");
            return oss.str();
        }

        // The bucket-backed counterpart to fillAttributes(): a TransferEntry carries only what
        // ESM knows about an object, so mode bits are synthesised rather than read.
        void fillRemoteAttributes(const Transfer::TransferEntry &entry, sftp_attributes_struct &attr) {
            attr.flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
            attr.type = entry.isDirectory ? SSH_FILEXFER_TYPE_DIRECTORY : SSH_FILEXFER_TYPE_REGULAR;
            attr.size = static_cast<std::uint64_t>(entry.size);
            attr.permissions = (entry.isDirectory ? kModeDirectory | 0755u : kModeRegular | 0644u);
            const auto mtime = static_cast<std::uint32_t>(std::chrono::system_clock::to_time_t(entry.modified));
            attr.mtime = mtime;
            attr.atime = mtime;
        }

        std::string remoteLongName(const Transfer::TransferEntry &entry) {
            std::ostringstream oss;
            oss << (entry.isDirectory ? 'd' : '-') << "rw-r--r-- 1 sftp sftp " << std::setw(10) << entry.size
                << " Jan  1 00:00 " << entry.name;
            return oss.str();
        }

    }// namespace

    SftpSession::SftpSession(ssh_session session, SftpServerConfig config) : _session(session), _config(std::move(config)) {}

    SftpSession::~SftpSession() {
        if (_sftp != nullptr) {
            sftp_server_free(_sftp);
        }
        if (_channel != nullptr) {
            ssh_channel_close(_channel);
            ssh_channel_free(_channel);
        }
        if (_session != nullptr) {
            ssh_disconnect(_session);
            ssh_free(_session);
        }
    }

    void SftpSession::Run() {

        if (ssh_handle_key_exchange(_session) != SSH_OK) {
            log_warning << "SFTP key exchange failed: " << ssh_get_error(_session);
            return;
        }

        if (!authenticate()) {
            return;
        }

        if (!openSftpChannel()) {
            log_warning << "SFTP client did not open an sftp channel, user=" << _username;
            return;
        }

        _sftp = sftp_server_new(_session, _channel);
        if (_sftp == nullptr) {
            log_error << "SFTP could not create server session, user=" << _username;
            return;
        }

        // Answers the client's SSH_FXP_INIT with SSH_FXP_VERSION. Deprecated in libssh 0.11 in
        // favour of the callback-driven server API, but there is no replacement for this
        // message-loop style of server, and the function is still exported and functional.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        const auto initialized = sftp_server_init(_sftp);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
        if (initialized != SSH_OK) {
            log_error << "SFTP server init failed, user=" << _username;
            return;
        }

        messageLoop();
        log_info << "SFTP session ended: user=" << _username;
    }

    bool SftpSession::authenticate() {

        while (true) {
            ssh_message message = ssh_message_get(_session);
            if (message == nullptr) {
                // Client disconnected, or the transport broke.
                return false;
            }

            bool authenticated = false;
            if (ssh_message_type(message) == SSH_REQUEST_AUTH && ssh_message_subtype(message) == SSH_AUTH_METHOD_PASSWORD) {

                // Deprecated alongside the whole message-based server API, which libssh wants
                // replaced by auth_password_function callbacks. Kept deliberately: this server
                // is built on the message loop throughout (see sftp_server_init() in Run()),
                // and half-migrating just the authentication would be worse than either end
                // state. Both functions remain exported and functional.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
                const auto *passwordPtr = ssh_message_auth_password(message);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
                const auto *userPtr = ssh_message_auth_user(message);
                const std::string user = userPtr != nullptr ? userPtr : "";
                const std::string password = passwordPtr != nullptr ? passwordPtr : "";

                // EAM is the only credential source: the transfer server definition decides
                // which of its users may log in, and success also decides which bucket this
                // session sees and under whose identity it reaches it.
                const Transfer::TransferAuthenticator authenticator(_config.transferServer);
                if (const auto identity = authenticator.Authenticate(user, password)) {
                    _username = identity->userId;
                    _homeDir = _config.rootDir;
                    _storage.emplace(_config.transferServer.bucketErn, identity->token, _config.transferServer.region, _config.transferServer.accountId,
                                     _config.transferServer.serverId, identity->userId);
                    authenticated = true;

                    ssh_message_auth_reply_success(message, 0);
                    log_info << "SFTP login: user=" << _username << ", server=" << _config.transferServer.serverId
                            << ", bucket=" << _config.transferServer.bucketName;
                } else {
                    log_warning << "SFTP login failed: user=" << user;
                    ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                }
            } else {
                // Advertise password as the only method, which is what makes a client that
                // opened with "none" or a public key fall back to asking for one.
                ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
                ssh_message_reply_default(message);
            }

            ssh_message_free(message);
            if (authenticated) return true;
        }
    }

    bool SftpSession::openSftpChannel() {

        while (_channel == nullptr) {
            ssh_message message = ssh_message_get(_session);
            if (message == nullptr) return false;

            if (ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN && ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
                _channel = ssh_message_channel_request_open_reply_accept(message);
            } else {
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
        }

        while (true) {
            ssh_message message = ssh_message_get(_session);
            if (message == nullptr) return false;

            bool opened = false;
            if (ssh_message_type(message) == SSH_REQUEST_CHANNEL && ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SUBSYSTEM) {

                const auto *subsystem = ssh_message_channel_request_subsystem(message);
                if (subsystem != nullptr && std::string(subsystem) == "sftp") {
                    ssh_message_channel_request_reply_success(message);
                    opened = true;
                } else {
                    ssh_message_reply_default(message);
                }
            } else {
                // Everything else on the channel - notably "shell" and "exec" - is refused:
                // this is a file transfer service, not a login host.
                ssh_message_reply_default(message);
            }

            ssh_message_free(message);
            if (opened) return true;
        }
    }

    void SftpSession::messageLoop() {

        while (true) {
            sftp_client_message message = sftp_get_client_message(_sftp);
            if (message == nullptr) break;

            switch (sftp_client_message_get_type(message)) {
                case SSH_FXP_REALPATH:
                    handleRealpath(message);
                    break;
                case SSH_FXP_OPENDIR:
                    handleOpenDir(message);
                    break;
                case SSH_FXP_READDIR:
                    handleReadDir(message);
                    break;
                case SSH_FXP_CLOSE:
                    handleClose(message);
                    break;
                case SSH_FXP_OPEN:
                    handleOpen(message);
                    break;
                case SSH_FXP_READ:
                    handleRead(message);
                    break;
                case SSH_FXP_WRITE:
                    handleWrite(message);
                    break;
                case SSH_FXP_STAT:
                    handleStat(message, true);
                    break;
                case SSH_FXP_LSTAT:
                    handleStat(message, false);
                    break;
                case SSH_FXP_FSTAT:
                    handleFstat(message);
                    break;
                case SSH_FXP_MKDIR:
                    handleMkdir(message);
                    break;
                case SSH_FXP_RMDIR:
                    handleRmdir(message);
                    break;
                case SSH_FXP_REMOVE:
                    handleRemove(message);
                    break;
                case SSH_FXP_RENAME:
                    handleRename(message);
                    break;
                case SSH_FXP_SETSTAT:
                case SSH_FXP_FSETSTAT:
                    handleSetstat(message);
                    break;
                default:
                    sftp_reply_status(message, SSH_FX_OP_UNSUPPORTED, "Operation not supported");
                    break;
            }

            sftp_client_message_free(message);
        }
    }

    void SftpSession::handleRealpath(sftp_client_message msg) {

        const auto *filename = sftp_client_message_get_filename(msg);
        const auto resolved = resolve(filename != nullptr ? filename : "");

        // The attributes are ignored by every client for REALPATH, but the field is not
        // optional in the reply, so a zeroed struct is sent along.
        sftp_attributes_struct attr{};
        sftp_reply_name(msg, resolved.virtualPath.c_str(), &attr);
    }

    void SftpSession::handleOpenDir(sftp_client_message msg) {

        const auto *filename = sftp_client_message_get_filename(msg);
        const auto resolved = resolve(filename != nullptr ? filename : "");

        auto handle = std::make_unique<Handle>();
        handle->isDirectory = true;
        handle->path = resolved.physicalPath;
        handle->virtualPath = resolved.virtualPath;
        handle->key = keyOf(resolved.virtualPath);

        std::error_code fsEc;
        if (_storage) {
            // A bucket has no directory to open, so there is nothing to fail on here: an
            // unknown prefix simply lists empty, which is also what it looks like in the bucket.
            handle->remoteEntries = _storage->List(handle->key);
        } else {
            if (!std::filesystem::is_directory(resolved.physicalPath, fsEc)) {
                sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "No such directory");
                return;
            }
        }

        if (!_storage) {
            for (std::filesystem::directory_iterator it(resolved.physicalPath, fsEc), end; it != end && !fsEc; it.increment(fsEc)) {
                handle->entries.push_back(*it);
            }
            if (fsEc) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "Could not read directory");
                return;
            }
        }

        ssh_string handleString = sftp_handle_alloc(_sftp, handle.get());
        if (handleString == nullptr) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "Could not allocate handle");
            return;
        }

        _handles.push_back(std::move(handle));
        sftp_reply_handle(msg, handleString);
        ssh_string_free(handleString);
    }

    void SftpSession::handleReadDir(sftp_client_message msg) {

        Handle *handle = handleOf(msg);
        if (handle == nullptr || !handle->isDirectory) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        const auto total = _storage ? handle->remoteEntries.size() : handle->entries.size();
        if (handle->nextEntry >= total) {
            sftp_reply_status(msg, SSH_FX_EOF, "End of directory");
            return;
        }

        if (_storage) {
            int remoteAdded = 0;
            while (handle->nextEntry < handle->remoteEntries.size()) {
                const auto &entry = handle->remoteEntries[handle->nextEntry++];

                sftp_attributes_struct attr{};
                fillRemoteAttributes(entry, attr);
                const auto longname = remoteLongName(entry);
                sftp_reply_names_add(msg, entry.name.c_str(), longname.c_str(), &attr);
                ++remoteAdded;
            }
            if (remoteAdded == 0) {
                sftp_reply_status(msg, SSH_FX_EOF, "End of directory");
                return;
            }
            sftp_reply_names(msg);
            return;
        }

        // The whole remaining listing goes out in one reply. Fine for the directory sizes this
        // server is meant to serve; a client that cannot take it all at once simply issues
        // another READDIR, which then finds the handle drained and gets EOF.
        int added = 0;
        while (handle->nextEntry < handle->entries.size()) {
            const auto &entry = handle->entries[handle->nextEntry++];
            const auto name = entry.path().filename().string();

            sftp_attributes_struct attr{};
            if (!fillAttributes(entry.path(), attr)) continue;

            const auto longname = longName(name, entry.path());
            sftp_reply_names_add(msg, name.c_str(), longname.c_str(), &attr);
            ++added;
        }

        if (added == 0) {
            sftp_reply_status(msg, SSH_FX_EOF, "End of directory");
            return;
        }
        sftp_reply_names(msg);
    }

    void SftpSession::handleClose(sftp_client_message msg) {

        Handle *handle = handleOf(msg);
        if (handle == nullptr) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        if (handle->stream.is_open()) {
            handle->stream.close();
        }

        // The upload happens here rather than on every WRITE because the bucket stores whole
        // objects: only once the client closes the handle is the file complete, and only then is
        // there something worth storing. A failure has to be reported now, since CLOSE is the
        // client's last chance to learn the transfer did not succeed.
        bool closeFailed = false;
        if (_storage && !handle->isDirectory && handle->dirty) {
            if (!_storage->Upload(handle->key, handle->path)) {
                log_error << "SFTP upload to bucket failed, key: " << handle->key;
                closeFailed = true;
            } else {
                Transfer::Metrics::FileReceived(_config.transferServer.serverId);
            }
        }
        if (_storage && !handle->isDirectory) {
            std::error_code spoolEc;
            std::filesystem::remove(handle->path, spoolEc);
        }

        sftp_handle_remove(_sftp, handle);
        std::erase_if(_handles, [handle](const auto &owned) { return owned.get() == handle; });

        if (closeFailed) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "Could not store object");
            return;
        }
        sftp_reply_status(msg, SSH_FX_OK, "Closed");
    }

    void SftpSession::handleOpen(sftp_client_message msg) {

        const auto *filename = sftp_client_message_get_filename(msg);
        const auto resolved = resolve(filename != nullptr ? filename : "");
        const auto flags = sftp_client_message_get_flags(msg);

        if (_storage) {
            const auto key = keyOf(resolved.virtualPath);
            const auto existing = _storage->Stat(key);

            if (existing.has_value() && (flags & SSH_FXF_EXCL)) {
                sftp_reply_status(msg, SSH_FX_FILE_ALREADY_EXISTS, "File already exists");
                return;
            }
            if (!existing.has_value() && !(flags & SSH_FXF_CREAT)) {
                sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "No such file");
                return;
            }

            auto handle = std::make_unique<Handle>();
            handle->isDirectory = false;
            handle->key = key;
            handle->path = spoolPathFor(key);
            handle->dirty = (flags & SSH_FXF_WRITE) != 0;

            // An existing object is pulled down first unless the open truncates it anyway, so
            // that a partial write leaves the untouched part of the file intact.
            const bool needsExisting = existing.has_value() && !(flags & SSH_FXF_TRUNC);
            if (needsExisting) {
                if (!_storage->Download(key, handle->path)) {
                    sftp_reply_status(msg, SSH_FX_FAILURE, "Could not read object");
                    return;
                }
                // One file out of the bucket, but only for a handle the client can read from:
                // this branch also pre-loads an object that is about to be written into, and
                // nothing is being sent anywhere in that case. Reported here rather than on CLOSE
                // because this is where the object is actually fetched - a client that opens a
                // file and reads none of it has still cost the server the download.
                if (flags & SSH_FXF_READ) Transfer::Metrics::FileSent(_config.transferServer.serverId);
            } else {
                std::ofstream create(handle->path, std::ios::binary | std::ios::trunc);
                if (!create) {
                    sftp_reply_status(msg, SSH_FX_FAILURE, "Could not create spool file");
                    return;
                }
            }

            handle->stream.open(handle->path, std::ios::binary | std::ios::in | std::ios::out);
            if (!handle->stream.is_open()) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Could not open spool file");
                return;
            }

            ssh_string remoteHandle = sftp_handle_alloc(_sftp, handle.get());
            if (remoteHandle == nullptr) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Could not allocate handle");
                return;
            }
            _handles.push_back(std::move(handle));
            sftp_reply_handle(msg, remoteHandle);
            ssh_string_free(remoteHandle);
            return;
        }

        std::error_code fsEc;
        const bool exists = std::filesystem::exists(resolved.physicalPath, fsEc);

        if (exists && (flags & SSH_FXF_EXCL)) {
            sftp_reply_status(msg, SSH_FX_FILE_ALREADY_EXISTS, "File already exists");
            return;
        }
        if (!exists) {
            if (!(flags & SSH_FXF_CREAT)) {
                sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "No such file");
                return;
            }
            // Created up front so the read/write open below can use in|out ("r+"), which needs
            // the file to already exist but preserves its contents when TRUNC is not requested.
            if (std::ofstream created(resolved.physicalPath, std::ios::binary); !created) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "Could not create file");
                return;
            }
        }

        std::ios::openmode mode = std::ios::binary;
        if (flags & SSH_FXF_WRITE) {
            mode |= std::ios::in | std::ios::out;
            if (flags & SSH_FXF_TRUNC) mode |= std::ios::trunc;
            if (flags & SSH_FXF_APPEND) mode |= std::ios::app;
        } else {
            // A read-only open stays read-only, so files the server may not write can still
            // be downloaded.
            mode |= std::ios::in;
        }

        auto handle = std::make_unique<Handle>();
        handle->isDirectory = false;
        handle->path = resolved.physicalPath;
        handle->stream.open(resolved.physicalPath, mode);
        if (!handle->stream.is_open()) {
            sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "Could not open file");
            return;
        }

        ssh_string handleString = sftp_handle_alloc(_sftp, handle.get());
        if (handleString == nullptr) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "Could not allocate handle");
            return;
        }

        _handles.push_back(std::move(handle));
        sftp_reply_handle(msg, handleString);
        ssh_string_free(handleString);
    }

    void SftpSession::handleRead(sftp_client_message msg) {

        Handle *handle = handleOf(msg);
        if (handle == nullptr || handle->isDirectory || !handle->stream.is_open()) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        const auto length = std::min(msg->len, kMaxReadLength);

        handle->stream.clear();
        handle->stream.seekg(static_cast<std::streamoff>(msg->offset), std::ios::beg);
        if (!handle->stream) {
            sftp_reply_status(msg, SSH_FX_EOF, "End of file");
            return;
        }

        std::vector<char> buffer(length);
        handle->stream.read(buffer.data(), static_cast<std::streamsize>(length));
        const auto read = handle->stream.gcount();

        if (read <= 0) {
            sftp_reply_status(msg, SSH_FX_EOF, "End of file");
            return;
        }
        Transfer::Metrics::BytesSent(_config.transferServer.serverId, static_cast<long>(read));
        sftp_reply_data(msg, buffer.data(), static_cast<int>(read));
    }

    void SftpSession::handleWrite(sftp_client_message msg) {

        Handle *handle = handleOf(msg);
        if (handle == nullptr || handle->isDirectory || !handle->stream.is_open()) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        // The payload is read from the ssh_string rather than from
        // sftp_client_message_get_data(), which returns a C string and would stop at the first
        // embedded NUL - fatal for binary uploads.
        if (msg->data == nullptr) {
            sftp_reply_status(msg, SSH_FX_BAD_MESSAGE, "Missing payload");
            return;
        }
        const auto *data = static_cast<const char *>(ssh_string_data(msg->data));
        const auto length = ssh_string_len(msg->data);

        handle->dirty = true;
        handle->stream.clear();
        handle->stream.seekp(static_cast<std::streamoff>(msg->offset), std::ios::beg);
        handle->stream.write(data, static_cast<std::streamsize>(length));
        if (!handle->stream) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "Write failed");
            return;
        }

        Transfer::Metrics::BytesReceived(_config.transferServer.serverId, static_cast<long>(length));
        sftp_reply_status(msg, SSH_FX_OK, "Written");
    }

    void SftpSession::handleStat(sftp_client_message msg, const bool followSymlinks) {

        const auto *filename = sftp_client_message_get_filename(msg);
        const auto resolved = resolve(filename != nullptr ? filename : "");

        if (_storage) {
            const auto entry = _storage->Stat(keyOf(resolved.virtualPath));
            if (!entry.has_value()) {
                sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "No such file");
                return;
            }
            sftp_attributes_struct remoteAttr{};
            fillRemoteAttributes(*entry, remoteAttr);
            sftp_reply_attr(msg, &remoteAttr);
            return;
        }

        auto target = resolved.physicalPath;
        if (followSymlinks) {
            std::error_code fsEc;
            if (auto canonical = std::filesystem::weakly_canonical(target, fsEc); !fsEc) {
                target = canonical;
            }
        }

        sftp_attributes_struct attr{};
        if (!fillAttributes(target, attr)) {
            sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "No such file");
            return;
        }
        sftp_reply_attr(msg, &attr);
    }

    void SftpSession::handleFstat(sftp_client_message msg) {

        const Handle *handle = handleOf(msg);
        if (handle == nullptr) {
            sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        sftp_attributes_struct attr{};
        if (_storage) {
            // Described from the spool file, not from the bucket: mid-transfer the spool is the
            // only place the file's current length exists, and that is what a client asking
            // about its own open handle wants to know.
            Transfer::TransferEntry entry;
            entry.name = handle->key;
            entry.isDirectory = handle->isDirectory;
            std::error_code sizeEc;
            entry.size = static_cast<long>(std::filesystem::file_size(handle->path, sizeEc));
            if (sizeEc) entry.size = 0;
            fillRemoteAttributes(entry, attr);
            sftp_reply_attr(msg, &attr);
            return;
        }
        if (!fillAttributes(handle->path, attr)) {
            sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "No such file");
            return;
        }
        sftp_reply_attr(msg, &attr);
    }

    void SftpSession::handleMkdir(sftp_client_message msg) {

        const auto *filename = sftp_client_message_get_filename(msg);
        const auto resolved = resolve(filename != nullptr ? filename : "");

        if (_storage) {
            if (!_storage->MakeDirectory(keyOf(resolved.virtualPath))) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Could not create directory");
                return;
            }
            sftp_reply_status(msg, SSH_FX_OK, "Created");
            return;
        }

        std::error_code fsEc;
        if (std::filesystem::exists(resolved.physicalPath, fsEc)) {
            sftp_reply_status(msg, SSH_FX_FILE_ALREADY_EXISTS, "Already exists");
            return;
        }
        if (!std::filesystem::create_directory(resolved.physicalPath, fsEc) || fsEc) {
            sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "Could not create directory");
            return;
        }
        sftp_reply_status(msg, SSH_FX_OK, "Created");
    }

    void SftpSession::handleRmdir(sftp_client_message msg) {

        const auto *filename = sftp_client_message_get_filename(msg);
        const auto resolved = resolve(filename != nullptr ? filename : "");

        if (_storage) {
            if (!_storage->DeleteDirectory(keyOf(resolved.virtualPath))) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Could not remove directory");
                return;
            }
            sftp_reply_status(msg, SSH_FX_OK, "Removed");
            return;
        }

        std::error_code fsEc;
        if (!std::filesystem::is_directory(resolved.physicalPath, fsEc)) {
            sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "No such directory");
            return;
        }
        // remove() rather than remove_all(): RMDIR is defined to fail on a non-empty directory,
        // and silently deleting a subtree is not a surprise worth handing a client.
        if (!std::filesystem::remove(resolved.physicalPath, fsEc) || fsEc) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "Could not remove directory");
            return;
        }
        sftp_reply_status(msg, SSH_FX_OK, "Removed");
    }

    void SftpSession::handleRemove(sftp_client_message msg) {

        const auto *filename = sftp_client_message_get_filename(msg);
        const auto resolved = resolve(filename != nullptr ? filename : "");

        if (_storage) {
            if (!_storage->Remove(keyOf(resolved.virtualPath))) {
                sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "Could not remove file");
                return;
            }
            sftp_reply_status(msg, SSH_FX_OK, "Removed");
            return;
        }

        std::error_code fsEc;
        if (std::filesystem::is_directory(resolved.physicalPath, fsEc)) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "Is a directory");
            return;
        }
        if (!std::filesystem::remove(resolved.physicalPath, fsEc) || fsEc) {
            sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "Could not remove file");
            return;
        }
        sftp_reply_status(msg, SSH_FX_OK, "Removed");
    }

    void SftpSession::handleRename(sftp_client_message msg) {

        const auto *oldName = sftp_client_message_get_filename(msg);
        const auto *newName = sftp_client_message_get_data(msg);
        if (oldName == nullptr || newName == nullptr) {
            sftp_reply_status(msg, SSH_FX_BAD_MESSAGE, "Missing path");
            return;
        }

        // Both ends go through resolve(), so a rename can neither read from nor write to
        // anything outside the user's home directory.
        const auto from = resolve(oldName);
        const auto to = resolve(newName);

        if (_storage) {
            if (!_storage->Rename(keyOf(from.virtualPath), keyOf(to.virtualPath))) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Could not rename");
                return;
            }
            sftp_reply_status(msg, SSH_FX_OK, "Renamed");
            return;
        }

        std::error_code fsEc;
        std::filesystem::rename(from.physicalPath, to.physicalPath, fsEc);
        if (fsEc) {
            sftp_reply_status(msg, SSH_FX_FAILURE, "Could not rename");
            return;
        }
        sftp_reply_status(msg, SSH_FX_OK, "Renamed");
    }

    void SftpSession::handleSetstat(sftp_client_message msg) {

        // Clients send SETSTAT routinely after an upload (sftp -p, WinSCP and friends) to apply
        // the local mode and timestamps. Permissions are honoured; ownership and timestamps are
        // accepted and ignored, because answering OP_UNSUPPORTED makes those clients report the
        // whole transfer as failed even though the bytes arrived intact.
        // A bucket object has no mode bits to set, so in transfer mode there is nothing to
        // apply - but the reply still has to be OK, for the reason above.
        if (!_storage && msg->attr != nullptr && (msg->attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS)) {

            const auto *filename = sftp_client_message_get_filename(msg);
            const Handle *handle = handleOf(msg);
            const auto target = handle != nullptr ? handle->path : resolve(filename != nullptr ? filename : "").physicalPath;

            std::error_code fsEc;
            std::filesystem::permissions(target, static_cast<std::filesystem::perms>(msg->attr->permissions & 0777),
                                         std::filesystem::perm_options::replace, fsEc);
            if (fsEc) {
                log_debug << "SFTP setstat could not apply permissions to " << target.string() << ": " << fsEc.message();
            }
        }

        sftp_reply_status(msg, SSH_FX_OK, "Applied");
    }

    SftpSession::ResolvedPath SftpSession::resolve(const std::string &path) const {

        // ".." is clamped at the virtual root instead of rejected: popping an already-empty
        // `parts` is simply a no-op, so the loop below can never walk `physical` above
        // _homeDir - there is no path string this can ever produce that isn't itself rooted at
        // (or below) _homeDir. Caveat, identical to the FTP module's: this is confinement by
        // construction of the path, not a defense against a symlink under the home directory
        // pointing back out of it.
        std::vector<std::string> parts;
        std::istringstream iss(path);
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

    std::string SftpSession::keyOf(const std::string &virtualPath) {
        std::string key = virtualPath;
        while (!key.empty() && key.front() == '/') key.erase(key.begin());
        return key;
    }

    std::filesystem::path SftpSession::spoolPathFor(const std::string &key) const {
        // A UUID rather than the key: two sessions may hold the same object open at once, and a
        // key contains slashes that would otherwise have to be flattened into a filename.
        return _config.rootDir / ("spool-" + Core::UuidUtils::CreateRandomUuid());
    }

    SftpSession::Handle *SftpSession::handleOf(sftp_client_message msg) const {
        if (msg->handle == nullptr) return nullptr;
        return static_cast<Handle *>(sftp_handle(_sftp, msg->handle));
    }

    bool SftpSession::fillAttributes(const std::filesystem::path &path, sftp_attributes_struct &attr) {

        std::error_code fsEc;
        const auto status = std::filesystem::symlink_status(path, fsEc);
        if (fsEc) return false;

        const bool isDirectory = std::filesystem::is_directory(status);
        const bool isSymlink = std::filesystem::is_symlink(status);

        attr.flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
        attr.type = isDirectory  ? SSH_FILEXFER_TYPE_DIRECTORY
                    : isSymlink  ? SSH_FILEXFER_TYPE_SYMLINK
                    : std::filesystem::is_regular_file(status)
                                 ? SSH_FILEXFER_TYPE_REGULAR
                                 : SSH_FILEXFER_TYPE_SPECIAL;

        attr.size = isDirectory ? 0 : std::filesystem::file_size(path, fsEc);
        if (fsEc) attr.size = 0;

        // SFTP version 3 carries POSIX mode bits, type included - a client that only gets the
        // permission bits shows every entry as a regular file, directories included.
        const auto typeBits = isDirectory ? kModeDirectory : isSymlink ? kModeSymlink : kModeRegular;
        attr.permissions = typeBits | (static_cast<std::uint32_t>(status.permissions() & std::filesystem::perms::mask) & 0777);

        const auto ftime = std::filesystem::last_write_time(path, fsEc);
        const auto mtime = fsEc ? 0 : static_cast<std::uint32_t>(toTimeT(ftime));
        attr.mtime = mtime;
        attr.atime = mtime;

        return true;
    }

    std::string SftpSession::longName(const std::string &name, const std::filesystem::path &path) {

        std::error_code fsEc;
        const bool isDirectory = std::filesystem::is_directory(path, fsEc);
        const auto size = isDirectory ? 0 : std::filesystem::file_size(path, fsEc);
        const auto ftime = std::filesystem::last_write_time(path, fsEc);
        const std::string timestamp = fsEc ? "Jan  1 00:00" : formatListTimestamp(ftime);

        // Owner/group are fixed placeholders: this server has no concept of Unix uid/gid, and
        // clients parse only the leading type character, the size and the name out of this.
        std::ostringstream oss;
        oss << (isDirectory ? 'd' : '-') << "rw-r--r-- 1 sftp sftp " << std::setw(10) << size << " " << timestamp << " " << name;
        return oss.str();
    }

}// namespace Euclid::SFTP
