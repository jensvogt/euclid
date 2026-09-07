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

    /**
     * @brief The certificate a listener actually serves, whether or not it named one.
     *
     * @par
     * A listener that names no certificate does not have none - it has the conventional one for
     * its namespace, which is what gets generated for it. Anything reporting on a listener has to
     * say that name rather than an empty field, or an operator is left looking for a certificate
     * that is there under a name nothing told them.
     *
     * @param certificateName the certificate the listener names, or empty for the conventional one.
     * @param nameSpace the namespace the listener serves.
     */
    [[nodiscard]]
    std::string ListenerCertificateName(const std::string &certificateName, const std::string &nameSpace);

    /**
     * @brief The account listener certificates belong to, or empty when none is configured.
     *
     * @par
     * Empty rather than an error, because the callers that only want to look one up can say "no
     * certificate" and carry on. LoadListenerCertificate() is the one that cannot, and it is the
     * one that throws.
     */
    [[nodiscard]]
    std::string ListenerAccountId();

}// namespace Euclid::EAG
