// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <cstddef>
#include <cstdint>
#include <string_view>

// Euclid includes
#include <euclid/core/HttpUtils.h>

// The query string of an OIDC or SAML redirect, percent-decoded by hand. The provider chooses the
// URL, so the bytes are not euclid's - and a malformed escape is deliberately passed through as
// the literal text it is rather than rejected, which is exactly the kind of lenient parsing worth
// pointing a fuzzer at.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {

    std::ignore = Euclid::Core::ParseQueryParameters(
            std::string_view(reinterpret_cast<const char *>(data), size));
    return 0;
}
