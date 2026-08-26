#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/dto/com/Variant.h>

namespace Euclid::CLI {

    /**
     * @brief Convert a value/type to a Variant
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct BaseCli {

        static Dto::COM::Variant optionToVariant(const std::string &value, const std::string &type) {
            if (type.empty() || type == "string") {
                return Dto::COM::Variant(value);
            }
            if (type == "int") {
                return Dto::COM::Variant(std::stoi(value));
            }
            if (type == "long") {
                return Dto::COM::Variant(std::stol(value));
            }
            if (type == "double") {
                return Dto::COM::Variant(std::stod(value));
            }
            if (type == "float") {
                return Dto::COM::Variant(std::stof(value));
            }
            if (type == "bool") {
                return Dto::COM::Variant(value == "true" || value == "1");
            }
            if (type == "binary") {
                // decode binary representation
            }
            return {};
        }

    };

}