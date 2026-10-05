// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Euclid includes
#include <euclid/core/DateTimeUtils.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/eap/ApplicationIdentity.h>

namespace Euclid::Database::Entity::EAP {

    std::string ApplicationNamespace(const Application &application) {

        if (!application.nameSpace.empty()) return application.nameSpace;

        const auto repository = RepositoryFactory::instance().eamRepository();
        const auto user = repository->findUserByUserId(application.userId);
        if (!user.has_value()) return {};

        // The principal's role grants, which is where "granted exactly one namespace" lives. One
        // namespace leaves no room for doubt; several or none leaves this empty rather than guessing.
        for (const auto &grant: repository->findGrantsByPrincipals({user->ern})) {
            if (grant.accountId != application.accountId) continue;
            if (grant.namespaces.size() == 1 && grant.namespaces.front() != "*") return grant.namespaces.front();
        }
        return {};
    }

    std::map<std::string, std::string> ApplicationEnvironment(const Application &application, const bool withAccessKey) {

        auto environment = application.environment;
        environment["EUCLID_APPLICATION_ID"] = application.applicationId;
        // Which version of the definition this process was started from. Also what the manager's
        // reconciler compares to notice that the definition changed under a running pool.
        environment["EUCLID_APPLICATION_REVISION"] = Core::DateTimeUtils::ToISO8601(application.modified);
        // Which build this is, as the deployment named it. An application that logs this on
        // start-up answers "what is actually running?" from its own log, without anyone having to
        // compare checksums.
        environment["EUCLID_APPLICATION_VERSION"] = application.version;
        environment["EUCLID_APPLICATION_ERN"] = application.ern;
        environment["EUCLID_ACCOUNT_ID"] = application.accountId;
        environment["EUCLID_REGION"] = application.region;
        environment["EUCLID_USER_ID"] = application.userId;

        const auto user = RepositoryFactory::instance().eamRepository()->findUserByUserId(application.userId);

        // A technical principal deliberately gets no key: its long-lived secret stays in EAM, and
        // the process holds nothing but a token that expires. A user the caller named is different
        // - the operator owns that key and may well want an application signing with it - so it is
        // passed through where the caller says it may be.
        //
        // Says nothing about a user that does not exist: this is asked on every renewal of every
        // placed instance, and the manager reports a missing principal once, where it starts one.
        if (withAccessKey && user.has_value() && user->loginEnabled && !user->accessKeys.empty()) {
            environment["EUCLID_ACCESS_KEY_ID"] = user->accessKeys.front().accessKeyId;
            environment["EUCLID_SECRET_ACCESS_KEY"] = user->accessKeys.front().secretAccessKey;
        }
        return environment;
    }

}// namespace Euclid::Database::Entity::EAP
