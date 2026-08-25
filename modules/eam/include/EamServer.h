//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Boost includes
#include <boost/json.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/ErnUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/PasswordUtils.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/dto/eam/CreateAccessKeyResponse.h>
#include <euclid/dto/eam/CreateUserGroupRequest.h>
#include <euclid/dto/eam/CreateUserGroupResponse.h>
#include <euclid/dto/eam/DeleteAccessKeyRequest.h>
#include <euclid/dto/eam/DeleteUserRequest.h>
#include <euclid/dto/eam/EamMapper.h>
#include <euclid/dto/eam/ListAccessKeysResponse.h>
#include <euclid/dto/eam/ListUserRequest.h>
#include <euclid/dto/eam/ListUserResponse.h>
#include <euclid/dto/eam/LoginRequest.h>
#include <euclid/dto/eam/LoginResponse.h>
#include <euclid/dto/eam/RegisterRequest.h>
#include <euclid/dto/eam/RegisterResponse.h>

namespace Euclid::EAM {

    using namespace boost::beast::http;

    /**
     * @brief EAM (Euclid Access Management) service server listening on a Unix domain socket.
     *
     * Receives HTTP requests forwarded by the gateway and handles user login.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EamServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server.
         *
         * Also starts the background task that periodically reports the "access-current-users"
         * gauge.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EamServer(std::string socketPath, int threads = 2);

        /**
         * @brief Cancels the background current-users reporting task.
         */
        ~EamServer() override;

    protected:

        /**
         * @brief Dispatch the request.
         *
         * @param req HTTP request
         * @return HTTP response
         */
        [[nodiscard]]
        response<string_body> Dispatch(const request<string_body> &req) override;

    private:

        /**
         * @brief Id of the periodic task that reports the current-users gauge, used to cancel it
         * on destruction.
         */
        std::string _currentUsersTaskId;
    };

}