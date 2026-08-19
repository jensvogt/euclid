// C++ includes
#include <mutex>

// Libmagic includes
#include <magic.h>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/ContentTypeUtils.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Core {

    namespace {

        constexpr const char *kDefaultContentType = "application/octet-stream";

        struct MagicHandle {

            magic_t magic = magic_open(MAGIC_MIME_TYPE);
            std::mutex mutex;

            MagicHandle() {
                if (!magic) {
                    log_error << "Failed to open libmagic";
                    return;
                }
                const auto magicFile = Configuration::instance().getOr<std::string>("euclid.magic-file", std::string());
                if (magic_load(magic, magicFile.empty() ? nullptr : magicFile.c_str()) != 0) {
                    log_error << "Failed to load magic database, file: " << magicFile;
                    magic_close(magic);
                    magic = nullptr;
                }
            }

            ~MagicHandle() {
                if (magic) magic_close(magic);
            }
        };

        MagicHandle &handle() {
            static MagicHandle instance;
            return instance;
        }

    }// namespace

    std::string ContentTypeUtils::fromContent(const std::string &content) {

        auto &h = handle();
        if (!h.magic) return kDefaultContentType;

        std::lock_guard lock(h.mutex);
        const char *result = magic_buffer(h.magic, content.data(), content.size());
        return result ? result : kDefaultContentType;
    }

    std::string ContentTypeUtils::fromFile(const std::string &filePath) {

        auto &h = handle();
        if (!h.magic) return kDefaultContentType;

        std::lock_guard lock(h.mutex);
        const char *result = magic_file(h.magic, filePath.c_str());
        return result ? result : kDefaultContentType;
    }

}// namespace Euclid::Core
