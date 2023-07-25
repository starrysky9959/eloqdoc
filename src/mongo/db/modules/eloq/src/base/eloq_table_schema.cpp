/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the license:
 *    1. GNU Affero General Public License, version 3, as published by the Free
 *    Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/util/log.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/service_context.h"

#include "mongo/db/modules/eloq/src/base/eloq_record.h"
#include "mongo/db/modules/eloq/src/base/eloq_table_schema.h"
#include "mongo/db/modules/eloq/src/base/eloq_util.h"
#include "mongo/db/modules/eloq/src/eloq_record_store.h"
#include "mongo/db/modules/eloq/src/store_handler/kv_store.h"

namespace Eloq {
extern std::unique_ptr<txservice::store::DataStoreHandler> storeHandler;

MongoSkEncoder::MongoSkEncoder(const mongo::BSONObj& spec) {
    mongo::Collection* collection = nullptr;
    mongo::BSONObj key_pattern = spec.getObjectField("key");
    const std::string& access_method = mongo::IndexNames::findPluginName(key_pattern);

    key_descriptor_ = std::make_unique<mongo::IndexDescriptor>(collection, access_method, spec);
    ordering_ = mongo::Ordering::make(key_descriptor_->keyPattern());

    // Prepare parameters for BtreeKeyGenerator::make. Reference:
    // BtreeAccessMethod::BtreeAccessMethod.
    std::vector<const char*> field_names;
    std::vector<mongo::BSONElement> fixed;
    mongo::BSONObjIterator it(key_descriptor_->keyPattern());
    while (it.more()) {
        mongo::BSONElement elt = it.next();
        field_names.emplace_back(elt.fieldName());
        fixed.emplace_back();
    }

    if (mongo::BSONElement collation_element = key_descriptor_->getInfoElement("collation");
        collation_element) {
        invariant(collation_element.isABSONObj());
        mongo::BSONObj collation = collation_element.Obj();
        mongo::ServiceContext* service_context = mongo::getGlobalServiceContext();
        collator_ = std::move(mongo::CollatorFactoryInterface::get(service_context)
                                  ->makeFromBSON(collation)
                                  .getValue());
    }

    key_generator_ = mongo::BtreeKeyGenerator::make(key_descriptor_->version(),
                                                    field_names,
                                                    fixed,
                                                    key_descriptor_->isSparse(),
                                                    collator_.get());
    invariant(key_generator_);
}

mongo::BSONObjSet MongoSkEncoder::GenerateBSONKeys(const txservice::TxKey* pk,
                                                   const txservice::TxRecord* record) const
    noexcept(false) {

    const MongoRecord* mongo_rec = static_cast<const MongoRecord*>(record);

    mongo::BSONObj record_obj(mongo_rec->EncodedBlobData());

    invariant(([pk, &record_obj]() {
                  const MongoKey* mongo_pk = pk->GetKey<MongoKey>();
                  mongo::RecordId record_id(mongo_pk->Data(), mongo_pk->Size());
                  mongo::BSONObj id_obj = mongo::getIdBSONObjWithoutFieldName(record_obj);
                  mongo::KeyString keystring_pk(
                      mongo::KeyString::kLatestVersion, id_obj, mongo::kIdOrdering);
                  return record_id ==
                      mongo::RecordId(keystring_pk.getBuffer(), keystring_pk.getSize());
              }()) == true);

    mongo::BSONObjSet keys = mongo::SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    mongo::MultikeyPaths multikey_paths;  // Feature multikey is not supported yet.

    // Reference: IndexAccessMethod::getKeys().
    //
    // Suppress no indexing errors when mode is kEnforceConstraints.
    // IndexAccessMethod::GetKeysMode getKeysMode =
    //     IndexAccessMethod::GetKeysMode::kEnforceConstraints;
    //
    // The below call may raise Exception. However TxService doesn't handle it
    // currently.
    key_generator_->getKeys(record_obj, &keys, &multikey_paths);


    invariant(keys.size() >= 1 && multikey_paths.size() >= 1);
    return keys;
}

mongo::RecordId MongoSkEncoder::GenerateRecordID(const txservice::TxKey* pk) const {
    const MongoKey* mongo_pk = pk->GetKey<MongoKey>();
    return mongo::RecordId(mongo_pk->Data(), mongo_pk->Size());
}


bool MongoStandardSkEncoder::AppendPackedSk(const txservice::TxKey* pk,
                                            const txservice::TxRecord* record,
                                            uint64_t version_ts,
                                            std::vector<txservice::WriteEntry>& dest_vec) {
    bool succeed = true;
    try {
        mongo::RecordId record_id = GenerateRecordID(pk);
        mongo::BSONObjSet keys = GenerateBSONKeys(pk, record);
        for (const mongo::BSONObj& bson_sk : keys) {
            mongo::KeyString keystring_sk(
                mongo::KeyString::kLatestVersion, bson_sk, ordering_.value(), record_id);
            auto mongo_sk =
                std::make_unique<MongoKey>(keystring_sk.getBuffer(), keystring_sk.getSize());
            auto mongo_sk_rec = std::make_unique<MongoRecord>();
            if (const auto& type_bits = keystring_sk.getTypeBits(); !type_bits.isAllZeros()) {
                mongo_sk_rec->SetUnpackInfo(type_bits.getBuffer(), type_bits.getSize());
            }
            dest_vec.emplace_back(
                txservice::TxKey(std::move(mongo_sk)), std::move(mongo_sk_rec), version_ts);
        }
    } catch (const mongo::DBException& e) {
        succeed = false;
        SkEncoder::SetError(e.code(), e.what());
        MONGO_LOG(1) << "MongoStandardSkEncoder::AppendPackedSk raise DBException" << e.what();
    }
    return succeed;
}

bool MongoUniqueSkEncoder::AppendPackedSk(const txservice::TxKey* pk,
                                          const txservice::TxRecord* record,
                                          uint64_t version_ts,
                                          std::vector<txservice::WriteEntry>& dest_vec) {
    bool succeed = true;
    try {
        mongo::BSONObjSet keys = GenerateBSONKeys(pk, record);
        mongo::RecordId record_id = GenerateRecordID(pk);
        std::string_view record_id_sv = record_id.getStringView();

        for (const mongo::BSONObj& bson_sk : keys) {
            mongo::KeyString keystring_sk(
                mongo::KeyString::kLatestVersion, bson_sk, ordering_.value());
            auto mongo_sk =
                std::make_unique<MongoKey>(keystring_sk.getBuffer(), keystring_sk.getSize());
            auto mongo_sk_rec = std::make_unique<MongoRecord>();
            mongo_sk_rec->SetEncodedBlob(record_id_sv);
            if (const auto& type_bits = keystring_sk.getTypeBits(); !type_bits.isAllZeros()) {
                mongo_sk_rec->SetUnpackInfo(type_bits.getBuffer(), type_bits.getSize());
            }
            dest_vec.emplace_back(
                txservice::TxKey(std::move(mongo_sk)), std::move(mongo_sk_rec), version_ts);
        }
    } catch (const mongo::DBException& e) {
        succeed = false;
        SkEncoder::SetError(e.code(), e.what());
        MONGO_LOG(1) << "MongoUniqueSkEncoder::AppendPackedSk raise DBException" << e.what();
    }
    return succeed;
}

MongoTableSchema::MongoTableSchema(const txservice::TableName& table_name,
                                   const std::string& catalog_image,
                                   uint64_t version)
    : base_table_name_{table_name.StringView().data(),
                       table_name.StringView().size(),
                       table_name.Type()},
      schema_image_{catalog_image},
      version_{version},
      version_s_{std::to_string(version)} {
    std::string kv_info, schemas_ts_str;
    Eloq::DeserializeSchemaImage(catalog_image, metadata_, kv_info, schemas_ts_str);
    invariant(!metadata_.empty());

    txservice::TableKeySchemaTs key_schemas_ts;
    size_t ts_offset = 0;
    key_schemas_ts.Deserialize(schemas_ts_str.data(), ts_offset);

    uint64_t key_schema_ts = key_schemas_ts.GetKeySchemaTs(table_name);
    key_schema_ts = key_schema_ts == 1 ? version_ : key_schema_ts;
    key_schema_ = std::make_unique<MongoKeySchema>(key_schema_ts);

    meta_obj_ = mongo::BSONObj(metadata_.c_str()).getOwned();
    auto [ns, indexes] = ExtractReadyIndexesVector(meta_obj_);

    for (size_t kid = 0; kid < indexes.size(); ++kid) {
        key_schema_ts = key_schemas_ts.GetKeySchemaTs(indexes[kid]);
        key_schema_ts = key_schema_ts == 1 ? version_ : key_schema_ts;

        auto sk_schema = std::make_unique<MongoKeySchema>(key_schema_ts);

        indexes_.try_emplace(
            kid,
            std::move(indexes[kid]),
            txservice::SecondaryKeySchema{std::move(sk_schema), key_schema_.get()});
    }

    size_t offset = 0;
    kv_info_ = storeHandler->DeserializeKVCatalogInfo(kv_info, offset);
}

void MongoTableSchema::SetKVCatalogInfo(const std::string& kv_info) {
    size_t offset = 0;
    kv_info_ = storeHandler->DeserializeKVCatalogInfo(kv_info, offset);
}

txservice::SkEncoder::uptr MongoTableSchema::CreateSkEncoder(
    const txservice::TableName& index_name) const {

    txservice::SkEncoder::uptr sk_encoder;

    mongo::BSONObj md_obj = meta_obj_.getObjectField("md");
    const char* ns = md_obj.getStringField("ns");
    mongo::BSONObj indexes_obj = md_obj.getObjectField("indexes");
    invariant(indexes_obj.couldBeArray());
    for (const mongo::BSONElement& index_element : indexes_obj) {
        mongo::BSONObj index_obj = index_element.Obj();
        mongo::BSONObj spec_obj = index_obj.getObjectField("spec");
        const char* name = spec_obj.getStringField("name");
        bool is_unique = spec_obj.getBoolField("unique");
        txservice::TableName sk_name = MongoIndexToTxServiceTableName(ns, name, is_unique);
        if (sk_name == index_name) {
            if (is_unique) {
                sk_encoder = std::make_unique<MongoUniqueSkEncoder>(spec_obj);
            } else {
                sk_encoder = std::make_unique<MongoStandardSkEncoder>(spec_obj);
            }
            break;
        }
    }

    return sk_encoder;
}
}  // namespace Eloq
