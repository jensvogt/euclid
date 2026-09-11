// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/10/26.
//

#pragma once

// Boost includes
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/ekv/Item.h>
#include <euclid/database/entity/ekv/SortCondition.h>
#include <euclid/database/entity/ekv/Table.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ekv/CreateTableRequest.h>
#include <euclid/dto/ekv/DeleteItemRequest.h>
#include <euclid/dto/ekv/DeleteTableRequest.h>
#include <euclid/dto/ekv/DescribeTableRequest.h>
#include <euclid/dto/ekv/GetItemRequest.h>
#include <euclid/dto/ekv/ListTablesRequest.h>
#include <euclid/dto/ekv/PutItemRequest.h>
#include <euclid/dto/ekv/QueryRequest.h>
#include <euclid/dto/ekv/ScanRequest.h>
#include <euclid/dto/ekv/TableDescription.h>

namespace Euclid::EKV {

    using namespace boost::beast::http;

    /**
     * @brief EKV service server listening on a Unix domain socket.
     *
     * @par
     * The key/value store: tables of items, each identified by a partition key and optionally
     * ordered within that partition by a sort key. What it is for is the shape of data that has no
     * business being an object in a bucket - a record that is read one at a time, by name, and
     * updated in place - which today gets stored as a JSON file per record and read back by
     * listing a prefix.
     *
     * @par
     * Items are ordinary JSON: an object of attributes, nested as deeply as the caller likes, with
     * no type annotations to write and none to read back. The types survive the round trip anyway
     * (see Database::Entity::EKV::Value), which is the part that a bucket of JSON files does not
     * give you for free.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EkvServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EkvServer(std::string socketPath, int threads = 4);

    protected:

        /**
         * @brief Dispatch the request.
         *
         * @param req HTTP request
         * @return HTTP response
         */
        [[nodiscard]]
        response<string_body> Dispatch(const request<string_body> &req) override;
    };

}// namespace Euclid::EKV
