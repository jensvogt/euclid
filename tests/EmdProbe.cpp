#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <iostream>
int main() {
    Euclid::Core::LogStream::Initialize();
    Euclid::Database::Database::instance().initializeRemote("/tmp/emd-test/run/emd.sock");
    const auto c = Euclid::Database::Database::instance().collection("probe_collection");
    std::ignore = c.insert_one(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("hello", "world")));
    std::cout << "after insert, count = " << c.count_documents({}) << "\n";
    return 0;
}
