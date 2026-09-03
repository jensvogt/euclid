#pragma once

// C++ includes
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Euclid::Transfer {

    /**
     * @brief Renders string attributes as the JSON object ESM's x-euclid-attributes header takes.
     *
     * @par
     * ESM stores an attribute as a typed value, so each one is written out with its type
     * alongside it - everything a transfer server has to say about an object is a string, hence
     * the plain map in.
     *
     * @param attributes attribute names and their values; empty names are skipped.
     * @return the JSON object, or an empty string if there is nothing to send.
     */
    [[nodiscard]]
    std::string AttributesHeader(const std::map<std::string, std::string> &attributes);

    /**
     * @brief One entry in a directory listing.
     */
    struct TransferEntry {

        /**
         * @brief Leaf name, without any path.
         */
        std::string name;

        /**
         * @brief Whether this entry is a directory.
         */
        bool isDirectory{false};

        /**
         * @brief Size in bytes; always 0 for a directory.
         */
        long size{};

        /**
         * @brief Last modification time.
         */
        std::chrono::system_clock::time_point modified;
    };

    /**
     * @brief A filesystem view over an ESM bucket, which is the system of record.
     *
     * @par
     * The bucket is what clients actually see: a listing reports the objects it holds, a
     * download reads one back out of it, and an upload is only complete once the object is
     * stored. Nothing is kept on local disk beyond the spool file an in-progress transfer needs,
     * so two transfer servers pointed at one bucket show the same contents, and a file put there
     * by any other route shows up here too.
     *
     * @par Directories
     * ESM buckets are flat: a key is a string, and "a/b/c.txt" has no parent object. Directories
     * are therefore synthesised from key prefixes, and an explicitly created (or emptied)
     * directory is remembered by a zero-byte marker object whose key ends in "/". Without the
     * marker an empty directory could not exist at all, since nothing would carry its name.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class TransferStorage {

    public:

        /**
         * @brief Constructs a storage view of one bucket, acting as one authenticated user.
         *
         * @param bucketErn ERN of the bucket backing this view.
         * @param token bearer token for the logged-in user, used on every ESM call.
         * @param region region this transfer server runs in; sent on every call, since a module
         * rejects a request whose region does not match the one it is configured for.
         * @param accountId account the transfer server (and therefore its bucket) belongs to.
         * @param serverId ID of the transfer server, recorded on every object stored through it.
         * @param userId the logged-in user, recorded on every object stored through it.
         */
        TransferStorage(std::string bucketErn, std::string token, std::string region, std::string accountId, std::string serverId, std::string userId)
            : _bucketErn(std::move(bucketErn)), _token(std::move(token)), _region(std::move(region)), _accountId(std::move(accountId)),
              _serverId(std::move(serverId)), _userId(std::move(userId)) {}

        /**
         * @brief Lists the immediate children of a directory.
         *
         * @param directory directory key, without a leading slash; empty means the bucket root.
         * @return entries directly beneath it, files and directories both.
         */
        [[nodiscard]]
        std::vector<TransferEntry> List(const std::string &directory) const;

        /**
         * @brief Describes one file or directory.
         *
         * @param key object key, without a leading slash.
         * @return the entry, or std::nullopt if nothing exists at that key.
         */
        [[nodiscard]]
        std::optional<TransferEntry> Stat(const std::string &key) const;

        /**
         * @brief Downloads an object into a local spool file.
         *
         * @par
         * Transfers are spooled rather than streamed because both protocols need random access
         * - SFTP reads at an explicit offset, and FTP restarts a transfer at one - which a
         * single sequential ESM response cannot provide.
         *
         * @param key object key.
         * @param spoolPath local file to write.
         * @return true on success.
         */
        [[nodiscard]]
        bool Download(const std::string &key, const std::filesystem::path &spoolPath) const;

        /**
         * @brief Uploads a local spool file as an object, replacing any object at that key.
         *
         * @par
         * A small file goes up in one put-object call; anything past the inline limit is split
         * into parts instead (see PartSize()). Neither end can hold a multi-gigabyte object in
         * memory - the module refuses a request body past euclid.gateway.http.max-body, and even
         * under it the bytes would exist twice in this process and once more in ESM.
         *
         * @param key object key.
         * @param spoolPath local file to read.
         * @return true on success.
         */
        [[nodiscard]]
        bool Upload(const std::string &key, const std::filesystem::path &spoolPath) const;

        /**
         * @brief Deletes an object.
         *
         * @param key object key.
         * @return true if the object was deleted.
         */
        [[nodiscard]]
        bool Remove(const std::string &key) const;

        /**
         * @brief Creates a directory by writing its marker object.
         *
         * @param directory directory key.
         * @return true on success.
         */
        [[nodiscard]]
        bool MakeDirectory(const std::string &directory) const;

        /**
         * @brief Creates whichever of these directories does not exist yet.
         *
         * @par
         * Called once per login to give a session the folder skeleton its clients expect (see
         * Transfer::HomeDirectoryKeys). Existing directories are left alone rather than written
         * over: a marker rewritten on every connection would publish an object event each time,
         * and every listener of the bucket would see a change that did not happen.
         *
         * @par
         * Best effort. A directory that cannot be created is logged and skipped rather than
         * failing the login - a client that cannot see its inbox is a smaller problem than one
         * that cannot log in at all, and the next connection tries again.
         *
         * @param keys full object keys, each ending in "/", parents before their children.
         * @return how many were actually created.
         */
        long EnsureDirectories(const std::vector<std::string> &keys) const;

        /**
         * @brief Removes an empty directory by deleting its marker object.
         *
         * @par
         * Deliberately not called RemoveDirectory: <windows.h> defines that as a macro expanding
         * to RemoveDirectoryA, which rewrites this declaration in any translation unit that has
         * already pulled Windows in and leaves it disagreeing with the definition. MakeDirectory
         * dodges the same trap - CreateDirectory is a macro too.
         *
         * @param directory directory key.
         * @return true if it was removed; false if it does not exist or still has contents.
         */
        [[nodiscard]]
        bool DeleteDirectory(const std::string &directory) const;

        /**
         * @brief Renames an object.
         *
         * @par
         * ESM has no rename, so this is a copy followed by a delete, with the bytes passing
         * through this process. Fine for the file sizes a transfer server sees, but worth
         * knowing that it is not atomic and not free.
         *
         * @param fromKey existing object key.
         * @param toKey new object key.
         * @return true on success.
         */
        [[nodiscard]]
        bool Rename(const std::string &fromKey, const std::string &toKey) const;

    private:

        /**
         * @brief Every object in the bucket whose key starts with this prefix.
         */
        [[nodiscard]]
        std::vector<TransferEntry> listRaw(const std::string &prefix, std::vector<std::string> &keys) const;

        /**
         * @brief Resolves an object key to its ERN, which is what ESM's delete-object takes.
         */
        [[nodiscard]]
        std::optional<std::string> ernOf(const std::string &key) const;

        /**
         * @brief Adds the caller's region and account to a call's own headers.
         *
         * @par
         * Every module runs Core::HttpActionServer::Authenticate() over these before it looks at
         * the action, so a call that names neither is answered with 403 as soon as the deployment
         * configures euclid.region - a bearer token on its own is not enough.
         *
         * @param headers headers this particular call needs, e.g. x-euclid-bucket-ern.
         * @return those headers plus the scope headers.
         */
        [[nodiscard]]
        std::vector<std::pair<std::string, std::string> > scopedHeaders(std::vector<std::pair<std::string, std::string> > headers) const;

        /**
         * @brief The attributes every object stored through this view carries: which transfer
         * server took it and who was logged in when they did.
         *
         * @par
         * Recorded on the object rather than only in the log, because a bucket is shared - files
         * arriving through several servers, several users and possibly other routes entirely all
         * end up side by side, and where one came from is not otherwise recoverable from it.
         *
         * @return the attributes as the JSON object put-object's x-euclid-attributes header takes.
         */
        [[nodiscard]]
        std::string provenanceHeader() const;

        /**
         * @brief Uploads a spool file as a multipart upload, one part-sized chunk per request.
         *
         * @param key object key.
         * @param spoolPath local file to read.
         * @return true if every part went up and the upload was completed.
         */
        [[nodiscard]]
        bool uploadInParts(const std::string &key, const std::filesystem::path &spoolPath) const;

        /**
         * @brief Downloads an object into a spool file, one part-sized chunk per request.
         *
         * @param key object key.
         * @param spoolPath local file to write.
         * @return true if the whole object was written.
         */
        [[nodiscard]]
        bool downloadInParts(const std::string &key, const std::filesystem::path &spoolPath) const;

        std::string _bucketErn;
        std::string _token;
        std::string _region;
        std::string _accountId;
        std::string _serverId;
        std::string _userId;
    };

}// namespace Euclid::Transfer
