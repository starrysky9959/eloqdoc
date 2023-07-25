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

#include <cassert>
#include <utility>

#include "boost/optional/optional.hpp"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/util/log.h"

#include "mongo/db/modules/eloq/src/base/eloq_key.h"
#include "mongo/db/modules/eloq/src/base/eloq_record.h"
#include "mongo/db/modules/eloq/src/base/eloq_util.h"
#include "mongo/db/modules/eloq/src/eloq_record_store.h"
#include "mongo/db/modules/eloq/src/eloq_recovery_unit.h"
#include "mongo/db/modules/eloq/src/store_handler/kv_store.h"

#include "mongo/db/modules/eloq/tx_service/include/catalog_key_record.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_key.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_request.h"
#include "mongo/db/modules/eloq/tx_service/include/type.h"

#include <butil/time.h>
#include <bvar/latency_recorder.h>

namespace recorder {
bvar::LatencyRecorder kCatalogReadLatency{"mongo_catalog_read"};
}

namespace Eloq {
extern std::unique_ptr<txservice::store::DataStoreHandler> storeHandler;
}
namespace mongo {
thread_local std::random_device r;
thread_local std::default_random_engine randomEngine{r()};
thread_local std::uniform_int_distribution<int> uniformDist{1, 500};

class EloqCatalogRecordStoreCursor : public SeekableRecordCursor {
public:
    explicit EloqCatalogRecordStoreCursor(OperationContext* opCtx)
        : _ru{EloqRecoveryUnit::get(opCtx)} {
        MONGO_LOG(1) << "EloqCatalogRecordStoreCursor::EloqCatalogRecordStoreCursor";
        // always do full table scan
        Eloq::storeHandler->DiscoverAllTableNames(_tableNameVector);
        std::string output;
        for (const auto& name : _tableNameVector) {
            output.append(name).append("|");
        }
        MONGO_LOG(1) << "tables: " << output;
        _iter = _tableNameVector.begin();
    }

    EloqCatalogRecordStoreCursor(const EloqCatalogRecordStoreCursor&) = delete;
    EloqCatalogRecordStoreCursor(EloqCatalogRecordStoreCursor&&) = delete;
    EloqCatalogRecordStoreCursor& operator=(const EloqCatalogRecordStoreCursor&) = delete;
    EloqCatalogRecordStoreCursor& operator=(EloqCatalogRecordStoreCursor&&) = delete;

    ~EloqCatalogRecordStoreCursor() override {
        MONGO_LOG(1) << "EloqCatalogRecordStoreCursor::~EloqCatalogRecordStoreCursor";
    }

    boost::optional<Record> next() override {
        MONGO_LOG(1) << "EloqCatalogRecordStoreCursor::next";
        // Traverse the _tableNamevector until find a exist table and then return the metadata
        while (_iter != _tableNameVector.end()) {
            RecordId id{*_iter};
            txservice::TableName tableName{std::move(*_iter), txservice::TableType::Primary};
            ++_iter;

            txservice::CatalogKey catalogKey{tableName};
            txservice::CatalogRecord catalogRecord;
            auto [exists, errorCode] = _ru->readCatalog(catalogKey, catalogRecord, false);
            if (!exists) {
                continue;
            }

            // Make sure _metadata is empty before DeserializeSchemaImage
            _metadata.clear();
            Eloq::DeserializeSchemaImage(catalogRecord.Schema()->SchemaImage(), _metadata);
            if (!_metadata.empty()) {
                MONGO_LOG(1) << "metadata: " << BSONObj{_metadata.data()}.jsonString();
            }
            return {{std::move(id), {_metadata.data(), static_cast<int>(_metadata.size())}}};
        }

        return {};
    }

    boost::optional<Record> seekExact(const RecordId& id) override {
        MONGO_UNREACHABLE;
    }

    void saveUnpositioned() override {
        MONGO_UNREACHABLE;
    }

    void save() override {
        MONGO_UNREACHABLE;
    }

    bool restore() override {
        MONGO_UNREACHABLE;
    }

    void detachFromOperationContext() override {
        MONGO_UNREACHABLE;
    }
    void reattachToOperationContext(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

private:
    EloqRecoveryUnit* _ru;  // not owned

    std::vector<std::string> _tableNameVector;
    std::vector<std::string>::iterator _iter;

    // The RecordData returned after calling next() is actually store in here.
    // We should guarantee the corresponding memory is valid until the next time next() is called
    // so that thereis no need let the RecordData object getOwned and  we can avoid a memory
    // allocation.
    std::string _metadata;
    // std::string _kvInfo;              // useless now
    // std::string _keySchemasTsString;  // useless now
};


EloqCatalogRecordStore::EloqCatalogRecordStore(OperationContext* opCtx, StringData ns)
    : RecordStore{ns} {
    MONGO_LOG(1) << "EloqCatalogRecordStore::EloqCatalogRecordStore";
}

EloqCatalogRecordStore::~EloqCatalogRecordStore() {
    MONGO_LOG(1) << "EloqCatalogRecordStore::~EloqCatalogRecordStore";
}

RecordData EloqCatalogRecordStore::dataFor(OperationContext* opCtx, const RecordId& loc) const {
    MONGO_LOG(1) << "EloqCatalogRecordStore::dataFor";
    RecordData data;
    invariant(findRecord(opCtx, loc, &data));
    return data;
}

bool EloqCatalogRecordStore::findRecord(OperationContext* opCtx,
                                        const RecordId& id,
                                        RecordData* out) const {
    MONGO_LOG(1) << "EloqCatalogRecordStore::findRecord"
                 << ". id: " << id.toString();

    txservice::TableName tableName{id.getStringView(), txservice::TableType::Primary};
    MONGO_LOG(1) << "tableName: " << tableName.StringView();

    auto ru = EloqRecoveryUnit::get(opCtx);
    const Eloq::MongoTableSchema* tableSchema = ru->getTableSchema(tableName);
    if (tableSchema == nullptr) {
        return false;
    }

    const std::string& metadata = tableSchema->MetaData();

    MONGO_LOG(1) << "id: " << id.toString() << " record: " << BSONObj(metadata.data()).jsonString();
    *out = RecordData{metadata.data(), static_cast<int>(metadata.size())};
    return true;
}

void EloqCatalogRecordStore::deleteRecord(OperationContext* opCtx, const RecordId& id) {
    MONGO_LOG(1) << "EloqCatalogRecordStore::deleteRecord"
                 << ". id: " << id;

    txservice::TableName tableName{id.getStringView(), txservice::TableType::Primary};
    MONGO_LOG(1) << "tableName: " << tableName.StringView();
    txservice::CatalogKey catalogKey{tableName};
    txservice::CatalogRecord catalogRecord;

    auto ru = EloqRecoveryUnit::get(opCtx);
    for (uint16_t i = 1; i < kMaxRetryLimit; ++i) {
        auto [exist, errorCode] = ru->readCatalog(catalogKey, catalogRecord, true);
        if (errorCode != txservice::TxErrorCode::NO_ERROR) {
            MONGO_LOG(1) << "Eloq readCatalog error with write intent. Another transaction "
                            "may do DDL on the same table.";
        } else {
            if (!exist) {
                return;
            }

            auto status = ru->dropTable(tableName, catalogRecord);
            if (status.isOK()) {
                ru->deleteTableSchema(tableName);
                return;
            }
        }

        std::chrono::milliseconds duration{uniformDist(randomEngine)};
        MONGO_LOG(1) << "Fail to drop table in Eloq";
        MONGO_LOG(1) << "Sleep for " << duration.count() << "ms";
        std::this_thread::sleep_for(duration);
        MONGO_LOG(1) << "Retry count: " << i;
        catalogRecord.Reset();
    }

    error() << "[Drop Table] opertion reaches the maximum number of retries.";
    uasserted(70001, "[Drop Table] opertion reaches the maximum number of retries.");
}

StatusWith<RecordId> EloqCatalogRecordStore::insertRecord(
    OperationContext* opCtx, const char* data, int len, Timestamp timestamp, bool enforceQuota) {
    MONGO_LOG(1) << "EloqCatalogRecordStore::insertRecord. Insert into txservice catalog_cc_map";

    RecordId recordId;
    BSONObj obj{data};
    if (auto nsElem = obj["ns"]; nsElem.ok() && !nsElem.isNull()) {
        recordId = RecordId{nsElem.valuestr(), static_cast<size_t>(nsElem.valuestrsize() - 1)};
    } else {
        dassert(KVCatalog::FeatureTracker::isFeatureDocument(obj));
        recordId = RecordId{kFeatureDocumentSV};
    }
    MONGO_LOG(1) << "record id: " << recordId.toString() << ". data: " << obj.jsonString();

    std::string_view metadata{data, static_cast<size_t>(len)};
    txservice::TableName tableName{recordId.getStringView(), txservice::TableType::Primary};
    txservice::CatalogKey catalogKey{tableName};
    txservice::CatalogRecord catalogRecord;

    auto ru = EloqRecoveryUnit::get(opCtx);
    for (uint16_t i = 1; i < kMaxRetryLimit; ++i) {
        auto [exist, errorCode] = ru->readCatalog(catalogKey, catalogRecord, true);
        if (errorCode != txservice::TxErrorCode::NO_ERROR) {
            MONGO_LOG(1) << "Eloq readCatalog error with write intent. Another transaction "
                            "may do DDL on the same table.";
        } else {
            if (exist) {
                return {ErrorCodes::NamespaceExists,
                        "Collection already exists in Eloq storage engine"};
            }

            auto status = ru->createTable(tableName, metadata);
            if (status.isOK()) {
                return {recordId};
            }
        }

        std::chrono::milliseconds duration{uniformDist(randomEngine)};
        MONGO_LOG(1) << "Fail to create table in Eloq. Sleep for " << duration.count() << "ms";
        std::this_thread::sleep_for(duration);
        MONGO_LOG(1) << "Retry count: " << i;
        catalogRecord.Reset();
    }

    return {ErrorCodes::InternalError,
            "[Create Table] opertion reaches the maximum number of retries."};
}

Status EloqCatalogRecordStore::updateRecord(OperationContext* opCtx,
                                            const RecordId& id,
                                            const char* data,
                                            int len,
                                            bool enforceQuota,
                                            UpdateNotifier* notifier) {
    MONGO_LOG(1) << "EloqCatalogRecordStore::updateRecord"
                 << ". id: " << id << ". data: " << BSONObj{data}.jsonString();
    std::string_view newMetadata{data, static_cast<size_t>(len)};
    txservice::TableName tableName{id.getStringView(), txservice::TableType::Primary};
    MONGO_LOG(1) << "tableName: " << tableName.StringView();
    txservice::CatalogKey catalogKey{tableName};
    txservice::CatalogRecord catalogRecord;

    auto ru = EloqRecoveryUnit::get(opCtx);
    for (uint16_t i = 1; i < kMaxRetryLimit; ++i) {
        auto [exist, errorCode] = ru->readCatalog(catalogKey, catalogRecord, true);
        if (errorCode != txservice::TxErrorCode::NO_ERROR) {
            MONGO_LOG(1) << "Eloq readCatalog error with write intent. Another transaction "
                            "may do DDL on the same table.";
        } else {
            if (!exist) {
                return {ErrorCodes::InternalError, "Try to Update a non-exist table"};
            }
            std::string oldMetadata, kvInfo, keySchemaTsString;
            Eloq::DeserializeSchemaImage(
                catalogRecord.Schema()->SchemaImage(), oldMetadata, kvInfo, keySchemaTsString);

            auto status = ru->updateTable(tableName, catalogRecord, oldMetadata, newMetadata);
            if (status.isOK()) {
                ru->deleteTableSchema(tableName);
                return Status::OK();
            } else {
                return status;
            }
        }

        std::chrono::milliseconds duration{uniformDist(randomEngine)};
        MONGO_LOG(1) << "Fail to create table in Eloq. Sleep for " << duration.count() << "ms";
        std::this_thread::sleep_for(duration);
        MONGO_LOG(1) << "Retry count: " << i;
        catalogRecord.Reset();
    }

    return {ErrorCodes::InternalError,
            "[Create Table] opertion reaches the maximum number of retries."};
}

std::unique_ptr<SeekableRecordCursor> EloqCatalogRecordStore::getCursor(OperationContext* opCtx,
                                                                        bool forward) const {
    MONGO_LOG(1) << "EloqCatalogRecordStore::getCursor";

    return std::make_unique<EloqCatalogRecordStoreCursor>(opCtx);
}

void EloqCatalogRecordStore::getAllCollections(std::vector<std::string>& collections) const {
    MONGO_LOG(1) << "EloqCatalogRecordStore::getAllCollections";
    Eloq::storeHandler->DiscoverAllTableNames(collections);
    std::string output;
    for (const auto& name : collections) {
        output.append(name).append("|");
    }
    MONGO_LOG(1) << "tables: " << output;
}

class EloqRecordStoreCursor : public SeekableRecordCursor {
public:
    explicit EloqRecordStoreCursor(OperationContext* opCtx, const EloqRecordStore* rs, bool forward)
        : _opCtx{opCtx},
          _ru{EloqRecoveryUnit::get(opCtx)},
          _tableName{rs->tableName()},
          _keySchema(_ru->getIndexSchema(*_tableName)),
          _forward{forward} {
        MONGO_LOG(1) << "EloqRecordStoreCursor::EloqRecordStoreCursor";
    }

    EloqRecordStoreCursor(const EloqRecordStoreCursor&) = delete;
    EloqRecordStoreCursor(EloqRecordStoreCursor&&) = delete;
    EloqRecordStoreCursor& operator=(const EloqRecordStoreCursor&) = delete;
    EloqRecordStoreCursor& operator=(EloqRecordStoreCursor&&) = delete;

    ~EloqRecordStoreCursor() override {
        MONGO_LOG(1) << "EloqRecordStoreCursor::~EloqRecordStoreCursor";
    }

    void reset(OperationContext* opCtx, const EloqRecordStore* rs, bool forward) {
        _opCtx = opCtx;
        _ru = EloqRecoveryUnit::get(opCtx);
        _tableName = rs->tableName();
        _keySchema = _ru->getIndexSchema(*_tableName);
        _forward = forward;
        _eof = false;
        _lastMongoKey.reset();
        _cursor.reset();
    }

    boost::optional<Record> next() override {
        MONGO_LOG(1) << "EloqRecordStoreCursor::next"
                     << ". forward: " << _forward;
        if (_eof) {
            return {};
        }

        if (!_cursor) {
            _seekCursor();
        }
        assert(_cursor);

        const txservice::ScanBatchTuple* scanTuple = _cursor->nextBatchTuple();
        if (scanTuple == nullptr) {
            MONGO_LOG(1) << "reach the end";
            _eof = true;
            return {};
        }

        const auto* key = scanTuple->key_.GetKey<Eloq::MongoKey>();
        const auto* record = static_cast<const Eloq::MongoRecord*>(scanTuple->record_);
        if (key == nullptr) {
            MONGO_LOG(1) << "reach the end";
            _eof = true;
            return {};
        }

        RecordId id = key->ToRecordId(false);
        MONGO_LOG(1) << "id: " << id
                     << ". record:" << BSONObj{record->EncodedBlobData()}.jsonString();
        return {{std::move(id),
                 {record->EncodedBlobData(), static_cast<int>(record->EncodedBlobSize())}}};
    }

    boost::optional<Record> seekExact(const RecordId& id) override {
        MONGO_LOG(1) << "EloqRecordStoreCursor::seekExact";

        if (_cursor) {
            _cursor.reset();
        }

        MONGO_LOG(1) << "id: " << id;
        auto& kvPair = _ru->getKVPair();
        // _id don't need getKV if it has been stored in KVPair
        if (!kvPair.getValuePtr()) {
            kvPair.keyRef().SetPackedKey(id);
            auto [success, err] = _ru->getKVInternal(_opCtx, _tableName, _keySchema->SchemaTs());
            if (!success) {
                MONGO_LOG(1) << "no found. id: " << id << ". Txservice error code: " << err;
                return {};
            }
            MONGO_LOG(1) << "keyStore:" << kvPair.keyRef().ToString()
                         << ". recordStore: " << kvPair.getValuePtr()->ToString();
        }
        MONGO_LOG(1) << "record: " << BSONObj{kvPair.getValuePtr()->EncodedBlobData()}.jsonString();

        if (_lastMongoKey) {
            _lastMongoKey->Copy(kvPair.keyRef());
        } else {
            _lastMongoKey.emplace(kvPair.keyRef());
        }

        return {{id,
                 {kvPair.getValuePtr()->EncodedBlobData(),
                  static_cast<int>(kvPair.getValuePtr()->EncodedBlobSize())}}};
    }

    void saveUnpositioned() override {
        MONGO_LOG(1) << "EloqRecordStoreCursor::saveUnpositioned";
        _lastMongoKey.reset();
        _cursor.reset();
    }

    void save() override {
        MONGO_LOG(1) << "EloqRecordStoreCursor::save";
        if (!_eof && _cursor && _cursor->currentBatchKey() != nullptr) {
            _lastMongoKey.emplace(*_cursor->currentBatchKey());
        }
        _cursor.reset();
    }

    bool restore() override {
        MONGO_LOG(1) << "EloqRecordStoreCursor::restore";
        assert(!_cursor);
        // Don't open scan here.
        // Mongo may call seekExact which don't need a scan in TxService
        return true;
    }

    void detachFromOperationContext() override {
        MONGO_LOG(1) << "EloqRecordStoreCursor::detachFromOperationContext";
        assert(_opCtx);
        _opCtx = nullptr;
        _ru = nullptr;
    }

    void reattachToOperationContext(OperationContext* opCtx) override {
        MONGO_LOG(1) << "EloqRecordStoreCursor::reattachToOperationContext";
        assert(!_opCtx);
        _opCtx = opCtx;
        _ru = EloqRecoveryUnit::get(opCtx);
    }

private:
    void _seekCursor(bool startInclusive = false) {
        MONGO_LOG(1) << "EloqRecordStoreCursor::_seekIter";

        _cursor.emplace(_opCtx);
        if (_lastMongoKey) {
            _startKey = txservice::TxKey(&_lastMongoKey.get());
        } else {
            if (_forward) {
                _startKey = Eloq::MongoKey::GetNegInfTxKey();
            } else {
                _startKey = Eloq::MongoKey::GetPosInfTxKey();
            }
        }
        if (_forward) {
            _endKey = Eloq::MongoKey::GetPosInfTxKey();
        } else {
            _endKey = Eloq::MongoKey::GetNegInfTxKey();
        }

        _cursor->indexScanOpen(_tableName,
                               _keySchema->SchemaTs(),
                               txservice::ScanIndexType::Primary,
                               &_startKey,
                               false,
                               &_endKey,
                               false,
                               _forward ? txservice::ScanDirection::Forward
                                        : txservice::ScanDirection::Backward);
    }

    OperationContext* _opCtx;                         // not owned
    EloqRecoveryUnit* _ru;                            // not owned
    const txservice::TableName* _tableName{nullptr};  // not owned
    const txservice::KeySchema* _keySchema{nullptr};  // not owned

    bool _forward;
    bool _eof{false};
    boost::optional<Eloq::MongoKey> _lastMongoKey;

    // const Eloq::MongoKey* _scanTupleKey{nullptr};
    // const Eloq::MongoRecord* _scanTupleRecord{nullptr};

    txservice::TxKey _startKey;
    txservice::TxKey _endKey;

    // Mongo use EloqRecordStoreCursor even for exact match operation
    // which actually does not need construct a Cursor in Eloq's design.
    // So use boost::optional to delay the contruction
    boost::optional<EloqCursor> _cursor{boost::none};
};


EloqRecordStore::EloqRecordStore(OperationContext* opCtx, Params& params)
    : RecordStore{params.ns},
      _tableName{std::move(params.tableName)},
      _isCatalog{isMongoCatalog(params.ident.toStringView())},
      _isCapped{params.isCapped},
      _cappedMaxSize{params.cappedMaxSize},
      _cappedMaxDocs{params.cappedMaxDocs},
      _cappedCallback{params.cappedCallback},
      _shuttingDown{false} {
    MONGO_LOG(1) << "EloqRecordStore::EloqRecordStore";

    if (_isCapped) {
        invariant(_cappedMaxSize > 0);
        invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
    } else {
        invariant(_cappedMaxSize == -1);
        invariant(_cappedMaxDocs == -1);
    }
}

EloqRecordStore::~EloqRecordStore() {
    MONGO_LOG(1) << "EloqRecordStoreCursor::~EloqRecordStore";
    {
        stdx::lock_guard<stdx::mutex> lk(_cappedCallbackMutex);
        _shuttingDown = true;
    }

    MONGO_LOG(1) << "EloqRecordStore ns: " << ns();
}

const char* EloqRecordStore::name() const {
    return kEloqEngineName.rawData();
}

const std::string& EloqRecordStore::getIdent() const {
    return _ident;
}

long long EloqRecordStore::dataSize(OperationContext* opCtx) const {
    MONGO_LOG(1) << "EloqRecordStore::dataSize";
    return 0;
}

long long EloqRecordStore::numRecords(OperationContext* opCtx) const {
    MONGO_LOG(1) << "EloqRecordStore::numRecords";

    auto ru = EloqRecoveryUnit::get(opCtx);
    const Eloq::MongoTableSchema* tableSchema = ru->getTableSchema(_tableName);
    if (tableSchema == nullptr || tableSchema->StatisticsObject() == nullptr) {
        MONGO_LOG(1) << "EloqRecordStore::numRecords"
                     << ". Fail to get TableSchema or StatisticsObject.";
        return 0;
    }

    auto distribution = tableSchema->StatisticsObject()->GetDistribution(_tableName);
    if (distribution != nullptr) {
        // Currently, the accuracy of the record count is guaranteed only for single-core
        // single-machine setups and for up to 1000 entries in multi-core single-machine setups.
        // You should not rely on it.
        auto size = distribution->Records();
        MONGO_LOG(1) << "EloqRecordStore::numRecords"
                     << ". size: " << size;
        return static_cast<long long>(size);
    } else {
        MONGO_LOG(1) << "EloqRecordStore::numRecords"
                     << ". distribution == nullptr";
        return 0;
    }
}

bool EloqRecordStore::isCapped() const {
    MONGO_LOG(1) << "EloqRecordStore::isCapped";
    return _isCapped;
}

void EloqRecordStore::setCappedCallback(CappedCallback* cb) {
    MONGO_LOG(1) << "EloqRecordStore::setCappedCallback";
    std::scoped_lock<stdx::mutex> lk{_cappedCallbackMutex};
    _cappedCallback = cb;
}

int64_t EloqRecordStore::storageSize(OperationContext* opCtx,
                                     BSONObjBuilder* extraInfo,
                                     int infoLevel) const {
    MONGO_LOG(1) << "EloqRecordStore::storageSize";
    int64_t size = dataSize(opCtx);
    MONGO_LOG(1) << "size: " << size;
    return size;
}

RecordData EloqRecordStore::dataFor(OperationContext* opCtx, const RecordId& loc) const {
    MONGO_LOG(1) << "EloqRecordStore::dataFor";
    RecordData data;
    invariant(findRecord(opCtx, loc, &data));
    return data;
}

bool EloqRecordStore::findRecord(OperationContext* opCtx,
                                 const RecordId& id,
                                 RecordData* out) const {
    // butil::Timer timer;
    // timer.start();
    MONGO_LOG(1) << "EloqRecordStore::findRecord"
                 << ". id: " << id.toString();

    auto ru = EloqRecoveryUnit::get(opCtx);

    Eloq::MongoKey mongoKey(id);
    Eloq::MongoRecord mongoRecord;
    uint64_t keySchemaVersion = ru->getIndexSchema(_tableName)->SchemaTs();

    auto [success, err] =
        ru->getKV(opCtx, &_tableName, keySchemaVersion, &mongoKey, &mongoRecord, false);
    if (!success) {
        MONGO_LOG(1) << "not exists";
        return false;
    }

    *out =
        RecordData{mongoRecord.EncodedBlobData(), static_cast<int>(mongoRecord.EncodedBlobSize())}
            .getOwned();


    // timer.stop();
    // recorder::kCatalogReadLatency << timer.u_elapsed();
    return true;
}

void EloqRecordStore::deleteRecord(OperationContext* opCtx, const RecordId& id) {
    MONGO_LOG(1) << "EloqRecordStore::deleteRecord"
                 << ". id: " << id;
    int64_t oldLength{0};
    auto ru = EloqRecoveryUnit::get(opCtx);

    auto mongoKey = std::make_unique<Eloq::MongoKey>(id);
    auto mongoRecord = std::make_unique<Eloq::MongoRecord>();
    uint64_t keySchemaVersion = ru->getIndexSchema(_tableName)->SchemaTs();

    auto [success, err] =
        ru->getKV(opCtx, &_tableName, keySchemaVersion, mongoKey.get(), mongoRecord.get(), true);
    oldLength = static_cast<int64_t>(mongoRecord->EncodedBlobSize());

    std::tie(success, err) = ru->setKV(&_tableName,
                                       keySchemaVersion,
                                       std::move(mongoKey),
                                       std::move(mongoRecord),
                                       txservice::OperationType::Delete);
}

StatusWith<RecordId> EloqRecordStore::insertRecord(
    OperationContext* opCtx, const char* data, int len, Timestamp timestamp, bool enforceQuota) {
    MONGO_LOG(1) << "EloqRecordStore::insertRecord";
    Record record{RecordId{}, RecordData{data, len}};
    Status status = _insertRecords(opCtx, &record, &timestamp, 1);
    if (!status.isOK()) {
        return {status};
    }
    return {record.id};
}

Status EloqRecordStore::insertRecords(OperationContext* opCtx,
                                      std::vector<Record>* records,
                                      std::vector<Timestamp>* timestamps,
                                      bool enforceQuota) {
    MONGO_LOG(1) << "EloqRecordStore::insertRecords";
    return _insertRecords(opCtx, records->data(), timestamps->data(), records->size());
}


Status EloqRecordStore::insertRecordsWithDocWriter(OperationContext* opCtx,
                                                   const DocWriter* const* docs,
                                                   const Timestamp* timestamps,
                                                   size_t nDocs,
                                                   RecordId* idsOut) {
    MONGO_LOG(1) << "EloqRecordStore::insertRecordsWithDocWriter";
    return Status::OK();
}

Status EloqRecordStore::updateRecord(OperationContext* opCtx,
                                     const RecordId& id,
                                     const char* data,
                                     int len,
                                     bool enforceQuota,
                                     UpdateNotifier* notifier) {
    MONGO_LOG(1) << "EloqRecordStore::updateRecord"
                 << ". id: " << id << ". data: " << BSONObj{data}.jsonString();

    auto ru = EloqRecoveryUnit::get(opCtx);

    int64_t oldLength{0};

    auto mongoKey = std::make_unique<Eloq::MongoKey>(id);
    auto mongoRecord = std::make_unique<Eloq::MongoRecord>();
    uint64_t keySchemaVersion = ru->getIndexSchema(_tableName)->SchemaTs();


    // don't need getKV
    // if ( auto valuePtr = ru->getKVPair()->getValuePtr(); valuePtr) {
    //     oldLength = static_cast<int64_t>(valuePtr->EncodedBlobSize());
    // } else {

    auto [success, err] =
        ru->getKV(opCtx, &_tableName, keySchemaVersion, mongoKey.get(), mongoRecord.get(), true);
    if (err != txservice::TxErrorCode::NO_ERROR) {
        return {ErrorCodes::OperationFailed, "acquire write lock failed"};
    }
    oldLength = static_cast<int64_t>(mongoRecord->EncodedBlobSize());
    // }

    mongoRecord->SetEncodedBlob(reinterpret_cast<const unsigned char*>(data), len);
    std::tie(success, err) = ru->setKV(&_tableName,
                                       keySchemaVersion,
                                       std::move(mongoKey),
                                       std::move(mongoRecord),
                                       txservice::OperationType::Update);
    if (!success) {
        return TxErrorCodeToMongoStatus(err);
    }

    return Status::OK();
}

bool EloqRecordStore::updateWithDamagesSupported() const {
    return false;
}

StatusWith<RecordData> EloqRecordStore::updateWithDamages(
    OperationContext* opCtx,
    const RecordId& loc,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {
    MONGO_UNREACHABLE;
    return Status::OK();
}

std::unique_ptr<SeekableRecordCursor> EloqRecordStore::getCursor(OperationContext* opCtx,
                                                                 bool forward) const {
    MONGO_LOG(1) << "EloqRecordStore::getCursor";
    if (_isCatalog) {
        MONGO_UNREACHABLE;
        return std::make_unique<EloqCatalogRecordStoreCursor>(opCtx);
    } else {
        return std::make_unique<EloqRecordStoreCursor>(opCtx, this, forward);
    }
}

std::unique_ptr<RecordCursor> EloqRecordStore::getCursorForRepair(OperationContext* opCtx) const {
    MONGO_LOG(1) << "EloqRecordStore::getCursorForRepair";
    MONGO_UNREACHABLE;
    return {};
}

std::unique_ptr<RecordCursor> EloqRecordStore::getRandomCursor(OperationContext* opCtx) const {
    MONGO_LOG(1) << "EloqRecordStore::getRandomCursor";
    MONGO_UNREACHABLE;
    return {};
}

std::vector<std::unique_ptr<RecordCursor>> EloqRecordStore::getManyCursors(
    OperationContext* opCtx) const {
    MONGO_LOG(1) << "EloqRecordStore::getManyCursors";
    // MONGO_UNREACHABLE;
    return RecordStore::getManyCursors(opCtx);
}

Status EloqRecordStore::truncate(OperationContext* opCtx) {
    MONGO_LOG(1) << "EloqRecordStore::truncate";
    MONGO_UNREACHABLE;
    return Status::OK();
}

void EloqRecordStore::cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {
    MONGO_LOG(1) << "EloqRecordStore::cappedTruncateAfter";
    MONGO_UNREACHABLE;
}

bool EloqRecordStore::compactSupported() const {
    MONGO_LOG(1) << "EloqRecordStore::compactSupported";
    MONGO_UNREACHABLE;
    return false;
}

Status EloqRecordStore::validate(OperationContext* opCtx,
                                 ValidateCmdLevel level,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results,
                                 BSONObjBuilder* output) {
    MONGO_LOG(1) << "EloqRecordStore::validate";
    long long nrecords{0};
    long long dataSizeTotal{0};
    long long nInvalid{0};

    results->valid = true;
    auto cursor = getCursor(opCtx, true);
    // int interruptInterval {4096};

    while (auto record = cursor->next()) {
        // if (!(nrecords % interruptInterval))
        //     opCtx->checkForInterrupt();
        ++nrecords;
        auto dataSize = record->data.size();
        dataSizeTotal += dataSize;
        size_t validatedSize{0};
        Status status = adaptor->validate(record->id, record->data, &validatedSize);

        // The validatedSize equals dataSize below is not a general requirement, but must be
        // true for WT today because we never pad records.
        if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
            if (results->valid) {
                // Only log once.
                results->errors.emplace_back("detected one or more invalid documents (see logs)");
            }
            nInvalid++;
            results->valid = false;
            log() << "document at location: " << record->id << " is corrupted";
        }
    }

    if (results->valid) {
        updateStatsAfterRepair(opCtx, nrecords, dataSizeTotal);
    }

    output->append("nInvalidDocuments", nInvalid);
    output->appendNumber("nrecords", nrecords);
    return Status::OK();
}

/**
 * @param scaleSize - amount by which to scale size metrics
 * appends any custom stats from the RecordStore or other unique stats
 */
void EloqRecordStore::appendCustomStats(OperationContext* opCtx,
                                        BSONObjBuilder* result,
                                        double scale) const {
    MONGO_LOG(1) << "EloqRecordStore::appendCustomStats";
    // MONGO_UNREACHABLE;
}

void EloqRecordStore::updateStatsAfterRepair(OperationContext* opCtx,
                                             long long numRecords,
                                             long long dataSize) {
    MONGO_LOG(1) << "EloqRecordStore::updateStatsAfterRepair";
}

void EloqRecordStore::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const {
    //
}


Status EloqRecordStore::_insertRecords(OperationContext* opCtx,
                                       Record* records,
                                       const Timestamp* timestamps,
                                       size_t nRecords) {
    MONGO_LOG(1) << "EloqRecordStore::_insertRecords"
                 << ". tableName: " << _tableName.StringView() << ". nRecords: " << nRecords;
    // Only check if a write lock is held for regular (non-temporary) record stores.
    // dassert(opCtx->lockState()->isWriteLocked());

    invariant(nRecords != 0);

    int64_t totalLength = 0;
    for (size_t i = 0; i < nRecords; i++) {
        const auto& record = records[i];
        assert(record.id.isNull());
        totalLength += record.data.size();
    }

    if (_isCapped && totalLength > _cappedMaxSize) {
        return {ErrorCodes::BadValue, "object to insert exceeds cappedMaxSize"};
    }

    auto ru = EloqRecoveryUnit::get(opCtx);
    MONGO_LOG(1) << "Insert into a Data Table.";

    uint64_t keySchemaVersion = ru->getIndexSchema(_tableName)->SchemaTs();

    for (size_t i = 0; i < nRecords; i++) {
        auto& record = records[i];
        BSONObj obj{record.data.data()};

        const auto idObj = getIdBSONObjWithoutFieldName(obj);
        MONGO_LOG(1) << idObj.jsonString();
        Status s = checkKeySize(idObj, "RecordStore");
        if (!s.isOK()) {
            return s;
        }

        const KeyString ks{KeyString::kLatestVersion, idObj, kIdOrdering};
        record.id = RecordId{ks.getBuffer(), ks.getSize()};
        MONGO_LOG(1) << "record id: " << record.id.toString() << ". data: " << obj.jsonString();

        auto mongoKey = std::make_unique<Eloq::MongoKey>(ks);
        auto mongoRecord = std::make_unique<Eloq::MongoRecord>();
        auto [success, err] = ru->getKV(
            opCtx, &_tableName, keySchemaVersion, mongoKey.get(), mongoRecord.get(), true);
        if (success) {
            return {ErrorCodes::DuplicateKey, "DuplicateKey"};
        }

        mongoRecord->SetEncodedBlob(reinterpret_cast<const unsigned char*>(record.data.data()),
                                    record.data.size());
        if (const auto& typeBits = ks.getTypeBits(); !typeBits.isAllZeros()) {
            mongoRecord->SetUnpackInfo(typeBits.getBuffer(), typeBits.getSize());
        }

        std::tie(success, err) = ru->setKV(&_tableName,
                                           keySchemaVersion,
                                           std::move(mongoKey),
                                           std::move(mongoRecord),
                                           txservice::OperationType::Insert);
        if (!success) {
            return TxErrorCodeToMongoStatus(err);
        }
    }

    return Status::OK();
}


}  // namespace mongo
