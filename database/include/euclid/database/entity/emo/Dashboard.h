// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 10/20/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

namespace Euclid::Database::Entity::Monitoring {

    /**
     * @brief One saved dashboard layout, belonging to one user.
     *
     * @par Why this lives in the monitoring module
     * A dashboard is a description of what to read out of EMO, so it is stored beside the data it
     * describes. It is the only thing in this module that is not a measurement: everything else
     * here is written by the flush task and expires on its own retention, while a dashboard is
     * written by a person and stays until that person removes it. That difference is why the rows
     * live in their own collection rather than beside the metrics - the metrics collection carries
     * a TTL index, and a dashboard that quietly disappeared after seven days would be a dashboard
     * somebody rebuilt every week.
     *
     * @par Identity
     * userId plus name. The user id is the authenticated subject of the request that wrote it -
     * never a field of the request body - so one user cannot write into another's dashboards by
     * asking to. Two users may each have an "Overview" and they are two different dashboards.
     *
     * @par Why the layout is a string
     * `document` holds the dashboard JSON exactly as the client sent it, as text, rather than as a
     * BSON subdocument. Nothing on this side knows or should know what a panel contains - the same
     * reasoning the client's own store is written with - and storing it as BSON would quietly make
     * this module a party to that schema: BSON refuses keys containing dots or a leading dollar,
     * and it has no single number type, so a layout would not necessarily read back as the one
     * that was written. A string round-trips whatever it was given, and a panel type added to the
     * client needs no change here.
     */
    struct Dashboard {

        /**
         * @brief Database id, empty until it has been written.
         */
        std::string oid;

        /**
         * @brief Who it belongs to, as EAM knows them: "jvo", "admin".
         */
        std::string userId;

        /**
         * @brief The dashboard's name, unique within one user.
         */
        std::string name;

        /**
         * @brief The layout, as the JSON text the client sent.
         */
        std::string document;

        /**
         * @brief When this dashboard was first saved.
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief When it was last written. What the client shows beside a name when choosing one.
         */
        std::chrono::system_clock::time_point modified;

        /**
         * @brief Converts the entity to a MongoDB document.
         *
         * @return entity as a BSON document
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Reads the entity back from a MongoDB document.
         *
         * @param doc document to read, or nothing
         * @return the dashboard, default-constructed when there is no document
         */
        [[nodiscard]]
        static Dashboard fromDocument(const std::optional<bsoncxx::document::view> &doc);
    };

}// namespace Euclid::Database::Entity::Monitoring
