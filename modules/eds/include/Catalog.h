// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Euclid includes
#include <Resolver.h>
#include <euclid/database/entity/eds/HostedZone.h>
#include <euclid/database/entity/eds/Record.h>

namespace Euclid::EDS {

    /**
     * @brief The names euclid gives the things it runs, before they are made into a zone.
     */
    struct ServiceNames {

        /**
         * @brief One running instance of an application.
         */
        struct Instance {
            std::string application;
            std::string address;
            int port{};

            /**
             * @brief The worker node it runs on; empty for the manager's own host.
             */
            std::string node;
        };

        /**
         * @brief One live worker node.
         */
        struct Node {
            std::string name;
            std::string address;
        };

        std::vector<Instance> instances;
        std::vector<Node> nodes;
    };

    /**
     * @brief How the zones are built.
     */
    struct CatalogOptions {

        /**
         * @brief The zone the service names live in; empty switches them off.
         *
         * @par
         * ".internal" because ICANN reserved it in 2024 for exactly this - private names that will
         * never be delegated in the public DNS - so a name here cannot collide with a real one.
         */
        std::string serviceDomain{"euclid.internal"};

        /**
         * @brief The address "manager.<serviceDomain>" answers, and the one instances the manager
         * runs itself are reached at.
         */
        std::string managerAddress{"127.0.0.1"};

        /**
         * @brief SOA MNAME of every zone, and the NS record of one that stores none.
         */
        std::string primaryNs{"localhost"};

        /**
         * @brief TTL of the service names. Short, because an instance comes and goes with the
         * autoscaler, and a resolver holding its address for five minutes sends traffic to a
         * process that is gone.
         */
        std::uint32_t serviceTtl{5};

        /**
         * @brief How often the zones are reloaded when nothing asks for it sooner.
         */
        std::chrono::seconds refresh{5};

        /**
         * @brief How long a node may be silent and still be named - EAP's node lease.
         */
        std::chrono::seconds nodeLease{45};
    };

    /**
     * @brief Makes a DNS label of an application or node name: lowercase, anything but a letter,
     * a digit or a hyphen turned into a hyphen, hyphens trimmed at both ends.
     *
     * @return the label, or empty when nothing usable is left or it is longer than 63.
     */
    std::string ToLabel(const std::string &name);

    /**
     * @brief Builds the zones from what is stored and what is running.
     *
     * @par
     * The service zone, when serviceDomain is set, holds:
     * - manager.<domain>                      A/AAAA  the manager
     * - <node>.node.<domain>                  A/AAAA  each live worker node
     * - <application>.app.<domain>            A/AAAA  every address an instance runs at
     * - _http._tcp.<application>.app.<domain> SRV     each instance's port and host
     *
     * @param zones the stored zones.
     * @param records the stored record sets, of any zone.
     * @param services what is running.
     * @param options how.
     * @param problems a record that cannot be encoded is left out and said here, rather than
     * taking its zone down with it.
     */
    ZoneSet BuildZoneSet(const std::vector<Database::Entity::EDS::HostedZone> &zones,
                         const std::vector<Database::Entity::EDS::Record> &records,
                         const ServiceNames &services, const CatalogOptions &options,
                         std::vector<std::string> &problems);

    /**
     * @brief The zones the name server is answering from, kept current in the background.
     *
     * @par
     * A query never touches the database. It reads a snapshot - an immutable ZoneSet behind a
     * shared_ptr, swapped whole - which a thread rebuilds every refresh interval, and at once when
     * Invalidate() says something was written. A database that stops answering leaves the last
     * snapshot in place: the zones go stale, they do not go away.
     */
    class Catalog {

    public:
        explicit Catalog(CatalogOptions options);
        ~Catalog();

        Catalog(const Catalog &) = delete;
        Catalog &operator=(const Catalog &) = delete;

        /**
         * @brief Loads the zones once, synchronously, and starts the refresher.
         */
        void Start();

        /**
         * @brief Stops the refresher.
         */
        void Stop();

        /**
         * @brief The current snapshot. Never null once Start() has run.
         */
        [[nodiscard]] std::shared_ptr<const ZoneSet> Current() const;

        /**
         * @brief Asks for a reload now, so a record that was just written is answered on the
         * next query rather than within the refresh interval. Returns without waiting.
         */
        void Invalidate();

        [[nodiscard]] const CatalogOptions &Options() const { return _options; }

    private:
        void reload();
        void run();

        CatalogOptions _options;
        mutable std::mutex _snapshotMutex;
        std::shared_ptr<const ZoneSet> _snapshot;

        std::mutex _wakeMutex;
        std::condition_variable _wake;
        bool _dirty{false};
        bool _stopping{false};
        std::thread _thread;

        /**
         * @brief What the last reload could not encode - logged again only when it changes, or a
         * bad record would be reported every refresh interval for as long as it is stored.
         */
        std::vector<std::string> _lastProblems;
    };

}// namespace Euclid::EDS
