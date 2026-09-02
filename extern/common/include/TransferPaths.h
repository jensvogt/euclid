#pragma once

// C++ includes
#include <string>

namespace Euclid::Transfer {

    /**
     * @brief Expands a transfer server's home directory template into the bucket key prefix one
     * user's session writes under.
     *
     * @par
     * A transfer server fronts a single bucket, and without this every client of it shares one
     * flat key space: whatever path an FTP client types is the object key, so two users can see -
     * and overwrite - each other's files, and nothing in the key says who delivered it. A home
     * directory is what gives each user its own corner of the bucket, the way an FTP server's
     * per-user home directory always has.
     *
     * @par
     * The template is expanded per session rather than stored per user, because a server admits
     * users by group as well as by name (TransferServer::userGroups): a map from user to home
     * could not cover a user who has not logged in yet. {user} is replaced by the EAM user ID
     * that authenticated, so "{user}" gives every client its own prefix and
     * "lieferanten/{user}" nests them under a common one.
     *
     * @par
     * The result is normalised so that it can be concatenated with a key directly: no leading
     * slash, no empty or "." segments, exactly one trailing slash, and empty when the template
     * is empty - which is the previous behaviour, every session at the bucket root. ".."
     * segments are dropped rather than honoured, so a template can only ever name a prefix
     * below the bucket root.
     *
     * @param homeDirectory the server's home directory template, e.g. "{user}"
     * @param userId the EAM user ID of the session, substituted for {user}
     * @return the key prefix, ending in "/", or empty for the bucket root
     */
    [[nodiscard]] std::string HomePrefix(const std::string &homeDirectory, const std::string &userId);

}// namespace Euclid::Transfer
