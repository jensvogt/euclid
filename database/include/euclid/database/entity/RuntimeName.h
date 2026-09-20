// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/11/26.
//

#pragma once

// C++ includes
#include <functional>
#include <string>

namespace Euclid::Database::Entity {

    /**
     * @brief Issues the name a spawned thing will run under, for as long as it exists.
     *
     * @par
     * Some of what euclid stores is also something euclid runs: an EAP application, an ETS
     * transfer server. Those are named within an account and a namespace like every other
     * resource, but what the manager makes of them is not - a process pool, a module row, a data
     * directory, a unix socket and a log channel are installation-wide, with nowhere to put an
     * account or a namespace. So they are given a second name, from here, and that is what all of
     * those are built from.
     *
     * @par
     * The thing's own id, so that ps output, a module list, a log channel and a directory under
     * the data dir all still say what they belong to, and a random suffix, which is what actually
     * makes it unique and what lets it stay put while everything around it changes. The id is cut
     * at 32 characters because the whole of this ends up in a unix socket path, and sun_path is
     * capped at 108 bytes.
     *
     * @par
     * Unique by construction rather than by rule, so there is nothing about accounts or namespaces
     * to enforce at the call site - but a caller should still check the result against what is
     * already issued, because a suffix is random and "unlikely" is not "cannot".
     *
     * @param id the thing's own id, e.g. an applicationId or a serverId
     * @return a name to run it under
     */
    std::string GenerateRuntimeName(const std::string &id);

    /**
     * @brief Whether a runtime name is one it is safe to build a path from.
     *
     * @par
     * A runtime name names a directory under the data dir and a unix socket, so it is a path
     * component before it is anything else. An empty name, "." or "..", or one carrying a
     * separator, each resolves somewhere other than where the caller meant - which matters most to
     * the one caller that deletes recursively. Names this issues are always safe; names read back
     * out of a database row are not, because a row is not a promise about what wrote it.
     *
     * @param name the name to check
     * @return true if it is a single, ordinary path component
     */
    bool IsSafeRuntimeName(const std::string &name);

    /**
     * @brief Chooses the name a new thing will run under, given a way to ask what is already taken.
     *
     * @par
     * The plain id when that is free, because a runtime name is read by people - off ps output, a
     * socket path, a data directory, a log channel - and "protocolizing" says everything
     * "protocolizing-5yrdi4gh" says without the noise. A suffixed name only when the plain one is
     * taken, which is the case it exists for: two namespaces may each define an application of the
     * same name, and they cannot share a directory or a socket.
     *
     * @par
     * This used to suffix unconditionally, because deleting an application left its data directory
     * behind and a reused name would start up on a dead application's files. The manager removes
     * that directory now, so the suffix is no longer carrying that weight.
     *
     * @param id the thing's own id, e.g. an applicationId or a serverId
     * @param taken answers whether a candidate name is already in use
     * @return a free name to run it under
     * @throws std::runtime_error if no free name could be found
     */
    std::string IssueRuntimeName(const std::string &id, const std::function<bool(const std::string &)> &taken);

}// namespace Euclid::Database::Entity
