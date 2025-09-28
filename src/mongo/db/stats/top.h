// top.h : DB usage monitor.

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <mutex>
#include <vector>

#include "mongo/db/server_options.h"
#include <boost/date_time/posix_time/posix_time.hpp>

#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/operation_latency_histogram.h"
#include "mongo/util/string_map.h"

namespace mongo {

class ServiceContext;

/**
 * tracks usage by collection
 */
class Top {
public:
    static Top& get(ServiceContext* service);

    Top() = default;

    struct UsageData {
        UsageData() : time(0), count(0) {}
        UsageData(const UsageData& older, const UsageData& newer);
        void inc(long long micros) {
            count++;
            time += micros;
        }

        void operator+=(const UsageData& other) {
            time += other.time;
            count += other.count;
        }

        long long time;
        long long count;
    };

    struct CollectionData {
        /**
         * constructs a diff
         */
        CollectionData() {}
        CollectionData(const CollectionData& older, const CollectionData& newer);

        void operator+=(const CollectionData& other) {
            total += other.total;
            readLock += other.readLock;
            writeLock += other.writeLock;
            queries += other.queries;
            getmore += other.getmore;
            insert += other.insert;
            update += other.update;
            remove += other.remove;
            commands += other.commands;
            opLatencyHistogram += other.opLatencyHistogram;
        }

        UsageData total;

        UsageData readLock;
        UsageData writeLock;

        UsageData queries;
        UsageData getmore;
        UsageData insert;
        UsageData update;
        UsageData remove;
        UsageData commands;
        OperationLatencyHistogram opLatencyHistogram;
    };

    enum class LockType {
        ReadLocked,
        WriteLocked,
        NotLocked,
    };

    // typedef StringMap<CollectionData> UsageMap;
    using UsageMap = StringMap<CollectionData>;


public:
    void record(OperationContext* opCtx,
                StringData ns,
                LogicalOp logicalOp,
                LockType lockType,
                long long micros,
                bool command,
                Command::ReadWriteType readWriteType);

    void append(BSONObjBuilder& b);

    void cloneMap(UsageMap& out) const;

    void collectionDropped(StringData ns, bool databaseDropped = false);

    /**
     * Appends the collection-level latency statistics
     */
    void appendLatencyStats(StringData ns, bool includeHistograms, BSONObjBuilder* builder);

    /**
     * Increments the global histogram only if the operation came from a user.
     */
    void incrementGlobalLatencyStats(OperationContext* opCtx,
                                     uint64_t latency,
                                     Command::ReadWriteType readWriteType);

    /**
     * Increments the global transactions histogram.
     */
    void incrementGlobalTransactionLatencyStats(uint64_t latency);

    /**
     * Appends the global latency statistics.
     */
    void appendGlobalLatencyStats(bool includeHistograms, BSONObjBuilder* builder);

private:
    void _appendToUsageMap(BSONObjBuilder& b, const UsageMap& map) const;

    void _appendStatsEntry(BSONObjBuilder& b, const char* statsName, const UsageData& map) const;

    void _record(OperationContext* opCtx,
                 CollectionData& c,
                 LogicalOp logicalOp,
                 LockType lockType,
                 uint64_t micros,
                 Command::ReadWriteType readWriteType);

    void _incrementHistogram(OperationContext* opCtx,
                             uint64_t latency,
                             OperationLatencyHistogram* histogram,
                             Command::ReadWriteType readWriteType);

    UsageMap _mergeUsageVector();

    std::vector<std::mutex> _histogramMutexVector{serverGlobalParams.reservedThreadNum + 1};
    std::vector<OperationLatencyHistogram> _histogramVector{serverGlobalParams.reservedThreadNum +
                                                            1};

    std::vector<std::mutex> _usageMutexVector{serverGlobalParams.reservedThreadNum + 1};
    std::vector<UsageMap> _usageVector{serverGlobalParams.reservedThreadNum + 1};

    std::mutex _lastDroppedMutex;
    std::string _lastDropped;
};

}  // namespace mongo
