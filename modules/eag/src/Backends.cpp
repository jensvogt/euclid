// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/5/26.
//

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

// Euclid includes
#include <Backends.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/entity/eap/Application.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAG {

    namespace {

        // Where to connect for an instance that records this host name, or nothing when the name
        // is one this machine cannot turn into an address.
        //
        // The empty name - which is every instance on a single-host installation, and every record
        // written before instances carried a host at all - is loopback without asking anybody.
        // That is what it meant before the field existed and it stays exact: no resolver, no DNS,
        // no way for a broken name service to take the gateway's own backends away.
        //
        // A numeric address is used as written, for the same reason. Only a real name reaches the
        // resolver, and it does so here, on the refresh timer, rather than in front of a request.
        std::optional<boost::asio::ip::address> resolveHost(const std::string &host) {

            if (host.empty()) return boost::asio::ip::make_address("127.0.0.1");

            boost::system::error_code ec;
            if (auto numeric = boost::asio::ip::make_address(host, ec); !ec) return numeric;

            try {
                // Its own context: this runs on the refresh timer, not on the proxy's io_context,
                // and a resolver outliving a context it borrowed would be a use-after-free in a
                // place nobody would look.
                boost::asio::io_context ioc;
                boost::asio::ip::tcp::resolver resolver(ioc);

                // Resolved against no particular port - the port comes from the instance record -
                // so the service is empty and only the address is taken.
                if (const auto results = resolver.resolve(host, "", ec); !ec && !results.empty()) {
                    return results.begin()->endpoint().address();
                }
            } catch (const std::exception &e) {
                log_warning << "Could not resolve a backend host, host: " << host << ", error: " << e.what();
                return std::nullopt;
            }

            log_warning << "Could not resolve a backend host, host: " << host
                        << (ec ? ", error: " + ec.message() : std::string(", no address returned"));
            return std::nullopt;
        }

    }// namespace

    void Backends::refresh(const std::vector<ApplicationRef> &applications) {

        std::map<std::string, std::vector<Backend> > backends;
        try {
            const auto modules = Database::RepositoryFactory::instance().emmRepository();
            const auto applicationRepository = Database::RepositoryFactory::instance().eapRepository();

            for (const auto &application: applications) {

                // Two lookups rather than one, because a route names an application the way a
                // person does and the module registry knows it by the name it runs under. Done
                // here, on the timer, so that serving a request needs neither of them.
                const auto definition = applicationRepository->findApplicationByApplicationId(
                        application.accountId, application.nameSpace, application.applicationId);
                if (!definition.has_value()) continue;

                const auto module = modules->findByName(Database::Entity::EAP::RuntimeName(*definition));
                if (!module.has_value()) continue;

                for (const auto &instance: module->instances) {
                    if (instance.state != Database::Entity::ModuleState::RUNNING) continue;

                    // An instance with no port is one the manager could not give one to - either
                    // no range is configured, or the range is exhausted. Sending a request to port
                    // zero would fail in a way that reads as the application being broken, so it
                    // is left out of the rotation and says so once, here.
                    if (instance.httpPort <= 0) {
                        log_debug << "Instance has no HTTP port and cannot be routed to, application: " << application.applicationId
                                  << ", instance: " << instance.instanceId;
                        continue;
                    }

                    // Left out of the rotation exactly as a portless instance is, and for the same
                    // reason: a backend that cannot be connected to fails in a way that reads as
                    // the application being broken. An unresolvable host is the manager on that
                    // machine recording a name this one has no way to look up, which is a
                    // configuration problem worth one line rather than a request-time surprise.
                    const auto address = resolveHost(instance.host);
                    if (!address.has_value()) {
                        log_warning << "Instance host cannot be resolved and will not be routed to, application: "
                                    << application.applicationId << ", instance: " << instance.instanceId
                                    << ", host: " << instance.host;
                        continue;
                    }

                    backends[application.key()].push_back(Backend{.host = instance.host,
                                                                  .address = *address,
                                                                  .port = instance.httpPort});
                }
            }
        } catch (const std::exception &e) {
            log_error << "Could not refresh the backend list, keeping the previous one, error: " << e.what();
            return;
        }

        std::lock_guard lock(_mutex);
        _backends = std::move(backends);
    }

    std::optional<Backend> Backends::next(const ApplicationRef &application) {

        const auto key = application.key();

        std::lock_guard lock(_mutex);
        const auto it = _backends.find(key);
        if (it == _backends.end() || it->second.empty()) return std::nullopt;

        auto &cursor = _cursors[key];
        const auto backend = it->second[cursor % it->second.size()];
        cursor++;
        return backend;
    }

    std::size_t Backends::count(const ApplicationRef &application) const {
        std::lock_guard lock(_mutex);
        const auto it = _backends.find(application.key());
        return it != _backends.end() ? it->second.size() : 0;
    }

}// namespace Euclid::EAG
