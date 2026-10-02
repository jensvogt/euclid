// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/5/26.
//

#pragma once

// C++ includes
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Boost includes
#include <boost/asio/ip/address.hpp>

namespace Euclid::EAG {

    /**
     * @brief One instance a request can be sent to: where it is, and on which port.
     *
     * @par
     * A port alone was enough while every instance was on this machine. It stopped being enough
     * when an instance record gained a host - see Entity::ModuleInstance::host - because the port
     * is then only meaningful together with the machine it was allocated on, and two instances on
     * two hosts may hold the same number.
     */
    struct Backend {

        /**
         * @brief The host as the instance record names it. Empty means the gateway's own machine.
         *
         * @par
         * Kept beside the resolved address because it is what the backend should be told it was
         * called as. An application behind a name-based virtual host, or one that builds absolute
         * URLs from the Host header, needs the name rather than whatever the name resolved to.
         */
        std::string host;

        /**
         * @brief Where to connect, resolved when the backend list was refreshed.
         *
         * @par
         * Resolved there rather than here, for the reason the list is refreshed on a timer at all:
         * a name lookup in front of every proxied request costs every request, and the answer
         * changes far more slowly than requests arrive. A host that cannot be resolved never
         * becomes a Backend.
         */
        boost::asio::ip::address address;

        int port{};

        /**
         * @brief What to put in the Host header: "host:port", or "127.0.0.1:port" when the record
         * names no host.
         */
        [[nodiscard]] std::string authority() const {
            return (host.empty() ? std::string("127.0.0.1") : host) + ":" + std::to_string(port);
        }

        /**
         * @brief A backend on this machine, which is what euclid's own gateway always is.
         */
        [[nodiscard]] static Backend loopback(const int port) {
            return {.host = {}, .address = boost::asio::ip::make_address("127.0.0.1"), .port = port};
        }
    };

    /**
     * @brief An application as a route names it: the three fields that identify one.
     *
     * @par
     * What a route carries, and what a request being proxied has in hand - as opposed to the name
     * the application runs under, which only the application's own definition knows. Backends
     * resolves the one into the other while it refreshes, so that serving a request stays a lookup
     * in a map.
     */
    struct ApplicationRef {

        std::string accountId;
        std::string nameSpace;
        std::string applicationId;

        /**
         * @brief The key these three make, which is what Backends is keyed by.
         */
        [[nodiscard]]
        std::string key() const { return accountId + "/" + nameSpace + "/" + applicationId; }

        bool operator<(const ApplicationRef &other) const { return key() < other.key(); }
    };

    /**
     * @brief Where an application's instances can be reached, and whose turn it is.
     *
     * @par
     * An application's instances are started and stopped by the manager as its pool grows and
     * shrinks, and each is given a port of its own when it starts. Nothing here decides any of
     * that - it reads what the manager recorded, which is the only place the two facts that matter
     * are kept together: which instances are running, and which port each of them holds.
     *
     * @par
     * Refreshed on a timer rather than per request, for the same reason as the route table. Two or
     * three seconds of staleness costs an unlucky request one retry; a database round trip in
     * front of every proxied call costs all of them.
     *
     * @author jensvogt47\@gmail.com
     */
    class Backends {

    public:

        /**
         * @brief Re-reads the running instances of every application a route points at.
         *
         * @par
         * A failed read keeps what was there. An application whose instances have genuinely gone
         * away stops answering either way, and the alternative is dropping every backend because
         * the database was briefly unreachable.
         *
         * @param applications the applications the route table points at. Each is looked up to
         * find the name it runs under, which is what the module registry knows it by - see
         * Entity::EAP::RuntimeName().
         */
        void refresh(const std::vector<ApplicationRef> &applications);

        /**
         * @brief The next instance to send a request to, or nothing if the application has none.
         *
         * @par
         * Round robin, per application: each call advances that application's own cursor, so two
         * routes pointing at one application share the rotation rather than each hammering the
         * instance the other just used.
         *
         * @param application application to reach.
         * @return where the instance whose turn it is can be reached.
         */
        [[nodiscard]]
        std::optional<Backend> next(const ApplicationRef &application);

        /**
         * @brief How many instances an application currently has, for reporting.
         */
        [[nodiscard]]
        std::size_t count(const ApplicationRef &application) const;

    private:

        mutable std::mutex _mutex;

        /**
         * @brief Where the running instances of each application can be reached.
         */
        std::map<std::string, std::vector<Backend> > _backends;

        /**
         * @brief Whose turn it is, per application. Never reset by a refresh: an application whose
         * pool changed size should carry on from where the rotation had reached rather than
         * starting again at the first instance and favouring it.
         */
        std::map<std::string, std::size_t> _cursors;
    };

}// namespace Euclid::EAG
