#pragma once

// C++ includes
#include <optional>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

// Boost includes
#include <boost/json.hpp>

namespace Euclid::CLI {

    /**
     * @brief Persists the CLI's session between invocations.
     *
     * Stored as JSON (e.g. {"token": "...", "userId": "...", "accountId": "...", "region":
     * "..."}) at $HOME/.euclid/credentials, so a user only has to log in once per machine and
     * every subsequent command automatically knows who/where it's acting as.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class Credentials {

    public:

        /**
         * @brief Session data persisted to, and reloaded from, the credentials file.
         */
        struct Entry {
            std::string token;
            std::string userId;
            std::string accountId;
            std::string region;

            /**
             * @brief SigV4 access key ID. Populated automatically on "access login" (reusing the
             * account's existing active key, or provisioning one on first login), and refreshable
             * via "access create-access-key". Empty only if login has never succeeded.
             */
            std::string accessKeyId;

            /**
             * @brief SigV4 secret access key paired with accessKeyId.
             */
            std::string secretAccessKey;

            /**
             * @brief Whether this user has administrator privileges, as reported by the server
             * on login. Lets admin-only commands (e.g. "emm") fail fast client-side instead of
             * only after a round trip - the server enforces this independently regardless, since
             * a locally-cached flag isn't a security boundary on its own.
             */
            bool isAdmin{false};
        };

        /**
         * @brief Saves the session to the credentials file, creating $HOME/.euclid if it
         * doesn't exist yet. Called once, right after a successful login.
         *
         * @param entry session data to persist
         */
        static void Save(const Entry &entry);

        /**
         * @brief Reads the full session from the credentials file.
         *
         * @return the stored session, or std::nullopt if the file doesn't exist, isn't
         * readable, or doesn't contain a token.
         */
        [[nodiscard]]
        static std::optional<Entry> Load();

        /**
         * @brief Reads just the bearer token from the credentials file.
         *
         * @return the stored token, or std::nullopt if the file doesn't exist, isn't readable,
         * or doesn't contain a token.
         */
        [[nodiscard]]
        static std::optional<std::string> LoadToken();

        /**
         * @brief Removes the credentials file, e.g. after the server rejects the stored token
         * as missing/invalid/expired. A no-op if the file doesn't exist.
         */
        static void ClearToken();

    private:

        /**
         * @brief Path to the credentials file, e.g. "/home/alice/.euclid/credentials".
         */
        [[nodiscard]]
        static std::string FilePath();
    };

}
