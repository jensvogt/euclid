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
