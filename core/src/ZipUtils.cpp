//
// Created by vogje01 on 25/08/2026.
//

#include <euclid/core/ZipUtils.h>

// C++ includes
#include <fstream>
#include <stdexcept>

// Archive includes
#include <archive.h>
#include <archive_entry.h>

// Euclid includes
#include <euclid/core/LogStream.h>

namespace Euclid::Core {

    namespace fs = std::filesystem;

    void ZipUtils::Zip(const std::string &sourcePath, const std::string &zipFile) {

        const fs::path source(sourcePath);
        if (!fs::exists(source)) {
            throw std::runtime_error("Cannot zip, source does not exist: " + sourcePath);
        }

        log_trace << "Zipping started, source: " << sourcePath << ", zipFile: " << zipFile;

        archive *a = archive_write_new();
        archive_write_set_format_zip(a);

        if (archive_write_open_filename(a, zipFile.c_str()) != ARCHIVE_OK) {
            const std::string error = archive_error_string(a);
            archive_write_free(a);
            throw std::runtime_error("Cannot open ZIP file for writing, path: " + zipFile + ", error: " + error);
        }

        try {
            if (fs::is_directory(source)) {
                for (const auto &entry: fs::recursive_directory_iterator(source)) {
                    if (!entry.is_regular_file()) continue;
                    const fs::path relative = source.filename() / fs::relative(entry.path(), source);
                    AddFileEntry(a, entry.path(), relative.generic_string());
                }
            } else {
                AddFileEntry(a, source, source.filename().generic_string());
            }
        } catch (...) {
            archive_write_close(a);
            archive_write_free(a);
            throw;
        }

        archive_write_close(a);
        archive_write_free(a);

        log_trace << "Zipping finished, source: " << sourcePath << ", zipFile: " << zipFile;
    }

    void ZipUtils::AddFileEntry(archive *writer, const fs::path &filePath, const std::string &entryName) {

        std::error_code ec;
        const auto fileSize = fs::file_size(filePath, ec);
        if (ec) {
            throw std::runtime_error("Cannot determine file size, path: " + filePath.string() + ", error: " + ec.message());
        }

        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, entryName.c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(fileSize));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);

        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            const std::string error = archive_error_string(writer);
            archive_entry_free(entry);
            throw std::runtime_error("Cannot write ZIP entry header, path: " + entryName + ", error: " + error);
        }

        std::ifstream ifs(filePath, std::ios::binary);
        if (!ifs) {
            archive_entry_free(entry);
            throw std::runtime_error("Cannot open file for reading, path: " + filePath.string());
        }

        char buffer[8192];
        while (ifs.read(buffer, sizeof(buffer)) || ifs.gcount() > 0) {
            archive_write_data(writer, buffer, static_cast<std::size_t>(ifs.gcount()));
        }

        archive_write_finish_entry(writer);
        archive_entry_free(entry);
    }

} // namespace Euclid::Core
