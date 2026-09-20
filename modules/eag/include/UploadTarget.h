// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/20/26.
//

#pragma once

// C++ includes
#include <string>
#include <string_view>

// Euclid includes
#include <euclid/database/entity/eag/Route.h>

namespace Euclid::EAG {

    /**
     * @brief Where one upload's bytes are going, or why they are going nowhere.
     */
    struct UploadKey {

        /**
         * @brief Whether the request named a key this route is willing to write.
         */
        bool valid{false};

        /**
         * @brief The key within the bucket, prefix included. Empty unless valid.
         */
        std::string key;

        /**
         * @brief What was wrong with the request target, for the 400 the caller gets. Empty when
         * valid.
         */
        std::string reason;
    };

    /**
     * @brief Turns a request target into the key an upload route writes to.
     *
     * @par
     * The part of the path below the route's own prefix is the key: a route on "/upload" taking a
     * PUT of "/upload/onix/2026-09.xml" writes "onix/2026-09.xml". Nothing about the route appears
     * in the key, the same way nothing about a proxy route appears in what it forwards.
     *
     * @par
     * The route's keyPrefix then goes in front, and is what confines the route to part of its
     * bucket - the home directory of a transfer server, applied to keys. Which is why what comes
     * out of here cannot be allowed to climb back out of it: the target is percent-decoded first
     * and *then* checked, because a caller who writes "%2E%2E" means "..", and a check made before
     * decoding would not see it.
     *
     * @param route  the matched upload route.
     * @param target the request target, query string and all.
     * @return the key, or why there isn't one.
     */
    [[nodiscard]]
    UploadKey ResolveUploadKey(const Database::Entity::EAG::Route &route, std::string_view target);

}// namespace Euclid::EAG
