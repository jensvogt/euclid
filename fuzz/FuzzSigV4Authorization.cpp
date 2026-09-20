// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <cstddef>
#include <cstdint>
#include <string>

// Euclid includes
#include <euclid/core/SigV4.h>

// The other unauthenticated entry point: SigV4's Authorization header, and the query string
// canonicalisation that runs on whatever a caller put after the "?". Both read bytes nobody has
// vouched for, and both do their own splitting rather than handing it to a library.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {

    const std::string input(reinterpret_cast<const char *>(data), size);

    std::ignore = Euclid::Core::SigV4::ParseAuthorizationHeader(input);
    std::ignore = Euclid::Core::SigV4::CanonicalizeQueryString(input);
    return 0;
}
