// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/10/26.
//

// C++ includes
#include <algorithm>

// Euclid includes
#include <EkvServer.h>

namespace Euclid::EKV {

    using Database::Entity::EKV::Item;
    using Database::Entity::EKV::KeySchema;
    using Database::Entity::EKV::KeyType;
    using Database::Entity::EKV::Map;
    using Database::Entity::EKV::SortCondition;
    using Database::Entity::EKV::Table;
    using Database::Entity::EKV::Value;

    // Timer/counter names shared by every handler below - one series per action.
    constexpr auto kServiceTimer = "ekv-service-time";
    constexpr auto kServiceCounter = "ekv-service-count";

    namespace {

        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EkvServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EkvServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // How a table is described back to a caller. The item count is counted rather than kept: a
    // stored counter is a thing that drifts, and a count nobody can trust is worse than one that
    // costs a query.
    static Dto::EKV::TableDescription describe(const Table &table, const long itemCount) {

        Dto::EKV::TableDescription description;
        description.name = table.name;
        description.ern = table.ern;
        description.partitionKeyName = table.partitionKey.name;
        description.partitionKeyType = ToString(table.partitionKey.type);
        if (table.sortKey.has_value()) {
            description.sortKeyName = table.sortKey->name;
            description.sortKeyType = ToString(table.sortKey->type);
        }
        description.itemCount = itemCount;
        description.created = Core::DateTimeUtils::ToISO8601(table.created);
        description.modified = Core::DateTimeUtils::ToISO8601(table.modified);
        return description;
    }

    // An item as it goes back out: what was written, plus when. The timestamps are named with a
    // leading underscore so they cannot collide with an attribute - '$' and '.' are refused in
    // attribute names, but an underscore is not, so this is a convention rather than a guarantee
    // and is documented as one.
    static boost::json::value itemToJson(const Item &item) {

        boost::json::object object;
        for (const auto &[name, value]: item.attributes) object[name] = value.ToJson();
        object["_created"] = Core::DateTimeUtils::ToISO8601(item.created);
        object["_modified"] = Core::DateTimeUtils::ToISO8601(item.modified);
        return object;
    }

    // The table a request is about, or the response that says why there is none.
    //
    // Resolved in the caller's own account and namespace, the pair create-table built the table's
    // ERN from. Every item call below then works off the table this returns rather than off the
    // header again, so an item lands in the same namespace as the table it was addressed through.
    static std::optional<Table> tableFor(const request<string_body> &req, const AuthResult &auth, const std::string &name,
                                         std::optional<response<string_body> > &refusal) {

        if (name.empty()) {
            refusal = EkvServer::ErrorResponse(req, status::bad_request, "table is required");
            return std::nullopt;
        }

        const auto table = Database::RepositoryFactory::instance().ekvRepository()->findTable(
                auth.user->accountId, std::string(req["x-euclid-namespace"]), name);
        if (!table.has_value()) {
            refusal = EkvServer::ErrorResponse(req, status::not_found, "Table does not exist: " + name);
            return std::nullopt;
        }
        return table;
    }

    // Reads the key attributes a caller sent out of the object they sent them in, checking them
    // against what the table is keyed on.
    static bool readKey(const Table &table, const boost::json::value &key, Value &partitionKey,
                        std::optional<Value> &sortKey, std::string &error) {

        if (!key.is_object()) {
            error = "key has to be an object of the table's key attributes";
            return false;
        }

        Map attributes;
        try {
            attributes = Value::FromJson(key).get<Map>();
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }

        const auto partition = attributes.find(table.partitionKey.name);
        if (partition == attributes.end()) {
            error = "key has no '" + table.partitionKey.name + "' attribute, which is what '" + table.name + "' is keyed on";
            return false;
        }
        if (!Item::Matches(table.partitionKey, partition->second)) {
            error = "'" + table.partitionKey.name + "' has to be a " + ToString(table.partitionKey.type) +
                    ", and is a " + partition->second.TypeName();
            return false;
        }
        partitionKey = partition->second;

        if (table.sortKey.has_value()) {
            const auto sort = attributes.find(table.sortKey->name);
            if (sort == attributes.end()) {
                error = "key has no '" + table.sortKey->name + "' attribute, which this table is sorted by";
                return false;
            }
            if (!Item::Matches(*table.sortKey, sort->second)) {
                error = "'" + table.sortKey->name + "' has to be a " + ToString(table.sortKey->type) +
                        ", and is a " + sort->second.TypeName();
                return false;
            }
            sortKey = sort->second;
        }

        // Nothing but the key. An extra attribute here is a caller who thinks the table is keyed
        // on something it is not, and ignoring it would answer them with the wrong item rather
        // than telling them so.
        if (const std::size_t expected = table.sortKey.has_value() ? 2 : 1; attributes.size() > expected) {
            error = table.sortKey.has_value()
                            ? "'" + table.name + "' is keyed on '" + table.partitionKey.name + "' and '" + table.sortKey->name + "', and the key names more than those"
                            : "'" + table.name + "' has no sort key, so its key is '" + table.partitionKey.name + "' alone";
            return false;
        }
        return true;
    }

    // ── Tables ───────────────────────────────────────────────────────────────

    static response<string_body> handleCreateTable(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "create-table");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkvServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKV::CreateTableRequest>(jv);
        if (request.name.empty()) return EkvServer::ErrorResponse(req, status::bad_request, "name is required");
        if (request.partitionKeyName.empty()) return EkvServer::ErrorResponse(req, status::bad_request, "partitionKey is required");

        if (!Value::IsAcceptableAttributeName(request.partitionKeyName) ||
            (!request.sortKeyName.empty() && !Value::IsAcceptableAttributeName(request.sortKeyName))) {
            return EkvServer::ErrorResponse(req, status::bad_request,
                                            "A key attribute may not be empty, start with '$' or contain '.'");
        }
        if (!request.sortKeyName.empty() && request.sortKeyName == request.partitionKeyName) {
            return EkvServer::ErrorResponse(req, status::bad_request, "The partition key and the sort key have to be different attributes");
        }

        const auto partitionType = Database::Entity::EKV::KeyTypeFromString(request.partitionKeyType.empty() ? "string" : request.partitionKeyType);
        if (!partitionType.has_value()) {
            return EkvServer::ErrorResponse(req, status::bad_request, "partitionKeyType has to be string, number or binary");
        }
        const auto sortType = Database::Entity::EKV::KeyTypeFromString(request.sortKeyType.empty() ? "string" : request.sortKeyType);
        if (!request.sortKeyName.empty() && !sortType.has_value()) {
            return EkvServer::ErrorResponse(req, status::bad_request, "sortKeyType has to be string, number or binary");
        }

        const auto ns = std::string(req["x-euclid-namespace"]);

        const auto repository = Database::RepositoryFactory::instance().ekvRepository();
        if (repository->tableExists(auth.user->accountId, ns, request.name)) {
            return EkvServer::ErrorResponse(req, status::conflict, "Table exists already: " + request.name);
        }

        Table table;
        table.name = request.name;
        table.accountId = auth.user->accountId;
        table.region = auth.user->region;
        table.nameSpace = ns;
        table.ern = Core::createEkvTableErn(auth.user->accountId, ns, request.name);
        table.partitionKey = KeySchema{.name = request.partitionKeyName, .type = *partitionType};
        if (!request.sortKeyName.empty()) {
            table.sortKey = KeySchema{.name = request.sortKeyName, .type = *sortType};
        }
        table.created = table.modified = std::chrono::system_clock::now();

        const auto stored = repository->createTable(table);
        log_info << "EKV CreateTable, table: " << stored.name << ", partitionKey: " << stored.partitionKey.name
                 << (stored.sortKey.has_value() ? ", sortKey: " + stored.sortKey->name : "");

        return EkvServer::JsonResponse(req, status::created, describe(stored, 0).toJson());
    }

    static response<string_body> handleDescribeTable(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "describe-table");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkvServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKV::DescribeTableRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto table = tableFor(req, auth, request.name, refusal);
        if (!table.has_value()) return *refusal;

        const auto repository = Database::RepositoryFactory::instance().ekvRepository();
        return EkvServer::JsonResponse(req, status::ok, describe(*table, repository->countItems(table->accountId, table->nameSpace, table->name)).toJson());
    }

    static response<string_body> handleListTables(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "list-tables");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkvServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKV::ListTablesRequest>(jv);
        const auto repository = Database::RepositoryFactory::instance().ekvRepository();

        // One namespace's tables, not the account's: a listing that crossed namespaces would show
        // a caller tables they cannot address, since every other action here resolves a table name
        // in the namespace the request was made in.
        const auto ns = std::string(req["x-euclid-namespace"]);
        const auto tables = repository->listTables(auth.user->accountId, ns, request.prefix, request.pageSize,
                                                   request.pageIndex, request.sortColumn, request.sortDirection);

        boost::json::array described;
        described.reserve(tables.size());
        for (const auto &table: tables) {
            described.push_back(boost::json::parse(describe(table, repository->countItems(table.accountId, table.nameSpace, table.name)).toJson()));
        }

        return EkvServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{
                                               {"tables", described},
                                               {"total", repository->countTables(auth.user->accountId, ns)}}));
    }

    static response<string_body> handleDeleteTable(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-table");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkvServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKV::DeleteTableRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto table = tableFor(req, auth, request.name, refusal);
        if (!table.has_value()) return *refusal;

        const auto removed = Database::RepositoryFactory::instance().ekvRepository()->deleteTable(table->accountId, table->nameSpace, table->name);
        log_info << "EKV DeleteTable, table: " << table->name << ", items: " << removed;

        return EkvServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"deletedItems", removed}}));
    }

    // ── Items ────────────────────────────────────────────────────────────────

    static response<string_body> handlePutItem(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "put-item");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkvServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKV::PutItemRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto table = tableFor(req, auth, request.table, refusal);
        if (!table.has_value()) return *refusal;

        if (!request.item.is_object()) {
            return EkvServer::ErrorResponse(req, status::bad_request, "item has to be an object");
        }

        Map attributes;
        try {
            attributes = Value::FromJson(request.item).get<Map>();
        } catch (const std::exception &e) {
            return EkvServer::ErrorResponse(req, status::bad_request, e.what());
        }

        std::string error;
        auto item = Item::FromAttributes(*table, attributes, error);
        if (!item.has_value()) return EkvServer::ErrorResponse(req, status::bad_request, error);

        item->created = item->modified = std::chrono::system_clock::now();
        const auto stored = Database::RepositoryFactory::instance().ekvRepository()->putItem(*item);

        log_debug << "EKV PutItem, table: " << table->name;
        return EkvServer::JsonResponse(req, status::ok, boost::json::serialize(itemToJson(stored)));
    }

    static response<string_body> handleGetItem(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "get-item");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkvServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKV::GetItemRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto table = tableFor(req, auth, request.table, refusal);
        if (!table.has_value()) return *refusal;

        Value partitionKey;
        std::optional<Value> sortKey;
        std::string error;
        if (!readKey(*table, request.key, partitionKey, sortKey, error)) {
            return EkvServer::ErrorResponse(req, status::bad_request, error);
        }

        const auto item = Database::RepositoryFactory::instance().ekvRepository()->getItem(table->accountId, table->nameSpace, table->name, partitionKey, sortKey);
        if (!item.has_value()) {
            // A 404 rather than an empty answer: "there is no such item" and "here is an item with
            // nothing in it" are different, and a caller should not have to tell them apart.
            return EkvServer::ErrorResponse(req, status::not_found, "No such item");
        }

        return EkvServer::JsonResponse(req, status::ok, boost::json::serialize(itemToJson(*item)));
    }

    static response<string_body> handleDeleteItem(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "delete-item");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkvServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKV::DeleteItemRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto table = tableFor(req, auth, request.table, refusal);
        if (!table.has_value()) return *refusal;

        Value partitionKey;
        std::optional<Value> sortKey;
        std::string error;
        if (!readKey(*table, request.key, partitionKey, sortKey, error)) {
            return EkvServer::ErrorResponse(req, status::bad_request, error);
        }

        const auto deleted = Database::RepositoryFactory::instance().ekvRepository()->deleteItem(table->accountId, table->nameSpace, table->name, partitionKey, sortKey);
        log_debug << "EKV DeleteItem, table: " << table->name << ", deleted: " << deleted;

        return EkvServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"deleted", deleted}}));
    }

    static response<string_body> handleQuery(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "query");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkvServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKV::QueryRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto table = tableFor(req, auth, request.table, refusal);
        if (!table.has_value()) return *refusal;

        Value partitionKey;
        try {
            partitionKey = Value::FromJson(request.partitionKey);
        } catch (const std::exception &e) {
            return EkvServer::ErrorResponse(req, status::bad_request, e.what());
        }
        if (!Item::Matches(table->partitionKey, partitionKey)) {
            return EkvServer::ErrorResponse(req, status::bad_request,
                                            "partitionKey has to be a " + ToString(table->partitionKey.type) +
                                                    ", and is a " + partitionKey.TypeName());
        }

        const auto op = SortCondition::OperatorFromString(request.sortOperator);
        if (!op.has_value()) {
            return EkvServer::ErrorResponse(req, status::bad_request,
                                            "sortOperator has to be one of eq, lt, le, gt, ge, between, begins-with");
        }

        SortCondition condition;
        condition.op = *op;
        if (condition.op != SortCondition::Operator::None) {

            if (!table->sortKey.has_value()) {
                return EkvServer::ErrorResponse(req, status::bad_request, "'" + table->name + "' has no sort key to narrow by");
            }
            try {
                condition.value = Value::FromJson(request.sortValue);
                condition.upper = Value::FromJson(request.sortUpper);
            } catch (const std::exception &e) {
                return EkvServer::ErrorResponse(req, status::bad_request, e.what());
            }
            if (!Item::Matches(*table->sortKey, condition.value)) {
                return EkvServer::ErrorResponse(req, status::bad_request,
                                                "sortValue has to be a " + ToString(table->sortKey->type) +
                                                        ", and is a " + condition.value.TypeName());
            }
            if (condition.op == SortCondition::Operator::Between && !Item::Matches(*table->sortKey, condition.upper)) {
                return EkvServer::ErrorResponse(req, status::bad_request, "between needs a sortUpper of the sort key's type");
            }
            if (condition.op == SortCondition::Operator::BeginsWith && !condition.value.holds<std::string>()) {
                // The one operator that is not a comparison: a prefix of a number or of a blob is
                // not a thing, and pretending otherwise would answer with nonsense rather than an
                // error.
                return EkvServer::ErrorResponse(req, status::bad_request, "begins-with only applies to a string sort key");
            }
        }

        const auto items = Database::RepositoryFactory::instance().ekvRepository()->query(table->accountId, table->nameSpace, table->name, partitionKey,
                                                                                          condition, request.forward,
                                                                                          request.pageSize, request.pageIndex);

        boost::json::array array;
        array.reserve(items.size());
        for (const auto &item: items) array.push_back(itemToJson(item));

        return EkvServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{{"items", array}, {"count", static_cast<long>(items.size())}}));
    }

    static response<string_body> handleScan(const request<string_body> &req) {

        Core::Monitoring::MonitoringTimer measure(kServiceTimer, kServiceCounter, "method", "scan");

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);

        boost::json::value jv;
        if (const auto err = EkvServer::ParseJsonBody(req, jv)) return *err;

        const auto request = boost::json::value_to<Dto::EKV::ScanRequest>(jv);

        std::optional<response<string_body> > refusal;
        const auto table = tableFor(req, auth, request.table, refusal);
        if (!table.has_value()) return *refusal;

        const auto repository = Database::RepositoryFactory::instance().ekvRepository();
        const auto items = repository->scan(table->accountId, table->nameSpace, table->name, request.pageSize, request.pageIndex);

        boost::json::array array;
        array.reserve(items.size());
        for (const auto &item: items) array.push_back(itemToJson(item));

        return EkvServer::JsonResponse(req, status::ok,
                                       boost::json::serialize(boost::json::object{
                                               {"items", array},
                                               {"count", static_cast<long>(items.size())},
                                               {"total", repository->countItems(table->accountId, table->nameSpace, table->name)}}));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    namespace {
        enum class Action {
            Unknown,
            CreateTable,
            DescribeTable,
            ListTables,
            DeleteTable,
            PutItem,
            GetItem,
            DeleteItem,
            Query,
            Scan,
            GetMetrics
        };
    }

    static Action actionFromString(const std::string &action) {
        if (action == "create-table") return Action::CreateTable;
        if (action == "describe-table") return Action::DescribeTable;
        if (action == "list-tables") return Action::ListTables;
        if (action == "delete-table") return Action::DeleteTable;
        if (action == "put-item") return Action::PutItem;
        if (action == "get-item") return Action::GetItem;
        if (action == "delete-item") return Action::DeleteItem;
        if (action == "query") return Action::Query;
        if (action == "scan") return Action::Scan;
        if (action == "get-metrics") return Action::GetMetrics;
        return Action::Unknown;
    }

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EkvServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "EKV action=" << action;

        switch (actionFromString(action)) {

            case Action::CreateTable:
                return handleCreateTable(req);

            case Action::DescribeTable:
                return handleDescribeTable(req);

            case Action::ListTables:
                return handleListTables(req);

            case Action::DeleteTable:
                return handleDeleteTable(req);

            case Action::PutItem:
                return handlePutItem(req);

            case Action::GetItem:
                return handleGetItem(req);

            case Action::DeleteItem:
                return handleDeleteItem(req);

            case Action::Query:
                return handleQuery(req);

            case Action::Scan:
                return handleScan(req);

            case Action::GetMetrics:
                return EkvServer::MetricsResponse(req);

            case Action::Unknown:
            default:
                return EkvServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
        }
    }

    // ── EkvServer ────────────────────────────────────────────────────────────

    EkvServer::EkvServer(std::string socketPath, const int threads) : HttpActionServer("EKV", std::move(socketPath), threads) {}

    response<string_body> EkvServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::EKV
