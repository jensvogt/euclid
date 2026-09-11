// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/9/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <string>

namespace Euclid::Core {

    /**
     * @brief Time-based one-time passwords, RFC 6238.
     *
     * @par
     * The six digits an authenticator app shows, computed from a shared secret and the clock. What
     * needs them here is a login that has to run unattended: an identity provider that requires a
     * second factor will not mint an assertion without one, and a person watching a terminal is
     * exactly what an automated login does not have.
     *
     * @par
     * The secret is the same one the authenticator app was enrolled with, in the base32 form
     * providers hand out. Anything holding it can produce the second factor, which is worth saying
     * plainly: storing it beside the password turns two factors back into one. It belongs in a file
     * only the owner can read, or in an environment a scheduler supplies.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class Totp {

    public:

        /**
         * @brief The code for a moment in time.
         *
         * @param secretBase32 the shared secret, base32-encoded, with or without padding and
         * spaces.
         * @param when the moment to compute for; defaults to now.
         * @param digits how many digits the provider expects, usually 6.
         * @param step the time step, usually 30 seconds.
         * @return the code, zero-padded to @p digits.
         * @throws std::runtime_error if the secret is not valid base32.
         */
        static std::string Code(const std::string &secretBase32,
                                std::chrono::system_clock::time_point when = std::chrono::system_clock::now(),
                                int digits = 6, std::chrono::seconds step = std::chrono::seconds(30));

        /**
         * @brief The code for a Unix timestamp.
         *
         * @par
         * What Code() is written in terms of, and the one to use when the time comes from
         * somewhere that counts seconds rather than from this machine's clock. It also covers what
         * a time_point cannot: std::chrono::system_clock counts nanoseconds here, so it runs out
         * somewhere in the twenty-third century, which is a limit RFC 6238's own last test vector
         * happens to sit beyond.
         *
         * @param secretBase32 the shared secret, base32-encoded.
         * @param unixTime seconds since the Unix epoch.
         * @param digits how many digits the provider expects, usually 6.
         * @param step the time step, usually 30 seconds.
         * @return the code, zero-padded to @p digits.
         * @throws std::runtime_error if the secret is not valid base32.
         */
        static std::string CodeAt(const std::string &secretBase32, std::chrono::seconds unixTime,
                                  int digits = 6, std::chrono::seconds step = std::chrono::seconds(30));

        /**
         * @brief Decodes a base32 secret, ignoring padding, spaces and case.
         *
         * @param encoded base32 text.
         * @return the raw secret bytes.
         * @throws std::runtime_error if the input is not valid base32.
         */
        static std::string Base32Decode(const std::string &encoded);
    };

}// namespace Euclid::Core
