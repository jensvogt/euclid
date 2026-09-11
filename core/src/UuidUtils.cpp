// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 5/24/2026.
//

#include <../include/euclid/core/UuidUtils.h>

namespace Euclid::Core {

    std::string UuidUtils::CreateRandomUuid() {
        boost::uuids::random_generator gen;
        return boost::uuids::to_string(gen());
    }

} // namespace Euclid::Core
