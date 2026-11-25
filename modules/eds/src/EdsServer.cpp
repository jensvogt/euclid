// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <EdsServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/SystemUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/dto/eds/CreateZoneRequest.h>
#include <euclid/dto/eds/GetAnswerRequest.h>
#include <euclid/dto/eds/ListRecordsRequest.h>
#include <euclid/dto/eds/ListZonesRequest.h>
#include <euclid/dto/eds/PutRecordRequest.h>
#include <euclid/dto/eds/RecordRequest.h>
#include <euclid/dto/eds/UpdateRecordRequest.h>
#include <euclid/dto/eds/UpdateZoneRequest.h>
#include <euclid/dto/eds/ZoneRequest.h>

namespace Euclid::EDS {

    using Database::Entity::EDS::HostedZone;
    using Database::Entity::EDS::Record;

    constexpr auto kServiceTimer = "eds-service-time";
    constexpr auto kServiceCounter = "eds-service-count";

    // The longest TTL RFC 2181 §8 allows.
    constexpr long kMaxTtl = 2147483647;

    // The TTL of a zone created without one.
    constexpr long kDefaultTtl = 300;

    namespace {

        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
            bool unknownSubject{false};
        };

    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EdsServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        // Resolved rather than looked up - see Database::ResolveCaller and EkvServer.
        auto user = Database::ResolveCaller(*auth.subject, req);
        const auto unknown = !user.has_value();
        return {.user = std::move(user), .unknownSubject = unknown};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EdsServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason, .unknownSubject = auth.unknownSubject});
    }

    static std::string serviceDomain() {
        return Dns::Normalize(Core::Configuration::instance().getOr<std::string>("euclid.modules.eds.service-domain", "euclid.internal"));
    }

    // An owner name as a caller writes it, made fully qualified within the zone: "@" or nothing is
    // the apex, a name with a trailing dot is taken as written, and anything else is relative to
    // the zone unless it already ends in it.
    static std::optional<std::string> ownerName(const std::string &zone, const std::string &input, std::string &error) {

        if (input.empty() || input == "@") return zone;

        std::string name;
        if (input.ends_with('.')) {
            name = Dns::Normalize(input);
        } else {
            const auto normalized = Dns::Normalize(input);
            name = Dns::IsInZone(normalized, zone) ? normalized : normalized + "." + zone;
        }

        if (!Dns::IsValidName(name)) {
            error = "not a valid domain name: " + input;
            return std::nullopt;
        }
        if (!Dns::IsInZone(name, zone)) {
            error = "'" + input + "' is not in the zone " + zone;
            return std::nullopt;
        }
        return name;
    }

    static boost::json::object describeZone(const HostedZone &zone, const long recordCount) {
        return boost::json::object{
                {"name", zone.name},
                {"ern", zone.ern},
                {"namespace", zone.nameSpace},
                {"ttl", zone.ttl},
                {"description", zone.description},
                {"recordCount", recordCount},
                {"serial", std::max<long long>(1, std::chrono::duration_cast<std::chrono::seconds>(zone.modified.time_since_epoch()).count())},
                {"created", Core::DateTimeUtils::ToISO8601(zone.created)},
                {"modified", Core::DateTimeUtils::ToISO8601(zone.modified)},
        };
    }

    static boost::json::object describeRecord(const Record &record) {
        boost::json::array values;
        for (const auto &value: record.values) values.emplace_back(value);
        return boost::json::object{
                {"zone", record.zoneName},
                {"name", record.name},
                {"type", record.type},
                {"ttl", record.ttl},
                {"values", values},
                {"created", Core::DateTimeUtils::ToISO8601(record.created)},
                {"modified", Core::DateTimeUtils::ToISO8601(record.modified)},
        };
    }

    static boost::json::array describeRecords(const std::vector<Dns::ResourceRecord> &records) {
        boost::json::array array;
        for (const auto &record: records) {
            array.emplace_back(boost::json::object{
                    {"name", record.name},
                    {"type", Dns::TypeToString(record.type)},
                    {"ttl", record.ttl},
                    {"value", Dns::RdataToString(record.type, record.rdata)},
            });
        }
        return array;
    }

    // The zone a request names, if the caller owns it and may touch it.
    //
    // A zone of another account answers 404, as one that does not exist does: the caller may not
    // change it either way, and saying "it exists, but not for you" only tells them who else is
    // using the installation.
    static std::optional<HostedZone> zoneFor(const request<string_body> &req, const AuthResult &auth, const std::string &input,
                                             std::optional<response<string_body> > &refusal) {

        const auto name = Dns::Normalize(input);
        if (name.empty()) {
            refusal = EdsServer::ErrorResponse(req, status::bad_request, "zone is required");
            return std::nullopt;
        }

        const auto zone = Database::RepositoryFactory::instance().edsRepository()->findZone(name);
        if (!zone.has_value() || zone->accountId != auth.user->accountId) {
            refusal = EdsServer::ErrorResponse(req, status::not_found, "Zone does not exist: " + name);
            return std::nullopt;
        }
        if (auto denied = EdsServer::AuthorizeResource(req, zone->ern)) {
            log_warning << "EDS resource denied, userId: " << auth.user->userId << ", zone: " << name;
            refusal = std::move(denied);
            return std::nullopt;
        }
        return zone;
    }

    // ── Zones ────────────────────────────────────────────────────────────────

    static response<string_body> handleCreateZone(const request<string_body> &req, Catalog &catalog) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-zone");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::CreateZoneRequest>(jv);

        const auto name = Dns::Normalize(request.name);
        if (name.empty()) return EdsServer::ErrorResponse(req, status::bad_request, "name is required");
        if (!Dns::IsValidName(name) || name.starts_with("*.") || name.find('*') != std::string::npos) {
            return EdsServer::ErrorResponse(req, status::bad_request, "not a valid zone name: " + request.name);
        }
        if (request.ttl < 0 || request.ttl > kMaxTtl) {
            return EdsServer::ErrorResponse(req, status::bad_request, "ttl has to be between 0 and 2147483647 seconds");
        }

        // The service names are made, not stored; a zone in their domain would either hide them
        // or be hidden by them, depending on which is more specific.
        if (const auto services = serviceDomain(); !services.empty() && Dns::IsInZone(name, services)) {
            return EdsServer::ErrorResponse(req, status::bad_request, "'" + services + "' holds the names euclid gives what it runs, and cannot have zones of its own");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto ern = Core::createEdsZoneErn(auth.user->accountId, ns, name);
        if (auto denied = EdsServer::AuthorizeResource(req, ern)) return *denied;

        const auto repository = Database::RepositoryFactory::instance().edsRepository();
        if (repository->findZone(name).has_value()) {
            return EdsServer::ErrorResponse(req, status::conflict, "Zone exists already: " + name);
        }

        HostedZone zone;
        zone.name = name;
        zone.ern = ern;
        zone.accountId = auth.user->accountId;
        zone.region = auth.user->region;
        zone.nameSpace = ns;
        zone.ttl = request.ttl > 0 ? request.ttl : kDefaultTtl;
        zone.description = request.description;
        zone.created = zone.modified = std::chrono::system_clock::now();

        HostedZone stored;
        try {
            stored = repository->createZone(zone);
        } catch (const std::exception &) {
            // The unique index refused it: somebody created it between the check and the insert.
            return EdsServer::ErrorResponse(req, status::conflict, "Zone exists already: " + name);
        }
        catalog.Invalidate();

        log_info << "EDS CreateZone, zone: " << stored.name << ", accountId: " << stored.accountId;
        return EdsServer::JsonResponse(req, status::created, boost::json::serialize(describeZone(stored, 0)));
    }

    static response<string_body> handleGetZone(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-zone");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::ZoneRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto zone = zoneFor(req, auth, request.name, refusal);
        if (!zone.has_value()) return *refusal;

        const auto count = Database::RepositoryFactory::instance().edsRepository()->countRecords(zone->name, {}, {});
        return EdsServer::JsonResponse(req, status::ok, boost::json::serialize(describeZone(*zone, count)));
    }

    static response<string_body> handleListZones(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-zones");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::ListZonesRequest>(jv);

        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto repository = Database::RepositoryFactory::instance().edsRepository();
        const auto prefix = Dns::Normalize(request.prefix);

        boost::json::array zones;
        for (const auto &zone: repository->listZones(auth.user->accountId, ns, prefix, request.pageSize, request.pageIndex)) {
            // Listed only when it may be read, so a grant naming one zone lists that one.
            if (!EdsServer::IsResourceAuthorized(req, zone.ern)) continue;
            zones.emplace_back(describeZone(zone, repository->countRecords(zone.name, {}, {})));
        }

        return EdsServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{{"zones", zones},
                                                                                  {"total", repository->countZones(auth.user->accountId, ns, prefix)}}));
    }

    // Changes what a zone says about itself - its TTL and description - and nothing else. Only the
    // fields the request carries are changed, so "ttl" can be set without resending the
    // description, and an empty description clears it rather than being taken as "not given".
    static response<string_body> handleUpdateZone(const request<string_body> &req, Catalog &catalog) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "update-zone");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::UpdateZoneRequest>(jv);

        std::optional<response<string_body> > refusal;
        auto zone = zoneFor(req, auth, request.name, refusal);
        if (!zone.has_value()) return *refusal;

        const bool ttlGiven = Core::AttributeExists(jv, "ttl");
        const bool descriptionGiven = Core::AttributeExists(jv, "description");
        if (!ttlGiven && !descriptionGiven) {
            return EdsServer::ErrorResponse(req, status::bad_request, "nothing to change; give ttl, description or both");
        }

        if (ttlGiven) {
            // Not 0 here, unlike create-zone where 0 means "the default": an update that set a zone
            // to 300 because somebody typed 0 would be a change nobody asked for.
            if (request.ttl < 1 || request.ttl > kMaxTtl) {
                return EdsServer::ErrorResponse(req, status::bad_request, "ttl has to be between 1 and 2147483647 seconds");
            }
            zone->ttl = request.ttl;
        }
        if (descriptionGiven) zone->description = request.description;

        // Modified moves even when only the description changed. The SOA serial is made from it,
        // and the zone's TTL is in the SOA (its negative-caching field), so a TTL change is a
        // change to what the server answers - and a serial that did not move would hide it.
        zone->modified = std::chrono::system_clock::now();

        const auto repository = Database::RepositoryFactory::instance().edsRepository();
        if (!repository->updateZone(*zone)) {
            // Deleted between the lookup and the write.
            return EdsServer::ErrorResponse(req, status::not_found, "Zone does not exist: " + zone->name);
        }
        catalog.Invalidate();

        log_info << "EDS UpdateZone, zone: " << zone->name << (ttlGiven ? ", ttl: " + std::to_string(zone->ttl) : std::string{})
                 << (descriptionGiven ? ", description changed" : "");
        return EdsServer::JsonResponse(req, status::ok, boost::json::serialize(describeZone(*zone, repository->countRecords(zone->name, {}, {}))));
    }

    static response<string_body> handleDeleteZone(const request<string_body> &req, Catalog &catalog) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-zone");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::ZoneRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto zone = zoneFor(req, auth, request.name, refusal);
        if (!zone.has_value()) return *refusal;

        const auto removed = Database::RepositoryFactory::instance().edsRepository()->deleteZone(zone->name);
        catalog.Invalidate();

        log_info << "EDS DeleteZone, zone: " << zone->name << ", records: " << removed;
        return EdsServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"deletedRecords", removed}}));
    }

    // ── Records ──────────────────────────────────────────────────────────────

    static response<string_body> handlePutRecord(const request<string_body> &req, Catalog &catalog) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "put-record");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::PutRecordRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto zone = zoneFor(req, auth, request.zone, refusal);
        if (!zone.has_value()) return *refusal;

        std::string error;
        const auto name = ownerName(zone->name, request.name, error);
        if (!name.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, error);

        const auto type = Dns::TypeFromString(request.type);
        if (!type.has_value() || !Dns::IsStorableType(*type)) {
            return EdsServer::ErrorResponse(req, status::bad_request, "type has to be one of A, AAAA, CNAME, MX, NS, PTR, SRV, TXT");
        }
        if (request.ttl < 0 || request.ttl > kMaxTtl) {
            return EdsServer::ErrorResponse(req, status::bad_request, "ttl has to be between 0 and 2147483647 seconds");
        }
        if (request.values.empty()) {
            return EdsServer::ErrorResponse(req, status::bad_request, "values is required; delete-record removes a record set");
        }
        // Encoded now as well as when the zone is loaded, so a value the server could not serve
        // is refused to the person typing it, not logged later where nobody reads it.
        for (const auto &value: request.values) {
            if (!Dns::EncodeRdata(*type, value, error).has_value()) return EdsServer::ErrorResponse(req, status::bad_request, error);
        }

        const auto repository = Database::RepositoryFactory::instance().edsRepository();
        const auto typeName = Dns::TypeToString(*type);

        // A CNAME is the only record at its name (RFC 1034 §3.6.2): it says "this name is that
        // other one", which no other record could then be about.
        if (*type == Dns::Type::CNAME) {
            if (request.values.size() != 1) return EdsServer::ErrorResponse(req, status::bad_request, "a CNAME has exactly one value");
            if (*name == zone->name) return EdsServer::ErrorResponse(req, status::bad_request, "the zone apex cannot be a CNAME; it holds the zone's SOA and NS");
            for (const auto &other: repository->findRecords(zone->name, *name)) {
                if (other.type != typeName) {
                    return EdsServer::ErrorResponse(req, status::conflict, *name + " has " + other.type + " records, and a CNAME cannot sit beside them");
                }
            }
        } else {
            for (const auto &other: repository->findRecords(zone->name, *name)) {
                if (other.type == "CNAME") {
                    return EdsServer::ErrorResponse(req, status::conflict, *name + " is a CNAME, and nothing else can sit beside it");
                }
            }
        }

        const auto now = std::chrono::system_clock::now();
        Record record;
        record.accountId = zone->accountId;
        record.zoneName = zone->name;
        record.name = *name;
        record.type = typeName;
        record.ttl = request.ttl;
        record.values = request.values;
        record.created = record.modified = now;

        const auto stored = repository->putRecord(record);
        repository->touchZone(zone->name, now);
        catalog.Invalidate();

        log_info << "EDS PutRecord, name: " << stored.name << ", type: " << stored.type << ", values: " << stored.values.size();
        return EdsServer::JsonResponse(req, status::ok, boost::json::serialize(describeRecord(stored)));
    }

    // Changes part of a record set without resending the rest of it: its TTL, values removed and
    // values added. Removal comes first, so one request can swap one address for another. Values are
    // compared encoded rather than as typed, so "Host.Example.COM." removes "host.example.com".
    static response<string_body> handleUpdateRecord(const request<string_body> &req, Catalog &catalog) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "update-record");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::UpdateRecordRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto zone = zoneFor(req, auth, request.zone, refusal);
        if (!zone.has_value()) return *refusal;

        std::string error;
        const auto name = ownerName(zone->name, request.name, error);
        if (!name.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, error);

        const auto type = Dns::TypeFromString(request.type);
        if (!type.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, "type is required");
        const auto typeName = Dns::TypeToString(*type);

        const bool ttlGiven = Core::AttributeExists(jv, "ttl");
        if (!ttlGiven && request.addValues.empty() && request.removeValues.empty()) {
            return EdsServer::ErrorResponse(req, status::bad_request, "nothing to change; give ttl, addValues, removeValues or a combination");
        }
        // 0 is allowed here, unlike for a zone: on a record it means "the zone's TTL", which is a
        // setting a record can be put back to.
        if (ttlGiven && (request.ttl < 0 || request.ttl > kMaxTtl)) {
            return EdsServer::ErrorResponse(req, status::bad_request, "ttl has to be between 0 (the zone's) and 2147483647 seconds");
        }

        const auto repository = Database::RepositoryFactory::instance().edsRepository();
        std::optional<Record> record;
        for (auto &found: repository->findRecords(zone->name, *name)) {
            if (found.type == typeName) record = std::move(found);
        }
        if (!record.has_value()) {
            return EdsServer::ErrorResponse(req, status::not_found, "No " + typeName + " records at " + *name + "; put-record creates them");
        }

        const auto encode = [&](const std::string &value) { return Dns::EncodeRdata(*type, value, error); };

        // Removals first, each of which has to be there: a typo that removed nothing and said
        // "done" would leave the address somebody meant to take out still being served.
        std::vector<std::string> values = record->values;
        for (const auto &value: request.removeValues) {
            const auto target = encode(value);
            if (!target.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, error);
            const auto match = std::ranges::find_if(values, [&](const std::string &stored) {
                std::string ignored;
                return Dns::EncodeRdata(*type, stored, ignored) == target;
            });
            if (match == values.end()) {
                return EdsServer::ErrorResponse(req, status::bad_request, "'" + value + "' is not one of the " + typeName + " values at " + *name);
            }
            values.erase(match);
        }

        // Additions, each checked as put-record checks it. One already there is not added twice.
        for (const auto &value: request.addValues) {
            const auto added = encode(value);
            if (!added.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, error);
            const bool present = std::ranges::any_of(values, [&](const std::string &stored) {
                std::string ignored;
                return Dns::EncodeRdata(*type, stored, ignored) == added;
            });
            if (!present) values.push_back(value);
        }

        if (values.empty()) {
            return EdsServer::ErrorResponse(req, status::bad_request, "that would leave " + *name + " with no " + typeName + " values; delete-record removes a record set");
        }
        if (*type == Dns::Type::CNAME && values.size() != 1) {
            return EdsServer::ErrorResponse(req, status::bad_request, "a CNAME has exactly one value; give the new one with addValues and the old one with removeValues");
        }

        const auto now = std::chrono::system_clock::now();
        record->values = std::move(values);
        if (ttlGiven) record->ttl = request.ttl;
        record->modified = now;

        const auto stored = repository->putRecord(*record);
        repository->touchZone(zone->name, now);
        catalog.Invalidate();

        log_info << "EDS UpdateRecord, name: " << stored.name << ", type: " << stored.type << ", values: " << stored.values.size()
                 << (ttlGiven ? ", ttl: " + std::to_string(stored.ttl) : std::string{});
        return EdsServer::JsonResponse(req, status::ok, boost::json::serialize(describeRecord(stored)));
    }

    static response<string_body> handleDeleteRecord(const request<string_body> &req, Catalog &catalog) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-record");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::RecordRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto zone = zoneFor(req, auth, request.zone, refusal);
        if (!zone.has_value()) return *refusal;

        std::string error;
        const auto name = ownerName(zone->name, request.name, error);
        if (!name.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, error);

        const auto type = Dns::TypeFromString(request.type);
        if (!type.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, "type is required");

        const auto repository = Database::RepositoryFactory::instance().edsRepository();
        if (!repository->deleteRecord(zone->name, *name, Dns::TypeToString(*type))) {
            return EdsServer::ErrorResponse(req, status::not_found, "No " + Dns::TypeToString(*type) + " records at " + *name);
        }
        repository->touchZone(zone->name, std::chrono::system_clock::now());
        catalog.Invalidate();

        log_info << "EDS DeleteRecord, name: " << *name << ", type: " << Dns::TypeToString(*type);
        return EdsServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"deleted", true}}));
    }

    static response<string_body> handleListRecords(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-records");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::ListRecordsRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto zone = zoneFor(req, auth, request.zone, refusal);
        if (!zone.has_value()) return *refusal;

        std::string name;
        if (!request.name.empty()) {
            std::string error;
            const auto owner = ownerName(zone->name, request.name, error);
            if (!owner.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, error);
            name = *owner;
        }
        std::string type;
        if (!request.type.empty()) {
            const auto parsed = Dns::TypeFromString(request.type);
            if (!parsed.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, "unknown type: " + request.type);
            type = Dns::TypeToString(*parsed);
        }

        const auto repository = Database::RepositoryFactory::instance().edsRepository();
        boost::json::array records;
        for (const auto &record: repository->listRecords(zone->name, name, type, request.pageSize, request.pageIndex)) {
            records.emplace_back(describeRecord(record));
        }
        return EdsServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{{"records", records},
                                                                                  {"total", repository->countRecords(zone->name, name, type)}}));
    }

    // ── Lookups ──────────────────────────────────────────────────────────────

    // What the name server would answer, without a DNS client. Not resource-checked: the same
    // answer is given to anybody who sends the question to the DNS port.
    static response<string_body> handleGetAnswer(const request<string_body> &req, const Catalog &catalog) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-answer");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EdsServer::ParseJsonBody(req, jv)) return *err;
        const auto request = boost::json::value_to<Dto::EDS::GetAnswerRequest>(jv);

        if (request.name.empty()) return EdsServer::ErrorResponse(req, status::bad_request, "name is required");
        const auto type = Dns::TypeFromString(request.type.empty() ? "A" : request.type);
        if (!type.has_value()) return EdsServer::ErrorResponse(req, status::bad_request, "unknown type: " + request.type);

        const auto zones = catalog.Current();
        const auto answer = Resolve(*zones, request.name, *type);

        boost::json::object result{{"name", request.name}, {"type", Dns::TypeToString(*type)}};
        if (!answer.has_value()) {
            // Not ours. The DNS port would forward it or refuse it; this says which zone is missing.
            result["authoritative"] = false;
            result["rcode"] = "REFUSED";
            result["answers"] = boost::json::array{};
            return EdsServer::JsonResponse(req, status::ok, boost::json::serialize(result));
        }

        const auto *zone = zones->Find(Dns::Normalize(request.name));
        result["authoritative"] = true;
        result["zone"] = zone != nullptr ? zone->name : std::string{};
        result["rcode"] = answer->rcode == Dns::Rcode::NoError    ? "NOERROR"
                          : answer->rcode == Dns::Rcode::NxDomain ? "NXDOMAIN"
                                                                  : "SERVFAIL";
        result["answers"] = describeRecords(answer->answers);
        result["authority"] = describeRecords(answer->authority);
        return EdsServer::JsonResponse(req, status::ok, boost::json::serialize(result));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    static response<string_body> dispatch(const request<string_body> &req, Catalog &catalog) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EdsServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EDS action=" << action;

        if (action == "create-zone") return handleCreateZone(req, catalog);
        if (action == "get-zone") return handleGetZone(req);
        if (action == "list-zones") return handleListZones(req);
        if (action == "update-zone") return handleUpdateZone(req, catalog);
        if (action == "delete-zone") return handleDeleteZone(req, catalog);
        if (action == "put-record") return handlePutRecord(req, catalog);
        if (action == "update-record") return handleUpdateRecord(req, catalog);
        if (action == "delete-record") return handleDeleteRecord(req, catalog);
        if (action == "list-records") return handleListRecords(req);
        if (action == "get-answer") return handleGetAnswer(req, catalog);
        if (action == "get-metrics") return EdsServer::MetricsResponse(req);

        return EdsServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

    // ── EdsServer ────────────────────────────────────────────────────────────

    EdsServer::EdsServer(std::string socketPath, const int threads) : HttpActionServer("EDS", std::move(socketPath), threads) {

        const auto &configuration = Core::Configuration::instance();

        CatalogOptions catalogOptions;
        catalogOptions.serviceDomain = serviceDomain();
        catalogOptions.managerAddress = configuration.getOr<std::string>("euclid.modules.eds.manager-address", "");
        if (catalogOptions.managerAddress.empty()) {
            // The first address another host could reach this one at; IPv4 first. Loopback when
            // there is none, which is right for a single host and says so in the zone.
            const auto addresses = Core::SystemUtils::GetLocalAddresses();
            catalogOptions.managerAddress = addresses.empty() ? "127.0.0.1" : addresses.front();
        }
        catalogOptions.primaryNs = Dns::Normalize(configuration.getOr<std::string>("euclid.modules.eds.nameserver", "localhost"));
        catalogOptions.serviceTtl = static_cast<std::uint32_t>(std::clamp<long>(configuration.getOr<long>("euclid.modules.eds.service-ttl", 5), 0, kMaxTtl));
        catalogOptions.refresh = std::chrono::seconds{std::max(1L, configuration.getOr<long>("euclid.modules.eds.refresh-seconds", 5))};
        catalogOptions.nodeLease = std::chrono::seconds{std::max(1L, configuration.getOr<long>("euclid.modules.eap.node-lease-seconds", 45))};

        _catalog = std::make_unique<Catalog>(catalogOptions);
        _catalog->Start();

        ListenerOptions listenerOptions;
        listenerOptions.address = configuration.getOr<std::string>("euclid.modules.eds.address", "0.0.0.0");
        listenerOptions.port = static_cast<unsigned short>(configuration.getOr<long>("euclid.modules.eds.port", 1053));
        listenerOptions.ioThreads = static_cast<int>(configuration.getOr<long>("euclid.modules.eds.io-threads", 2));
        listenerOptions.workerThreads = static_cast<int>(configuration.getOr<long>("euclid.modules.eds.worker-threads", 8));
        listenerOptions.forwardTimeout = std::chrono::milliseconds{std::max(100L, configuration.getOr<long>("euclid.modules.eds.forward-timeout-ms", 2000))};
        listenerOptions.tcpIdleTimeout = std::chrono::seconds{std::max(1L, configuration.getOr<long>("euclid.modules.eds.tcp-idle-seconds", 10))};
        try {
            if (configuration.has("euclid.modules.eds.forwarders")) {
                listenerOptions.forwarders = configuration.getArray<std::string>("euclid.modules.eds.forwarders");
            }
            if (configuration.has("euclid.modules.eds.allow-recursion")) {
                listenerOptions.allowRecursion = configuration.getArray<std::string>("euclid.modules.eds.allow-recursion");
            }
        } catch (const std::exception &e) {
            log_error << "EDS forwarders or allow-recursion are not a list of strings, nothing is forwarded, error: " << e.what();
            listenerOptions.forwarders.clear();
        }

        log_info << "EDS service names, domain: " << (catalogOptions.serviceDomain.empty() ? "(off)" : catalogOptions.serviceDomain)
                 << ", manager: " << catalogOptions.managerAddress;

        _listener = std::make_unique<DnsListener>(*_catalog, listenerOptions);
        try {
            _listener->Start();
        } catch (const std::exception &e) {
            log_error << "EDS could not listen on " << listenerOptions.address << ":" << listenerOptions.port
                      << " - zones can be managed but no DNS query is answered, error: " << e.what();
        }
    }

    EdsServer::~EdsServer() {
        if (_listener) _listener->Stop();
        if (_catalog) _catalog->Stop();
    }

    response<string_body> EdsServer::DispatchAction(const request<string_body> &req) {
        return dispatch(req, *_catalog);
    }

}// namespace Euclid::EDS
