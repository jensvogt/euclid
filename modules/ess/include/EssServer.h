// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Boost includes
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/EventBus.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/ess/Secret.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ess/CreateSecretRequest.h>
#include <euclid/dto/ess/DeleteSecretResponse.h>
#include <euclid/dto/ess/GetSecretResponse.h>
#include <euclid/dto/ess/ListSecretsRequest.h>
#include <euclid/dto/ess/ListSecretsResponse.h>
#include <euclid/dto/ess/SecretNameRequest.h>
#include <euclid/dto/ess/SecretResponse.h>
#include <euclid/dto/ess/UpdateSecretRequest.h>
#include <euclid/dto/ess/mapper/EssMapper.h>

namespace Euclid::ESS {

    using namespace boost::beast::http;

    /**
     * @brief ESS service server listening on a Unix domain socket.
     *
     * @par
     * The secrets store: the passwords, connection strings and tokens the things euclid runs need
     * in order to reach anything else. It exists because those otherwise end up in a configuration
     * file, an environment variable or a deployment script - readable by everyone who can read the
     * host, copied into every backup, and impossible to rotate without a release.
     *
     * @par
     * A secret's value is encrypted under an EKM key before it is stored and decrypted only when
     * somebody asks for it by name. That is the module's whole reason for being, and it is why
     * there is no code path here that writes a value in the clear: the key management module keeps
     * the key, this one keeps the ciphertext, and a copy of either alone is worth nothing.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EssServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EssServer(std::string socketPath, int threads = 2);

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

}// namespace Euclid::ESS
