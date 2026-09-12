// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <string>
#include <string_view>
#include <vector>

namespace Euclid::Core {

    /**
     * @brief Everything a caller can be granted the right to do, named the way the wire names it.
     *
     * @par
     * A permission is `<module>:<action>` - the `x-euclid-target` and `x-euclid-action` headers of
     * the request it authorizes, joined by a colon. `ens:publish-message`, `eqs:receive-messages`,
     * `esm:put-object`. Nothing is invented here: every entry in All() is an action some module's
     * dispatch table already answers, which is what PermissionVocabularyTest holds it to.
     *
     * @par
     * That test is the point of this file. A list of permissions maintained separately from the
     * actions it describes drifts the first time a module gains one, and the drift is invisible:
     * the new action simply cannot be granted, or worse, a permission names something that no
     * longer exists and quietly grants nothing. Here the two are compared on every build.
     *
     * @par Two modules are deliberately absent
     * `emm` exposes every other module's live process pool, and its export/import reach every
     * collection; `emd` *is* the document store, and dispatches the storage primitives themselves -
     * a caller who could invoke `emd:replace-one` would rewrite any collection directly, past every
     * check the owning module makes. Neither is grantable at all. They are reached by the
     * `administrator` group and by euclid's own inter-module traffic respectively, and by nothing
     * else.
     *
     * @par
     * Leaving their actions out of the vocabulary, rather than listing them as ungrantable, means
     * this list describes exactly what can be granted - UnbindableModules() is the one place that
     * says why anything is missing.
     *
     * @par
     * See docs/role-concept.md for what consumes this.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class Permissions {

    public:

        /**
         * @brief The modules whose actions are never grantable, sorted.
         */
        static const std::vector<std::string> &UnbindableModules();

        /**
         * @brief The permission that grants every action of every bindable module.
         */
        static constexpr std::string_view Everything = "*:*";

        /**
         * @brief Every permission, sorted, `<module>:<action>`.
         *
         * @return the canonical vocabulary.
         */
        static const std::vector<std::string> &All();

        /**
         * @brief The modules whose actions can be granted, sorted. Excludes UnbindableModules().
         */
        static const std::vector<std::string> &Modules();

        /**
         * @brief Whether a module's actions can be granted at all.
         *
         * @param module module name, e.g. "ens".
         * @return false for anything in UnbindableModules(), true for a module in Modules().
         */
        [[nodiscard]]
        static bool IsBindable(std::string_view module);

        /**
         * @brief The permission naming one action.
         *
         * @param target module target, e.g. "ens".
         * @param action module action, e.g. "publish-message".
         * @return "ens:publish-message".
         */
        [[nodiscard]]
        static std::string Of(std::string_view target, std::string_view action);

        /**
         * @brief Whether a permission names an action that exists and can be granted.
         *
         * @par
         * Wildcards are not permissions in this sense - they are a way of writing a set of them -
         * so this answers false for "ens:*". Use Matches() to test one against a grant.
         *
         * @param permission e.g. "ens:publish-message".
         * @return true if it is in All().
         */
        [[nodiscard]]
        static bool Exists(std::string_view permission);

        /**
         * @brief Whether something granted covers something required.
         *
         * @par
         * Three forms are accepted on the granted side and no more: the exact permission,
         * `<module>:*` for every action of one module, and `*:*` for everything. A grant of `*:*`
         * still does not reach an unbindable module - the required permission would have to be one
         * of theirs, and those are not in the vocabulary, so nothing can require them through here.
         *
         * @param granted  what a role holds, possibly a wildcard.
         * @param required what the request needs, never a wildcard.
         * @return true if the grant covers it.
         */
        [[nodiscard]]
        static bool Matches(std::string_view granted, std::string_view required);
    };

}// namespace Euclid::Core
