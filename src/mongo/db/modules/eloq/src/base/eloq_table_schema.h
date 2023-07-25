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
#pragma once

#include <memory>
#include <string>

#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/query/collation/collator_interface.h"

#include "mongo/db/modules/eloq/src/base/eloq_key.h"

#include "mongo/db/modules/eloq/tx_service/include/catalog_factory.h"
#include "mongo/db/modules/eloq/tx_service/include/schema.h"
#include "mongo/db/modules/eloq/tx_service/include/table_statistics.h"
#include "mongo/db/modules/eloq/tx_service/include/type.h"

namespace Eloq {
class MongoKeySchema final : public txservice::KeySchema {
public:
    explicit MongoKeySchema(uint64_t key_version) : schema_ts_(key_version) {}

    bool CompareKeys(const txservice::TxKey& key1,
                     const txservice::TxKey& key2,
                     size_t* const column_index) const override {
        *column_index = 0;

        // are they different?
        auto mongoKey1 = key1.GetKey<MongoKey>();
        auto mongoKey2 = key2.GetKey<MongoKey>();
        return mongoKey1 != mongoKey2;
    }

    uint16_t ExtendKeyParts() const override {
        return 1;
    }
    uint64_t SchemaTs() const override {
        return schema_ts_;
    }

private:
    // The timestamp when this key was created.
    uint64_t schema_ts_{1};
};

/*
 * Actually there is nothing about record schema beyond BSON in MongoDB.
 */
class MongoRecordSchema final : public txservice::RecordSchema {
public:
    explicit MongoRecordSchema() = default;
};

class MongoSkEncoder : public txservice::SkEncoder {
public:
    explicit MongoSkEncoder(const mongo::BSONObj& spec);

protected:
    mongo::BSONObjSet GenerateBSONKeys(const txservice::TxKey* pk,
                                       const txservice::TxRecord* record) const noexcept(false);

    mongo::RecordId GenerateRecordID(const txservice::TxKey* pk) const;

protected:
    std::unique_ptr<mongo::IndexDescriptor> key_descriptor_;
    std::optional<mongo::Ordering> ordering_;  // Ordering has private constructor.
                                               //
    std::unique_ptr<mongo::CollatorInterface> collator_;
    std::unique_ptr<mongo::BtreeKeyGenerator> key_generator_;
};

class MongoStandardSkEncoder : public MongoSkEncoder {
public:
    explicit MongoStandardSkEncoder(const mongo::BSONObj& spec) : MongoSkEncoder(spec) {}

    bool AppendPackedSk(const txservice::TxKey* pk,
                        const txservice::TxRecord* record,
                        uint64_t version,
                        std::vector<txservice::WriteEntry>& dest_vec) override;
};

class MongoUniqueSkEncoder : public MongoSkEncoder {
public:
    explicit MongoUniqueSkEncoder(const mongo::BSONObj& spec) : MongoSkEncoder(spec) {}

    bool AppendPackedSk(const txservice::TxKey* pk,
                        const txservice::TxRecord* record,
                        uint64_t version_ts,
                        std::vector<txservice::WriteEntry>& dest_vec) override;
};

class MongoTableSchema final : public txservice::TableSchema {
public:
    explicit MongoTableSchema(const txservice::TableName& table_name,
                              const std::string& catalog_image,
                              uint64_t version);

    const txservice::TableName& GetBaseTableName() const override {
        return base_table_name_;
    }

    const txservice::KeySchema* KeySchema() const override {
        return key_schema_.get();
    }

    const txservice::RecordSchema* RecordSchema() const override {
        return &record_schema_;
    }

    const std::string& SchemaImage() const override {
        return schema_image_;
    }

    const std::unordered_map<uint, std::pair<txservice::TableName, txservice::SecondaryKeySchema>>*
    GetIndexes() const override {
        return &indexes_;
    }

    txservice::KVCatalogInfo* GetKVCatalogInfo() const override {
        return kv_info_.get();
    }

    void SetKVCatalogInfo(const std::string& kv_info) override;

    uint64_t Version() const override {
        return version_;
    }

    std::string_view VersionStringView() const override {
        return version_s_;
    }

    std::vector<txservice::TableName> IndexNames() const override {
        std::vector<txservice::TableName> index_names;
        index_names.reserve(indexes_.size());
        for (const auto& index_entry : indexes_) {
            index_names.emplace_back(index_entry.second.first);
        }
        return index_names;
    }

    size_t IndexesSize() const override {
        return indexes_.size();
    }

    const txservice::SecondaryKeySchema* IndexKeySchema(
        const txservice::TableName& index_name) const override {
        for (const auto& index_entry : indexes_) {
            if (index_entry.second.first == index_name) {
                return &index_entry.second.second;
            }
        }

        return nullptr;
    }

    void BindStatistics(std::shared_ptr<txservice::Statistics> statistics) override {
        std::shared_ptr<txservice::TableStatistics<MongoKey>> table_statistics =
            std::dynamic_pointer_cast<txservice::TableStatistics<MongoKey>>(statistics);
        invariant(table_statistics != nullptr);
        table_statistics_ = table_statistics;
    }

    std::shared_ptr<txservice::Statistics> StatisticsObject() const override {
        return table_statistics_;
    }

    txservice::SkEncoder::uptr CreateSkEncoder(
        const txservice::TableName& index_name) const override;

    bool HasAutoIncrement() const override {
        return false;
    }

    const txservice::TableName* GetSequenceTableName() const override {
        return nullptr;
    }

    std::pair<txservice::TxKey, txservice::TxRecord::Uptr> GetSequenceKeyAndInitRecord(
        const txservice::TableName& table_name) const override {
        return {MongoKey::GetNegInfTxKey(), nullptr};
    }

    const std::string& MetaData() const {
        return metadata_;
    }

private:
    mongo::BSONObj meta_obj_;

    std::unique_ptr<MongoKeySchema> key_schema_;  // pk schema
    MongoRecordSchema record_schema_;
    std::unordered_map<uint, std::pair<txservice::TableName, txservice::SecondaryKeySchema>>
        indexes_;  // sk schema

    const txservice::TableName base_table_name_;  // string owner
    const std::string schema_image_;
    std::string metadata_;
    const uint64_t version_;
    const std::string version_s_;
    txservice::KVCatalogInfo::uptr kv_info_;

    std::shared_ptr<txservice::TableStatistics<MongoKey>> table_statistics_{nullptr};
};


}  // namespace Eloq
