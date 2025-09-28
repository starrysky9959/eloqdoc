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
#include <stdexcept>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

#include "mongo/db/modules/eloq/eloq_metrics/include/metrics.h"
#include "mongo/db/modules/eloq/src/base/eloq_key.h"
#include "mongo/db/modules/eloq/src/base/eloq_log_agent.h"
#include "mongo/db/modules/eloq/src/base/eloq_record.h"
#include "mongo/db/modules/eloq/src/base/eloq_util.h"
#include "mongo/db/modules/eloq/src/base/metrics_registry_impl.h"
#include "mongo/db/modules/eloq/src/eloq_global_options.h"
#include "mongo/db/modules/eloq/src/eloq_index.h"
#include "mongo/db/modules/eloq/src/eloq_kv_engine.h"
#include "mongo/db/modules/eloq/src/eloq_record_store.h"
#include "mongo/db/modules/eloq/src/eloq_recovery_unit.h"
#include "mongo/db/modules/eloq/store_handler/kv_store.h"
#include "mongo/db/modules/eloq/tx_service/include/catalog_key_record.h"
#include "mongo/db/modules/eloq/tx_service/include/dead_lock_check.h"
#include "mongo/db/modules/eloq/tx_service/include/sequences/sequences.h"

#include "mongo/db/modules/eloq/tx_service/include/tx_key.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_record.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_service.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_service_metrics.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_util.h"
#include "mongo/db/modules/eloq/tx_service/include/type.h"

#if (defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||  \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS) || \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB) || defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE))
#define ELOQDS 1
#endif

#if defined(DATA_STORE_TYPE_CASSANDRA)
#include "store_handler/cass_handler.h"
#elif defined(DATA_STORE_TYPE_DYNAMODB)
#include "store_handler/dynamo_handler.h"
#elif defined(DATA_STORE_TYPE_BIGTABLE)
#include "store_handler/bigtable_handler.h"
#elif ELOQDS
#include "mongo/db/modules/eloq/store_handler/data_store_service_client.h"
#include "mongo/db/modules/eloq/store_handler/eloq_data_store_service/data_store_service.h"
#include "mongo/db/modules/eloq/store_handler/eloq_data_store_service/data_store_service_config.h"
#if (defined(ROCKSDB_CLOUD_FS_TYPE) &&                     \
     (ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3 || \
      ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_GCS))
#include "store_handler/eloq_data_store_service/rocksdb_cloud_data_store_factory.h"
#include "store_handler/eloq_data_store_service/rocksdb_config.h"
#elif defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB)
#include "store_handler/eloq_data_store_service/rocksdb_config.h"
#include "store_handler/eloq_data_store_service/rocksdb_data_store_factory.h"
#elif defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE)
#include "store_handler/eloq_data_store_service/eloq_store_data_store_factory.h"
#endif
#else
#endif

#if (defined(DATA_STORE_TYPE_DYNAMODB) ||                                      \
     (defined(USE_ROCKSDB_LOG_STATE) && (WITH_ROCKSDB_CLOUD == CS_TYPE_S3)) || \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3))
#include <aws/core/Aws.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#endif

namespace Eloq {
std::unique_ptr<txservice::store::DataStoreHandler> storeHandler;
#if ELOQDS
std::unique_ptr<EloqDS::DataStoreService> dataStoreService;
#endif

}  // namespace Eloq
namespace mongo {

#if (defined(DATA_STORE_TYPE_DYNAMODB) ||                                      \
     (defined(USE_ROCKSDB_LOG_STATE) && (WITH_ROCKSDB_CLOUD == CS_TYPE_S3)) || \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3))


Aws::SDKOptions aws_options;

static int awsInit() {
    log() << "AWS init";
    aws_options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Info;
    Aws::InitAPI(aws_options);
    return 0;
}

static int aws_deinit() {
    log() << "AWS deinit";
    Aws::ShutdownAPI(aws_options);
    return 0;
}
#endif


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

bool EloqKVEngine::InitMetricsRegistry() {

    Eloq::MetricsRegistryImpl::MetricsRegistryResult metricsRegistryResult =
        Eloq::MetricsRegistryImpl::GetRegistry();

    if (metricsRegistryResult.not_ok_ != nullptr) {
        return false;
    }

    _metricsRegistry = std::move(metricsRegistryResult.metrics_registry_);
    return true;
}

EloqKVEngine::EloqKVEngine(const std::string& path) : _dbPath(path) {
#if (defined(DATA_STORE_TYPE_DYNAMODB) ||                                      \
     (defined(USE_ROCKSDB_LOG_STATE) && (WITH_ROCKSDB_CLOUD == CS_TYPE_S3)) || \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3))
    awsInit();
#endif

    RegisterFactory();

    log() << "Starting Eloq storage engine. dbPath: " << path;

    bool bootstrap = serverGlobalParams.bootstrap;

    std::string localPath("local://");
    localPath.append(path);
    if (localPath.back() == '/') {
        localPath.pop_back();
    }

    std::map<std::string, uint32_t> txServiceConf{
        {"core_num", serverGlobalParams.reservedThreadNum},
        {"range_split_worker_num", eloqGlobalOptions.rangeSplitWorkerNum},
        {"checkpointer_interval", eloqGlobalOptions.checkpointerIntervalSec},
        {"node_memory_limit_mb", eloqGlobalOptions.nodeMemoryLimitMB},
        {"node_log_limit_mb", 100 /*deprecated option*/},
        {"checkpointer_delay_seconds", eloqGlobalOptions.checkpointerDelaySec},
        {"collect_active_tx_ts_interval_seconds", eloqGlobalOptions.collectActiveTxTsIntervalSec},
        {"realtime_sampling", eloqGlobalOptions.realtimeSampling},
        {"rep_group_cnt", eloqGlobalOptions.nodeGroupReplicaNum},
        {"enable_key_cache", eloqGlobalOptions.useKeyCache ? 1 : 0},
        {"enable_shard_heap_defragment", eloqGlobalOptions.enableHeapDefragment ? 1 : 0},
        {"bthread_worker_num", eloqGlobalOptions.bthreadWorkerNum},
        {"kickout_data_for_test", eloqGlobalOptions.kickoutDataForTest},
    };

    const std::string& hmIP = eloqGlobalOptions.hostManagerAddr.host();
    uint16_t hmPort = eloqGlobalOptions.hostManagerAddr.port();
    const std::string& hmBinPath = eloqGlobalOptions.hostManagerBinPath;
#ifdef FORK_HM_PROCESS
    // If the Eloqdoc is under bootstrap mode, we will not fork host manager.
    // Otherwise, we will fork host manager if the option is enabled.
    // Currently, when deploying on the cloud we do not fork host manager.
    bool forkHostManager = !bootstrap && eloqGlobalOptions.forkHostManager;
#else
    bool forkHostManager = false;
#endif

    initDataStoreService();

    std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>> ngConfigs;
    std::string clusterConfigPath = path + "/tx_service/cluster_config";
    uint64_t clusterConfigVersion = 2;

    if (bootstrap) {
        std::vector<txservice::NodeConfig> soloConfig;
        soloConfig.emplace_back(
            0, eloqGlobalOptions.localAddr.host(), eloqGlobalOptions.localAddr.port());
        ngConfigs.try_emplace(0, std::move(soloConfig));
    } else {
        if (!txservice::ReadClusterConfigFile(clusterConfigPath, ngConfigs, clusterConfigVersion)) {
            bool parse_res = txservice::ParseNgConfig(eloqGlobalOptions.ipList,
                                                      "",
                                                      "",
                                                      ngConfigs,
                                                      eloqGlobalOptions.nodeGroupReplicaNum,
                                                      0);
            if (!parse_res) {
                LOG(ERROR) << "Failed to extract cluster configs from ip_port_list.";
                uasserted(ErrorCodes::InvalidOptions,
                          "Failed to extract cluster configs from ip_port_list.");
            }
        }
    }

    log() << "local: " << eloqGlobalOptions.localAddr.host() << ":"
          << eloqGlobalOptions.localAddr.port() << " ";
    bool found = false;
    uint32_t nodeId = UINT32_MAX, nativeNgId = 0;
    // check whether this node is in cluster.
    for (auto& pair : ngConfigs) {
        auto& ngNodes = pair.second;
        for (auto& ngNode : ngNodes) {
            log() << "Node ip:" << ngNode.host_name_ << ":" << ngNode.port_ << " ";
            if (ngNode.host_name_ == eloqGlobalOptions.localAddr.host() &&
                ngNode.port_ == eloqGlobalOptions.localAddr.port()) {
                nodeId = ngNode.node_id_;
                found = true;
                if (ngNode.is_candidate_) {
                    // found native_ng_id.
                    nativeNgId = pair.first;
                    break;
                }
            }
        }
    }

    if (!found) {
        error() << "Current node does not belong to any node group.";
        uasserted(ErrorCodes::InternalError, "Current node does not belong to any node group.");
    }

    std::vector<std::string> txlogIPs;
    std::vector<uint16_t> txlogPorts;

    if (eloqGlobalOptions.txlogServiceAddrs.empty()) {
        log() << "Stand-alone txlog service is not provided, start bounded txlog service.";
        std::string txlogPath(localPath);
        txlogPath.append("/tx_log");

        std::string txlogRocksDBPath;  // default rocksdb path is <data_home>/tx_log/rocksdb
        if (eloqGlobalOptions.txlogRocksDBStoragePath.empty()) {
            txlogRocksDBPath = txlogPath.substr(8) + "/rocksdb";  // remove "local://" prefix
        } else {
            txlogRocksDBPath = eloqGlobalOptions.txlogRocksDBStoragePath;
        }

        uint16_t logServerPort = eloqGlobalOptions.localAddr.port() + 2;
        for (uint32_t ng = 0; ng < ngConfigs.size(); ng++) {
            // Use cc node port + 2 for log server
            txlogIPs.emplace_back(ngConfigs[ng][0].host_name_);
            txlogPorts.emplace_back(ngConfigs[ng][0].port_ + 2);
        }


        bool enable_txlog_request_checkpoint = true;
        std::string eloq_notify_checkpointer_threshold_size = "1GB";
        uint64_t notify_checkpointer_threshold_size =
            txlog::parse_size(eloq_notify_checkpointer_threshold_size);
        log() << "eloq_enable_txlog_request_checkpoint: "
              << (enable_txlog_request_checkpoint ? "ON" : "OFF");
        if (enable_txlog_request_checkpoint) {
            log() << "eloq_notify_checkpointer_threshold_size: "
                  << eloq_notify_checkpointer_threshold_size;
        }

#ifdef USE_ROCKSDB_LOG_STATE
        std::string eloq_rocksdb_target_file_size_base = "10MB";
        size_t rocksdb_target_file_size_base_val =
            txlog::parse_size(eloq_rocksdb_target_file_size_base);
#ifdef WITH_ROCKSDB_CLOUD
        txlog::RocksDBCloudConfig rocksdb_cloud_config;
#if WITH_ROCKSDB_CLOUD == CS_TYPE_S3
        rocksdb_cloud_config.aws_access_key_id_ = eloqGlobalOptions.awsAccessKeyId;
        rocksdb_cloud_config.aws_secret_key_ = eloqGlobalOptions.awsSecretKey;
#endif /* WITH_ROCKSDB_CLOUD == CS_TYPE_S3 */
        rocksdb_cloud_config.bucket_name_ = eloqGlobalOptions.txlogRocksDBCloudBucketName;
        rocksdb_cloud_config.bucket_prefix_ = eloqGlobalOptions.txlogRocksDBCloudBucketPrefix;
        rocksdb_cloud_config.object_path_ = eloqGlobalOptions.txlogRocksDBCloudObjectPath;
        rocksdb_cloud_config.region_ = eloqGlobalOptions.txlogRocksDBCloudRegion;
        rocksdb_cloud_config.endpoint_url_ = eloqGlobalOptions.txlogRocksDBCloudEndpointUrl;
        rocksdb_cloud_config.sst_file_cache_size_ =
            txlog::parse_size(eloqGlobalOptions.txlogRocksDBCloudSstFileCacheSize);
        rocksdb_cloud_config.sst_file_cache_num_shard_bits_ =
            eloqGlobalOptions.txlogRocksDBCloudSstFileCacheNumShardBits;
        rocksdb_cloud_config.db_ready_timeout_us_ =
            eloqGlobalOptions.txlogRocksDBCloudReadyTimeout * 1000 * 1000;
        rocksdb_cloud_config.db_file_deletion_delay_ =
            eloqGlobalOptions.txlogRocksDBCloudFileDeletionDelay;

#if defined(OPEN_LOG_SERVICE)
        _logServer = std::make_unique<txlog::LogServer>(
            nodeId, logServerPort, txlogPath, 1, rocksdb_cloud_config);
#else
        _logServer = std::make_unique<txlog::LogServer>(
            nodeId,
            logServerPort,
            txlogIPs,
            txlogPorts,
            txlogPath,
            0,
            eloqGlobalOptions.txlogGroupReplicaNum,
            txlogRocksDBPath,
            eloqGlobalOptions.txlogRocksDBScanThreads,
            rocksdb_cloud_config
            // eloq_rocksdb_cloud_in_mem_log_size_high_watermark,
            // eloq_rocksdb_max_write_buffer_number,
            // eloq_rocksdb_max_background_jobs, rocksdb_target_file_size_base_val,
            // eloq_logserver_snapshot_interval, enable_txlog_request_checkpoint,
            // eloq_check_replay_log_size_interval_sec,
            // notify_checkpointer_threshold_size
        );
#endif

#else  // rocksdb
#if defined(OPEN_LOG_SERVICE)
        _logServer = std::make_unique<txlog::LogServer>(nodeId, logServerPort, txlogPath, 1);
#else
        // size_t rocksdb_sst_files_size_limit_val =
        //     txlog::parse_size(eloq_rocksdb_sst_files_size_limit);
        _logServer = std::make_unique<txlog::LogServer>(
            nodeId,
            logServerPort,
            txlogIPs,
            txlogPorts,
            txlogPath,
            0,
            eloqGlobalOptions.txlogGroupReplicaNum,
            txlogRocksDBPath,
            eloqGlobalOptions.txlogRocksDBScanThreads
            // rocksdb_sst_files_size_limit_val

            // eloq_rocksdb_max_write_buffer_number,
            // eloq_rocksdb_max_background_jobs, rocksdb_target_file_size_base_val,
            // eloq_logserver_snapshot_interval, enable_txlog_request_checkpoint,
            // eloq_check_replay_log_size_interval_sec,
            // notify_checkpointer_threshold_size
        );
#endif
#endif
#endif /* USE_ROCKSDB_LOG_STATE */


        int err = _logServer->Start();
        if (err != 0) {
            error() << "Failed to start log service.";
            uasserted(ErrorCodes::InternalError, "Failed to start log service");
        }
    } else {
        txlogIPs = eloqGlobalOptions.TxlogIPs();
        txlogPorts = eloqGlobalOptions.TxlogPorts();
    }

    // config metrics
    metrics::enable_metrics = eloqGlobalOptions.enableMetrics;
    setenv("ELOQ_METRICS_PORT", eloqGlobalOptions.metricsPortString.data(), false);
    metrics::enable_memory_usage = eloqGlobalOptions.enableMemoryUsage;
    metrics::collect_memory_usage_round = eloqGlobalOptions.collectMemoryUsageRound;
    metrics::enable_cache_hit_rate = eloqGlobalOptions.enableCacheHitRate;
    metrics::enable_tx_metrics = eloqGlobalOptions.enableTxMetrics;
    metrics::collect_tx_duration_round = eloqGlobalOptions.collectTxDurationRound;
    metrics::enable_busy_round_metrics = eloqGlobalOptions.enableBusyRoundMetrics;
    metrics::busy_round_threshold = eloqGlobalOptions.busyRoundThreshold;
    metrics::enable_remote_request_metrics = eloqGlobalOptions.enableRemoteRequestMetrics;
    metrics::CommonLabels tx_service_common_labels{};
    if (metrics::enable_metrics) {
        if (!InitMetricsRegistry()) {
            error() << "Failed to initialize MetricsRegristry!";
            uasserted(ErrorCodes::InternalError, "MetricsRegristry initialization failed");
        }
        // tx_service_common_labels
        tx_service_common_labels["node_ip"] = eloqGlobalOptions.localAddr.host();
        tx_service_common_labels["node_port"] = std::to_string(eloqGlobalOptions.localAddr.port());
        tx_service_common_labels["node_id"] = std::to_string(nodeId);
        log() << "Export metrics data on port: " << eloqGlobalOptions.metricsPort;
    }

    _logAgent = std::make_unique<Eloq::MongoLogAgent>(eloqGlobalOptions.txlogGroupReplicaNum);
    _txService = std::make_unique<txservice::TxService>(&_catalogFactory,
                                                        &_mongoSystemHandler,
                                                        txServiceConf,
                                                        nodeId,
                                                        nativeNgId,
                                                        &ngConfigs,
                                                        clusterConfigVersion,
                                                        Eloq::storeHandler.get(),
                                                        _logAgent.get(),
                                                        eloqGlobalOptions.enableMVCC,
                                                        eloqGlobalOptions.skipRedoLog,
                                                        false,
                                                        true,
                                                        true,
                                                        _metricsRegistry.get(),
                                                        tx_service_common_labels);


    if (_txService->Start(nodeId,
                          nativeNgId,
                          &ngConfigs,
                          clusterConfigVersion,
                          &txlogIPs,
                          &txlogPorts,
                          forkHostManager ? nullptr : &hmIP,
                          forkHostManager ? nullptr : &hmPort,
                          forkHostManager ? nullptr : &hmBinPath,
                          txServiceConf,
                          std::move(_logAgent),
                          localPath,
                          clusterConfigPath,
                          true,
                          forkHostManager) < 0) {
        error() << "Fail to start tx service. Terminated!";
        uasserted(ErrorCodes::InternalError, "Failed to start tx service.");
    }
    log() << "Eloq tx service starting.";

    txservice::Sequences::InitSequence(_txService.get(), Eloq::storeHandler.get());
    txservice::DeadLockCheck::SetTimeInterval(eloqGlobalOptions.deadlockIntervalSec);
    Eloq::storeHandler->SetTxService(_txService.get());

    log() << "Tx Service waiting for cluster ready.";
    _txService->WaitClusterReady();
    _txService->WaitNodeBecomeNativeGroupLeader();

    if (eloqGlobalOptions.localAddr !=
        mongo::HostAndPort(ngConfigs[0][0].host_name_, ngConfigs[0][0].port_)) {
        // waitBootstrap();
    }

    log() << "Start Eloq storage engine successfully.";
#ifdef EXT_TX_PROC_ENABLED
    getTxServiceFunctors = _txService->GetTxProcFunctors();
#endif
}

void EloqKVEngine::initDataStoreService() {
    auto localIp = eloqGlobalOptions.localAddr.host();
    auto localPort = eloqGlobalOptions.localAddr.port();
#if defined(DATA_STORE_TYPE_CASSANDRA)
    bool storeBootstrap = false;
    bool storeDdlSkipKv = false;

    Eloq::storeHandler =
        std::make_unique<EloqDS::CassHandler>(eloqGlobalOptions.cassHosts,
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
        throw std::runtime_error("Failed to connect to cassandra server. Terminated.");
    }
    return;
#endif

    bool opt_bootstrap = serverGlobalParams.bootstrap;
    // TODO(starrysky)
    bool is_single_node = true;
    std::string ds_peer_node = eloqGlobalOptions.dssPeerNode;
    std::string dss_config_file_path = eloqGlobalOptions.dataStoreServiceConfigFilePath;
    if (dss_config_file_path == "") {
        dss_config_file_path = _dbPath + "/dss_config.ini";
    }
    log() << "Data Store Service Config File Path: " << dss_config_file_path;
    EloqDS::DataStoreServiceClusterManager ds_config;
    if (ds_config.Load(dss_config_file_path)) {
        log() << "EloqDataStoreService loaded config file: " << dss_config_file_path;
    } else {
        if (!ds_peer_node.empty()) {
            ds_config.SetThisNode(localIp, localPort + 7);
            // Fetch ds topology from peer node
            if (!EloqDS::DataStoreService::FetchConfigFromPeer(ds_peer_node, ds_config)) {
                error() << "Failed to fetch config from peer node: " << ds_peer_node;
                uasserted(ErrorCodes::InternalError, "DataStoreService initialization failed");
            }

            // Save the fetched config to the local file
            if (!ds_config.Save(dss_config_file_path)) {
                error() << "Failed to save config to file: " << dss_config_file_path;
                uasserted(ErrorCodes::InternalError,
                          "DataStoreService failed to save config to file: " +
                              dss_config_file_path);
            }
        } else if (opt_bootstrap || is_single_node) {
            // Initialize the data store service config
            ds_config.Initialize(localIp, localPort + 7);
            if (!ds_config.Save(dss_config_file_path)) {
                error() << "Failed to save config to file: " << dss_config_file_path;
                uasserted(ErrorCodes::InternalError,
                          "DataStoreService failed to save config to file: " +
                              dss_config_file_path);
            }
        } else {
            error() << "Failed to load data store service config file: " << dss_config_file_path;
            uasserted(ErrorCodes::InternalError,
                      "DataStoreService initialization failed, config file not found: " +
                          dss_config_file_path);
        }
    }

#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) || \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS)
    // std::string ds_rocksdb_config_file_path=
    //     "/home/lzx/test-eloqsql/eloq_ds.ini";
    INIReader fake_config_reader(nullptr, 0);
    EloqDS::RocksDBConfig rocksdb_config(fake_config_reader, _dbPath);
    rocksdb_config.max_background_jobs_ = eloqGlobalOptions.rocksdbMaxBackgroundJobs;
    rocksdb_config.max_subcompactions_ = eloqGlobalOptions.rocksdbMaxSubCompactions;

    rocksdb_config.storage_path_ = eloqGlobalOptions.rocksdbCloudStoragePath;
    if (rocksdb_config.storage_path_.empty()) {
        rocksdb_config.storage_path_ = _dbPath + "/dss_rocksdb_cloud";
    }
    EloqDS::RocksDBCloudConfig rocksdb_cloud_config(fake_config_reader);
#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
    rocksdb_cloud_config.aws_access_key_id_ = eloqGlobalOptions.awsAccessKeyId;
    rocksdb_cloud_config.aws_secret_key_ = eloqGlobalOptions.awsSecretKey;
#endif
    rocksdb_cloud_config.bucket_name_ = eloqGlobalOptions.rocksdbCloudBucketName;
    rocksdb_cloud_config.bucket_prefix_ = eloqGlobalOptions.rocksdbCloudBucketPrefix;
    rocksdb_cloud_config.object_path_ = eloqGlobalOptions.rocksdbCloudObjectPath;
    rocksdb_cloud_config.region_ = eloqGlobalOptions.rocksdbCloudRegion;
    rocksdb_cloud_config.s3_endpoint_url_ = eloqGlobalOptions.rocksdbCloudEndpointUrl;
    rocksdb_cloud_config.sst_file_cache_size_ =
        txlog::parse_size(eloqGlobalOptions.rocksdbCloudSstFileCacheSize);
    rocksdb_cloud_config.sst_file_cache_num_shard_bits_ =
        eloqGlobalOptions.txlogRocksDBCloudSstFileCacheNumShardBits;
    rocksdb_cloud_config.db_ready_timeout_us_ =
        eloqGlobalOptions.txlogRocksDBCloudReadyTimeout * 1000 * 1000;
    rocksdb_cloud_config.db_file_deletion_delay_ =
        eloqGlobalOptions.txlogRocksDBCloudFileDeletionDelay;

    bool enable_cache_replacement_ =
        fake_config_reader.GetBoolean("local", "enable_cache_replacement", false);
    auto ds_factory = std::make_unique<EloqDS::RocksDBCloudDataStoreFactory>(
        rocksdb_config, rocksdb_cloud_config, enable_cache_replacement_);
#elif defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB)
    // setup rocksdb data store
    INIReader fake_config_reader(nullptr, 0);
    EloqDS::RocksDBConfig rocksdb_config(fake_config_reader, _dbPath);
    bool enable_cache_replacement_ =
        fake_config_reader.GetBoolean("local", "enable_cache_replacement", false);
    auto ds_factory = std::make_unique<EloqDS::RocksDBDataStoreFactory>(rocksdb_config,
                                                                        enable_cache_replacement_);
#elif defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE)
    EloqDS::EloqStoreConfig eloq_store_config;
    eloq_store_config.worker_count_ = eloqGlobalOptions.eloqStoreWorkerCount;
    eloq_store_config.storage_path_ = eloqGlobalOptions.eloqStoreStoragePath;
    if (eloq_store_config.storage_path_.empty()) {
        eloq_store_config.storage_path_ = _dbPath + "/dss_eloqstore";
    }
    eloq_store_config.open_files_limit_ = eloqGlobalOptions.eloqStoreOpenFilesLimit;
    eloq_store_config.cloud_store_path_ = eloqGlobalOptions.eloqStoreCloudStorePath;
    if (!eloq_store_config.cloud_store_path_.empty()) {
        log() << "EloqStore cloud store enabled";
    }
    eloq_store_config.gc_threads_ =
        eloq_store_config.cloud_store_path_.empty() ? eloqGlobalOptions.eloqStoreGcThreads : 0;
    eloq_store_config.cloud_worker_count_ = eloqGlobalOptions.eloqStoreCloudWorkerCount;
    auto ds_factory = std::make_unique<EloqDS::EloqStoreDataStoreFactory>(eloq_store_config);
#endif

    Eloq::dataStoreService = std::make_unique<EloqDS::DataStoreService>(
        ds_config, dss_config_file_path, _dbPath + "/DSMigrateLog", std::move(ds_factory));
    std::vector<uint32_t> dss_shards = ds_config.GetShardsForThisNode();
    std::unordered_map<uint32_t, std::unique_ptr<EloqDS::DataStore>> dss_shards_map;
    // setup rocksdb cloud data store
    for (int shard_id : dss_shards) {
#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) || \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS)
        // TODO(lzx):move setup datastore to data_store_service
        auto ds = std::make_unique<EloqDS::RocksDBCloudDataStore>(rocksdb_cloud_config,
                                                                  rocksdb_config,
                                                                  (opt_bootstrap || is_single_node),
                                                                  enable_cache_replacement_,
                                                                  shard_id,
                                                                  Eloq::dataStoreService.get());
#elif defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB)
        auto ds = std::make_unique<EloqDS::RocksDBDataStore>(rocksdb_config,
                                                             (opt_bootstrap || is_single_node),
                                                             enable_cache_replacement_,
                                                             shard_id,
                                                             Eloq::dataStoreService.get());
#elif defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE)
        ::eloqstore::KvOptions store_config;
        store_config.num_threads = eloq_store_config.worker_count_;
        store_config.store_path.emplace_back()
            .append(eloq_store_config.storage_path_)
            .append("/ds_")
            .append(std::to_string(shard_id));
        store_config.fd_limit = eloq_store_config.open_files_limit_;
        if (!eloq_store_config.cloud_store_path_.empty()) {
            store_config.cloud_store_path.append(eloq_store_config.cloud_store_path_)
                .append("/ds_")
                .append(std::to_string(shard_id));
        }
        store_config.num_gc_threads = eloq_store_config.gc_threads_;
        store_config.rclone_threads = eloq_store_config.cloud_worker_count_;

        DLOG(INFO) << "Create EloqStore storage with workers: " << store_config.num_threads
                   << ", store path: " << store_config.store_path.front()
                   << ", open files limit: " << store_config.fd_limit
                   << ", cloud store path: " << store_config.cloud_store_path
                   << ", gc threads: " << store_config.num_gc_threads
                   << ", cloud worker count: " << store_config.rclone_threads;
        auto ds = std::make_unique<EloqDS::EloqStoreDataStore>(
            shard_id, Eloq::dataStoreService.get(), store_config);
#endif
        ds->Initialize();

        // Start db if the shard status is not closed
        if (ds_config.FetchDSShardStatus(shard_id) != EloqDS::DSShardStatus::Closed) {
            bool ret = ds->StartDB();
            if (!ret) {
                error() << "Failed to start db instance in data store service";
                uasserted(ErrorCodes::InternalError,
                          "DataStoreService failed to start db instance");
            }
        }
        dss_shards_map[shard_id] = std::move(ds);
    }

    // setup local data store service
    bool ret = Eloq::dataStoreService->StartService();
    if (!ret) {
        error() << "Failed to start data store service";
        uasserted(ErrorCodes::InternalError, "DataStoreService failed to start service");
    }
    Eloq::dataStoreService->ConnectDataStore(std::move(dss_shards_map));
    // setup data store service client
    Eloq::storeHandler =
        std::make_unique<EloqDS::DataStoreServiceClient>(ds_config, Eloq::dataStoreService.get());

    if (!Eloq::storeHandler->Connect()) {
        error() << "!!!!!!!! Failed to connect ELOQ_DS server, EloqDB "
                   "startup is terminated !!!!!!!!";
        uasserted(ErrorCodes::InternalError, "Failed to connect ELOQ_DS server");
    }
    log() << "Data store service initialize success.";
}


EloqKVEngine::~EloqKVEngine() {
    MONGO_LOG(1) << "EloqKVEngine::~EloqKVEngine";
    if (_txService) {
        cleanShutdown();
    }
}

// void EloqKVEngine::waitBootstrap() {
//     std::thread bootstrapThread([this]() {
//         MONGO_LOG(1) << "EloqKVEngine::waitBootstrap started in a background thread";
//         std::this_thread::sleep_for(std::chrono::milliseconds{500});
//         auto txm = txservice::NewTxInit(_txService.get(),
//                                         txservice::IsolationLevel::ReadCommitted,
//                                         txservice::CcProtocol::OCC,
//                                         UINT32_MAX,
//                                         -1);
//         txservice::TableName tableName{std::string{"local.startup_log"},
//                                        txservice::TableType::Primary,
//                                        txservice::TableEngine::EloqDoc};
//         txservice::CatalogKey catalogKey{tableName};
//         txservice::CatalogRecord catalogRecord;
//         bool exists{false};

//         for (uint16_t i = 0; i < 1000; ++i) {
//             txservice::TxKey catalogTxKey(&catalogKey);
//             txservice::ReadTxRequest readTxReq(&txservice::catalog_ccm_name,
//                                                0,
//                                                &catalogTxKey,
//                                                &catalogRecord,
//                                                false,
//                                                false,
//                                                true,
//                                                0,
//                                                false,
//                                                false);

//             auto errorCode = txservice::TxReadCatalog(txm, readTxReq, exists);
//             if (errorCode != txservice::TxErrorCode::NO_ERROR) {
//                 MONGO_LOG(1) << "readCatalog for local.startup_log error occur. ErrorCode: "
//                              << errorCode;
//             }
//             if (exists) {
//                 txservice::AbortTx(txm);
//                 MONGO_LOG(1) << "local.startup_log exists. Bootstrap success. Exiting program.";
//                 std::exit(0); // Exit program on successful bootstrap
//             } else {
//                 MONGO_LOG(1) << "local.startup_log not exists. Wait for bootstrap.";
//             }

//             MONGO_LOG(0) << "Retry count: " << i;
//             std::this_thread::sleep_for(std::chrono::milliseconds{50});
//             catalogRecord.Reset();
//         }
//         error() << "Retry too many times. Waiting for boostrap fail. Exiting program.";
//         std::exit(-1); // Exit program on bootstrap failure
//     });
//     bootstrapThread.detach(); // Detach the thread to run in the background
//     MONGO_LOG(1) << "EloqKVEngine::waitBootstrap";
//     std::this_thread::sleep_for(std::chrono::milliseconds{500});
//     auto txm = txservice::NewTxInit(_txService.get(),
//                                     txservice::IsolationLevel::ReadCommitted,
//                                     txservice::CcProtocol::OCC,
//                                     UINT32_MAX,
//                                     -1);
//     txservice::TableName tableName{std::string{"local.startup_log"},
//                                    txservice::TableType::Primary,
//                                    txservice::TableEngine::EloqDoc};
//     txservice::CatalogKey catalogKey{tableName};
//     txservice::CatalogRecord catalogRecord;
//     bool exists{false};

//     for (uint16_t i = 0; i < 1000; ++i) {
//         txservice::TxKey catalogTxKey(&catalogKey);
//         txservice::ReadTxRequest readTxReq(&txservice::catalog_ccm_name,
//                                            0,
//                                            &catalogTxKey,
//                                            &catalogRecord,
//                                            false,
//                                            false,
//                                            true,
//                                            0,
//                                            false,
//                                            false);

//         auto errorCode = txservice::TxReadCatalog(txm, readTxReq, exists);
//         if (errorCode != txservice::TxErrorCode::NO_ERROR) {
//             MONGO_LOG(1) << "readCatalog for local.startup_log error occur. ErrorCode: "
//                          << errorCode;
//         }
//         if (exists) {
//             txservice::AbortTx(txm);
//             MONGO_LOG(1) << "local.startup_log exists. Bootstrap success.";
//             return;
//         } else {
//             MONGO_LOG(1) << "local.startup_log not exists. Wait for bootstrap.";
//         }

//         MONGO_LOG(0) << "Retry count: " << i;
//         std::this_thread::sleep_for(std::chrono::milliseconds{50});
//         catalogRecord.Reset();
//     }
//     error() << "Retry too many times. Waiting for boostrap fail.";
//     uasserted(ErrorCodes::InternalError, "Retry too many times. Waiting for boostrap fail.");
// }

bool EloqKVEngine::supportsRecoveryTimestamp() const {
    return false;
}

boost::optional<Timestamp> EloqKVEngine::getRecoveryTimestamp() const {
    assert(!supportsRecoveryTimestamp());
    return boost::none;
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
    // bool success = Eloq::storeHandler->DiscoverAllTableNames(tables);
    bool success = Eloq::GetAllTables(tables);
    if (!success) {
        error() << "Failed to discover table names.";
        uassertStatusOK(Status{ErrorCodes::InternalError, "Failed to discover collection names."});
        return;
    }

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
    // Eloq::storeHandler->DiscoverAllTableNames(tables);
    bool success = Eloq::GetAllTables(tables);

    if (!success) {
        error() << "Failed to discover collection names.";
        uassertStatusOK(Status{ErrorCodes::InternalError, "Failed to discover collection names."});
    }
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
    // Eloq::storeHandler->DiscoverAllTableNames(allCollections);
    bool success = Eloq::GetAllTables(allCollections);

    if (!success) {
        error() << "Failed to discover collection names.";
        uassertStatusOK(Status{ErrorCodes::InternalError, "Failed to discover collection names."});
    }
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
    // Eloq::storeHandler->DiscoverAllTableNames(allCollections);
    bool success = Eloq::GetAllTables(allCollections);

    if (!success) {
        error() << "Failed to discover collection names.";
        uassertStatusOK(Status{ErrorCodes::InternalError, "Failed to discover collection names."});
    }
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

Status EloqKVEngine::lockCollection(
    OperationContext* opCtx, StringData ns, bool isForWrite, bool* exists, std::string* version) {
    MONGO_LOG(1) << "EloqKVEngine::lockCollection"
                 << ". ns: " << ns << ", isForWrite: " << isForWrite;
    auto ru = EloqRecoveryUnit::get(opCtx);
    txservice::TableName tableName = Eloq::MongoTableToTxServiceTableName(ns.toStringView(), false);

    // lockCollection bypass read from discovered table map. DatabaseImpl::getCollection() will
    // rebuild Collection handler/cache if version changed.
    txservice::CatalogKey catalogKey{tableName};
    txservice::CatalogRecord catalogRecord;
    auto [found, err] = ru->readCatalog(catalogKey, catalogRecord, isForWrite);
    if (err != txservice::TxErrorCode::NO_ERROR) {
        MONGO_LOG(1) << "ReadCatalog Error. [ErrorCode]: " << err << ". " << tableName.StringView();
        return TxErrorCodeToMongoStatus(err);
    }

    if (found) {
        *exists = true;
        *version = catalogRecord.Schema()->VersionStringView();

        auto schema =
            std::static_pointer_cast<const Eloq::MongoTableSchema>(catalogRecord.CopySchema());
        auto dirtySchema =
            std::static_pointer_cast<const Eloq::MongoTableSchema>(catalogRecord.CopyDirtySchema());
        ru->tryInsertDiscoveredTable(tableName, std::move(schema), std::move(dirtySchema));
    } else {
        *exists = false;
    }
    return Status::OK();
}

void EloqKVEngine::onAuthzDataChanged(OperationContext* opCtx) {
    MONGO_LOG(1) << "EloqKVEngine::onAuthzDataChanged";
    auto ru = EloqRecoveryUnit::get(opCtx);
    ru->notifyReloadCache(opCtx);
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

    auto recordStore = std::make_unique<EloqRecordStore>(opCtx, params);
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
    // Eloq::storeHandler->DiscoverAllTableNames(tableNameVector);
    bool success = Eloq::GetAllTables(tableNameVector);

    if (!success) {
        error() << "Failed to discover collection names.";
        uassertStatusOK(Status{ErrorCodes::InternalError, "Failed to discover collection names."});
    }

    auto ru = EloqRecoveryUnit::get(opCtx);

    for (auto& nameString : tableNameVector) {
        if (isMongoCatalog(nameString)) {
            continue;
        }
        txservice::TableName tableName{
            std::move(nameString), txservice::TableType::Primary, txservice::TableEngine::EloqDoc};
        txservice::CatalogKey catalogKey{tableName};
        txservice::CatalogRecord catalogRecord;
        auto [exists, errorCode] = ru->readCatalog(catalogKey, catalogRecord, false);
        // todo: Does exists==false valid? When does Mongo do full table scan on Catalog?
        assert(exists);

        std::string metadata, kvInfo, keySchemaTsString;
        EloqDS::DeserializeSchemaImage(
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

    _txService->Shutdown();
    Eloq::storeHandler.reset();
    Eloq::dataStoreService.reset();

#if defined(DATA_STORE_TYPE_DYNAMODB) ||                                      \
    (defined(USE_ROCKSDB_LOG_STATE) && (WITH_ROCKSDB_CLOUD == CS_TYPE_S3)) || \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
    aws_deinit();
#endif

    _txService.reset();
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

MongoSystemHandler::MongoSystemHandler() {
    thd_ = std::thread([this]() {
        while (shutdown_.load(std::memory_order_acquire) == false) {
            std::unique_lock lk(mux_);
            cv_.wait(lk, [this]() {
                return !work_queue_.empty() || shutdown_.load(std::memory_order_acquire);
            });

            if (!work_queue_.empty()) {
                std::packaged_task<bool()> work = std::move(work_queue_.front());
                work_queue_.pop_front();
                lk.unlock();
                work();
            }
        }
    });
}

MongoSystemHandler::~MongoSystemHandler() {
    std::unique_lock<std::mutex> lk(mux_);
    bool status = false;
    if (shutdown_.compare_exchange_strong(status, true, std::memory_order_relaxed)) {
        cv_.notify_one();
        lk.unlock();
        thd_.join();
    }
}

void MongoSystemHandler::ReloadCache(std::function<void(bool)> done) {
    std::packaged_task<bool()> work([done = std::move(done)]() {
        mongo::Status status = mongo::Status::OK();

        auto serviceContext = mongo::getGlobalServiceContext();
        auto client = mongo::getGlobalServiceContext()->makeClient("eloq_table_schema");
        auto opCtx = serviceContext->makeOperationContext(client.get());
        auto const globalAuthzManager = mongo::AuthorizationManager::get(serviceContext);

        for (int i = 0; i < 5; i++) {
            status = globalAuthzManager->initialize(opCtx.get());
            if (status.isOK()) {
                break;
            }
        }

        if (!status.isOK()) {
            mongo::error() << "reload_acl_and_cache failed";
        }
        done(status.isOK());
        return status.isOK();
    });

    SubmitWork(std::move(work));
}

void MongoSystemHandler::SubmitWork(std::packaged_task<bool()> work) {
    std::unique_lock<std::mutex> lk(mux_);
    if (shutdown_.load(std::memory_order_acquire) == false) {
        work_queue_.push_back(std::move(work));
        cv_.notify_one();
    }
}

}  // namespace mongo
