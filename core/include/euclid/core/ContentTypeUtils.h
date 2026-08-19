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
    };

}// namespace Euclid::Core
