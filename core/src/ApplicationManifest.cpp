// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <euclid/core/ApplicationManifest.h>

// C++ includes
#include <algorithm>
#include <fstream>
#include <map>
#include <ranges>
#include <sstream>

// Euclid includes
#include <euclid/core/LogStream.h>

namespace Euclid::Core {

    namespace {

        using Access = ApplicationManifest::Access;
        using Kind = ApplicationManifest::Kind;

        constexpr int kSupportedVersion = 1;

        // The longest a name may be. Every module has its own rules about what it will accept, and
        // those stay where they are - this is only here so that a runaway value is reported against
        // the file that holds it rather than as a module error four steps later.
        constexpr std::size_t kMaxNameLength = 255;

        // The section names, in both halves. Plural, because a section holds a list; singular in
        // messages, because a message is about one object.
        const std::map<std::string, Kind> &sections() {
            static const std::map<std::string, Kind> kSections{
                    {"buckets", Kind::Bucket},
                    {"queues", Kind::Queue},
                    {"secrets", Kind::Secret},
                    {"topics", Kind::Topic},
            };
            return kSections;
        }

        // Which access levels mean anything on a kind. Only secrets are restricted, because they
        // are the one kind with a single verb: a secret is read, and "write" or "subscribe" on one
        // is not a narrower request but a misunderstanding of what the entry does. The other three
        // are left to the module-side table in Infrastructure.h, which is where the queue/topic/
        // bucket access pairs already live and the only place they should be enumerated.
        bool accessApplies(const Kind kind, const Access access) {
            if (kind != Kind::Secret) return true;
            return access == Access::Read;
        }

        const std::map<std::string, Access> &accessLevels() {
            static const std::map<std::string, Access> kLevels{
                    {"read", Access::Read},
                    {"write", Access::Write},
                    {"consume", Access::Consume},
                    {"produce", Access::Produce},
                    {"subscribe", Access::Subscribe},
            };
            return kLevels;
        }

        std::string joinKeys(const auto &map) {
            std::string all;
            for (const auto &name: map | std::views::keys) {
                if (!all.empty()) all += ", ";
                all += name;
            }
            return all;
        }

        // What a name may be. Not the module's rules - those are the module's - but enough that a
        // declaration which cannot possibly work is reported against its file.
        std::optional<std::string> nameProblem(const std::string &name) {

            if (name.empty()) return "is empty";
            if (name.size() > kMaxNameLength) return "is longer than " + std::to_string(kMaxNameLength) + " characters";

            if (name.starts_with("ern:")) {
                // An ERN names an account and a namespace, and a manifest is applied into whichever
                // namespace the application is deployed to. Accepting one would mean a file that
                // silently reaches into production from a development deployment.
                return "is an ERN; declare the name alone and let the deployment's namespace decide";
            }
            if (std::ranges::any_of(name, [](const unsigned char c) { return std::isspace(c) != 0; })) {
                return "contains whitespace";
            }
            return std::nullopt;
        }

        std::string describe(const Kind kind, const std::string &name) {
            return ToString(kind) + " '" + name + "'";
        }

    }// namespace

    std::string ToString(const Access access) {
        for (const auto &[text, value]: accessLevels()) {
            if (value == access) return text;
        }
        return "read";
    }

    std::string ToString(const std::vector<Access> &access) {
        std::string all;
        for (const auto &one: access) {
            if (!all.empty()) all += ", ";
            all += ToString(one);
        }
        return all.empty() ? "read" : all;
    }

    std::string ToString(const Kind kind) {
        for (const auto &[text, value]: sections()) {
            // "buckets" -> "bucket": a message is about one of them.
            if (value == kind) return text.substr(0, text.size() - 1);
        }
        return "object";
    }

    ManifestResult ParseApplicationManifest(const std::string &json, const std::string &source) {

        ManifestResult result;
        const auto fail = [&result, &source](const std::string &message) { result.errors.push_back(source + ": " + message); };

        // A byte order mark, which every Windows editor writes by default and JSON does not allow.
        // Skipped rather than rejected: a manifest saved from Notepad is not a mistake anybody can
        // see, and "syntax error at line 1" for an invisible character is the least actionable
        // message there is. PowerShell's own Set-Content -Encoding utf8 writes one.
        std::string_view text = json;
        if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);

        // The error code rather than the exception: Boost.JSON's what() carries the absolute path
        // of the header it was built from on somebody else's machine, which is four lines of noise
        // around "syntax error" in a message meant to send somebody to their own file.
        boost::system::error_code ec;
        const auto document = boost::json::parse(text, ec);
        if (ec) {
            fail("is not valid JSON: " + ec.message());
            return result;
        }

        if (!document.is_object()) {
            fail("must be a JSON object");
            return result;
        }
        const auto &root = document.as_object();

        // Version first: everything below is read according to it, and a file from a later format
        // must not be half-understood by an older euclid.
        if (const auto *version = root.if_contains("version")) {
            if (!version->is_int64()) {
                fail("\"version\" must be a number");
                return result;
            }
            result.manifest.version = static_cast<int>(version->as_int64());
        }
        if (result.manifest.version != kSupportedVersion) {
            fail("has version " + std::to_string(result.manifest.version) + ", and only version " +
                 std::to_string(kSupportedVersion) + " is understood");
            return result;
        }

        for (const auto &entry: root) {
            const auto key = std::string(entry.key());
            if (key == "version" || key == "creates" || key == "uses") continue;
            // Rejected rather than ignored. A misspelt section that is quietly skipped is a
            // deployment that comes up having created nothing, with everything looking fine.
            fail("has an unknown section \"" + key + "\"; expected \"creates\" or \"uses\"");
        }

        // ── creates ────────────────────────────────────────────────────────────────────────
        if (const auto *creates = root.if_contains("creates")) {
            if (!creates->is_object()) {
                fail("\"creates\" must be an object of sections");
            } else {
                for (const auto &section: creates->as_object()) {
                    const auto sectionName = std::string(section.key());
                    const auto kind = sections().find(sectionName);
                    if (kind == sections().end()) {
                        fail("\"creates\" has an unknown section \"" + sectionName + "\"; expected one of: " + joinKeys(sections()));
                        continue;
                    }
                    if (kind->second == Kind::Secret) {
                        // Refused at the section rather than per entry: it is one mistake about what
                        // a manifest is for, not one per secret. A secret's value would have to be
                        // in the file to create it, and the file is in the artifact.
                        fail("\"creates\" cannot hold secrets; a secret's value would have to travel in the "
                             "manifest, so an operator writes it with \"ess create-secret\" and the manifest "
                             "names it under \"uses\"");
                        continue;
                    }
                    if (!section.value().is_array()) {
                        fail("\"creates." + sectionName + "\" must be an array");
                        continue;
                    }
                    for (const auto &item: section.value().as_array()) {
                        if (!item.is_object()) {
                            fail("\"creates." + sectionName + "\" must hold objects");
                            continue;
                        }
                        const auto &object = item.as_object();
                        const auto *name = object.if_contains("name");
                        if (name == nullptr || !name->is_string()) {
                            fail("a " + ToString(kind->second) + " in \"creates\" has no \"name\"");
                            continue;
                        }

                        ApplicationManifest::Creates declaration;
                        declaration.kind = kind->second;
                        declaration.name = std::string(name->as_string());
                        declaration.source = source;

                        if (const auto problem = nameProblem(declaration.name)) {
                            fail(describe(declaration.kind, declaration.name) + " " + *problem);
                            continue;
                        }

                        // Everything but the name goes to the module that makes the object.
                        for (const auto &field: object) {
                            if (field.key() == "name") continue;
                            declaration.settings[field.key()] = field.value();
                        }
                        result.manifest.creates.push_back(std::move(declaration));
                    }
                }
            }
        }

        // ── uses ───────────────────────────────────────────────────────────────────────────
        if (const auto *uses = root.if_contains("uses")) {
            if (!uses->is_object()) {
                fail("\"uses\" must be an object of sections");
            } else {
                for (const auto &section: uses->as_object()) {
                    const auto sectionName = std::string(section.key());
                    const auto kind = sections().find(sectionName);
                    if (kind == sections().end()) {
                        fail("\"uses\" has an unknown section \"" + sectionName + "\"; expected one of: " + joinKeys(sections()));
                        continue;
                    }
                    if (!section.value().is_array()) {
                        fail("\"uses." + sectionName + "\" must be an array");
                        continue;
                    }
                    for (const auto &item: section.value().as_array()) {
                        if (!item.is_object()) {
                            fail("\"uses." + sectionName + "\" must hold objects");
                            continue;
                        }
                        const auto &object = item.as_object();
                        const auto *name = object.if_contains("name");
                        if (name == nullptr || !name->is_string()) {
                            fail("a " + ToString(kind->second) + " in \"uses\" has no \"name\"");
                            continue;
                        }

                        ApplicationManifest::Uses declaration;
                        declaration.kind = kind->second;
                        declaration.name = std::string(name->as_string());
                        declaration.source = source;

                        if (const auto problem = nameProblem(declaration.name)) {
                            fail(describe(declaration.kind, declaration.name) + " " + *problem);
                            continue;
                        }

                        // No default. An application that does not say how it reaches somebody
                        // else's object has not said what it needs, and guessing "read" would
                        // quietly grant the wrong thing to whichever half of the guess was wrong.
                        //
                        // One way or several: a @BucketListener attaches to a bucket's events and
                        // then fetches what each event names, which is subscribe and read, and
                        // making it choose would understate what it needs.
                        const auto *access = object.if_contains("access");
                        if (access == nullptr || (!access->is_string() && !access->is_array())) {
                            fail(describe(declaration.kind, declaration.name) + " in \"uses\" needs an \"access\"; one or more of: " + joinKeys(accessLevels()));
                            continue;
                        }

                        std::vector<boost::json::value> wanted;
                        if (access->is_string()) {
                            wanted.push_back(*access);
                        } else {
                            for (const auto &entry: access->as_array()) wanted.push_back(entry);
                        }

                        declaration.access.clear();
                        bool understood = true;
                        for (const auto &entry: wanted) {
                            if (!entry.is_string()) {
                                fail(describe(declaration.kind, declaration.name) + " has an \"access\" that is not a name");
                                understood = false;
                                break;
                            }
                            const auto level = accessLevels().find(std::string(entry.as_string()));
                            if (level == accessLevels().end()) {
                                fail(describe(declaration.kind, declaration.name) + " has an unknown access \"" +
                                     std::string(entry.as_string()) + "\"; expected one or more of: " + joinKeys(accessLevels()));
                                understood = false;
                                break;
                            }
                            if (!accessApplies(declaration.kind, level->second)) {
                                fail(describe(declaration.kind, declaration.name) + " has no \"" +
                                     std::string(entry.as_string()) + "\" access; a secret is read, and only read");
                                understood = false;
                                break;
                            }
                            if (!std::ranges::contains(declaration.access, level->second)) declaration.access.push_back(level->second);
                        }
                        if (!understood) continue;
                        if (declaration.access.empty()) {
                            fail(describe(declaration.kind, declaration.name) + " has an empty \"access\"; name at least one of: " + joinKeys(accessLevels()));
                            continue;
                        }

                        // Sorted, so that ["read","subscribe"] and ["subscribe","read"] are the
                        // same declaration rather than a conflict between two files.
                        std::ranges::sort(declaration.access);

                        if (const auto *owner = object.if_contains("owner"); owner != nullptr && owner->is_string()) {
                            declaration.owner = std::string(owner->as_string());
                        }

                        for (const auto &field: object) {
                            if (field.key() == "name" || field.key() == "access" || field.key() == "owner") continue;
                            fail(describe(declaration.kind, declaration.name) + " in \"uses\" has an unknown field \"" +
                                 std::string(field.key()) + "\"; a used object is named and reached, not configured");
                        }

                        result.manifest.uses.push_back(std::move(declaration));
                    }
                }
            }
        }

        return result;
    }

    ManifestResult LoadApplicationManifest(const std::filesystem::path &directory) {

        ManifestResult result;

        std::error_code ec;
        if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
            // Most applications declare nothing. That is not a failure, and a deploy that started
            // refusing them would be this feature breaking every existing deployment on arrival.
            return result;
        }

        // Sorted, so that the same directory always merges the same way and "declared twice"
        // always names the same two files in the same order. Directory iteration order is the
        // filesystem's business and differs between machines.
        std::vector<std::filesystem::path> files;
        for (const auto &entry: std::filesystem::directory_iterator(directory, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;
            files.push_back(entry.path());
        }
        std::ranges::sort(files);

        // Where each name was first declared, so the second one can say where the first is.
        std::map<std::pair<int, std::string>, std::string> declaredIn;

        for (const auto &file: files) {

            std::ifstream stream(file, std::ios::binary);
            if (!stream.is_open()) {
                result.errors.push_back(file.filename().string() + ": could not be opened");
                continue;
            }
            std::ostringstream buffer;
            buffer << stream.rdbuf();

            auto parsed = ParseApplicationManifest(buffer.str(), file.filename().string());
            result.errors.insert(result.errors.end(), parsed.errors.begin(), parsed.errors.end());
            if (!parsed.ok()) continue;

            for (auto &declaration: parsed.manifest.creates) {
                const auto key = std::pair{static_cast<int>(declaration.kind), declaration.name};
                if (const auto first = declaredIn.find(key); first != declaredIn.end()) {
                    result.errors.push_back(declaration.source + ": " + describe(declaration.kind, declaration.name) +
                                            " is already declared in " + first->second);
                    continue;
                }
                declaredIn.emplace(key, declaration.source);
                result.manifest.creates.push_back(std::move(declaration));
            }

            for (auto &declaration: parsed.manifest.uses) {
                result.manifest.uses.push_back(std::move(declaration));
            }
        }

        // An object cannot be both this application's and somebody else's. Checked after the merge
        // because the two halves are routinely in different files, and neither file is wrong on its
        // own - it is the pair that cannot be true.
        for (const auto &used: result.manifest.uses) {
            const auto owned = std::ranges::find_if(result.manifest.creates, [&used](const auto &created) {
                return created.kind == used.kind && created.name == used.name;
            });
            if (owned != result.manifest.creates.end()) {
                result.errors.push_back(used.source + ": " + describe(used.kind, used.name) +
                                        " is used here and created in " + owned->source +
                                        "; an application owns an object or consumes one, not both");
            }
        }

        // The same object used twice with different access is two answers to one question, and the
        // grant that comes out of it would be whichever was read last.
        std::map<std::pair<int, std::string>, const ApplicationManifest::Uses *> usedOnce;
        for (const auto &used: result.manifest.uses) {
            const auto key = std::pair{static_cast<int>(used.kind), used.name};
            const auto [entry, inserted] = usedOnce.emplace(key, &used);
            // Both are sorted, so this is set equality: the same ways in a different order are the
            // same declaration, and a different set is two answers to one question.
            if (!inserted && entry->second->access != used.access) {
                result.errors.push_back(used.source + ": " + describe(used.kind, used.name) + " is used as '" +
                                        ToString(used.access) + "' here and as '" + ToString(entry->second->access) +
                                        "' in " + entry->second->source);
            }
        }

        if (!result.ok()) result.manifest = {};
        return result;
    }

}// namespace Euclid::Core
