//
// Created by vogje01 on 9/10/26.
//

// C++ includes
#include <algorithm>
#include <cctype>

// Euclid includes
#include <euclid/database/entity/ekv/SortCondition.h>

namespace Euclid::Database::Entity::EKV {

    std::optional<SortCondition::Operator> SortCondition::OperatorFromString(const std::string &name) {

        std::string lowered = name;
        std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lowered.empty() || lowered == "none") return Operator::None;
        if (lowered == "eq" || lowered == "=") return Operator::Equals;
        if (lowered == "lt" || lowered == "<") return Operator::LessThan;
        if (lowered == "le" || lowered == "lte" || lowered == "<=") return Operator::LessOrEqual;
        if (lowered == "gt" || lowered == ">") return Operator::GreaterThan;
        if (lowered == "ge" || lowered == "gte" || lowered == ">=") return Operator::GreaterOrEqual;
        if (lowered == "between") return Operator::Between;
        if (lowered == "begins-with" || lowered == "begins_with" || lowered == "beginswith") return Operator::BeginsWith;
        return std::nullopt;
    }

}// namespace Euclid::Database::Entity::EKV
