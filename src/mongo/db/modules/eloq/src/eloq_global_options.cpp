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

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/modules/eloq/src/eloq_global_options.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/constraints.h"
#include "mongo/util/options_parser/option_description.h"

namespace mongo {
EloqGlobalOptions eloqGlobalOptions;

Status EloqGlobalOptions::add(moe::OptionSection* options) {
    moe::OptionSection eloqOptions("Eloq options");

    // Eloq TxService Options
    eloqOptions
        .addOptionChaining("storage.eloq.txService.localIP",
                           "eloqLocalIP",
                           moe::String,
                           "IP address of the local node")
        .setDefault(moe::Value("127.0.0.1:8000"));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.ipList",
                           "eloqIPList",
                           moe::StringVector,
                           "IP addresses of the nodes in the cluster")
        .setDefault(moe::Value(std::vector<std::string>(1, "127.0.0.1:8000")));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.txlogServiceList",
                           "eloqTxlogServiceList",
                           moe::StringVector,
                           "IP address of the tx log service node")
        .setDefault(moe::Value(std::vector<std::string>()));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.hmIP",
                           "eloqHMIP",
                           moe::String,
                           "IP addresses of the host manager")
        .setDefault(moe::Value(""));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.hmBinPath",
                           "eloqHMBinPath",
                           moe::String,
                           "Path to host manager binary path.")
        .setDefault(moe::Value(""));
    eloqOptions
        .addOptionChaining(
            "storage.eloq.txService.coreNum", "eloqCoreNum", moe::Int, "Number of CPU cores")
        .validRange(1, 1024)
        .setDefault(moe::Value(1));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.rangeSplitWorkerNum",
                           "eloqRangeSplitWorkerNum",
                           moe::Int,
                           "Number of range split worker")
        .validRange(1, 1024)
        .setDefault(moe::Value(1));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.nodeMemoryLimitMB",
                           "eloqNodeMemoryLimitMB",
                           moe::Int,
                           "memory limit per node (MB)")
        .validRange(1, 1000000)
        .setDefault(moe::Value(8000));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.nodeLogLimitMB",
                           "eloqNodeLogLimitMB",
                           moe::Int,
                           "log limit per node (MB)")
        .validRange(1, 1000000)
        .setDefault(moe::Value(16000));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.checkpointerIntervalSec",
                           "eloqCheckpointerIntervalSec",
                           moe::Int,
                           "Interval of checkpointer(s)")
        .validRange(1, 86400)
        .setDefault(moe::Value(1000));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.checkpointerDelaySec",
                           "eloqCheckpointerDelaySec",
                           moe::Int,
                           "The time(second) which ckpt_ts is less than min lock ts")
        .validRange(0, 86400)
        .setDefault(moe::Value(5));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.collectActiveTxTsIntervalSec",
                           "eloqCollectActiveTxTsIntervalSec",
                           moe::Int,
                           "Interval of collect active tx start timestamp(s)")
        .validRange(1, 86400)
        .setDefault(moe::Value(2));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.deadlockIntervalSec",
                           "eloqDeadlockIntervalSec",
                           moe::Int,
                           "Interval of dead lock check(s)")
        .validRange(1, 3600)
        .setDefault(moe::Value(300));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.txlogGroupReplicaNum",
                           "eloq.TxlogGroupReplicaNum",
                           moe::Int,
                           "Replicate number of tx log group")
        .validRange(1, 10)
        .setDefault(moe::Value(3));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.useKeyCache",
                           "eloqUseKeyCache",
                           moe::Bool,
                           "Use key cache in primary key to avoid kv read if key does not exists.")
        .setDefault(moe::Value(false));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.enableMVCC",
                           "eloqEnableMVCC",
                           moe::Bool,
                           "When enabled, use muliti-versions. Repeatable Read isolation level "
                           "will be converted to "
                           "Snapshot isolation level")
        .setDefault(moe::Value(true));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.skipRedoLog",
                           "eloqSkipRedoLog",
                           moe::Bool,
                           "Skip write redo log in tx_service")
        .setDefault(moe::Value(false));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.realtimeSampling",
                           "eloqRealtimeSampling",
                           moe::Bool,
                           "Whether enable realtime sampling. If disable it, user may need execute "
                           "analyze command at some time.")
        .setDefault(moe::Value(false));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.enableHeapDefragment",
                           "eloqEnableHeapDefragment",
                           moe::Bool,
                           "Enable heap defragment.")
        .setDefault(moe::Value(false));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.nodeGroupReplicaNum",
                           "eloqNodeGroupReplicaNum",
                           moe::Int,
                           "Replicate number of node group(Max: 9)")
        .setDefault(moe::Value(3));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.bthreadWorkerNum",
                           "eloqNodeBthreadWorkerNum",
                           moe::Int,
                           "Number of bthread worker threads")
        .setDefault(moe::Value(0));
    eloqOptions
        .addOptionChaining("storage.eloq.txService.logserverRocksDBScanThreadNum",
                           "eloqLogServerRocksDBScanThreadNum",
                           moe::Int,
                           "Number of rocksdb scan threads")
        .setDefault(moe::Value(1));

    // Eloq Storage Options
    eloqOptions
        .addOptionChaining("storage.eloq.storage.keyspaceName",
                           "eloqKeyspaceName",
                           moe::String,
                           "Keyspace of KV Storage")
        .setDefault(moe::Value("mono_mongo"));
    eloqOptions
        .addOptionChaining("storage.eloq.storage.cassHosts",
                           "eloqCassHosts",
                           moe::String,
                           "Contact points of Cassandra")
        .setDefault(moe::Value("127.0.0.1"));
    eloqOptions
        .addOptionChaining(
            "storage.eloq.storage.cassPort", "eloqCassPort", moe::Int, "Port of Cassandra")
        .validRange(1, UINT16_MAX)
        .setDefault(moe::Value(9042));
    eloqOptions
        .addOptionChaining("storage.eloq.storage.cassQueueSizeIO",
                           "eloqCassQueueSizeIO",
                           moe::Int,
                           "Queue_size_io of Cassandra client")
        .validRange(0, INT_MAX)
        .setDefault(moe::Value(300000));
    eloqOptions
        .addOptionChaining("storage.eloq.storage.cassKeyspaceClass",
                           "eloqCassKeyspaceClass",
                           moe::String,
                           "Keyspace class of Cassandra")
        .setDefault(moe::Value("SimpleStrategy"));
    eloqOptions
        .addOptionChaining("storage.eloq.storage.cassReplicationFactor",
                           "eloqCassReplicationFactor",
                           moe::String,
                           "Keyspace replication factor of Cassandra")
        .setDefault(moe::Value("1"));
    eloqOptions
        .addOptionChaining("storage.eloq.storage.cassHighCompressionRatio",
                           "eloqCassHighCompressionRatio",
                           moe::Bool,
                           "Cassandra enable high compression ratio")
        .setDefault(moe::Value(false));
    eloqOptions
        .addOptionChaining(
            "storage.eloq.storage.cassUser", "eloqCassUser", moe::String, "Cassandra username")
        .setDefault(moe::Value("cassandra"));
    eloqOptions
        .addOptionChaining("storage.eloq.storage.cassPassword",
                           "eloqCassPassword",
                           moe::String,
                           "Cassandra password")
        .setDefault(moe::Value("cassandra"));

    return options->addSection(eloqOptions);
}

Status EloqGlobalOptions::store(const moe::Environment& params,
                                const std::vector<std::string>& args) {
    // Eloq TxService Options
    if (params.count("storage.eloq.txService.localIP")) {
        auto localIP = params["storage.eloq.txService.localIP"].as<std::string>();
        if (localIP.find_first_of(':') == std::string::npos) {
            localIP.append(":8000");
        }
        auto hostAndPort = mongo::HostAndPort::parse(localIP);
        if (hostAndPort.isOK()) {
            invariant(hostAndPort.getValue().hasPort());
            eloqGlobalOptions.localAddr = std::move(hostAndPort.getValue());
        } else {
            return hostAndPort.getStatus();
        }
    }
    if (params.count("storage.eloq.txService.ipList")) {
        auto ipList = params["storage.eloq.txService.ipList"].as<std::vector<std::string>>();
        for (std::string& ip : ipList) {
            if (ip.find_first_of(':') == std::string::npos) {
                ip.append(":8000");
            }
            auto hostAndPort = mongo::HostAndPort::parse(ip);
            if (hostAndPort.isOK()) {
                invariant(hostAndPort.getValue().hasPort());
                eloqGlobalOptions.nodeGroupAddrs.push_back(std::move(hostAndPort.getValue()));
            } else {
                return hostAndPort.getStatus();
            }
        }
    }
    if (params.count("storage.eloq.txService.txlogServiceList")) {
        auto txlogServiceList =
            params["storage.eloq.txService.txlogServiceList"].as<std::vector<std::string>>();
        for (std::string& txlog : txlogServiceList) {
            auto hostAndPort = mongo::HostAndPort::parse(txlog);
            if (hostAndPort.isOK()) {
                if (hostAndPort.getValue().hasPort()) {
                    eloqGlobalOptions.txlogServiceAddrs.push_back(
                        std::move(hostAndPort.getValue()));
                } else {
                    return Status{ErrorCodes::InvalidOptions,
                                  str::stream() << txlog << " does not include a port"};
                }
            } else {
                return hostAndPort.getStatus();
            }
        }
    }
#ifdef FORK_HM_PROCESS
    if (params.count("storage.eloq.txService.hmIP")) {
        auto hmIP = params["storage.eloq.txService.hmIP"].as<std::string>();

        auto hmAddr = [&hmIP]() -> StatusWith<mongo::HostAndPort> {
            if (hmIP.find_first_of(':') != std::string::npos) {
                return mongo::HostAndPort::parse(hmIP);
            } else {
                return mongo::HostAndPort(eloqGlobalOptions.localAddr.host(),
                                          eloqGlobalOptions.localAddr.port() + 4);
            }
        }();

        if (hmAddr.isOK()) {
            invariant(hmAddr.getValue().hasPort());
            eloqGlobalOptions.hostManagerAddr = std::move(hmAddr.getValue());
        } else {
            return hmAddr.getStatus();
        }
    }
    if (params.count("storage.eloq.txService.hmBinPath")) {

        auto hmBinPath = params["storage.eloq.txService.hmBinPath"].as<std::string>();
        if (hmBinPath.empty()) {
            char pathBuf[PATH_MAX];
            ssize_t len = ::readlink("/proc/self/exe", pathBuf, sizeof(pathBuf));
            len -= strlen("/mongod");
            pathBuf[len] = '\0';
            hmBinPath = std::string(pathBuf, len);
            hmBinPath.append("/host_manager");
        }

        eloqGlobalOptions.hostManagerBinPath = std::move(hmBinPath);
    }
#endif
    if (params.count("storage.eloq.txService.coreNum")) {
        eloqGlobalOptions.coreNum = params["storage.eloq.txService.coreNum"].as<int>();
    }
    if (params.count("storage.eloq.txService.rangeSplitWorkerNum")) {
        eloqGlobalOptions.rangeSplitWorkerNum =
            params["storage.eloq.txService.rangeSplitWorkerNum"].as<int>();
    }
    if (params.count("storage.eloq.txService.nodeMemoryLimitMB")) {
        eloqGlobalOptions.nodeMemoryLimitMB =
            params["storage.eloq.txService.nodeMemoryLimitMB"].as<int>();
    }
    if (params.count("storage.eloq.txService.nodeLogLimitMB")) {
        eloqGlobalOptions.nodeLogLimitMB =
            params["storage.eloq.txService.nodeLogLimitMB"].as<int>();
    }
    if (params.count("storage.eloq.txService.checkpointerIntervalSec")) {
        eloqGlobalOptions.checkpointerIntervalSec =
            params["storage.eloq.txService.checkpointerIntervalSec"].as<int>();
    }
    if (params.count("storage.eloq.txService.checkpointerDelaySec")) {
        eloqGlobalOptions.checkpointerDelaySec =
            params["storage.eloq.txService.checkpointerDelaySec"].as<int>();
    }
    if (params.count("storage.eloq.txService.collectActiveTxTsIntervalSec")) {
        eloqGlobalOptions.collectActiveTxTsIntervalSec =
            params["storage.eloq.txService.collectActiveTxTsIntervalSec"].as<int>();
    }
    if (params.count("storage.eloq.txService.deadlockIntervalSec")) {
        eloqGlobalOptions.deadlockIntervalSec =
            params["storage.eloq.txService.deadlockIntervalSec"].as<int>();
    }
    if (params.count("storage.eloq.txService.txlogGroupReplicaNum")) {
        eloqGlobalOptions.txlogGroupReplicaNum =
            params["storage.eloq.txService.txlogGroupReplicaNum"].as<int>();
    }
    if (params.count("storage.eloq.txService.useKeyCache")) {
        eloqGlobalOptions.useKeyCache = params["storage.eloq.txService.useKeyCache"].as<bool>();
    }
    if (params.count("storage.eloq.txService.enableMVCC")) {
        eloqGlobalOptions.enableMVCC = params["storage.eloq.txService.enableMVCC"].as<bool>();
    }
    if (params.count("storage.eloq.txService.skipRedoLog")) {
        eloqGlobalOptions.skipRedoLog = params["storage.eloq.txService.skipRedoLog"].as<bool>();
    }
    if (params.count("storage.eloq.txService.realtimeSampling")) {
        eloqGlobalOptions.realtimeSampling =
            params["storage.eloq.txService.realtimeSampling"].as<bool>();
    }
    if (params.count("storage.eloq.txService.enableHeapDefragment")) {
        eloqGlobalOptions.enableHeapDefragment =
            params["storage.eloq.txService.enableHeapDefragment"].as<bool>();
    }
    if (params.count("storage.eloq.txService.nodeGroupReplicaNum")) {
        eloqGlobalOptions.nodeGroupReplicaNum =
            params["storage.eloq.txService.nodeGroupReplicaNum"].as<int>();
    }
    if (params.count("storage.eloq.txService.bthreadWorkerNum")) {
        eloqGlobalOptions.bthreadWorkerNum =
            params["storage.eloq.txService.bthreadWorkerNum"].as<int>();
    }
    if (params.count("storage.eloq.txService.logserverRocksDBScanThreadNum")) {
        eloqGlobalOptions.logserverRocksDBScanThreadNum =
            params["storage.eloq.txService.logserverRocksDBScanThreadNum"].as<int>();
    }

    // Eloq Storage Options
    if (params.count("storage.eloq.storage.keyspaceName")) {
        eloqGlobalOptions.keyspaceName =
            params["storage.eloq.storage.keyspaceName"].as<std::string>();
    }
    if (params.count("storage.eloq.storage.cassHosts")) {
        eloqGlobalOptions.cassHosts = params["storage.eloq.storage.cassHosts"].as<std::string>();
    }
    if (params.count("storage.eloq.storage.cassPort")) {
        eloqGlobalOptions.cassPort = params["storage.eloq.storage.cassPort"].as<int>();
    }
    if (params.count("storage.eloq.storage.cassQueueSizeIO")) {
        eloqGlobalOptions.cassQueueSizeIO =
            params["storage.eloq.storage.cassQueueSizeIO"].as<int>();
    }
    if (params.count("storage.eloq.storage.cassKeyspaceClass")) {
        eloqGlobalOptions.cassKeyspaceClass =
            params["storage.eloq.storage.cassKeyspaceClass"].as<std::string>();
    }
    if (params.count("storage.eloq.storage.cassReplicationFactor")) {
        eloqGlobalOptions.cassReplicationFactor =
            params["storage.eloq.storage.cassReplicationFactor"].as<std::string>();
    }
    if (params.count("storage.eloq.storage.cassHighCompressionRatio")) {
        eloqGlobalOptions.cassHighCompressionRatio =
            params["storage.eloq.storage.cassHighCompressionRatio"].as<bool>();
    }
    if (params.count("storage.eloq.storage.cassUser")) {
        eloqGlobalOptions.cassUser = params["storage.eloq.storage.cassUser"].as<std::string>();
    }
    if (params.count("storage.eloq.storage.cassPassword")) {
        eloqGlobalOptions.cassPassword =
            params["storage.eloq.storage.cassPassword"].as<std::string>();
    }

    return Status::OK();
}

std::vector<mongo::HostAndPort> EloqGlobalOptions::NodeGroupAddrs() const {
    std::vector<mongo::HostAndPort> addrs;
    addrs.reserve(nodeGroupAddrs.size());
    for (const mongo::HostAndPort& addr : nodeGroupAddrs) {
        addrs.push_back(addr);
    }
    return addrs;
}

std::vector<std::string> EloqGlobalOptions::TxlogIPs() const {
    std::vector<std::string> ips;
    ips.reserve(txlogServiceAddrs.size());
    for (const mongo::HostAndPort& addr : txlogServiceAddrs) {
        ips.push_back(addr.host());
    }
    return ips;
}

std::vector<uint16_t> EloqGlobalOptions::TxlogPorts() const {
    std::vector<uint16_t> ports;
    for (const mongo::HostAndPort& addr : txlogServiceAddrs) {
        invariant(addr.hasPort());
        ports.push_back(addr.port());
    }
    return ports;
}

}  // namespace mongo
