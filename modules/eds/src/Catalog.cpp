// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

// Boost includes
#include <boost/asio/ip/address.hpp>

// Euclid includes
#include <Catalog.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EDS {

    namespace {

        std::uint32_t secondsOf(const std::chrono::system_clock::time_point when) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
            // A serial of 0 reads as "never loaded" to some tools; a zone always has at least 1.
            return static_cast<std::uint32_t>(std::max<long long>(1, seconds));
        }

        // An address as an A or AAAA record, or nothing when it is a host name - which a node may
        // have registered with, and which DNS cannot put in an address record.
        void addAddress(Zone &zone, const std::string &owner, const std::string &address, const std::uint32_t ttl) {
            boost::system::error_code ec;
            const auto parsed = boost::asio::ip::make_address(address, ec);
            if (ec) return;
            if (parsed.is_v4()) {
                const auto bytes = parsed.to_v4().to_bytes();
                zone.Add(owner, Dns::Type::A, ttl, {bytes.begin(), bytes.end()});
            } else {
                const auto bytes = parsed.to_v6().to_bytes();
                zone.Add(owner, Dns::Type::AAAA, ttl, {bytes.begin(), bytes.end()});
            }
        }

        Zone serviceZone(const ServiceNames &services, const CatalogOptions &options) {

            const auto &domain = options.serviceDomain;
            Zone zone;
            zone.name = domain;
            zone.ttl = options.serviceTtl;
            // Rebuilt every refresh, and nothing compares its serial; the time is as good as any
            // and moves forward.
            zone.serial = secondsOf(std::chrono::system_clock::now());
            zone.primaryNs = options.primaryNs;

            const auto manager = "manager." + domain;
            addAddress(zone, manager, options.managerAddress, options.serviceTtl);

            std::map<std::string, std::string> nodeHosts;
            for (const auto &node: services.nodes) {
                const auto label = ToLabel(node.name);
                if (label.empty()) continue;
                const auto host = label + ".node." + domain;
                nodeHosts[node.name] = host;
                addAddress(zone, host, node.address, options.serviceTtl);
            }

            for (const auto &instance: services.instances) {
                const auto label = ToLabel(instance.application);
                if (label.empty()) continue;
                const auto owner = label + ".app." + domain;
                addAddress(zone, owner, instance.address, options.serviceTtl);

                if (instance.port <= 0 || instance.port > 65535) continue;
                // The SRV target has to be a name with an address record, never an address
                // (RFC 2782) - which is what the node and manager names are for.
                const auto node = nodeHosts.find(instance.node);
                const auto target = instance.node.empty() ? manager : node != nodeHosts.end() ? node->second : std::string{};
                if (target.empty()) continue;

                std::string error;
                if (auto rdata = Dns::EncodeRdata(Dns::Type::SRV, "0 1 " + std::to_string(instance.port) + " " + target, error)) {
                    zone.Add("_http._tcp." + owner, Dns::Type::SRV, options.serviceTtl, std::move(*rdata));
                }
            }
            return zone;
        }

    }// namespace

    std::string ToLabel(const std::string &name) {

        std::string label;
        for (const char ch: name) {
            // Through unsigned char: <cctype> is undefined for a negative char, which any byte
            // above 0x7F is where char is signed.
            const auto c = static_cast<unsigned char>(ch);
            if (std::isalnum(c) != 0) label += static_cast<char>(std::tolower(c));
            else label += '-';
        }
        const auto first = label.find_first_not_of('-');
        if (first == std::string::npos) return {};
        label = label.substr(first, label.find_last_not_of('-') - first + 1);
        return label.size() > 63 ? std::string{} : label;
    }

    ZoneSet BuildZoneSet(const std::vector<Database::Entity::EDS::HostedZone> &zones,
                         const std::vector<Database::Entity::EDS::Record> &records,
                         const ServiceNames &services, const CatalogOptions &options,
                         std::vector<std::string> &problems) {

        std::map<std::string, std::vector<const Database::Entity::EDS::Record *> > byZone;
        for (const auto &record: records) byZone[record.zoneName].push_back(&record);

        ZoneSet set;
        for (const auto &stored: zones) {

            Zone zone;
            zone.name = stored.name;
            zone.ttl = static_cast<std::uint32_t>(std::clamp<long>(stored.ttl, 0, 2147483647));
            zone.serial = secondsOf(stored.modified);
            zone.primaryNs = options.primaryNs;

            for (const auto *record: byZone[stored.name]) {

                const auto type = Dns::TypeFromString(record->type);
                if (!type.has_value() || !Dns::IsStorableType(*type)) {
                    problems.push_back("unknown type " + record->type + " at " + record->name);
                    continue;
                }
                const auto ttl = record->ttl > 0 ? static_cast<std::uint32_t>(std::min<long>(record->ttl, 2147483647)) : zone.ttl;

                for (const auto &value: record->values) {
                    std::string error;
                    auto rdata = Dns::EncodeRdata(*type, value, error);
                    if (!rdata.has_value()) {
                        problems.push_back(record->name + " " + record->type + ": " + error);
                        continue;
                    }
                    zone.Add(record->name, *type, ttl, std::move(*rdata), *type == Dns::Type::CNAME ? Dns::Normalize(value) : std::string{});
                }
            }
            set.Add(std::move(zone));
        }

        if (!options.serviceDomain.empty()) set.Add(serviceZone(services, options));
        return set;
    }

    Catalog::Catalog(CatalogOptions options) : _options(std::move(options)) {
        _snapshot = std::make_shared<const ZoneSet>();
    }

    Catalog::~Catalog() {
        Stop();
    }

    void Catalog::Start() {
        reload();
        _thread = std::thread([this] { run(); });
    }

    void Catalog::Stop() {
        {
            std::lock_guard lock(_wakeMutex);
            _stopping = true;
        }
        _wake.notify_all();
        if (_thread.joinable()) _thread.join();
    }

    std::shared_ptr<const ZoneSet> Catalog::Current() const {
        std::lock_guard lock(_snapshotMutex);
        return _snapshot;
    }

    void Catalog::Invalidate() {
        {
            std::lock_guard lock(_wakeMutex);
            _dirty = true;
        }
        _wake.notify_all();
    }

    void Catalog::run() {
        std::unique_lock lock(_wakeMutex);
        while (!_stopping) {
            _wake.wait_for(lock, _options.refresh, [this] { return _stopping || _dirty; });
            if (_stopping) break;
            _dirty = false;
            lock.unlock();
            reload();
            lock.lock();
        }
    }

    void Catalog::reload() {

        try {
            const auto &factory = Database::RepositoryFactory::instance();
            const auto eds = factory.edsRepository();
            const auto zones = eds->listAllZones();
            const auto records = eds->listAllRecords();

            ServiceNames services;
            if (!_options.serviceDomain.empty()) {

                const auto eap = factory.eapRepository();
                const auto applications = eap->listAllApplications("");

                // Nodes are registered per account; the accounts that have applications are the
                // ones whose nodes can be running anything.
                std::set<std::string> accounts;
                for (const auto &application: applications) accounts.insert(application.accountId);

                const auto now = std::chrono::system_clock::now();
                std::map<std::string, std::string> nodeAddress;
                for (const auto &account: accounts) {
                    for (const auto &node: eap->listNodes(account)) {
                        if (!node.isLive(_options.nodeLease, now)) continue;
                        nodeAddress[node.name] = node.address;
                        services.nodes.push_back({.name = node.name, .address = node.address});
                    }
                }

                std::map<std::string, Database::Entity::Module> modules;
                for (auto &module: factory.emmRepository()->findAll()) modules.emplace(module.name, std::move(module));

                for (const auto &application: applications) {
                    const auto module = modules.find(application.runtimeName);
                    if (module == modules.end()) continue;
                    for (const auto &instance: module->second.instances) {
                        if (instance.state != Database::Entity::ModuleState::RUNNING) continue;

                        std::string address;
                        if (instance.assignedTo.empty()) {
                            address = _options.managerAddress;
                        } else if (const auto node = nodeAddress.find(instance.assignedTo); node != nodeAddress.end()) {
                            address = node->second;
                        } else {
                            // Assigned to a node that is not live: not reachable, so not named.
                            continue;
                        }
                        services.instances.push_back({.application = application.runtimeName, .address = address,
                                                      .port = instance.httpPort, .node = instance.assignedTo});
                    }
                }
            }

            std::vector<std::string> problems;
            auto built = std::make_shared<const ZoneSet>(BuildZoneSet(zones, records, services, _options, problems));
            if (problems != _lastProblems) {
                for (const auto &problem: problems) log_warning << "EDS record skipped, " << problem;
                _lastProblems = std::move(problems);
            }

            std::lock_guard lock(_snapshotMutex);
            _snapshot = std::move(built);

        } catch (const std::exception &e) {
            log_error << "EDS could not reload its zones, keeping the previous ones, error: " << e.what();
        }
    }

}// namespace Euclid::EDS
