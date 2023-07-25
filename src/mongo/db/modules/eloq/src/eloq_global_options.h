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

#include "mongo/util/net/hostandport.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include <cstdint>

namespace mongo {

namespace moe = mongo::optionenvironment;

class EloqGlobalOptions {
public:
    EloqGlobalOptions() = default;
    Status add(moe::OptionSection* options);
    Status store(const moe::Environment& params, const std::vector<std::string>& args);

    std::vector<mongo::HostAndPort> NodeGroupAddrs() const;

    std::vector<std::string> TxlogIPs() const;
    std::vector<uint16_t> TxlogPorts() const;

    // tx_service
    mongo::HostAndPort localAddr;
    std::vector<mongo::HostAndPort> nodeGroupAddrs;
    std::vector<mongo::HostAndPort> txlogServiceAddrs;
    mongo::HostAndPort hostManagerAddr;
    std::string hostManagerBinPath;
    uint16_t coreNum{0};
    uint16_t rangeSplitWorkerNum{0};
    uint32_t nodeMemoryLimitMB{0};
    uint32_t nodeLogLimitMB{0};
    uint32_t checkpointerIntervalSec{0};
    uint32_t checkpointerDelaySec{0};
    uint32_t collectActiveTxTsIntervalSec{0};
    uint32_t deadlockIntervalSec{0};
    uint32_t txlogGroupReplicaNum{0};
    uint32_t nodeGroupReplicaNum{0};
    uint16_t bthreadWorkerNum{0};
    uint16_t logserverRocksDBScanThreadNum{0};
    bool useKeyCache{false};
    bool enableMVCC{false};
    bool skipRedoLog{false};
    bool realtimeSampling{false};
    bool enableHeapDefragment{false};

    // storage
    std::string keyspaceName;

    std::string cassHosts;
    uint16_t cassPort{0};
    uint32_t cassQueueSizeIO{0};
    std::string cassKeyspaceClass;
    std::string cassReplicationFactor;
    bool cassHighCompressionRatio{false};
    std::string cassUser;
    std::string cassPassword;
};

extern EloqGlobalOptions eloqGlobalOptions;
}  // namespace mongo
