// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ApplicationManifestTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#endif

// Euclid includes
#include <euclid/core/ApplicationManifest.h>

using Euclid::Core::ApplicationManifest;
using Euclid::Core::LoadApplicationManifest;
using Euclid::Core::ParseApplicationManifest;
using Access = ApplicationManifest::Access;
using Kind = ApplicationManifest::Kind;

// A manifest is authored by hand and read by a deploy, which is the worst combination for silent
// mistakes: nobody is watching when it is read, and whoever wrote it is not there. So the parser
// refuses what it does not understand rather than skipping it, and reports every problem in the
// directory at once with the file that holds it.

namespace {

    // A directory per test, because merging is the thing being tested and a leftover file from the
    // previous one would be a second manifest nobody wrote.
    class ManifestDirectory {
    public:
        explicit ManifestDirectory(const std::string &name) {
#ifdef _WIN32
            const auto pid = _getpid();
#else
            const auto pid = ::getpid();
#endif
            _path = std::filesystem::temp_directory_path() / ("euclid-manifest-" + std::to_string(pid) + "-" + name);
            std::filesystem::remove_all(_path);
            std::filesystem::create_directories(_path);
        }

        ~ManifestDirectory() {
            std::error_code ec;
            std::filesystem::remove_all(_path, ec);
        }

        ManifestDirectory(const ManifestDirectory &) = delete;
        ManifestDirectory &operator=(const ManifestDirectory &) = delete;

        void write(const std::string &fileName, const std::string &content) const {
            std::ofstream file(_path / fileName, std::ios::binary);
            file << content;
        }

        [[nodiscard]] const std::filesystem::path &path() const { return _path; }

    private:
        std::filesystem::path _path;
    };

    bool mentions(const std::vector<std::string> &errors, const std::string &text) {
        return std::ranges::any_of(errors, [&text](const std::string &error) { return error.find(text) != std::string::npos; });
    }

}// namespace

BOOST_AUTO_TEST_SUITE(ApplicationManifestTest)

BOOST_AUTO_TEST_CASE(a_directory_of_files_becomes_one_manifest) {

    const ManifestDirectory directory("merge");
    directory.write("queues.json", R"({"version":1,"creates":{"queues":[{"name":"parsing-in","visibility":300}]}})");
    directory.write("buckets.json", R"({"version":1,"creates":{"buckets":[{"name":"parsing-work"}]}})");
    directory.write("access.json", R"({"version":1,"uses":{"topics":[{"name":"artikel-updates","access":"subscribe","owner":"transformation"}]}})");

    const auto result = LoadApplicationManifest(directory.path());

    const std::string firstError = result.errors.empty() ? std::string{} : result.errors.front();
    BOOST_TEST_INFO(firstError);
    BOOST_TEST(result.ok());
    BOOST_TEST(result.manifest.creates.size() == 2u);
    BOOST_TEST(result.manifest.uses.size() == 1u);

    // Settings are passed through untouched - the module that makes the queue is the one that gets
    // to have an opinion about a visibility timeout.
    const auto queue = std::ranges::find_if(result.manifest.creates, [](const auto &c) { return c.kind == Kind::Queue; });
    BOOST_REQUIRE(queue != result.manifest.creates.end());
    BOOST_TEST(queue->name == "parsing-in");
    BOOST_TEST(queue->settings.at("visibility").as_int64() == 300);
    BOOST_TEST(queue->source == "queues.json");

    BOOST_TEST((result.manifest.uses.front().access == Access::Subscribe));
    BOOST_TEST(result.manifest.uses.front().owner == "transformation");
}

BOOST_AUTO_TEST_CASE(a_missing_directory_is_not_an_error) {

    // Most applications declare nothing. Refusing them would be this feature breaking every
    // existing deployment the day it arrives.
    const auto result = LoadApplicationManifest(std::filesystem::temp_directory_path() / "euclid-manifest-does-not-exist");
    BOOST_TEST(result.ok());
    BOOST_TEST(result.manifest.empty());
}

BOOST_AUTO_TEST_CASE(the_same_object_declared_in_two_files_names_both) {

    const ManifestDirectory directory("duplicate");
    directory.write("a-queues.json", R"({"version":1,"creates":{"queues":[{"name":"parsing-in"}]}})");
    directory.write("b-queues.json", R"({"version":1,"creates":{"queues":[{"name":"parsing-in"}]}})");

    const auto result = LoadApplicationManifest(directory.path());

    BOOST_TEST(!result.ok());
    BOOST_TEST(mentions(result.errors, "b-queues.json"));
    BOOST_TEST(mentions(result.errors, "already declared in a-queues.json"));
}

BOOST_AUTO_TEST_CASE(an_object_cannot_be_owned_and_borrowed_at_once) {

    const ManifestDirectory directory("both");
    directory.write("mine.json", R"({"version":1,"creates":{"buckets":[{"name":"work"}]}})");
    directory.write("theirs.json", R"({"version":1,"uses":{"buckets":[{"name":"work","access":"read"}]}})");

    const auto result = LoadApplicationManifest(directory.path());

    BOOST_TEST(!result.ok());
    BOOST_TEST(mentions(result.errors, "is used here and created in mine.json"));
}

BOOST_AUTO_TEST_CASE(one_object_used_two_ways_is_two_answers) {

    const ManifestDirectory directory("conflict");
    directory.write("a.json", R"({"version":1,"uses":{"buckets":[{"name":"transfer-server","access":"read"}]}})");
    directory.write("b.json", R"({"version":1,"uses":{"buckets":[{"name":"transfer-server","access":"write"}]}})");

    const auto result = LoadApplicationManifest(directory.path());

    BOOST_TEST(!result.ok());
    BOOST_TEST(mentions(result.errors, "used as 'write' here and as 'read' in a.json"));
}

BOOST_AUTO_TEST_CASE(a_misspelt_section_is_refused_not_skipped) {

    // The failure this prevents: a deployment that comes up having created nothing, with a manifest
    // that looks right and a log that says nothing.
    const auto result = ParseApplicationManifest(R"({"version":1,"creates":{"queue":[{"name":"parsing-in"}]}})", "queues.json");

    BOOST_TEST(!result.ok());
    BOOST_TEST(mentions(result.errors, "unknown section \"queue\""));
    BOOST_TEST(mentions(result.errors, "buckets, queues, topics"));
}

BOOST_AUTO_TEST_CASE(a_used_object_must_say_how_it_is_reached) {

    const auto result = ParseApplicationManifest(R"({"version":1,"uses":{"queues":[{"name":"orders"}]}})", "access.json");

    BOOST_TEST(!result.ok());
    BOOST_TEST(mentions(result.errors, "needs an \"access\""));

    const auto unknown = ParseApplicationManifest(R"({"version":1,"uses":{"queues":[{"name":"orders","access":"rw"}]}})", "access.json");
    BOOST_TEST(!unknown.ok());
    BOOST_TEST(mentions(unknown.errors, "unknown access \"rw\""));
}

BOOST_AUTO_TEST_CASE(a_name_is_a_name_and_not_an_ern) {

    // An ERN carries an account and a namespace. A manifest is applied into whichever namespace the
    // application is deployed to, so accepting one would let a development deployment reach into
    // production from a file nobody reads at deploy time.
    const auto result = ParseApplicationManifest(
            R"({"version":1,"creates":{"buckets":[{"name":"ern:esm:eu-central-1:000000000000:production:bucket:work"}]}})", "buckets.json");

    BOOST_TEST(!result.ok());
    BOOST_TEST(mentions(result.errors, "is an ERN"));
}

BOOST_AUTO_TEST_CASE(a_future_version_is_refused_whole) {

    // Half-understanding a later format is worse than refusing it: the sections this version knows
    // would be applied and the ones it does not would silently not be.
    const auto result = ParseApplicationManifest(R"({"version":2,"creates":{"queues":[{"name":"orders"}]}})", "queues.json");

    BOOST_TEST(!result.ok());
    BOOST_TEST(mentions(result.errors, "has version 2"));
    BOOST_TEST(result.manifest.creates.empty());
}

BOOST_AUTO_TEST_CASE(every_problem_in_the_directory_is_reported_at_once) {

    // Somebody fixing a manifest wants the list, not one error per deploy.
    const ManifestDirectory directory("many");
    directory.write("a.json", R"({"version":1,"creates":{"queues":[{"name":""}]}})");
    directory.write("b.json", R"({"version":1,"uses":{"topics":[{"name":"updates"}]}})");
    directory.write("c.json", R"({"version":1,"typo":{}})");

    const auto result = LoadApplicationManifest(directory.path());

    BOOST_TEST(!result.ok());
    BOOST_TEST(result.errors.size() >= 3u);
    BOOST_TEST(mentions(result.errors, "a.json"));
    BOOST_TEST(mentions(result.errors, "b.json"));
    BOOST_TEST(mentions(result.errors, "c.json"));

    // And nothing is half-applied out of a directory that did not parse.
    BOOST_TEST(result.manifest.empty());
}

BOOST_AUTO_TEST_CASE(files_that_are_not_manifests_are_left_alone) {

    const ManifestDirectory directory("mixed");
    directory.write("queues.json", R"({"version":1,"creates":{"queues":[{"name":"orders"}]}})");
    directory.write("README.md", "Not JSON, and not a declaration either.");
    std::filesystem::create_directories(directory.path() / "notes");

    const auto result = LoadApplicationManifest(directory.path());

    BOOST_TEST(result.ok());
    BOOST_TEST(result.manifest.creates.size() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
