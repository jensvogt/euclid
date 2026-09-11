// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 5/24/2026.
//

#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace Euclid::Core {

    /**
     * @brief UUID utility class.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class UuidUtils {
    public:
        /**
         * @brief Generates a random UUID string.
         *
         * @return random UUID as a string in the form xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
         */
        static std::string CreateRandomUuid();
    };

} // namespace Euclid::Core