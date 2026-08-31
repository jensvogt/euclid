//
// Created by vogje01 on 8/31/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::ETS {

    /**
     * @brief Wire protocol a transfer server speaks.
     *
     * @par
     * Decides which executable the manager spawns for a TransferServer definition
     * (euclid-ftp or euclid-sftp) and which of the protocol-specific fields on that
     * definition are meaningful - the passive port range for FTP, the host key for SFTP.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class TransferProtocol {
        FTP,
        SFTP,
        UNKNOWN
    };

    static std::map<TransferProtocol, std::string> TransferProtocolNames{
            {TransferProtocol::FTP, "FTP"},
            {TransferProtocol::SFTP, "SFTP"},
            {TransferProtocol::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string TransferProtocolToString(const TransferProtocol &protocol) {
        return TransferProtocolNames[protocol];
    }

    [[maybe_unused]]
    static TransferProtocol TransferProtocolFromString(const std::string &protocol) {
        std::string upper = protocol;
        std::ranges::transform(upper, upper.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const auto it = std::ranges::find_if(TransferProtocolNames, [&upper](const auto &pair) { return pair.second == upper; });
        return it != TransferProtocolNames.end() ? it->first : TransferProtocol::UNKNOWN;
    }

}// namespace Euclid::Database::Entity::ETS
