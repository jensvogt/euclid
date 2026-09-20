// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <cstddef>
#include <cstdint>
#include <string>

// Euclid includes
#include <euclid/core/HttpSignature.h>

// The Signature-Input header, parsed before anything is known about who sent it. Whatever is in
// it arrived from outside and has been through no check at all at this point - which makes this
// the first line of code in euclid that an unauthenticated stranger reaches.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {

    const std::string header(reinterpret_cast<const char *>(data), size);
    std::ignore = Euclid::Core::HttpSignature::ParseSignatureInput(header);
    return 0;
}
