//
// Created by vogje01 on 03/09/2023.
//

#include <euclid/database/entity/storage/Bucket.h>

namespace Euclid::Database::Entity::Storage {

    bsoncxx::document::value Bucket::toDocument() const {

        // bsoncxx::builder::basic::document tagsDoc;
        // for (const auto &[k, v]: tags) {
        //     tagsDoc.append(bsoncxx::builder::basic::kvp(k, v));
        // }

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("region", region),
                bsoncxx::builder::basic::kvp("owner", owner),
                bsoncxx::builder::basic::kvp("name", name),
                bsoncxx::builder::basic::kvp("ern", ern));
    }

    Bucket Bucket::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        Bucket bucket;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") bucket.oid = field.get_oid().value.to_string();
            else if (key == "region") bucket.region = std::string(field.get_string().value);
            else if (key == "owner") bucket.owner = std::string(field.get_string().value);
            else if (key == "name") bucket.name = std::string(field.get_string().value);
            else if (key == "ern") bucket.ern = std::string(field.get_string().value);
            else if (key == "created") bucket.created = system_clock::time_point{field.get_date().value};
            else if (key == "modified") bucket.modified = system_clock::time_point{field.get_date().value};
        }
        return bucket;
    }

}// namespace Euclid::Database::Entity::Storage