// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE PurgeJobTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/Database.h>
#include <euclid/database/entity/esm/PurgeJob.h>
#include <euclid/database/repository/esm/MongoEsmRepository.h>

using Euclid::Database::MongoEsmRepository;
using Euclid::Database::Entity::ESM::PurgeJob;
using namespace std::chrono_literals;

// An --async purge is answered before it is done and carried out on a detached thread that nothing
// outside the process knows about. The autoscaler stops an instance it sees no requests on, and a
// crash or a restart takes the thread with it - in every case the removal just stopped, with no
// record that it had ever been asked for.
//
// The job document is that record. What has to hold: a job is claimed by exactly one worker, a
// worker that dies has its job taken by somebody else, and a worker that is merely slow does not.

namespace {

    constexpr auto kBucket = "ern:esm:eu-central-1:000000000000:production:bucket:dropbox";
    constexpr auto kStale = 30s;

    MongoEsmRepository freshRepository() {
        Euclid::Database::Database::instance().initializeMemory();
        return MongoEsmRepository{};
    }

    PurgeJob jobOf(MongoEsmRepository &repo, const std::string &jobId, const std::string &prefix = "") {
        PurgeJob job;
        job.jobId = jobId;
        job.bucketErn = kBucket;
        job.prefix = prefix;
        job.userId = "jvo";
        return repo.upsertPurgeJob(job);
    }

    // Backdates a claim, which is what a worker that stopped looks like once staleAfter has passed.
    void abandon(MongoEsmRepository &repo, const std::string &jobId) {
        auto jobs = repo.listPurgeJobs();
        for (auto &job: jobs) {
            if (job.jobId != jobId) continue;
            job.claimedAt = std::chrono::system_clock::now() - 1h;
            std::ignore = repo.upsertPurgeJob(job);
        }
    }

}// namespace

BOOST_AUTO_TEST_CASE(AFreshJobIsClaimable) {

    auto repo = freshRepository();
    std::ignore = jobOf(repo, "job-1");

    const auto claimed = repo.claimPurgeJob("esm-0", kStale);

    BOOST_REQUIRE(claimed.has_value());
    BOOST_TEST(claimed->jobId == "job-1");
    BOOST_TEST(claimed->bucketErn == kBucket);
    BOOST_TEST(claimed->userId == "jvo");
    BOOST_TEST(claimed->claimedBy == "esm-0");
}

BOOST_AUTO_TEST_CASE(OnlyOneWorkerGetsAJob) {

    // The reason the claim is one operation rather than a read and a write. Two instances sweeping
    // in the same second would otherwise both take it and remove the same objects twice - harmless
    // for the objects, which are already gone, and not harmless for the bucket's counters, which
    // would be moved twice for one removal.
    auto repo = freshRepository();
    std::ignore = jobOf(repo, "job-1");

    const auto first = repo.claimPurgeJob("esm-0", kStale);
    const auto second = repo.claimPurgeJob("esm-1", kStale);

    BOOST_REQUIRE(first.has_value());
    BOOST_TEST(!second.has_value());
}

BOOST_AUTO_TEST_CASE(NothingToClaimAnswersNothing) {

    auto repo = freshRepository();
    BOOST_TEST(!repo.claimPurgeJob("esm-0", kStale).has_value());
}

BOOST_AUTO_TEST_CASE(AWorkerThatStoppedHasItsJobTakenOver) {

    // The case the whole mechanism exists for: the instance was scaled down mid-purge.
    auto repo = freshRepository();
    std::ignore = jobOf(repo, "job-1");

    BOOST_REQUIRE(repo.claimPurgeJob("esm-0", kStale).has_value());
    abandon(repo, "job-1");

    const auto resumed = repo.claimPurgeJob("esm-1", kStale);

    BOOST_REQUIRE(resumed.has_value());
    BOOST_TEST(resumed->claimedBy == "esm-1");
}

BOOST_AUTO_TEST_CASE(AWorkerThatIsMerelySlowKeepsItsJob) {

    // The other half, and the reason the heartbeat is refreshed per page rather than set once at
    // the start: a purge of a large bucket is legitimately long, and stealing it from a worker that
    // is getting on with it would have two workers removing the same objects.
    auto repo = freshRepository();
    std::ignore = jobOf(repo, "job-1");

    BOOST_REQUIRE(repo.claimPurgeJob("esm-0", kStale).has_value());
    abandon(repo, "job-1");

    // A page finished: the claim is refreshed, and the job is no longer up for grabs.
    BOOST_TEST(repo.heartbeatPurgeJob("job-1", "esm-0", 25, 2500));
    BOOST_TEST(!repo.claimPurgeJob("esm-1", kStale).has_value());
}

BOOST_AUTO_TEST_CASE(AHeartbeatFromSomebodyElsesWorkerIsRefused) {

    // What tells a worker whose job was taken while it was stalled to stop, rather than carry on
    // removing objects another worker is also removing and counting.
    auto repo = freshRepository();
    std::ignore = jobOf(repo, "job-1");

    BOOST_REQUIRE(repo.claimPurgeJob("esm-0", kStale).has_value());

    BOOST_TEST(!repo.heartbeatPurgeJob("job-1", "esm-1", 25, 2500));
    BOOST_TEST(!repo.heartbeatPurgeJob("no-such-job", "esm-0", 25, 2500));
}

BOOST_AUTO_TEST_CASE(ProgressAccumulatesAcrossWorkers) {

    // Written down because it is what the answer to "how far has it got" is made of, and because a
    // job that changed hands has had two workers contribute to it.
    auto repo = freshRepository();
    std::ignore = jobOf(repo, "job-1");

    BOOST_REQUIRE(repo.claimPurgeJob("esm-0", kStale).has_value());
    BOOST_TEST(repo.heartbeatPurgeJob("job-1", "esm-0", 25, 2500));
    BOOST_TEST(repo.heartbeatPurgeJob("job-1", "esm-0", 25, 2500));

    abandon(repo, "job-1");
    BOOST_REQUIRE(repo.claimPurgeJob("esm-1", kStale).has_value());
    BOOST_TEST(repo.heartbeatPurgeJob("job-1", "esm-1", 10, 1000));

    const auto jobs = repo.listPurgeJobs();
    BOOST_REQUIRE(jobs.size() == 1U);
    BOOST_TEST(jobs.front().removedObjects == 60L);
    BOOST_TEST(jobs.front().removedSize == 6000L);
}

BOOST_AUTO_TEST_CASE(AFinishedJobIsGoneAndNoLongerClaimable) {

    auto repo = freshRepository();
    std::ignore = jobOf(repo, "job-1");
    BOOST_REQUIRE(repo.claimPurgeJob("esm-0", kStale).has_value());

    repo.deletePurgeJob("job-1");

    BOOST_TEST(repo.listPurgeJobs().empty());
    BOOST_TEST(!repo.claimPurgeJob("esm-1", kStale).has_value());
}

BOOST_AUTO_TEST_CASE(SeveralJobsAreHandedOutOneEach) {

    auto repo = freshRepository();
    std::ignore = jobOf(repo, "job-1", "incoming/");
    std::ignore = jobOf(repo, "job-2", "archive/");

    const auto first = repo.claimPurgeJob("esm-0", kStale);
    const auto second = repo.claimPurgeJob("esm-1", kStale);
    const auto third = repo.claimPurgeJob("esm-2", kStale);

    BOOST_REQUIRE(first.has_value());
    BOOST_REQUIRE(second.has_value());
    BOOST_TEST(first->jobId != second->jobId);
    BOOST_TEST(!third.has_value());
}
