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

#include "mongo/db/modules/eloq/src/eloq_cursor.h"
#include "mongo/db/modules/eloq/src/eloq_recovery_unit.h"

#include "mongo/db/modules/eloq/tx_service/include/tx_execution.h"
#include "mongo/db/modules/eloq/tx_service/include/tx_record.h"

namespace mongo {
EloqCursor::EloqCursor(OperationContext* opCtx) : _opCtx(opCtx), _ru(EloqRecoveryUnit::get(opCtx)) {
    MONGO_LOG(1) << "EloqCursor::EloqCursor";
}

EloqCursor::~EloqCursor() {
    MONGO_LOG(1) << "EloqCursor::~EloqCursor"
                 << " .scanIsOpen: " << indexScanIsOpen();

    // automatically close index scan
    if (indexScanIsOpen()) {
        indexScanClose();
        _ru->unregisterCursor(this);
    }
}

bool EloqCursor::indexScanIsOpen() const {
    return _scanAlias < UINT64_MAX;
}

void EloqCursor::indexScanOpen(const txservice::TableName* tableName,
                               uint64_t keySchemaVersion,
                               txservice::ScanIndexType index_type,
                               const txservice::TxKey* start_key,
                               bool start_inclusive,
                               const txservice::TxKey* end_key,
                               bool end_inclusive,
                               txservice::ScanDirection direction) {
    MONGO_LOG(1) << "EloqCursor::indexScanOpen";
    _txm = _ru->getTxm();
    auto [yieldFunc, resumeFunc] = _opCtx->getCoroutineFunctors();

    bool is_ckpt = false;
    bool is_for_write = false;
    bool is_for_share = false;
    bool is_covering_keys = false;
    bool is_require_keys = true;
    bool is_require_recs = true;
    bool is_require_sort = true;
    bool is_read_local = false;

    _scanOpenTxReq.Reset(tableName,
                         keySchemaVersion,
                         index_type,
                         start_key,
                         start_inclusive,
                         end_key,
                         end_inclusive,
                         direction,
                         is_ckpt,
                         is_for_write,
                         is_for_share,
                         is_covering_keys,
                         is_require_keys,
                         is_require_recs,
                         is_require_sort,
                         is_read_local,
                         yieldFunc,
                         resumeFunc,
                         _txm);
    MONGO_LOG(1) << "table_name: " << _scanOpenTxReq.tab_name_->StringView()
                 << ". start_key: " << _scanOpenTxReq.start_key_->ToString()
                 << ". start_inclusive: " << _scanOpenTxReq.start_inclusive_
                 << ". end_key: " << _scanOpenTxReq.end_key_->ToString()
                 << ". end_inclusive: " << _scanOpenTxReq.end_inclusive_ << ". direction"
                 << _scanOpenTxReq.direct_;
    _ru->registerCursor(this);
    _txm = _ru->getTxm();
    _scanAlias = _txm->OpenTxScan(_scanOpenTxReq);
    assert(_scanAlias != UINT64_MAX);
    _isLastScanBatch = false;
    _scanBatchIdx = UINT64_MAX;
    _scanBatchVector.clear();
}

void EloqCursor::indexScanClose() {
    MONGO_LOG(1) << "EloqCursor::indexScanClose";

    std::vector<txservice::UnlockTuple> unlockBatch;
    if (_scanBatchIdx == UINT64_MAX) {
        unlockBatch.reserve(_scanBatchVector.size());
    } else {
        unlockBatch.reserve(_scanBatchVector.size() - _scanBatchIdx);
    }

    for (size_t idx = _scanBatchIdx; idx < _scanBatchVector.size(); ++idx) {
        const txservice::ScanBatchTuple& tuple = _scanBatchVector[idx];
        unlockBatch.emplace_back(tuple.cce_addr_, tuple.version_ts_, tuple.status_);
    }
    _txm->CloseTxScan(_scanAlias, *_scanOpenTxReq.tab_name_, unlockBatch);

    if (!_scanOpenTxReq.IsFinished()) {
        _scanOpenTxReq.Wait();
    }
    _scanAlias = UINT64_MAX;
    _isLastScanBatch = false;
    _scanBatchIdx = UINT64_MAX;
    _scanBatchVector.clear();
}

const Eloq::MongoKey* EloqCursor::currentBatchKey() const {
    return (_currentBatchTuple == nullptr) ? nullptr
                                           : _currentBatchTuple->key_.GetKey<Eloq::MongoKey>();
}

const txservice::ScanBatchTuple* EloqCursor::nextBatchTuple() {
    MONGO_LOG(1) << "EloqCursor::nextBatchTuple"
                 << ". _scanBatchIdx: " << _scanBatchIdx
                 << ". _isLastScanBatch: " << _isLastScanBatch
                 << ". _scanBatchVector.size(): " << _scanBatchVector.size();

    for (_currentBatchTuple = nullptr;
         !_currentBatchTuple || _currentBatchTuple->status_ != txservice::RecordStatus::Normal;) {
        // move iterator
        if (_scanBatchIdx < _scanBatchVector.size()) {
            _currentBatchTuple = &_scanBatchVector[_scanBatchIdx++];
            continue;
        }

        // no more data
        if (_isLastScanBatch) {
            return nullptr;
        }

        if (_fetchBatchTuples()) {
            _currentBatchTuple = &_scanBatchVector[_scanBatchIdx++];
            continue;
        } else {
            // reach the end
            assert(_isLastScanBatch);
            return nullptr;
        }
    }

    return _currentBatchTuple;
}

bool EloqCursor::_fetchBatchTuples() {
    MONGO_LOG(1) << "EloqCursor::fetchBatchTuples";
    _scanBatchIdx = 0;
    _scanBatchVector.clear();
    auto [yieldFunc, resumeFunc] = _opCtx->getCoroutineFunctors();
    txservice::ScanBatchTxRequest scanBatchTxReq(
        _scanAlias, *_scanOpenTxReq.tab_name_, &_scanBatchVector, yieldFunc, resumeFunc, _txm);
    _txm->Execute(&scanBatchTxReq);
    scanBatchTxReq.Wait();
    if (scanBatchTxReq.IsError()) {
        MONGO_LOG(1) << "EloqCursor::nextBatchTuple ScanBatchTxRequest fail"
                     << ". ErrorCode: " << scanBatchTxReq.ErrorCode()
                     << ". ErrorMsg: " << scanBatchTxReq.ErrorMsg();
    }
    _isLastScanBatch = scanBatchTxReq.Result();

    return !_scanBatchVector.empty();
}
}  // namespace mongo
