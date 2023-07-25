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

#include <atomic>
#include <exception>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"

#include "mongo/db/modules/eloq/src/base/eloq_key.h"
#include "mongo/db/modules/eloq/src/base/eloq_record.h"
#include "mongo/db/modules/eloq/src/base/eloq_table_schema.h"
#include "mongo/db/modules/eloq/src/base/eloq_util.h"
#include "mongo/db/modules/eloq/src/eloq_recovery_unit.h"
#include "mongo/db/modules/eloq/src/store_handler/kv_store.h"

#include "mongo/db/modules/eloq/tx_service/include/cc_protocol.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_util.h"

#include <butil/time.h>
#include <bvar/latency_recorder.h>

namespace recorder {
bvar::LatencyRecorder kCommitLatency{"mongo_commit"};
bvar::LatencyRecorder kDBRequestHandleLatency{"mongo_dbrequest_handle"};
}  // namespace recorder


namespace Eloq {
extern std::unique_ptr<txservice::store::DataStoreHandler> storeHandler;
}

namespace mongo {

extern thread_local int16_t localThreadId;

namespace {

std::atomic<uint64_t> nextSnapshotId{1};

}  // namespace

txservice::AlterTableInfo getAlterTableInfo(std::string_view oldMetadata,
                                            std::string_view newMetadata) {

    const BSONObj oldMetadataObj{oldMetadata.data()};
    const BSONObj newMetadataObj{newMetadata.data()};
    auto [oldNamespace, oldIndexes] = Eloq::ExtractReadyIndexesSet(oldMetadataObj);
    auto [newNamespace, newIndexes] = Eloq::ExtractReadyIndexesSet(newMetadataObj);
    dassert(oldNamespace == newNamespace);

    std::vector<txservice::TableName> addIndexNameVector;
    std::set_difference(newIndexes.begin(),
                        newIndexes.end(),
                        oldIndexes.begin(),
                        oldIndexes.end(),
                        std::back_inserter(addIndexNameVector));

    std::vector<txservice::TableName> dropIndexNameVector;
    std::set_difference(oldIndexes.begin(),
                        oldIndexes.end(),
                        newIndexes.begin(),
                        newIndexes.end(),
                        std::back_inserter(dropIndexNameVector));

    txservice::AlterTableInfo alterTableInfo;
    alterTableInfo.index_add_count_ = addIndexNameVector.size();
    alterTableInfo.index_drop_count_ = dropIndexNameVector.size();

    for (auto&& index : addIndexNameVector) {
        alterTableInfo.index_add_names_.try_emplace(std::move(index));
    }
    for (auto&& index : dropIndexNameVector) {
        alterTableInfo.index_drop_names_.try_emplace(std::move(index));
    }

    return alterTableInfo;
}

EloqRecoveryUnit::EloqRecoveryUnit(txservice::TxService* txService)
    : _txService(txService), _mySnapshotId(nextSnapshotId.fetch_add(1)) {
    // _timer.start();
    MONGO_LOG(1) << "EloqRecoveryUnit::EloqRecoveryUnit";
}

void EloqRecoveryUnit::reset() {
    MONGO_LOG(1) << "EloqRecoveryUnit::reset";
    _mySnapshotId = nextSnapshotId.fetch_add(1);
    // _timer.start();
    closeAllCursors();

    _opCtx = nullptr;
    _txm = nullptr;
    _areWriteUnitOfWorksBanned = false;
    _inUnitOfWork = false;
    _active = false;
    _isTimestamped = false;
    _inMultiDocumentTransation = false;
    _kvPair.reset();
    _commitTimestamp.reset();
    _prepareTimestamp.reset();
    _lastTimestampSet.reset();
    _changes.clear();
    _discoveredTableSchemaMap.clear();
}

EloqRecoveryUnit::~EloqRecoveryUnit() {
    MONGO_LOG(1) << "EloqRecoveryUnit::~EloqRecoveryUnit";
    invariant(!_inUnitOfWork);
    _abort();
    // _timer.stop();
    // recorder::kDBRequestHandleLatency << _timer.u_elapsed();
}

void EloqRecoveryUnit::setOperationContext(OperationContext* opCtx) {
    MONGO_LOG(1) << "EloqRecoveryUnit::setOperationContext";
    _opCtx = opCtx;
}

void EloqRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    MONGO_LOG(1) << "EloqRecoveryUnit::beginUnitOfWork";
    invariant(!_areWriteUnitOfWorksBanned);
    invariant(!_inUnitOfWork);
    _inUnitOfWork = true;
    _opCtx = opCtx;
}

void EloqRecoveryUnit::commitUnitOfWork() {
    MONGO_LOG(1) << "EloqRecoveryUnit::commitUnitOfWork";
    invariant(_inUnitOfWork);
    _inUnitOfWork = false;
    _commit();
}

void EloqRecoveryUnit::abortUnitOfWork() {
    MONGO_LOG(1) << "EloqRecoveryUnit::abortUnitOfWork";
    invariant(_inUnitOfWork);
    _inUnitOfWork = false;
    _abort();
}

bool EloqRecoveryUnit::waitUntilDurable() {
    MONGO_LOG(1) << "EloqRecoveryUnit::waitUntilDurable";
    return true;
}

void EloqRecoveryUnit::abandonSnapshot() {
    MONGO_LOG(1) << "EloqRecoveryUnit::abandonSnapshot";
    invariant(!_inUnitOfWork);
    if (_active) {
        // Can't be in a WriteUnitOfWork, so safe to rollback
        _txnClose(false);
    }
    _areWriteUnitOfWorksBanned = false;
}

void EloqRecoveryUnit::preallocateSnapshot() {
    MONGO_LOG(1) << "EloqRecoveryUnit::preallocateSnapshot";
    _inMultiDocumentTransation = true;
    // getTxm();
}

void EloqRecoveryUnit::setRollbackWritesDisabled() {
    MONGO_LOG(1) << "EloqRecoveryUnit::setRollbackWritesDisabled";
    //
}

void EloqRecoveryUnit::setOrderedCommit(bool orderedCommit) {
    MONGO_LOG(1) << "EloqRecoveryUnit::setOrderedCommit";
}

SnapshotId EloqRecoveryUnit::getSnapshotId() const {
    MONGO_LOG(1) << "EloqRecoveryUnit::getSnapshotId";
    return SnapshotId(_mySnapshotId);
}

Status EloqRecoveryUnit::setTimestamp(Timestamp timestamp) {
    MONGO_LOG(1) << "EloqRecoveryUnit::setTimestamp";
    return Status::OK();
}

void EloqRecoveryUnit::setCommitTimestamp(Timestamp timestamp) {
    MONGO_LOG(1) << "EloqRecoveryUnit::setCommitTimestamp"
                 << ". timestamp: " << timestamp;
    invariant(!_inUnitOfWork);
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp set to " << _commitTimestamp.toString()
                            << " and trying to set it to " << timestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to set commit timestamp to " << timestamp.toString());
    invariant(!_isTimestamped);

    _commitTimestamp = timestamp;
}

void EloqRecoveryUnit::clearCommitTimestamp() {
    MONGO_LOG(1) << "EloqRecoveryUnit::clearCommitTimestamp";
    invariant(!_inUnitOfWork);
    invariant(!_commitTimestamp.isNull());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to clear commit timestamp.");
    invariant(!_isTimestamped);

    _commitTimestamp = Timestamp();
}

Timestamp EloqRecoveryUnit::getCommitTimestamp() {
    MONGO_LOG(1) << "EloqRecoveryUnit::getCommitTimestamp";
    return _commitTimestamp;
}


void EloqRecoveryUnit::registerChange(Change* change) {
    MONGO_LOG(1) << "EloqRecoveryUnit::registerChange";
    invariant(_inUnitOfWork);
    _changes.push_back(std::unique_ptr<Change>{change});
}

void* EloqRecoveryUnit::writingPtr(void* data, size_t len) {
    MONGO_LOG(1) << "EloqRecoveryUnit::writingPtr";
    // This API should not be used for anything other than the MMAP V1 storage engine
    MONGO_UNREACHABLE;
}


EloqRecoveryUnit* EloqRecoveryUnit::get(OperationContext* opCtx) {
    return checked_cast<EloqRecoveryUnit*>(opCtx->recoveryUnit());
}

txservice::TransactionExecution* EloqRecoveryUnit::getTxm() {
    MONGO_LOG(1) << "EloqRecoveryUnit::getTxm";
    if (!_active) {
        auto isolationLevel = static_cast<txservice::IsolationLevel>(_opCtx->getIsolationLevel());
        _txnOpen(isolationLevel);
    }
    return _txm;
}

bool EloqRecoveryUnit::inActiveTxn() const {
    MONGO_LOG(1) << "EloqRecoveryUnit::inActiveTxn";
    return _active;
}

void EloqRecoveryUnit::registerCursor(EloqCursor* cursor) {
    _cursors.emplace(cursor);
}

void EloqRecoveryUnit::unregisterCursor(EloqCursor* cursor) {
    _cursors.erase(cursor);
}

void EloqRecoveryUnit::closeAllCursors() {
    MONGO_LOG(1) << "EloqRecoveryUnit::closeAllCursors";
    // scan close here
    for (auto& cursor : _cursors) {
        if (cursor->indexScanIsOpen()) {
            cursor->indexScanClose();
        }
    }
    _cursors.clear();
}

std::pair<bool, txservice::TxErrorCode> EloqRecoveryUnit::readCatalog(
    const txservice::CatalogKey& catalogKey,
    txservice::CatalogRecord& catalogRecord,
    bool isForWrite) {
    MONGO_LOG(1) << "EloqRecoveryUnit::readCatalog"
                 << ". catalogKey: " << catalogKey.ToString() << ". isForWrite: " << isForWrite;
    getTxm();

    txservice::TxKey catalogTxKey{&catalogKey};
    auto [yieldFunc, resumeFunc] = _opCtx->getCoroutineFunctors();
    // readlocal=true when read catalog
    txservice::ReadTxRequest readTxReq(&txservice::catalog_ccm_name,
                                       0,
                                       &catalogTxKey,
                                       &catalogRecord,
                                       isForWrite,
                                       false,
                                       true,
                                       0,
                                       false,
                                       false,
                                       false,
                                       yieldFunc,
                                       resumeFunc,
                                       _txm);
    bool exists{false};
    auto errorCode = txservice::TxReadCatalog(_txm, readTxReq, exists);

    return {exists, errorCode};
}

std::pair<bool, txservice::TxErrorCode> EloqRecoveryUnit::setKV(
    const txservice::TableName* tableName,
    uint64_t keySchemaVersion,
    std::unique_ptr<Eloq::MongoKey> key,
    std::unique_ptr<Eloq::MongoRecord> record,
    txservice::OperationType operationType,
    bool checkUnique) {
    MONGO_LOG(1) << "EloqRecoveryUnit::setKV. "
                 << "tableName: " << tableName->StringView() << ". mongoKey: " << key->ToString()
                 << ". mongoRecord: " << record->ToString() << ". OperationType: " << operationType;
    getTxm();
    auto err = _txm->TxUpsert(*tableName,
                              keySchemaVersion,
                              txservice::TxKey(std::move(key)),
                              std::move(record),
                              operationType);
    if (err != txservice::TxErrorCode::NO_ERROR) {
        MONGO_LOG(1) << "txservice TxUpsert failed"
                     << "ErrorCode" << err;
        return {false, err};
    }

    return {true, txservice::TxErrorCode::NO_ERROR};
}

std::pair<bool, txservice::TxErrorCode> EloqRecoveryUnit::getKV(
    OperationContext* opCtx,
    const txservice::TableName* tableName,
    uint64_t keySchemaVersion,
    const Eloq::MongoKey* key,
    Eloq::MongoRecord* record,
    bool isForWrite) {
    MONGO_LOG(1) << "EloqRecoveryUnit::getKV"
                 << ". tableName: " << tableName->StringView() << ". mongoKey: " << key->ToString();
    getTxm();
    txservice::TxKey txKey(key);
    auto [yieldFunc, resumeFunc] = opCtx->getCoroutineFunctors();

    while (true) {
        txservice::ReadTxRequest readTxReq(tableName,
                                           keySchemaVersion,
                                           &txKey,
                                           record,
                                           isForWrite,
                                           false,
                                           false,
                                           0,
                                           false,
                                           false,
                                           false,
                                           yieldFunc,
                                           resumeFunc,
                                           _txm);
        _txm->Execute(&readTxReq);
        readTxReq.Wait();
        MONGO_LOG(1) << "result"
                     << ". tableName: " << tableName->StringView()
                     << ". mongoKey: " << key->ToString()
                     << ". mongoRecord: " << record->ToString();
        if (readTxReq.IsError()) {
            MONGO_LOG(1) << "EloqRecoveryUnit::getKV fail"
                         << ". ErrorCode: " << readTxReq.ErrorCode() << ". ErrorMsg"
                         << readTxReq.ErrorMsg();
            return {false, readTxReq.ErrorCode()};
        }
        if (readTxReq.Result().first == txservice::RecordStatus::Normal) {
            MONGO_LOG(1) << "EloqRecoveryUnit::getKV. RecordStatus::Normal. ";
            return {true, txservice::TxErrorCode::NO_ERROR};
        } else {
            MONGO_LOG(1) << "EloqRecoveryUnit::getKV. RecordStatus::Non-Normal. "
                         << readTxReq.Result().first << " " << readTxReq.Result().second;
            if (readTxReq.Result().first == txservice::RecordStatus::Unknown) {
                MONGO_LOG(0) << "retry readtxrequest";
                continue;
            } else {
                return {false, readTxReq.ErrorCode()};
            }
        }
    }
}

std::pair<bool, txservice::TxErrorCode> EloqRecoveryUnit::getKVInternal(
    OperationContext* opCtx,
    const txservice::TableName* tableName,
    uint64_t keySchemaVersion,
    bool isForWrite,
    bool readLocal) {
    MONGO_LOG(1) << "EloqRecoveryUnit::getKVInternal";
    _kvPair.setInternalValuePtr();
    return getKV(
        opCtx, tableName, keySchemaVersion, &_kvPair.keyRef(), _kvPair.getValuePtr(), isForWrite);
}

Status EloqRecoveryUnit::createTable(const txservice::TableName& tableName,
                                     std::string_view metadata) {
    MONGO_LOG(1) << "EloqRecoveryUnit::createTable"
                 << ". tableName: " << tableName.StringView() << ". metadata: " << metadata;
    getTxm();

    std::string schemaImage{Eloq::SerializeSchemaImage(std::string{metadata}, "", "")};
    Eloq::MongoTableSchema tempSchema(tableName, schemaImage, 0);
    std::string kvInfo = Eloq::storeHandler->CreateKVCatalogInfo(&tempSchema);
    std::string emptyImage{""};
    std::string newImage = Eloq::SerializeSchemaImage(std::string{metadata}, kvInfo, "");
    auto [yieldFunc, resumeFunc] = _opCtx->getCoroutineFunctors();

    txservice::UpsertTableTxRequest upsertTableTxReq{&tableName,
                                                     &emptyImage,
                                                     1,
                                                     &newImage,
                                                     txservice::OperationType::CreateTable,
                                                     nullptr,
                                                     yieldFunc,
                                                     resumeFunc,
                                                     _txm};
    _txm->Execute(&upsertTableTxReq);
    upsertTableTxReq.Wait();
    MONGO_LOG(1) << "txNumber: " << _txm->TxNumber();
    switch (upsertTableTxReq.Result()) {
        case txservice::UpsertResult::Succeeded:
            MONGO_LOG(1) << "UpsertTableTxRequest success";
            return Status::OK();
            break;
        case txservice::UpsertResult::Failed:
            MONGO_LOG(1) << "UpsertTableTxRequest error. UpsertTableOp on multiple nodes at the "
                            "same time may conflict and then backoff.";
            return {ErrorCodes::Error::InternalError, upsertTableTxReq.ErrorMsg()};
            break;
        case txservice::UpsertResult::Unverified:
            MONGO_LOG(1)
                << "UpsertTableTxRequest error. Current transaction coordinator is no longer "
                   "the leader node. The alter table statement will be processed in a "
                   "failover node. Please recheck the result of alter table statement later.";
            return {ErrorCodes::Error::InternalError, upsertTableTxReq.ErrorMsg()};
            break;
        default:
            dassert(false);
            return {ErrorCodes::Error::InternalError, upsertTableTxReq.ErrorMsg()};
    }
}

Status EloqRecoveryUnit::dropTable(const txservice::TableName& tableName,
                                   const txservice::CatalogRecord& catalogRecord) {
    MONGO_LOG(1) << "EloqRecoveryUnit::dropTable"
                 << ". tableName: " << tableName.StringView();
    getTxm();

    std::string emptyImage{""};
    auto [yieldFunc, resumeFunc] = _opCtx->getCoroutineFunctors();

    txservice::UpsertTableTxRequest dropTableTxReq{&tableName,
                                                   &catalogRecord.Schema()->SchemaImage(),
                                                   catalogRecord.SchemaTs(),
                                                   &emptyImage,
                                                   txservice::OperationType::DropTable,
                                                   nullptr,
                                                   yieldFunc,
                                                   resumeFunc,
                                                   _txm};
    _txm->Execute(&dropTableTxReq);
    dropTableTxReq.Wait();

    switch (dropTableTxReq.Result()) {
        case txservice::UpsertResult::Succeeded:
            MONGO_LOG(1) << "UpsertTableTxRequest success";
            return Status::OK();
            break;
        case txservice::UpsertResult::Failed:
            MONGO_LOG(1) << "UpsertTableTxRequest error. Drop temporary table "
                         << tableName.StringView() << " failed at launch.";
            return {ErrorCodes::Error::InternalError, dropTableTxReq.ErrorMsg()};
            break;
        case txservice::UpsertResult::Unverified:
            MONGO_LOG(1)
                << "UpsertTableTxRequest error. Breaked during droping temporary table "
                << tableName.StringView()
                << " and will force to continue in log recover. Please verify it in following time";
            return {ErrorCodes::Error::InternalError, dropTableTxReq.ErrorMsg()};
            break;
        default:
            dassert(false);
            return {ErrorCodes::Error::InternalError, dropTableTxReq.ErrorMsg()};
    }
}

Status EloqRecoveryUnit::updateTable(const txservice::TableName& tableName,
                                     const txservice::CatalogRecord& catalogRecord,
                                     std::string_view oldMetadata,
                                     std::string_view newMetadata) {
    MONGO_LOG(1) << "EloqRecoveryUnit::updateTable"
                 << ". tableName: " << tableName.StringView();
    getTxm();

    /**
     * Generate new catalog image.
     * Using new table frm, current kvtablename, add new kvindexnames,
     * delete dropped kvindexnames.
     */
    // 1. Generate new frm string. Pass oldMetadata in Mongo
    // 2. Get altered table info whose index kv name is empty.
    auto alterTableInfo = getAlterTableInfo(oldMetadata, newMetadata);
    auto currentTableSchema = static_cast<const Eloq::MongoTableSchema*>(catalogRecord.Schema());

    // Get current key schemas ts, excluding the key to be dropped.
    txservice::TableKeySchemaTs key_schemas_ts;
    // The pk schema ts.
    key_schemas_ts.pk_schema_ts_ = currentTableSchema->KeySchema()->SchemaTs();
    auto sk_schemas = currentTableSchema->GetIndexes();
    for (const auto& sk_schema : *sk_schemas) {
        if (alterTableInfo.index_drop_names_.find(sk_schema.second.first) !=
            alterTableInfo.index_drop_names_.end()) {
            // This index will be dropped in the new table schema, so there is no
            // need to get its key schema ts.
            continue;
        }
        // The old sk schema ts.
        key_schemas_ts.sk_schemas_ts_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(sk_schema.second.first.StringView(),
                                  sk_schema.second.first.Type()),
            std::forward_as_tuple(sk_schema.second.second.SchemaTs()));
    }
    std::string schemas_ts_str = key_schemas_ts.Serialize();

    // 3. Generate new schema kv info and altered table info whose index
    // kv name is not empty.
    std::string new_kv_info =
        Eloq::storeHandler->CreateNewKVCatalogInfo(tableName, currentTableSchema, alterTableInfo);

    // 4. Serialized altered table info.
    std::string alterTableInfoImage = alterTableInfo.SerializeAlteredTableInfo();

    // 5. Generate new schema image string.
    // NOTE: At this stage, the key schema ts of the new index are unknown, the
    // value of which is the `commit_ts_` of the UpsertTable Transaction. So,
    // there is no new key's schema ts in the `schemas_ts_str`.
    std::string new_schema_image =
        Eloq::SerializeSchemaImage(std::string{newMetadata}, new_kv_info, schemas_ts_str);


    txservice::OperationType opType{txservice::OperationType::Update};
    if (alterTableInfo.index_add_count_ > 0) {
        opType = txservice::OperationType::AddIndex;
        MONGO_LOG(1) << "OperationType::AddIndex";
    } else if (alterTableInfo.index_drop_count_ > 0) {
        opType = txservice::OperationType::DropIndex;
        MONGO_LOG(1) << "OperationType::DropIndex";
    } else {
        MONGO_LOG(1) << "OperationType::Update";
    }

    auto [yieldFunc, resumeFunc] = _opCtx->getCoroutineFunctors();

    txservice::UpsertTableTxRequest upsertTableTxReq{&tableName,
                                                     &catalogRecord.Schema()->SchemaImage(),
                                                     catalogRecord.SchemaTs(),
                                                     &new_schema_image,
                                                     opType,
                                                     &alterTableInfoImage,
                                                     yieldFunc,
                                                     resumeFunc,
                                                     _txm};
    _txm->Execute(&upsertTableTxReq);
    upsertTableTxReq.Wait();
    MONGO_LOG(1) << "txNumber: " << _txm->TxNumber();
    switch (upsertTableTxReq.Result()) {
        case txservice::UpsertResult::Succeeded:
            MONGO_LOG(1) << "UpsertTableTxRequest success";
            return Status::OK();
            break;
        case txservice::UpsertResult::Failed: {
            txservice::TxErrorCode tx_err = upsertTableTxReq.ErrorCode();
            if (tx_err == txservice::TxErrorCode::UNIQUE_CONSTRAINT) {
                invariant(opType == txservice::OperationType::AddIndex);
                return {ErrorCodes::Error::DuplicateKey, upsertTableTxReq.ErrorMsg()};
            } else if (tx_err == txservice::TxErrorCode::CAL_ENGINE_DEFINED_CONSTRAINT) {
                invariant(opType == txservice::OperationType::AddIndex);
                const txservice::PackSkError& pack_sk_err = *upsertTableTxReq.pack_sk_err_;
                invariant(pack_sk_err.code_ > ErrorCodes::OK &&
                          pack_sk_err.code_ < ErrorCodes::MaxError);
                return {static_cast<ErrorCodes::Error>(pack_sk_err.code_), pack_sk_err.message_};
            } else {
                MONGO_LOG(1)
                    << "UpsertTableTxRequest error. UpsertTableOp on multiple nodes at the "
                       "same time may conflict and then backoff.";
                return {ErrorCodes::Error::InternalError, upsertTableTxReq.ErrorMsg()};
            }
            break;
        }
        case txservice::UpsertResult::Unverified:
            MONGO_LOG(1)
                << "UpsertTableTxRequest error. Current transaction coordinator is no longer "
                   "the leader node. The alter table statement will be processed in a "
                   "failover node. Please recheck the result of alter table statement later.";
            return {ErrorCodes::Error::InternalError, upsertTableTxReq.ErrorMsg()};
            break;
        default:
            dassert(false);
            return {ErrorCodes::Error::InternalError, upsertTableTxReq.ErrorMsg()};
    }
}

EloqKVPair& EloqRecoveryUnit::getKVPair() {
    return _kvPair;
}

const Eloq::MongoTableSchema* EloqRecoveryUnit::getTableSchema(
    const txservice::TableName& tableName) const {
    MONGO_LOG(1) << "EloqRecoveryUnit::getTableSchema. tableName: " << tableName.StringView();

    if (auto iter = _discoveredTableSchemaMap.find(tableName);
        iter != _discoveredTableSchemaMap.end()) {
        return iter->second.get();
    }

    txservice::CatalogKey catalogKey{tableName};
    txservice::CatalogRecord catalogRecord;
    auto [exist, errorCode] =
        const_cast<EloqRecoveryUnit*>(this)->readCatalog(catalogKey, catalogRecord, false);
    if (errorCode != txservice::TxErrorCode::NO_ERROR) {
        MONGO_LOG(1) << "ReadCatalog Error. [ErrorCode]: " << errorCode;
        return nullptr;
    }

    if (!exist) {
        MONGO_LOG(1) << "ReadCatalog no exists.";
        return nullptr;
    }

    std::shared_ptr<const Eloq::MongoTableSchema> tableSchema =
        std::static_pointer_cast<const Eloq::MongoTableSchema>(catalogRecord.CopySchema());
    auto [iter, inserted] = _discoveredTableSchemaMap.try_emplace(tableName, tableSchema);
    invariant(inserted);
    return iter->second.get();
}

const txservice::KeySchema* EloqRecoveryUnit::getIndexSchema(
    const txservice::TableName& tableName) const {
    invariant(tableName.Type() == txservice::TableType::Primary);
    const Eloq::MongoTableSchema* tableSchema = getTableSchema(tableName);
    return tableSchema->KeySchema();
}

const txservice::KeySchema* EloqRecoveryUnit::getIndexSchema(
    const txservice::TableName& tableName, const txservice::TableName& indexName) const {
    invariant(tableName.Type() == txservice::TableType::Primary);

    const Eloq::MongoTableSchema* tableSchema = getTableSchema(tableName);
    if (indexName.Type() == txservice::TableType::Primary) {
        invariant(tableName == indexName);
        return tableSchema->KeySchema();
    } else {
        invariant(indexName.Type() == txservice::TableType::Secondary ||
                  indexName.Type() == txservice::TableType::UniqueSecondary);
        return tableSchema->IndexKeySchema(indexName);
    }
}

void EloqRecoveryUnit::deleteTableSchema(const txservice::TableName& tableName) {
    MONGO_LOG(1) << "EloqRecoveryUnit::deleteTableSchema. tableName: " << tableName.StringView();
    _discoveredTableSchemaMap.erase(tableName);
}

void EloqRecoveryUnit::_abort() {
    MONGO_LOG(1) << "EloqRecoveryUnit::_abort";
    try {
        if (_active) {
            _txnClose(false);
        }

        for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend();
             it != end;
             ++it) {
            Change* change = it->get();
            MONGO_LOG(1) << "CUSTOM ROLLBACK " << redact(demangleName(typeid(*change)));
            change->rollback();
        }
        _changes.clear();

        invariant(!_active);
    } catch (...) {
        std::terminate();
    }
}

void EloqRecoveryUnit::_commit() {
    MONGO_LOG(1) << "EloqRecoveryUnit::_commit";

    // Since we cannot have both a _lastTimestampSet and a _commitTimestamp, we set the
    // commit time as whichever is non-empty. If both are empty, then _lastTimestampSet will
    // be boost::none and we'll set the commit time to that.
    auto commitTime = _commitTimestamp.isNull() ? _lastTimestampSet : _commitTimestamp;
    try {
        if (_active) {
            _txnClose(true);
        }

        for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit(commitTime);
        }
        _changes.clear();

        invariant(!_active);
    } catch (...) {
        std::terminate();
    }
}

void EloqRecoveryUnit::_txnOpen(txservice::IsolationLevel isolationLevel) {
    MONGO_LOG(1) << "EloqRecoveryUnit::_txnOpen";
    invariant(!_active);
    if (_inMultiDocumentTransation) {
        isolationLevel = txservice::IsolationLevel::Snapshot;
    }
    MONGO_LOG(1) << "Opening transaction with isolation level: " << isolationLevel;
    _txm = txservice::NewTxInit(
        _txService, isolationLevel, txservice::CcProtocol::OCC, UINT32_MAX, localThreadId);
    _active = true;
}

void EloqRecoveryUnit::_txnClose(bool commit) {
    MONGO_LOG(1) << "EloqRecoveryUnit::_txnClose";
    invariant(_active);

    closeAllCursors();

    auto [yieldFunc, resumeFunc] = _opCtx->getCoroutineFunctors();

    if (commit) {
        MONGO_LOG(1) << "EloqRecoveryUnit::_txnClose. "
                     << "txm commit";

        auto [success, commitErr] = txservice::CommitTx(_txm, yieldFunc, resumeFunc);
        if (!success) {
            MONGO_LOG(1) << "txm commit fail. "
                         << "errorCode:" << commitErr;
        }
    } else {
        MONGO_LOG(1) << "EloqRecoveryUnit::_txnClose. "
                     << "txm abort";
        // rollback
        txservice::AbortTx(_txm, yieldFunc, resumeFunc);
    }

    // We reset the _lastTimestampSet between transactions. Since it is legal for one
    // transaction on a RecoveryUnit to call setTimestamp() and another to call
    // setCommitTimestamp().
    _lastTimestampSet = boost::none;
    _txm = nullptr;
    _active = false;
    _inMultiDocumentTransation = false;
    _mySnapshotId = nextSnapshotId.fetch_add(1);
}

}  // namespace mongo
