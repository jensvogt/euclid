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
     * @author jensvogt47\@gmail.com
     */
    class Permissions {

    public:

        /**
         * @brief The modules whose actions are never grantable, sorted.
         */
        static const std::vector<std::string> &UnbindableModules();

        /**
         * @brief The actions the role gate does not decide, sorted, `<module>:<action>`.
         *
         * @par
         * Not "anybody may do these". They are the actions whose subject is the caller's own
         * session, where the gate is structurally unable to ask the right question and the handler
         * asks it instead - a narrower one, against the same grants.
         *
         * @par
         * `eam:change-namespace` is the case that named this. The gate decides a request from
         * `x-euclid-target`, `x-euclid-action` and `x-euclid-namespace`, and the namespace a caller
         * is asking to move to is in the body, which the gate does not read. So it was matching the
         * caller's grants against the namespace they are leaving - empty, on the change that follows
         * a login - and the question it answered, "may you act in the namespace you are already in",
         * is not the one being asked. Held to it, a namespace switch was possible only for a caller
         * granted `*`, and only if their role held an `eam:` permission at all: `operator` excludes
         * the whole module by design and `reader` keeps only the reads, so in practice nobody but an
         * account administrator could choose a namespace to work in.
         *
         * @par
         * What decides it instead is handleChangeNamespace(), which requires the namespace to exist
         * and the caller to be an account admin or to hold a grant naming it. That is the check the
         * gate was standing in front of, and it reads the body, so it can make it.
         *
         * @par
         * These stay in All(). The action is dispatched, so the vocabulary has to name it - see
         * PermissionVocabularyTest - and a role may hold it; it simply is not what admits the
         * request. Adding to this list moves a decision from the gate to a handler, so nothing
         * belongs here whose handler does not already make one.
         */
        static const std::vector<std::string> &UngatedActions();

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

        /**
         * @brief Whether an action only reads, judged from its name.
         *
         * @par
         * Prefix-matched on the action half, because euclid names its actions consistently enough
         * for that to be the honest rule rather than a list somebody has to maintain:
         * list-queues, get-object, get-table, count-objects.
         *
         * @par
         * "describe-" is still matched although no action carries it any more - ekv's
         * describe-table became get-table, for consistency with every other module's way of
         * asking for one of something. The prefix stays because it is a word a reader
         * reaches for, and an action that used it would be a read whether or not anyone
         * remembered to add it here.
         *
         * @par
         * `count-` is here because euclid names counting both ways: the cached figures are
         * get-object-count and get-message-count, which `get-` already covers, and the one action
         * that counts for real is count-objects. A reader that could list a bucket's objects but
         * not be told how many there are would be a strange thing to have built.
         *
         * @par
         * Two things depend on this and must not disagree: the `reader` built-in role is every
         * permission that satisfies it, and EAD records the actions that do not - so an action
         * misjudged here is both grantable to a reader and invisible to the audit, which is the
         * pair you least want to get wrong together.
         *
         * @param permission a full "<module>:<action>", or a bare action.
         * @return true when it only reads.
         */
        static bool IsRead(std::string_view permission);

        /**
         * @brief Whether an action's subject is a second-level thing rather than a resource.
         *
         * @par
         * The distinction the audit trail turns on. euclid's resources come in two tiers: the ones
         * somebody creates and manages - a queue, a topic, a bucket, a key, a secret, a table, a
         * user, an application - and the things that then flow through them: messages, objects,
         * items, events, parts. An operator acts on the first tier and reads about it afterwards;
         * programs act on the second tier, continuously, and that traffic is not what an audit is
         * for.
         *
         * @par
         * Judged by what the action acts on rather than by its verb, because euclid's verbs are
         * not uniform and a verb rule gets the important cases wrong in both directions.
         * "set-queue-delay" is an update of a queue though it is not spelled like one, and
         * "purge-queue" is about as consequential as an operation on a queue gets; both would fall
         * outside a create/update/delete rule. Meanwhile "delete-message" reads like a deletion and
         * is a consumer acknowledging work, thousands of times an hour.
         *
         * @par
         * Measured on a development installation over twenty minutes: 205,631 upload-part, 101,736
         * receive-messages and 95,248 delete-message, against a few thousand entries for
         * everything an operator would actually search for. The writer could not keep up and began
         * discarding the oldest entries - 46,000 in one process - so the volume was not merely
         * noisy, it was destroying the trail it was part of.
         *
         * @par
         * Each of these is already bracketed by something that is recorded: create-upload and
         * complete-upload record an upload with its key, its caller and its outcome, so the 1,479
         * parts of a 12 GB file are how the bytes arrived rather than what was done.
         *
         * @param permission a full "<module>:<action>", or a bare action.
         * @return true when it acts on a message, object, item, event, part or upload.
         */
        static bool IsSecondLevel(std::string_view permission);
    };

}// namespace Euclid::Core
