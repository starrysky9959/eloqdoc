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

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "mongo/base/object_pool.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/util/log.h"

#include "mongo/db/modules/eloq/src/base/eloq_key.h"
#include "mongo/db/modules/eloq/src/base/eloq_log_agent.h"
#include "mongo/db/modules/eloq/src/base/eloq_record.h"
#include "mongo/db/modules/eloq/src/base/eloq_util.h"
#include "mongo/db/modules/eloq/src/eloq_global_options.h"
#include "mongo/db/modules/eloq/src/eloq_index.h"
#include "mongo/db/modules/eloq/src/eloq_kv_engine.h"
#include "mongo/db/modules/eloq/src/eloq_record_store.h"
#include "mongo/db/modules/eloq/src/eloq_recovery_unit.h"
#include "mongo/db/modules/eloq/src/store_handler/cass_handler.h"
#include "mongo/db/modules/eloq/src/store_handler/kv_store.h"
#include "mongo/db/modules/eloq/tx_service/include/catalog_key_record.h"
#include "mongo/db/modules/eloq/tx_service/include/dead_lock_check.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_key.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_record.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_service.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_util.h"
#include "mongo/db/modules/eloq/tx_service/include/type.h"

namespace Eloq {
std::unique_ptr<txservice::store::DataStoreHandler> storeHandler;
}
namespace mongo {
extern std::function<std::pair<std::function<void()>, std::function<void(int16_t)>>(int16_t)>
    getTxServiceFunctors;

std::string_view extractDbName(std::string_view nss) {
    auto pos = nss.find('.');
    if (pos == std::string_view::npos) {
        return "";
    } else {
        return nss.substr(0, pos);
    }
}

void RegisterFactory() {
    txservice::TxKeyFactory::RegisterCreateTxKeyFunc(Eloq::MongoKey::Create);
    txservice::TxKeyFactory::RegisterNegInfTxKey(Eloq::MongoKey::NegInfTxKey());
    txservice::TxKeyFactory::RegisterPosInfTxKey(Eloq::MongoKey::PosInfTxKey());
    txservice::TxKeyFactory::RegisterPackedNegativeInfinity(
        Eloq::MongoKey::PackedNegativeInfinityTxKey());
    txservice::TxRecordFactory::RegisterCreateTxRecordFunc(Eloq::MongoRecord::Create);
}

EloqKVEngine::EloqKVEngine(const std::string& path) : _path(path) {

    RegisterFactory();

    MONGO_LOG(1) << "Eloq start. dbPath: " << path;

    std::string localPath("local://");
    localPath.append(path);
    if (localPath.back() == '/') {
        localPath.pop_back();
    }

    std::map<std::string, uint32_t> txServiceConf{
        {"core_num", eloqGlobalOptions.coreNum},
        {"range_split_worker_num", eloqGlobalOptions.rangeSplitWorkerNum},
        {"checkpointer_interval", eloqGlobalOptions.checkpointerIntervalSec},
        {"node_memory_limit_mb", eloqGlobalOptions.nodeMemoryLimitMB},
        {"node_log_limit_mb", eloqGlobalOptions.nodeLogLimitMB},
        {"checkpointer_delay_seconds", eloqGlobalOptions.checkpointerDelaySec},
        {"collect_active_tx_ts_interval_seconds", eloqGlobalOptions.collectActiveTxTsIntervalSec},
        {"realtime_sampling", eloqGlobalOptions.realtimeSampling},
        {"rep_group_cnt", eloqGlobalOptions.nodeGroupReplicaNum},
        {"enable_key_cache", eloqGlobalOptions.useKeyCache ? 1 : 0},
        {"enable_shard_heap_defragment", eloqGlobalOptions.enableHeapDefragment ? 1 : 0},
        {"bthread_worker_num", eloqGlobalOptions.bthreadWorkerNum}};

    const std::string& hmIP = eloqGlobalOptions.hostManagerAddr.host();
    uint16_t hmPort = eloqGlobalOptions.hostManagerAddr.port();
    const std::string& hmBinPath = eloqGlobalOptions.hostManagerBinPath;

    MONGO_LOG(1) << "core_num: " << eloqGlobalOptions.coreNum;
    bool storeBootstrap = false;
    bool storeDdlSkipKv = false;
    Eloq::storeHandler =
        std::make_unique<Eloq::CassHandler>(eloqGlobalOptions.cassHosts,
                                            eloqGlobalOptions.cassPort,
                                            eloqGlobalOptions.cassUser,
                                            eloqGlobalOptions.cassPassword,
                                            eloqGlobalOptions.keyspaceName,
                                            eloqGlobalOptions.cassKeyspaceClass,
                                            eloqGlobalOptions.cassReplicationFactor,
                                            eloqGlobalOptions.cassHighCompressionRatio,
                                            eloqGlobalOptions.cassQueueSizeIO,
                                            storeBootstrap,
                                            storeDdlSkipKv);


    if (!Eloq::storeHandler->Connect()) {
        error() << "Failed to connect to cassandra server. Terminated.";
        MONGO_UNREACHABLE;
    }


    std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>> ngConfigs;
    uint64_t clusterConfigVersion;
    bool uninitialized = false;
    if (!Eloq::storeHandler->ReadClusterConfig(ngConfigs, clusterConfigVersion, uninitialized)) {
        if (!uninitialized) {
            error() << "Failed to read cluster config from kv store. Terminated";
            MONGO_UNREACHABLE;
        }

        std::vector<mongo::HostAndPort> nodeAddrs = eloqGlobalOptions.NodeGroupAddrs();
        std::string nodeAddrList, standbyAddrList, voterAddrList;
        for (size_t i = 0; i < nodeAddrs.size(); ++i) {
            const mongo::HostAndPort& addr = nodeAddrs[i];
            nodeAddrList.append(addr.toString());
            if (i < nodeAddrs.size() - 1) {
                nodeAddrList.push_back(',');
            }
        }
        auto parseRes = txservice::ParseNgConfig(nodeAddrList,
                                                 standbyAddrList,
                                                 voterAddrList,
                                                 ngConfigs,
                                                 eloqGlobalOptions.nodeGroupReplicaNum);
        if (!parseRes) {
            error() << "Failed to extract cluster configs from ip_port_list.";
            MONGO_UNREACHABLE;
        }

        if (!Eloq::storeHandler->InitializeClusterConfig(ngConfigs)) {
            error() << "Failed to initialize cluster config in kvstore. Terminated.";
            MONGO_UNREACHABLE;
        }
        clusterConfigVersion = 2;
    }

    bool found = false;
    uint32_t nodeId = UINT32_MAX, nativeNgId = 0;
    // check whether this node is in cluster.
    for (auto& pair : ngConfigs) {
        auto& ngNodes = pair.second;
        for (size_t i = 0; i < ngNodes.size(); i++) {
            if (ngNodes[i].host_name_ == eloqGlobalOptions.localAddr.host() &&
                ngNodes[i].port_ == eloqGlobalOptions.localAddr.port()) {
                nodeId = ngNodes[i].node_id_;
                found = true;
                if (ngNodes[i].is_candidate_) {
                    // found native_ng_id.
                    nativeNgId = pair.first;
                    break;
                }
            }
        }
    }

    if (!found) {
        error() << "Current node does not belong to any node group. Terminated.";
        MONGO_UNREACHABLE;
    }

    std::vector<std::string> txlogIPs;
    std::vector<uint16_t> txlogPorts;

    if (eloqGlobalOptions.txlogServiceAddrs.empty()) {
        log() << "Stand-alone txlog service is not provided, start bounded txlog service.";
        std::string txlogPath(localPath);
        txlogPath.append("/tx_log");
        uint16_t logServerPort = eloqGlobalOptions.localAddr.port() + 2;
        for (uint32_t ng = 0; ng < ngConfigs.size(); ng++) {
            // Use cc node port + 2 for log server
            txlogIPs.emplace_back(ngConfigs[ng][0].host_name_);
            txlogPorts.emplace_back(ngConfigs[ng][0].port_ + 2);
        }
        _logServer = std::make_unique<txlog::LogServer>(nodeId, logServerPort, txlogPath, 1);
        int err = _logServer->Start();
        if (err != 0) {
            error() << "Fail to start log service. Terminated.";
            MONGO_UNREACHABLE;
        }
    } else {
        txlogIPs = eloqGlobalOptions.TxlogIPs();
        txlogPorts = eloqGlobalOptions.TxlogPorts();
    }
    _logAgent = std::make_unique<Eloq::MongoLogAgent>(eloqGlobalOptions.txlogGroupReplicaNum);
    _txService = std::make_unique<txservice::TxService>(
        // localPath,
        &_catalogFactory,
        &_mongoSystemHandler,
        // nullptr,
        txServiceConf,
        nodeId,
        nativeNgId,
        &ngConfigs,
        clusterConfigVersion,
        Eloq::storeHandler.get(),
        _logAgent.get(),
        // &txlogIPs,
        // &txlogPorts,
        eloqGlobalOptions.enableMVCC,
        eloqGlobalOptions.skipRedoLog);

#ifdef EXT_TX_PROC_ENABLED
    getTxServiceFunctors = _txService->GetTxProcFunctors();
#endif

    if (_txService->Start(nodeId,
                          nativeNgId,
                          &ngConfigs,
                          clusterConfigVersion,
                          &txlogIPs,
                          &txlogPorts,
                          &hmIP,
                          &hmPort,
                          &hmBinPath,
                          txServiceConf,
                          std::move(_logAgent),
                          localPath) < 0) {
        error() << "Fail to start tx service. Terminated!";
        MONGO_UNREACHABLE;
    }

    txservice::DeadLockCheck::SetTimeInterval(eloqGlobalOptions.deadlockIntervalSec);
    Eloq::storeHandler->SetTxService(_txService.get());

    _txService->WaitClusterReady();

    if (eloqGlobalOptions.localAddr != eloqGlobalOptions.nodeGroupAddrs[0]) {
        waitBootstrap();
    }
}

EloqKVEngine::~EloqKVEngine() {
    MONGO_LOG(1) << "EloqKVEngine::~MonograhKVEngine";
    _txService->Shutdown();
    Eloq::storeHandler.reset();
    _logServer.reset();
    _txService.reset();
}

void EloqKVEngine::waitBootstrap() {
    MONGO_LOG(1) << "EloqKVEngine::waitBootstrap";
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    auto txm = txservice::NewTxInit(_txService.get(),
                                    txservice::IsolationLevel::ReadCommitted,
                                    txservice::CcProtocol::OCC,
                                    UINT32_MAX,
                                    -1);
    txservice::TableName tableName{std::string{"local.startup_log"}, txservice::TableType::Primary};
    txservice::CatalogKey catalogKey{tableName};
    txservice::CatalogRecord catalogRecord;
    bool exists{false};

    for (uint16_t i = 0; i < 1000; ++i) {
        txservice::TxKey catalogTxKey(&catalogKey);
        txservice::ReadTxRequest readTxReq(&txservice::catalog_ccm_name,
                                           0,
                                           &catalogTxKey,
                                           &catalogRecord,
                                           false,
                                           false,
                                           true,
                                           0,
                                           false,
                                           false);

        auto errorCode = txservice::TxReadCatalog(txm, readTxReq, exists);
        if (errorCode != txservice::TxErrorCode::NO_ERROR) {
            MONGO_LOG(1) << "readCatalog for local.startup_log error occur. ErrorCode: "
                         << errorCode;
        }
        if (exists) {
            txservice::AbortTx(txm);
            MONGO_LOG(1) << "local.startup_log exists. Bootstrap success.";
            return;
        } else {
            MONGO_LOG(1) << "local.startup_log not exists. Wait for bootstrap.";
        }

        MONGO_LOG(0) << "Retry count: " << i;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        catalogRecord.Reset();
    }
    error() << "Retry too many times. Waiting for boostrap fail.";
}

bool EloqKVEngine::supportsRecoveryTimestamp() const {
    return false;
}

boost::optional<Timestamp> EloqKVEngine::getRecoveryTimestamp() const {
    if (!supportsRecoveryTimestamp()) {
        return boost::none;
    }
}

RecoveryUnit* EloqKVEngine::newRecoveryUnit() {
    MONGO_LOG(1) << "EloqKVEngine::newRecoveryUnit";
    return new EloqRecoveryUnit(_txService.get());
}

RecoveryUnit::UPtr EloqKVEngine::newRecoveryUnitUPtr() {
    MONGO_UNREACHABLE;
    // MONGO_LOG(1) << "EloqKVEngine::newRecoveryUnitUPtr";
    // return ObjectPool<EloqRecoveryUnit>::newObject<RecoveryUnit>(_txService.get()
    //                                                                   );
}

void EloqKVEngine::listDatabases(std::vector<std::string>& out) const {
    MONGO_LOG(1) << "EloqKVEngine::listDatabases";

    std::vector<std::string> tables;
    Eloq::storeHandler->DiscoverAllTableNames(tables);

    std::unordered_set<std::string> dbNames;
    for (const auto& tableName : tables) {
        std::string_view dbName = extractDbName(tableName);
        if (!dbName.empty()) {
            dbNames.emplace(dbName);
        }
    }

    out.reserve(dbNames.size());
    for (auto it = dbNames.begin(); it != dbNames.end();) {
        out.push_back(std::move(dbNames.extract(it++).value()));
    }

    std::string dbString;
    for (const auto& name : out) {
        dbString.append(name).append("|");
    }
    MONGO_LOG(1) << "tables: " << dbString;
}

bool EloqKVEngine::databaseExists(std::string_view dbName) const {
    MONGO_LOG(1) << "EloqKVEngine::databaseExists"
                 << ". dbName: " << dbName;

    std::vector<std::string> tables;
    Eloq::storeHandler->DiscoverAllTableNames(tables);

    for (const auto& tableName : tables) {
        if (dbName == extractDbName(tableName)) {
            return true;
        }
    }
    return false;
}

void EloqKVEngine::listCollections(std::string_view dbName, std::vector<std::string>& out) const {
    MONGO_LOG(1) << "EloqKVEngine::listCollections"
                 << ". db: " << dbName;
    std::vector<std::string> allCollections;
    Eloq::storeHandler->DiscoverAllTableNames(allCollections);
    for (auto& collection : allCollections) {
        MONGO_LOG(1) << "dbname:" << extractDbName(collection);
        if (extractDbName(collection) == dbName) {
            out.push_back(std::move(collection));
        }
    }
    std::string str;
    for (const auto& name : out) {
        str.append(name).append("|");
    }
    MONGO_LOG(1) << "tables: " << str;
}
void EloqKVEngine::listCollections(std::string_view dbName, std::set<std::string>& out) const {
    MONGO_LOG(1) << "EloqKVEngine::listCollections"
                 << ". db: " << dbName;
    std::vector<std::string> allCollections;
    Eloq::storeHandler->DiscoverAllTableNames(allCollections);
    for (auto& collection : allCollections) {
        MONGO_LOG(1) << "dbname:" << extractDbName(collection);
        if (extractDbName(collection) == dbName) {
            out.emplace(std::move(collection));
        }
    }
    std::string str;
    for (const auto& name : out) {
        str.append(name).append("|");
    }
    MONGO_LOG(1) << "tables: " << str;
}

std::pair<bool, Status> EloqKVEngine::lockCollection(OperationContext* opCtx,
                                                     StringData ns,
                                                     bool isForWrite) {
    MONGO_LOG(1) << "EloqKVEngine::lockCollection"
                 << ". ns: " << ns << ", isForWrite: " << isForWrite;
    auto ru = EloqRecoveryUnit::get(opCtx);
    txservice::TableName tableName = Eloq::MongoTableToTxServiceTableName(ns.toStringView(), false);
    const Eloq::MongoTableSchema* tableSchema = ru->getTableSchema(tableName);
    if (tableSchema) {
        return {true, Status::OK()};
    } else {
        return {false, Status::OK()};
    }
}

std::unique_ptr<RecordStore> EloqKVEngine::getRecordStore(OperationContext* opCtx,
                                                          StringData ns,
                                                          StringData ident,
                                                          const CollectionOptions& options) {
    MONGO_LOG(1) << "EloqKVEngine::getRecordStore"
                 << ". ns: " << ns << ". ident: " << ident;
    if (isMongoCatalog(ident.toStringView())) {
        return std::make_unique<EloqCatalogRecordStore>(opCtx, ns);
    }

    EloqRecordStore::Params params{
        Eloq::MongoTableToTxServiceTableName(ns.toStringView(), true), ns, ident, options.capped};

    if (options.capped) {
        params.cappedMaxSize = options.cappedSize ? options.cappedSize : 4096;
        params.cappedMaxDocs = options.cappedMaxDocs ? options.cappedMaxDocs : -1;
    }

    // {
    //     std::scoped_lock<std::mutex> lk{_tableNameIdentMapMutex};
    //     _tableNameIdentMap[params.tableName] = ident.toString();
    // }

    auto recordStore = std::make_unique<EloqRecordStore>(opCtx, params);

    {
        std::scoped_lock<std::mutex> lk(_identCollectionMapMutex);
        _identCollectionMap[ident.toString()] = recordStore.get();
    }

    return recordStore;
}

std::unique_ptr<RecordStore> EloqKVEngine::getGroupedRecordStore(OperationContext* opCtx,
                                                                 StringData ns,
                                                                 StringData ident,
                                                                 const CollectionOptions& options,
                                                                 KVPrefix prefix) {
    MONGO_LOG(1) << "EloqKVEngine::getGroupedRecordStore";

    invariant(prefix == KVPrefix::kNotPrefixed);
    return getRecordStore(opCtx, ns, ident, options);
}

Status EloqKVEngine::createRecordStore(OperationContext* opCtx,
                                       StringData ns,
                                       StringData ident,
                                       const CollectionOptions& options) {
    MONGO_LOG(1) << "EloqKVEngine::createRecordStore"
                 << ". ns: " << ns << ". ident: " << ident;
    if (!isMongoCatalog(ns.toStringView())) {
        MONGO_LOG(1) << "Eloq do nothing here for normal table";
        return Status::OK();
    }

    MONGO_UNREACHABLE;
}

Status EloqKVEngine::createGroupedRecordStore(OperationContext* opCtx,
                                              StringData ns,
                                              StringData ident,
                                              const CollectionOptions& options,
                                              KVPrefix prefix) {
    MONGO_LOG(1) << "EloqKVEngine::createGroupedRecordStore"
                 << ". prefix: " << prefix.toString();

    invariant(prefix == KVPrefix::kNotPrefixed);
    return createRecordStore(opCtx, ns, ident, options);
}

SortedDataInterface* EloqKVEngine::getSortedDataInterface(OperationContext* opCtx,
                                                          StringData ident,
                                                          const IndexDescriptor* desc) {
    MONGO_LOG(1) << "EloqKVEngine::getSortedDataInterface"
                 << ". ident: " << ident;

    txservice::TableName tableName{Eloq::MongoTableToTxServiceTableName(desc->parentNS(), true)};
    txservice::TableName indexName{desc->isIdIndex()
                                       ? tableName
                                       : Eloq::MongoIndexToTxServiceTableName(
                                             desc->parentNS(), desc->indexName(), desc->unique())};
    MONGO_LOG(1) << "tableName: " << tableName.StringView();

    std::unique_ptr<EloqIndex> index;

    if (desc->isIdIndex()) {
        MONGO_LOG(1) << "EloqKVEngine::getSortedDataInterface"
                     << ". IdIndex";
        index =
            std::make_unique<EloqIdIndex>(opCtx, std::move(tableName), std::move(indexName), desc);
    } else {
        if (desc->unique()) {
            MONGO_LOG(1) << "EloqKVEngine::getSortedDataInterface"
                         << ". UniqueIndex";
            index = std::make_unique<EloqUniqueIndex>(
                opCtx, std::move(tableName), std::move(indexName), desc);
        } else {
            MONGO_LOG(1) << "EloqKVEngine::getSortedDataInterface"
                         << ". StandardIndex";
            index = std::make_unique<EloqStandardIndex>(
                opCtx, std::move(tableName), std::move(indexName), desc);
        }
    }

    {
        std::scoped_lock<std::mutex> lk{_identIndexMapMutex};
        _identIndexMap[ident.toString()] = index.get();
    }

    return index.release();
}

SortedDataInterface* EloqKVEngine::getGroupedSortedDataInterface(OperationContext* opCtx,
                                                                 StringData ident,
                                                                 const IndexDescriptor* desc,
                                                                 KVPrefix prefix) {
    MONGO_LOG(1) << "EloqKVEngine::getGroupedSortedDataInterface";

    invariant(prefix == KVPrefix::kNotPrefixed);
    return getSortedDataInterface(opCtx, ident, desc);
}

Status EloqKVEngine::createSortedDataInterface(OperationContext* opCtx,
                                               StringData ident,
                                               const IndexDescriptor* desc) {
    MONGO_LOG(1) << "EloqKVEngine::createSortedDataInterface. "
                 << "ident: " << ident;

    assert(!isMongoCatalog(ident.toStringView()));
    MONGO_LOG(1) << "Eloq do nothing here";
    return Status::OK();
    MONGO_UNREACHABLE;
}

Status EloqKVEngine::createGroupedSortedDataInterface(OperationContext* opCtx,
                                                      StringData ident,
                                                      const IndexDescriptor* desc,
                                                      KVPrefix prefix) {
    MONGO_LOG(1) << "EloqKVEngine::createGroupedSortedDataInterface";
    invariant(prefix == KVPrefix::kNotPrefixed);
    return createSortedDataInterface(opCtx, ident, desc);
}

int64_t EloqKVEngine::getIdentSize(OperationContext* opCtx, StringData ident) {
    MONGO_LOG(1) << "EloqKVEngine::getIdentSize"
                 << ". ident: " << ident;
    {
        std::scoped_lock<std::mutex> lk{_identCollectionMapMutex};
        if (const auto& iter = _identCollectionMap.find(ident.toStringView());
            iter != _identCollectionMap.end()) {
            return iter->second->storageSize(opCtx);
        }
    }

    {
        std::scoped_lock<std::mutex> lk{_identIndexMapMutex};
        if (const auto& iter = _identIndexMap.find(ident.toStringView());
            iter != _identIndexMap.end()) {
            return iter->second->getSpaceUsedBytes(opCtx);
        }
    }

    MONGO_LOG(1) << "ident may not opened by call getRecordStore or getSortedDataInterface";
    // this can only happen if collection or index exists, but it's not opened (i.e.
    // getRecordStore or getSortedDataInterface are not called)
    return 0;
}

Status EloqKVEngine::repairIdent(OperationContext* opCtx, StringData ident) {
    // MONGO_UNREACHABLE;
    // No need to repair
    return Status::OK();
}

Status EloqKVEngine::dropIdent(OperationContext* opCtx, StringData ident) {
    MONGO_LOG(1) << "EloqKVEngine::dropIdent"
                 << ". ident: " << ident;
    // Attention please!
    // EloqRecordStore and EloqIndex now have been destructed
    {
        std::scoped_lock<std::mutex> lk{_identCollectionMapMutex};
        if (const auto& iter = _identCollectionMap.find(ident.toStringView());
            iter != _identCollectionMap.end()) {
            _identCollectionMap.erase(iter);
        }
    }
    {
        std::scoped_lock<std::mutex> lk{_identIndexMapMutex};
        if (const auto& iter = _identIndexMap.find(ident.toStringView());
            iter != _identIndexMap.end()) {
            _identIndexMap.erase(iter);
        }
    }

    return Status::OK();
}

bool EloqKVEngine::supportsDirectoryPerDB() const {
    return false;
}

bool EloqKVEngine::hasIdent(OperationContext* opCtx, StringData ident) const {
    MONGO_LOG(1) << "EloqKVEngine::hasIdent"
                 << ". ident: " << ident;
    if (isMongoCatalog(ident.toStringView())) {
        return true;
    }
    MONGO_UNREACHABLE;
    // This function seems to be only used for checking _mdb_catalog
    // which always exists in Eloq (catalog_cc_map)
}

std::vector<std::string> EloqKVEngine::getAllIdents(OperationContext* opCtx) const {
    MONGO_LOG(1) << "EloqKVEngine::getAllIdents";
    std::vector<std::string> all;

    std::vector<std::string> tableNameVector;
    Eloq::storeHandler->DiscoverAllTableNames(tableNameVector);

    auto ru = EloqRecoveryUnit::get(opCtx);

    for (auto& nameString : tableNameVector) {
        if (isMongoCatalog(nameString)) {
            continue;
        }
        txservice::TableName tableName{std::move(nameString), txservice::TableType::Primary};
        txservice::CatalogKey catalogKey{tableName};
        txservice::CatalogRecord catalogRecord;
        auto [exists, errorCode] = ru->readCatalog(catalogKey, catalogRecord, false);
        // todo: Does exists==false valid? When does Mongo do full table scan on Catalog?
        assert(exists);

        std::string metadata, kvInfo, keySchemaTsString;
        Eloq::DeserializeSchemaImage(
            catalogRecord.Schema()->SchemaImage(), metadata, kvInfo, keySchemaTsString);

        BSONObj obj{metadata.data()};
        if (!metadata.empty()) {
            MONGO_LOG(1) << "metadata: " << obj.jsonString();
        }

        if (KVCatalog::FeatureTracker::isFeatureDocument(obj)) {
            all.emplace_back(kFeatureDocumentSV);
        } else {
            all.emplace_back(obj["ident"].String());
        }

        BSONElement e = obj["idxIdent"];
        if (!e.isABSONObj()) {
            continue;
        }
        BSONObj idxIdent = e.Obj();

        for (const auto& ident : idxIdent) {
            all.emplace_back(ident.String());
        }
    }

    std::string output;
    for (const auto& name : all) {
        output.append(name).append("|");
    }
    MONGO_LOG(1) << "idents: " << output;

    return all;
}

void EloqKVEngine::cleanShutdown() {
    MONGO_LOG(0) << "EloqKVEngine::cleanShutdown";
}

void EloqKVEngine::setJournalListener(JournalListener* jl) {
    //
}

Timestamp EloqKVEngine::getAllCommittedTimestamp() const {
    return Timestamp{0};
}

void EloqKVEngine::startOplogManager(OperationContext* opCtx, EloqRecordStore* oplogRecordStore) {
    //
}

void EloqKVEngine::haltOplogManager(EloqRecordStore* oplogRecordStore, bool shuttingDown) {
    //
}

}  // namespace mongo
