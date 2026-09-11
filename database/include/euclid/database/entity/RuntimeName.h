// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/11/26.
//

#pragma once

// C++ includes
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

}// namespace Euclid::Database::Entity
