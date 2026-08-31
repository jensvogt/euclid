#pragma once

// C++ includes
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Euclid::Transfer {

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
         */
        TransferStorage(std::string bucketErn, std::string token, std::string region, std::string accountId)
            : _bucketErn(std::move(bucketErn)), _token(std::move(token)), _region(std::move(region)), _accountId(std::move(accountId)) {}

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
         * @brief Removes an empty directory by deleting its marker object.
         *
         * @param directory directory key.
         * @return true if it was removed; false if it does not exist or still has contents.
         */
        [[nodiscard]]
        bool RemoveDirectory(const std::string &directory) const;

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

        std::string _bucketErn;
        std::string _token;
        std::string _region;
        std::string _accountId;
    };

}// namespace Euclid::Transfer
