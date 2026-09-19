// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// Euclid includes
#include <EadServer.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/entity/eam/User.h>

namespace Euclid::EAD {

    namespace {

        constexpr auto kServiceTimer = "ead-service-time";
        constexpr auto kServiceCounter = "ead-service-count";

        /**
         * @brief How far "list-events" counts before it answers with the ceiling instead.
         *
         * @par
         * Big enough that anything a person actually pages through is still counted exactly -
         * fifty to a page is two thousand pages - and small enough that the count costs the same
         * on a trail of a million events as on one of a hundred million.
         */
        constexpr long kListTotalLimit = 100000;

        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        std::string stringField(const boost::json::object &obj, const std::string &key) {
            if (const auto *v = obj.if_contains(key); v && v->is_string()) return v->as_string().c_str();
            return {};
        }

        long longField(const boost::json::object &obj, const std::string &key, const long fallback) {
            if (const auto *v = obj.if_contains(key); v && v->is_number()) return v->to_number<long>();
            return fallback;
        }

        boost::json::object toJson(const Database::Entity::EAD::AuditEvent &event) {
            return boost::json::object{
                    {"accountId", event.accountId},
                    {"namespace", event.nameSpace},
                    {"userId", event.userId},
                    {"module", event.moduleName},
                    {"command", event.command},
                    {"parameters", event.parameters},
                    {"status", event.status},
                    {"created", Core::DateTimeUtils::ToISO8601(event.created)}};
        }

    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EadServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EadServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // The account a read is confined to.
    //
    // Always the caller's own, never what the body asked for. An audit that let one account read
    // another's trail would hand over the shape of somebody else's installation - which buckets
    // they keep, who works there, what they do - and it would do it through the one module built
    // to be read by people looking for exactly that.
    static std::string scopeOf(const AuthResult &auth) {
        return auth.user.has_value() ? auth.user->accountId : std::string();
    }

    // ── Action handlers ──────────────────────────────────────────────────────

    static response<string_body> handleListEvents(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-events");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EadServer::ParseJsonBody(req, jv)) return *err;
        const auto body = jv.is_object() ? jv.as_object() : boost::json::object{};

        const auto userId = stringField(body, "userId");
        const auto moduleName = stringField(body, "module");
        const auto command = stringField(body, "command");
        const auto pageSize = longField(body, "pageSize", 10);
        const auto pageIndex = longField(body, "pageIndex", 0);

        const auto repo = Database::RepositoryFactory::instance().eadRepository();
        const auto accountId = scopeOf(auth);

        boost::json::array events;
        for (const auto &event: repo->listEvents(accountId, userId, moduleName, command, pageSize, pageIndex)) {
            events.push_back(toJson(event));
        }

        // Counted only as far as a page of results can need it. An exact total means reading an
        // index entry per matching event, and on a trail of tens of millions that is seconds of
        // database time spent beside a page of fifty - paid again on every page somebody turns.
        //
        // "totalExact" is what makes the capped answer honest rather than wrong: false means there
        // are at least this many, which is what a pager needs to know there is a next page. Ask
        // "count-events" for the real number.
        const auto total = repo->countEvents(accountId, userId, moduleName, command, kListTotalLimit);

        return EadServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{
                                               {"events", events},
                                               {"total", total},
                                               {"totalExact", total < kListTotalLimit}}));
    }

    static response<string_body> handleCountEvents(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "count-events");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EadServer::ParseJsonBody(req, jv)) return *err;
        const auto body = jv.is_object() ? jv.as_object() : boost::json::object{};

        const auto count = Database::RepositoryFactory::instance().eadRepository()->countEvents(
                scopeOf(auth), stringField(body, "userId"), stringField(body, "module"), stringField(body, "command"));

        return EadServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{{"count", count}}));
    }

    static response<string_body> handlePurgeEvents(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "purge-events");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EadServer::ParseJsonBody(req, jv)) return *err;
        const auto body = jv.is_object() ? jv.as_object() : boost::json::object{};

        const auto days = longField(body, "olderThanDays", 0);
        if (days <= 0) {
            return EadServer::ErrorResponse(req, status::bad_request,
                                            "olderThanDays is required and must be positive - a purge with no window "
                                            "would remove the whole trail");
        }

        const auto before = std::chrono::system_clock::now() - std::chrono::hours(24 * days);
        const auto removed = Database::RepositoryFactory::instance().eadRepository()->purgeEvents(before);

        // Logged as well as answered. A command that removes audit records is itself the kind of
        // thing an audit is kept for, and the trail it prunes is where its own entry lives.
        log_info << "EAD purge-events, user: " << auth.user->userId << ", olderThanDays: " << days
                 << ", removed: " << removed;

        return EadServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{{"removed", removed}}));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EadServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EAD action=" << action;

        if (action == "list-events") return handleListEvents(req);
        if (action == "count-events") return handleCountEvents(req);
        if (action == "purge-events") return handlePurgeEvents(req);
        if (action == "get-metrics") return EadServer::MetricsResponse(req);

        return EadServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

    // ── EadServer ────────────────────────────────────────────────────────────

    EadServer::EadServer(std::string socketPath, const int threads) : HttpActionServer("EAD", std::move(socketPath), threads) {}

    response<string_body> EadServer::DispatchAction(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EAD
