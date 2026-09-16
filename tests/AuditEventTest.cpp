// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE AuditEventTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/Permissions.h>
#include <euclid/database/entity/ead/AuditEvent.h>

using Euclid::Core::Permissions;
using Server = Euclid::Core::HttpActionServer;
using Euclid::Database::Entity::EAD::Redact;

// The audit trail records what every command carried, and request bodies are where passwords,
// secrets and private keys travel. An audit log is read widely and exported freely - that is the
// point of it - so a plaintext credential in one is worse than the audit is useful.
//
// What this pins is that no such value reaches the collection, and that the entry is still worth
// reading afterwards: the field name stays, so the trail says *that* a password was set without
// saying what it was set to.

namespace {

    // The value at a dotted path, for asserting on shape rather than on serialised text - which
    // would make these tests about key order.
    std::string valueAt(const std::string &json, const std::string &key) {
        const auto parsed = boost::json::parse(json);
        const auto *found = parsed.as_object().if_contains(key);
        if (found == nullptr) return "<absent>";
        return found->is_string() ? std::string(found->as_string()) : boost::json::serialize(*found);
    }

}// namespace

BOOST_AUTO_TEST_SUITE(AuditEventTest)

// ── Secrets do not reach the trail ──────────────────────────────────────────

BOOST_AUTO_TEST_CASE(APasswordIsReplacedAndItsFieldIsKept) {

    const auto redacted = Redact(R"({"userId":"jvo","password":"hunter2"})");

    // The auditable fact is that a password was set, by whom, on which user. The value is not part
    // of it and never was.
    BOOST_TEST(valueAt(redacted, "userId") == "jvo");
    BOOST_TEST(valueAt(redacted, "password") == "***");
    BOOST_TEST(redacted.find("hunter2") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(EverySpellingOfASecretIsCovered) {

    // Matched on substring rather than on exact names, because euclid spells the same secret four
    // ways across its DTOs and a rule that named each one would miss the fifth somebody adds.
    for (const auto *field: {"password", "newPassword", "oldPassword", "passwordHash",
                             "secret", "clientSecret", "token", "refreshToken",
                             "privateKey", "accessKey", "signature", "passphrase", "credentials"}) {

        const auto body = R"({")" + std::string(field) + R"(":"leaked"})";
        BOOST_TEST_CONTEXT("field " << field) {
            const bool hidden = Redact(body).find("leaked") == std::string::npos;
            BOOST_TEST(hidden);
        }
    }
}

BOOST_AUTO_TEST_CASE(CaseDoesNotLetASecretThrough) {

    // A DTO that serialises "Password" or "PRIVATEKEY" is still carrying the same thing.
    for (const auto *body: {R"({"Password":"leaked"})", R"({"PRIVATEKEY":"leaked"})", R"({"Client_Secret":"leaked"})"}) {
        BOOST_TEST_CONTEXT(body) {
            const bool hidden = Redact(body).find("leaked") == std::string::npos;
            BOOST_TEST(hidden);
        }
    }
}

BOOST_AUTO_TEST_CASE(ASecretNestedInsideTheBodyIsStillFound) {

    // euclid's DTOs nest freely, and a secret one level down is exactly as sensitive as one at the
    // top. Redacting only the top level would be the kind of half-measure that reads as safe.
    const auto redacted = Redact(R"({"user":{"name":"jvo","password":"hunter2"},"note":"keep"})");

    BOOST_TEST(redacted.find("hunter2") == std::string::npos);
    BOOST_TEST(redacted.find("keep") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ASecretInsideAnArrayIsStillFound) {

    const auto redacted = Redact(R"({"users":[{"name":"a","password":"one"},{"name":"b","password":"two"}]})");

    BOOST_TEST(redacted.find("one") == std::string::npos);
    BOOST_TEST(redacted.find("two") == std::string::npos);
    BOOST_TEST(redacted.find("\"a\"") != std::string::npos);
}

// ── What is left is still worth reading ─────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnOrdinaryBodySurvivesIntact) {

    // The whole point of storing parameters: an audit entry that could not say which bucket was
    // deleted would answer "somebody deleted something", which is not an answer.
    const auto redacted = Redact(R"({"ern":"ern:esm:bucket/reports","prefix":"2025/"})");

    BOOST_TEST(valueAt(redacted, "ern") == "ern:esm:bucket/reports");
    BOOST_TEST(valueAt(redacted, "prefix") == "2025/");
}

BOOST_AUTO_TEST_CASE(AnEmptyBodyStaysEmpty) {
    BOOST_TEST(Redact("").empty());
}

BOOST_AUTO_TEST_CASE(ANonJsonBodyIsRecordedAsItsSizeAlone) {

    // An upload's bytes are not an audit entry, and a body that is not JSON has no field structure
    // to tell a secret from anything else - so guessing is how one ends up storing a certificate.
    const auto redacted = Redact(std::string(4096, 'x'));

    BOOST_TEST(redacted.find("xxxx") == std::string::npos);
    BOOST_TEST(redacted.find("4096") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AMessageOrObjectBodyIsNotCopiedIntoTheTrail) {

    // "body" is the field a queue message and an object's bytes travel under. Not a secret, but
    // megabytes of it, and an audit row is not where it belongs.
    const auto redacted = Redact(R"({"queueErn":"ern:eqs:queue/orders","body":"...a megabyte of payload..."})");

    BOOST_TEST(valueAt(redacted, "queueErn") == "ern:eqs:queue/orders");
    BOOST_TEST(valueAt(redacted, "body") == "***");
}

// ── What gets recorded at all ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheReadRuleIsTheOneTheReaderRoleUses) {

    // EAD records the actions this does not match; the `reader` built-in role is every permission
    // it does. One rule, because an action misjudged here is both grantable to a reader and
    // invisible to the audit - the pair you least want to get wrong together.
    for (const auto *action: {"list-queues", "get-object", "describe-table", "count-objects"}) {
        BOOST_TEST_CONTEXT(action) { BOOST_TEST(Permissions::IsRead(action)); }
    }

    for (const auto *action: {"delete-bucket", "purge-bucket", "create-queue", "send-message",
                              "put-object", "resend-messages", "report-load"}) {
        BOOST_TEST_CONTEXT(action) { BOOST_TEST(!Permissions::IsRead(action)); }
    }
}

BOOST_AUTO_TEST_CASE(TheRuleAnswersForAFullPermissionAsWellAsABareAction) {

    // Dispatch has the bare action; a role holds "<module>:<action>". Both reach this.
    BOOST_TEST(Permissions::IsRead("esm:list-objects"));
    BOOST_TEST(!Permissions::IsRead("esm:delete-object"));
}

BOOST_AUTO_TEST_CASE(TheMonitoringModuleIsNeverRecorded) {

    // EMO is excluded as a whole module. Measured on the development installation before this
    // existed: 1,610 emo:list and 125 emo:push-metrics against a few hundred real commands - a
    // trail made mostly of the machinery, with what it was kept for pushed off the first page.
    for (const auto *action: {"list", "average", "push-metrics", "get-metrics"}) {
        BOOST_TEST_CONTEXT("emo:" << action) {
            BOOST_TEST(!Server::ShouldAudit("emo", action, 200));
        }
    }
}

BOOST_AUTO_TEST_CASE(NotOneEmoActionWouldBeFilteredOutOnItsOwn) {

    // Why excluding the module is needed rather than tidy: none of EMO's actions reads as a read,
    // so the ordinary rule would keep every one. push-metrics genuinely writes; "list" and
    // "average" miss the list-/get- rule for want of a hyphen.
    for (const auto *action: {"push-metrics", "list", "average"}) {
        BOOST_TEST_CONTEXT("emo action " << action) { BOOST_TEST(!Permissions::IsRead(action)); }
    }
}

BOOST_AUTO_TEST_CASE(MachineryStaysOutEvenWhenItIsRefused) {

    // Everything else non-2xx is recorded, because a refusal is the entry an audit exists for.
    // Machinery is the exception: a metrics push that starts failing fails on a timer too, and
    // recording those would turn one broken pusher into a flood.
    for (const long status: {403L, 404L, 500L, 503L}) {
        BOOST_TEST_CONTEXT("status " << status) {
            BOOST_TEST(!Server::ShouldAudit("emo", "push-metrics", status));
            BOOST_TEST(!Server::ShouldAudit("emd", "insert", status));
        }
    }
}

BOOST_AUTO_TEST_CASE(TheDocumentStoreIsNeverRecorded) {

    // EMD sits underneath every other module's every read and write, including the audit's own.
    // Its main() does not wire the sink, so this is belt and braces - but the sink went into
    // thirteen modules by copying one line, which is how emd would acquire it by accident.
    BOOST_TEST(!Server::ShouldAudit("emd", "insert", 200));
    BOOST_TEST(!Server::ShouldAudit("emd", "find-one", 200));
}

// ── What is recorded ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnythingThatChangesSomethingIsRecorded) {

    for (const auto *action: {"delete-bucket", "purge-bucket", "create-queue", "put-object"}) {
        BOOST_TEST_CONTEXT("esm:" << action) { BOOST_TEST(Server::ShouldAudit("esm", action, 200)); }
    }
}

BOOST_AUTO_TEST_CASE(ARefusalIsRecordedEvenThoughItOnlyTriedToRead) {

    // The entry an audit is kept for. A read somebody was not allowed to make is a 403, and a 403
    // that is not recorded looks exactly like a read nobody attempted.
    BOOST_TEST(Server::ShouldAudit("esm", "list-objects", 403));
    BOOST_TEST(Server::ShouldAudit("eqs", "get-queue-ern", 404));
}

BOOST_AUTO_TEST_CASE(ASuccessfulReadIsNotRecordedByDefault) {

    // On a working installation these are the overwhelming majority - one parse run is millions of
    // esm:get-object calls - so they are out unless euclid.modules.ead.audit-reads asks for them.
    BOOST_TEST(!Server::ShouldAudit("esm", "get-object", 200));
    BOOST_TEST(!Server::ShouldAudit("eqs", "list-queues", 200));
}

BOOST_AUTO_TEST_SUITE_END()
