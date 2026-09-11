// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <string_view>


namespace Euclid::Database::Emd::Protocol {

    /**
     * @brief What one store operation is called on the wire.
     *
     * @par
     * The same names the store's own methods have, hyphenated - so a request in a log or a packet
     * capture reads as the call it is. They travel in the "x-euclid-action" header, like every
     * other request in euclid; the body is BSON rather than JSON, because what it carries is
     * documents that are already BSON and would otherwise be converted twice.
     */
    constexpr char kFindOne[] = "find-one";
    constexpr char kFind[] = "find";
    constexpr char kInsertOne[] = "insert-one";
    constexpr char kUpdateOne[] = "update-one";
    constexpr char kUpdateMany[] = "update-many";
    constexpr char kReplaceOne[] = "replace-one";
    constexpr char kFindOneAndUpdate[] = "find-one-and-update";
    constexpr char kFindOneAndDelete[] = "find-one-and-delete";
    constexpr char kDelete[] = "delete";
    constexpr char kCount[] = "count";
    constexpr char kGroupCount[] = "group-count";
    constexpr char kCreateIndex[] = "create-index";

    /**
     * @brief Field names inside the request and response documents.
     *
     * @par
     * Short, because every one of them is on the wire for every call, and named for what they are
     * rather than for the method they belong to - "filter" means the same thing in all nine
     * operations that take one.
     */
    constexpr char kCollection[] = "c";
    constexpr char kFilter[] = "f";
    constexpr char kUpdate[] = "u";
    constexpr char kDocument[] = "d";
    constexpr char kSort[] = "s";
    constexpr char kLimit[] = "l";
    constexpr char kSkip[] = "k";
    constexpr char kUpsert[] = "up";
    constexpr char kReturnAfter[] = "ra";
    constexpr char kSingle[] = "sg";
    constexpr char kUnique[] = "un";
    constexpr char kKeys[] = "ky";
    constexpr char kGroupFields[] = "gf";
    constexpr char kSumField[] = "sf";

    /**
     * @brief Field names in a reply.
     */
    constexpr char kFound[] = "found";
    constexpr char kDocuments[] = "documents";
    constexpr char kMatched[] = "matched";
    constexpr char kModified[] = "modified";
    constexpr char kUpsertedId[] = "upsertedId";
    constexpr char kCount_[] = "count";
    constexpr char kGroups[] = "groups";
    constexpr char kKey[] = "key";
    constexpr char kSum[] = "sum";
    constexpr char kOid[] = "oid";

    /**
     * @brief Carried on a reply the store could not produce, so the client can raise the same
     * error the store would have raised in-process - an unimplemented operator has to reach the
     * caller either way, or it silently becomes "nothing matched".
     */
    constexpr char kError[] = "error";

}// namespace Euclid::Database::Emd::Protocol
