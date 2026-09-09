//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <string>

namespace Euclid::Core {

    /**
     * @brief Content-type detection utilities, backed by libmagic.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class ContentTypeUtils {
    public:

        /**
         * @brief Determines the MIME content-type of a string by analyzing its content with libmagic.
         *
         * @param content content to analyze.
         * @return MIME content-type, or "application/octet-stream" if it cannot be determined.
         */
        static std::string fromContent(const std::string &content);

        /**
         * @brief Determines the MIME content-type of a file by analyzing its content with libmagic.
         *
         * @param filePath absolute path of the file to analyze.
         * @return MIME content-type, or "application/octet-stream" if it cannot be determined.
         */
        static std::string fromFile(const std::string &filePath);

        /**
         * @brief Determines the MIME content-type from the extension of a key or file name.
         *
         * Only the text formats libmagic cannot recognize from a prefix are mapped; everything
         * else is left to fromContent()/fromFile().
         *
         * @param key object key or file name, with or without directories in front of it.
         * @return MIME content-type, or an empty string if the extension is absent or unknown.
         */
        static std::string fromKey(const std::string &key);

        /**
         * @brief Determines the MIME content-type of an object from its first bytes, falling back
         * to its key's extension when the content itself says nothing specific.
         *
         * @param content first bytes of the object, as handed to fromContent().
         * @param key key the object is stored under, as handed to fromKey().
         * @return MIME content-type, never empty.
         */
        static std::string detect(const std::string &content, const std::string &key);
    };

}// namespace Euclid::Core
