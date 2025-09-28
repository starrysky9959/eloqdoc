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

#include "cc_protocol.h"
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "boost/optional/optional.hpp"

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"

#include "mongo/db/modules/eloq/src/base/eloq_key.h"
#include "mongo/db/modules/eloq/src/base/eloq_record.h"
#include "mongo/db/modules/eloq/src/base/eloq_table_schema.h"
#include "mongo/db/modules/eloq/src/eloq_cursor.h"

#include "mongo/db/modules/eloq/tx_service/include/catalog_key_record.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_execution.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_service.h"
#include "mongo/db/modules/eloq/tx_service/include/type.h"

namespace mongo {
class EloqKVPair {
public:
    EloqKVPair() = default;

    void reset() {
        _key.Reset();
        _valuePtr = nullptr;
        _internalStore.Reset();
    }

    Eloq::MongoKey& keyRef() {
        return _key;
    }

    void setInternalValuePtr() {
        _valuePtr = &_internalStore;
    }

    void setValuePtr(const Eloq::MongoRecord* ptr) {
        _valuePtr = ptr;
    }

    const Eloq::MongoRecord* getValuePtr() const {
        return _valuePtr;
    }

    Eloq::MongoRecord* getValuePtr() {
        return const_cast<Eloq::MongoRecord*>(_valuePtr);
    }

private:
    Eloq::MongoKey _key;
    const Eloq::MongoRecord* _valuePtr{nullptr};
    Eloq::MongoRecord _internalStore;
};

// The RecoveryUnit controls what snapshot a storage engine transaction uses for its reads.
class EloqRecoveryUnit final : public RecoveryUnit {
public:
    explicit EloqRecoveryUnit(txservice::TxService* txService);
    void reset() override;
    ~EloqRecoveryUnit() override;

    void setOperationContext(OperationContext* opCtx) override;

    void beginUnitOfWork(OperationContext* opCtx) override;
    void commitUnitOfWork() override;
    void abortUnitOfWork() override;

    bool waitUntilDurable() override;

    void abandonSnapshot() override;
    void preallocateSnapshot() override;

    SnapshotId getSnapshotId() const override;

    Status setTimestamp(Timestamp timestamp) override;
    void setCommitTimestamp(Timestamp timestamp) override;
    void clearCommitTimestamp() override;
    Timestamp getCommitTimestamp() override;

    ReadSource getTimestampReadSource() const override {
        return ReadSource::kNoTimestamp;
    };

    void registerChange(Change* change) override;

    void* writingPtr(void* data, size_t len) override;

    void setRollbackWritesDisabled() override;

    void setOrderedCommit(bool orderedCommit) override;

    static EloqRecoveryUnit* get(OperationContext* opCtx);
    txservice::TransactionExecution* getTxm();
    bool inActiveTxn() const;

    void registerCursor(EloqCursor* cursor);
    void closeAllCursors();
    void unregisterCursor(EloqCursor* cursor);

    // Eloq API Warpper
    [[nodiscard]] std::pair<bool, txservice::TxErrorCode> readCatalog(
        const txservice::CatalogKey& catalogKey,
        txservice::CatalogRecord& catalogRecord,
        bool isForWrite);

    [[nodiscard]] std::pair<bool, txservice::TxErrorCode> setKV(
        const txservice::TableName* tableName,
        uint64_t keySchemaVersion,
        std::unique_ptr<Eloq::MongoKey> key,
        std::unique_ptr<Eloq::MongoRecord> record,
        txservice::OperationType operationType,
        bool checkUnique = false);
    [[nodiscard]] std::pair<bool, txservice::TxErrorCode> getKV(
        OperationContext* opCtx,
        const txservice::TableName* tableName,
        uint64_t keySchemaVersion,
        const Eloq::MongoKey* key,
        Eloq::MongoRecord* record,
        bool isForWrite);
    // store in the internal kvpair
    [[nodiscard]] std::pair<bool, txservice::TxErrorCode> getKVInternal(
        OperationContext* opCtx,
        const txservice::TableName* tableName,
        uint64_t keySchemaVersion,
        bool isForWrite = false,
        bool readLocal = false);

    Status createTable(const txservice::TableName& tableName, std::string_view metadata);
    Status dropTable(const txservice::TableName& tableName,
                     const txservice::CatalogRecord& catalogRecord);
    Status updateTable(const txservice::TableName& tableName,
                       const txservice::CatalogRecord& catalogRecord,
                       std::string_view oldMetadata,
                       std::string_view newMetadata);

    EloqKVPair& getKVPair();

    const Eloq::MongoTableSchema* getTableSchema(const txservice::TableName& tableName) const;

    const txservice::KeySchema* getIndexSchema(const txservice::TableName& tableName) const;
    const txservice::KeySchema* getIndexSchema(const txservice::TableName& tableName,
                                               const txservice::TableName& indexName) const;

    void deleteTableSchema(const txservice::TableName& tableName);

private:
    void _abort();
    void _commit();

    void _txnOpen(txservice::IsolationLevel isolationLevel);
    void _txnClose(bool commit);

private:
    txservice::TxService* _txService;         // not owned
    const OperationContext* _opCtx{nullptr};  // not owned;
    txservice::TransactionExecution* _txm{nullptr};

    bool _areWriteUnitOfWorksBanned{false};
    bool _inUnitOfWork{false};
    bool _active{false};
    bool _isTimestamped{false};
    bool _inMultiDocumentTransation{false};

    std::set<EloqCursor*> _cursors;

    EloqKVPair _kvPair;

    Timestamp _commitTimestamp;
    Timestamp _prepareTimestamp;
    boost::optional<Timestamp> _lastTimestampSet;
    uint64_t _mySnapshotId;

    using Changes = std::vector<std::unique_ptr<Change>>;
    Changes _changes;

    mutable std::unordered_map<txservice::TableName, std::shared_ptr<const Eloq::MongoTableSchema>>
        _discoveredTableSchemaMap;
    // butil::Timer _timer;
};

}  // namespace mongo
