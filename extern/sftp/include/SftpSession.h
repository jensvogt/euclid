#pragma once

// C++ includes
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// libssh includes
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/sftp.h>

// Euclid includes
#include <SftpServer.h>
#include <TransferAuthenticator.h>
#include <TransferStorage.h>

namespace Euclid::SFTP {

    /**
     * @brief Handles one client's SSH connection: key exchange, password authentication,
     * the "sftp" subsystem channel, and then the SFTP request loop on that channel.
     *
     * @par
     * Runs entirely on the thread that constructs it (see SftpServer::acceptLoop()), using
     * blocking libssh calls throughout - there is no async state machine here, which keeps
     * the request handling readable at the cost of one OS thread per connected client. The
     * same tradeoff the FTP module makes, for the same reason.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class SftpSession {

    public:

        /**
         * @brief Takes ownership of an accepted ssh_session.
         *
         * @param session ssh_session already bound to an accepted socket, but before key exchange.
         * @param config  server configuration, copied per session.
         */
        SftpSession(ssh_session session, SftpServerConfig config);

        /**
         * @brief Frees the SFTP session, channel and ssh_session, and with them the
         * underlying socket.
         */
        ~SftpSession();

        SftpSession(const SftpSession &) = delete;
        SftpSession &operator=(const SftpSession &) = delete;

        /**
         * @brief Performs the handshake and serves SFTP requests until the client
         * disconnects or a protocol error occurs. Never throws.
         */
        void Run();

    private:

        /**
         * @brief What a client-supplied SFTP path resolves to: both the normalized virtual
         * path (what the client is told it is looking at) and the real filesystem path it
         * maps to under the user's home directory.
         */
        struct ResolvedPath {
            std::string virtualPath;
            std::filesystem::path physicalPath;
        };

        /**
         * @brief An open file or an open directory, referenced by the opaque handle string
         * the client is given. Kept in _handles for the lifetime of the session, since
         * libssh's handle table stores only the raw pointer.
         */
        struct Handle {
            bool isDirectory{false};

            // File handles
            std::fstream stream;
            std::filesystem::path path;

            // Directory handles: the listing is snapshotted at OPENDIR and then drained
            // across successive READDIR requests, which is what the protocol's
            // read-until-EOF loop expects.
            std::vector<std::filesystem::directory_entry> entries;
            std::size_t nextEntry{0};
            std::string virtualPath;

            // ── Transfer mode only ──────────────────────────────────────

            /**
             * @brief Bucket key this handle refers to. Empty in standalone mode.
             */
            std::string key;

            /**
             * @brief Whether anything was written through this handle, and so whether the spool
             * file has to be uploaded when it is closed. A handle opened for writing but never
             * written to still counts, so that truncating a file to zero length is stored.
             */
            bool dirty{false};

            /**
             * @brief Listing snapshotted at OPENDIR, drained across successive READDIRs - the
             * bucket-backed counterpart to `entries`.
             */
            std::vector<Transfer::TransferEntry> remoteEntries;
        };

        // ── Handshake ───────────────────────────────────────────────────

        /**
         * @brief Runs the SSH authentication loop, accepting the "password" method for an EAM
         * user this transfer server permits.
         *
         * @return true if a client authenticated successfully.
         */
        [[nodiscard]] bool authenticate();

        /**
         * @brief Waits for the client to open a session channel and request the "sftp"
         * subsystem on it, rejecting anything else (notably shell and exec requests, which
         * this server deliberately does not provide).
         *
         * @return true if an SFTP channel was established.
         */
        [[nodiscard]] bool openSftpChannel();

        // ── Request handling ────────────────────────────────────────────

        /**
         * @brief Reads and dispatches SFTP requests until the client disconnects.
         */
        void messageLoop();

        void handleRealpath(sftp_client_message msg);
        void handleOpenDir(sftp_client_message msg);
        void handleReadDir(sftp_client_message msg);
        void handleClose(sftp_client_message msg);
        void handleOpen(sftp_client_message msg);
        void handleRead(sftp_client_message msg);
        void handleWrite(sftp_client_message msg);
        void handleStat(sftp_client_message msg, bool followSymlinks);
        void handleFstat(sftp_client_message msg);
        void handleMkdir(sftp_client_message msg);
        void handleRmdir(sftp_client_message msg);
        void handleRemove(sftp_client_message msg);
        void handleRename(sftp_client_message msg);
        void handleSetstat(sftp_client_message msg);

        // ── Helpers ─────────────────────────────────────────────────────

        /**
         * @brief Resolves a client-supplied path (absolute or relative to the virtual root)
         * to a physical path under the user's home directory.
         *
         * ".." components are clamped at the virtual root rather than rejected, so the
         * physical path can never be constructed outside the home directory in the first
         * place - the same construction the FTP module uses, with the same symlink caveat.
         */
        [[nodiscard]] ResolvedPath resolve(const std::string &path) const;

        /**
         * @brief Looks up the Handle behind a request's handle string.
         *
         * @return the handle, or nullptr if the client sent one this session never issued.
         */
        [[nodiscard]] Handle *handleOf(sftp_client_message msg) const;

        /**
         * @brief Fills SFTP attributes from a directory entry's filesystem status.
         *
         * @param path entry to describe.
         * @param attr attributes to fill; owns no memory the caller must release.
         * @return false if the entry could not be stat'ed.
         */
        static bool fillAttributes(const std::filesystem::path &path, sftp_attributes_struct &attr);

        /**
         * @brief Renders an "ls -l"-style long name, which clients display verbatim in
         * directory listings.
         */
        static std::string longName(const std::string &name, const std::filesystem::path &path);

        /**
         * @brief Maps a virtual path to the bucket key it addresses, i.e. the same path without
         * its leading slash. Transfer mode only.
         */
        [[nodiscard]] static std::string keyOf(const std::string &virtualPath);

        /**
         * @brief Local scratch file backing one open handle in transfer mode.
         *
         * Transfers are spooled rather than streamed because SFTP reads and writes at explicit
         * offsets, which a single sequential ESM request/response cannot provide.
         */
        [[nodiscard]] std::filesystem::path spoolPathFor(const std::string &key) const;

        ssh_session _session{nullptr};
        ssh_channel _channel{nullptr};
        sftp_session _sftp{nullptr};

        SftpServerConfig _config;
        std::string _username;
        std::filesystem::path _homeDir;

        /**
         * @brief Bucket-backed storage for the logged-in user, present only in transfer mode.
         *
         * Constructed at login rather than at startup because it carries that user's own bearer
         * token: every ESM call a session makes is made as the user who logged in, not as the
         * server.
         */
        std::optional<Transfer::TransferStorage> _storage;

        /**
         * @brief Owns every Handle handed out this session. libssh's handle table stores
         * the bare pointer, so the objects have to outlive the individual requests that
         * use them; they are released together when the session ends.
         */
        std::vector<std::unique_ptr<Handle> > _handles;
    };

}// namespace Euclid::SFTP
