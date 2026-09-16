// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/BaseEntity.h>

namespace Euclid::Database::Entity::EAD {

    /**
     * @brief One command somebody ran, and who they were when they ran it.
     *
     * @par What is recorded
     * The five things the audit was asked for - account, namespace, user, module and command -
     * plus the parameters the command carried and when it happened. A timestamp is not optional
     * in a record of this kind: "who did what" without "when" cannot be reconciled against
     * anything else that happened.
     *
     * @par Why the outcome is here too
     * `status` is the HTTP status the command was answered with, and it is the field that makes
     * this an audit rather than a log of activity. The interesting entry is rarely the delete that
     * worked - it is the one that was refused, which is a 403 and is otherwise indistinguishable
     * from a delete that never happened. Recording only what succeeded would leave exactly the
     * attempts somebody would want to review out of the record.
     *
     * @par Parameters are redacted, not raw
     * A request body is where passwords, secrets and private keys travel. An audit log is read
     * widely and exported freely - that is the point of it - so storing those verbatim would make
     * this collection the softest copy of every credential in the installation. See
     * Redact() for what is replaced and why the rule is on field names rather than on actions.
     *
     * @author jensvogt47\@gmail.com
     */
    struct AuditEvent final : BaseEntity {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Account the command was run in.
         */
        std::string accountId;

        /**
         * @brief Namespace the command was run in; empty for a command that names none.
         */
        std::string nameSpace;

        /**
         * @brief Who ran it, as the verified caller identity - not as the body claimed.
         */
        std::string userId;

        /**
         * @brief Module the command was addressed to, e.g. "esm".
         */
        std::string moduleName;

        /**
         * @brief The command, e.g. "delete-bucket".
         */
        std::string command;

        /**
         * @brief What it was called with, as a string, with sensitive values replaced.
         */
        std::string parameters;

        /**
         * @brief The HTTP status it was answered with.
         */
        long status{};

        /**
         * @brief When it happened.
         */
        std::chrono::system_clock::time_point created = std::chrono::system_clock::now();

        /**
         * @brief When this record is removed, for the TTL index to act on.
         */
        std::chrono::system_clock::time_point expiresAt{};

        /**
         * @brief Converts the entity to a MongoDB document
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts a MongoDB document to an entity
         */
        static AuditEvent fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

    /**
     * @brief Replaces the sensitive values in a request body, leaving its shape readable.
     *
     * @par Why by field name rather than by action
     * A list of actions whose bodies are sensitive is a list somebody has to keep correct, and the
     * cost of missing one is a password in a collection people export. Field names are the thing
     * the secret actually travels under, they are consistent across euclid's DTOs, and a new
     * action that carries a `password` is covered the day it is written rather than the day
     * somebody notices.
     *
     * @par
     * The key is kept and only the value replaced, so the audit still says *that* a password was
     * set - which is the auditable fact - without saying what it was set to.
     *
     * @par
     * A body that is not JSON is not guessed at: it is recorded as its size alone. Bodies like
     * that are uploads, and an audit entry is not the place for a file.
     *
     * @param body the raw request body.
     * @return the body with sensitive values replaced by "***".
     */
    std::string Redact(const std::string &body);

}// namespace Euclid::Database::Entity::EAD
