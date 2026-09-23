// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <iostream>
#include <string>

// Boost includes
#include <boost/json.hpp>

namespace Euclid::CLI {

    /**
     * @brief The one place the exists-* contract is written down.
     *
     * @par
     * Six commands answer "is this there?" - exists-queue, exists-bucket, exists-topic,
     * exists-key, exists-secret, exists-table - and they are only useful to a script if they all
     * answer the same way. A module whose exists- command exited 1 for a refused permission while
     * the others exited 2 would be the one that quietly broke somebody's provisioning script, so
     * the codes and the words live here rather than being retyped per module.
     *
     * @par Why a shell needs three answers and not two
     * A shell reads a successful exit as true, so existence is the exit status: 0 there, 1 not
     * there. But "could not tell" is a third answer, and collapsing it into either of the first two
     * is what makes a check dangerous. get-queue-ern exits 1 both for "no such queue" and for "your
     * session expired" - branch on that and an expired token reads as an absent queue, and the
     * script goes on to create what already exists, or skips a step that mattered.
     *
     * @par
     * So could-not-tell is 2. A script that only writes `if` still gets the safe outcome, because
     * 2 is not success either and the true branch is not taken on an answer nobody actually got.
     *
     * @author jensvogt47\@gmail.com
     */
    namespace Exists {

        /**
         * @brief It is there. "true" was written to stdout.
         */
        constexpr int kYes = 0;

        /**
         * @brief It is not there. "false" was written to stdout.
         */
        constexpr int kNo = 1;

        /**
         * @brief Nobody knows: no name given, an unreachable or unauthenticated gateway, a refused
         * permission, a 5xx. The reason went to stderr and stdout was left empty - a script reading
         * `$(...)` gets "" rather than a word it would believe.
         */
        constexpr int kUnknown = 2;

        /**
         * @brief Answers yes or no, having found out.
         *
         * @par
         * One word and a newline on stdout, nothing else - no JSON, no metadata, nothing to cut out
         * of the way with sed. "true" and "false" rather than 1 and 0 so that
         * `[ "$(...)" = true ]` reads as what it means.
         *
         * @param present whether the resource is there
         * @return kYes or kNo
         */
        inline int Answer(const bool present) {
            std::cout << (present ? "true" : "false") << std::endl;
            return present ? kYes : kNo;
        }

        /**
         * @brief Answers that it could not find out, saying why.
         *
         * @par
         * Deliberately says nothing on stdout. A script that captures the word and gets "false"
         * from a failed call is worse off than one that gets the empty string, because the empty
         * string is obviously not an answer and "false" is indistinguishable from a real one.
         *
         * @param command the exists- command, for the message
         * @param reason what went wrong
         * @return kUnknown
         */
        inline int Unknown(const std::string &command, const std::string &reason) {
            std::cerr << "error: " << command << " could not tell: " << reason << std::endl;
            return kUnknown;
        }

        /**
         * @brief Turns a by-name lookup's response into an answer.
         *
         * @par
         * 404 is the only status that means "not there". Everything else that is not a success is
         * something the caller has to be told about rather than have read as absence: 401 and 403
         * are about the caller, 5xx is about the installation, and a resource that exists looks
         * exactly like one that does not if any of them is treated as a 404.
         *
         * @param command the exists- command, for the message
         * @param statusCode the HTTP status the lookup was answered with
         * @param success whether that status was a success
         * @param body the response body, quoted in the message when it was not
         * @return kYes, kNo or kUnknown
         */
        inline int FromLookup(const std::string &command, const long statusCode, const bool success,
                              const boost::json::value &body) {

            if (statusCode == 404) return Answer(false);
            if (!success) {
                return Unknown(command, "HTTP " + std::to_string(statusCode) + ": " + boost::json::serialize(body));
            }
            return Answer(true);
        }

    }// namespace Exists

}// namespace Euclid::CLI
