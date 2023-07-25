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

#include "mongo/db/modules/eloq/src/base/eloq_record.h"

namespace Eloq {

mongo::RecordId MongoRecord::ToRecordId(bool is_long) const {
    MONGO_LOG(1) << "MongoRecord::ToRecordId";
    if (is_long) {
        assert(false);
        invariant(EncodedBlobSize() == sizeof(int64_t));
        int64_t recordId =
            mongo::endian::bigToNative(*reinterpret_cast<const int64_t*>(EncodedBlobData()));
        return mongo::RecordId(recordId);
    } else {
        return mongo::RecordId(EncodedBlobData(), EncodedBlobSize());
    }
}

}  // namespace Eloq