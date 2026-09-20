// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <cstddef>
#include <cstdint>
#include <string>

// Euclid includes
#include <UploadTarget.h>

// The request target of an upload, turned into the key it writes to. The caller chooses every
// byte of it, it is percent-decoded before it is checked, and what comes out is confined to the
// route's prefix - so this is both a parser of hostile input and the place a path traversal
// would live if the checks were in the wrong order.
//
// The property worth asserting, rather than merely "it did not crash": whatever comes out stays
// under the prefix. A fuzzer that only watched for crashes would miss an escape that returned
// cleanly.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {

    Euclid::Database::Entity::EAG::Route route;
    route.path = "/upload";
    route.upload.keyPrefix = "incoming/tester";

    const std::string target(reinterpret_cast<const char *>(data), size);
    const auto resolved = Euclid::EAG::ResolveUploadKey(route, target);

    if (resolved.valid) {
        if (!resolved.key.starts_with("incoming/tester/")) __builtin_trap();

        // No segment may climb, whatever it was written as before decoding.
        if (resolved.key.find("/../") != std::string::npos) __builtin_trap();
        if (resolved.key.starts_with("../")) __builtin_trap();
        if (resolved.key.ends_with("/..")) __builtin_trap();
    }
    return 0;
}
