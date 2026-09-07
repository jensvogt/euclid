//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <memory>
#include <string>

// Boost includes
#include <boost/asio/ssl/context.hpp>

namespace Euclid::EAG {

    /**
     * @brief Builds the TLS context an HTTPS listener answers with, from the certificate the key
     * management module holds.
     *
     * @par
     * The certificate lives in EKM rather than in a file beside the gateway, so that what a
     * listener serves is the same thing the CLI can list, replace and watch expire - see
     * Database::Entity::EKM::Certificate. This is where a port's configuration ("protocol":
     * "https", "certificate": "<name>") turns into the material a handshake needs.
     *
     * @par
     * A listener whose certificate does not exist yet gets one: self-signed, stored under the
     * name it asked for, and marked as generated so that a later import can tell euclid's own
     * stopgap from a decision somebody made. This is the difference between an installation that
     * comes up speaking HTTPS with a certificate nobody has vouched for - which is what a
     * development or demonstration installation wants - and one that refuses to start until
     * somebody produces a certificate authority.
     *
     * @param certificateName the certificate the listener names, or empty to use the conventional
     * name for its namespace.
     * @param nameSpace the namespace the listener serves; certificates are scoped to one, as
     * every other named resource is.
     * @return the server context, ready to accept handshakes.
     * @throws std::runtime_error if no certificate could be loaded or generated - a listener that
     * cannot terminate TLS has nothing to offer a caller, and saying so at start-up is better
     * than a port that answers every connection with a handshake failure.
     */
    [[nodiscard]]
    std::shared_ptr<boost::asio::ssl::context> LoadListenerCertificate(const std::string &certificateName,
                                                                       const std::string &nameSpace);

}// namespace Euclid::EAG
