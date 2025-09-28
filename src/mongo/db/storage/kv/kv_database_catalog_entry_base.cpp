/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/util/assert_util.h"
#include <mutex>
#include <thread>
#include <utility>
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/storage/kv/kv_database_catalog_entry.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::vector;

class KVDatabaseCatalogEntryBase::AddCollectionChange : public RecoveryUnit::Change {
public:
    AddCollectionChange(OperationContext* opCtx,
                        KVDatabaseCatalogEntryBase* dce,
                        StringData collection,
                        StringData ident,
                        bool dropOnRollback)
        : _opCtx(opCtx),
          _dce(dce),
          _collection(collection.toString()),
          _ident(ident.toString()),
          _dropOnRollback(dropOnRollback) {}

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        if (_dropOnRollback) {
            // Intentionally ignoring failure
            _dce->_engine->getEngine()->dropIdent(_opCtx, _ident).transitional_ignore();
        }

        const CollectionCatalogMap::iterator it = _dce->_collections.find(_collection);
        if (it != _dce->_collections.end()) {
            // delete it->second;
            _dce->_collections.erase(it);
        }
    }

    OperationContext* const _opCtx;
    KVDatabaseCatalogEntryBase* const _dce;
    const std::string _collection;
    const std::string _ident;
    const bool _dropOnRollback;
};

class KVDatabaseCatalogEntryBase::RemoveCollectionChange : public RecoveryUnit::Change {
public:
    RemoveCollectionChange(OperationContext* opCtx,
                           KVDatabaseCatalogEntryBase* dce,
                           StringData collection,
                           StringData ident,
                           KVCollectionCatalogEntry* entry,
                           bool dropOnCommit)
        : _opCtx(opCtx),
          _dce(dce),
          _collection(collection.toString()),
          _ident(ident.toString()),
          _entry(entry),
          _dropOnCommit(dropOnCommit) {}

    virtual void commit(boost::optional<Timestamp>) {
        delete _entry;

        // Intentionally ignoring failure here. Since we've removed the metadata pointing to the
        // collection, we should never see it again anyway.
        if (_dropOnCommit)
            _dce->_engine->getEngine()->dropIdent(_opCtx, _ident).transitional_ignore();
    }

    virtual void rollback() {
        // _dce->_collections[_collection] = _entry;
    }

    OperationContext* const _opCtx;
    KVDatabaseCatalogEntryBase* const _dce;
    const std::string _collection;
    const std::string _ident;
    KVCollectionCatalogEntry* const _entry;
    const bool _dropOnCommit;
};

KVDatabaseCatalogEntryBase::KVDatabaseCatalogEntryBase(StringData db, KVStorageEngine* engine)
    : DatabaseCatalogEntry(db), _engine(engine) {}

KVDatabaseCatalogEntryBase::~KVDatabaseCatalogEntryBase() {
    // for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it)
    // {
    //     delete it->second;
    // }
    _collections.clear();
}

bool KVDatabaseCatalogEntryBase::exists() const {
    MONGO_UNREACHABLE;
    return !isEmpty();
}

bool KVDatabaseCatalogEntryBase::isEmpty() const {
    // std::scoped_lock<std::mutex> lk{_collectionsMutex};
    return _collections.empty();
}

bool KVDatabaseCatalogEntryBase::hasUserData() const {
    MONGO_UNREACHABLE;
    return !isEmpty();
}

int64_t KVDatabaseCatalogEntryBase::sizeOnDisk(OperationContext* opCtx) const {
    int64_t size = 0;

    // std::scoped_lock<std::mutex> lk{_collectionsMutex};
    for (const auto& _collection : _collections) {
        auto& coll = _collection.second;
        // if (!coll) {
        //     continue;
        // }
        size += coll->getRecordStore()->storageSize(opCtx);

        vector<string> indexNames;
        coll->getAllIndexes(opCtx, &indexNames);

        for (const auto& indexName : indexNames) {
            string ident = _engine->getCatalog()->getIndexIdent(opCtx, coll->ns().ns(), indexName);
            size += _engine->getEngine()->getIdentSize(opCtx, ident);
        }
    }

    return size;
}

void KVDatabaseCatalogEntryBase::appendExtraStats(OperationContext* opCtx,
                                                  BSONObjBuilder* out,
                                                  double scale) const {}

Status KVDatabaseCatalogEntryBase::currentFilesCompatible(OperationContext* opCtx) const {
    // Delegate to the FeatureTracker as to whether the data files are compatible or not.
    return _engine->getCatalog()->getFeatureTracker()->isCompatibleWithCurrentCode(opCtx);
}

void KVDatabaseCatalogEntryBase::getCollectionNamespaces(std::set<std::string>& out) const {
    _engine->getEngine()->listCollections(name(), out);
}

void KVDatabaseCatalogEntryBase::getCollectionNamespaces(std::vector<std::string>& out) const {
    _engine->getEngine()->listCollections(name(), out);
}

CollectionCatalogEntry* KVDatabaseCatalogEntryBase::getCollectionCatalogEntry(
    OperationContext* opCtx, StringData ns) {
    if (auto iter = _collections.find(ns.toString()); iter != _collections.end()) {
        return iter->second.get();
    } else {
        return createKVCollectionCatalogEntry(opCtx, ns);
    }
}

RecordStore* KVDatabaseCatalogEntryBase::getRecordStore(StringData ns) const {
    if (auto iter = _collections.find(ns.toString()); iter != _collections.end()) {
        return iter->second->getRecordStore();
    }
    return nullptr;
}

Status KVDatabaseCatalogEntryBase::createCollection(OperationContext* opCtx,
                                                    StringData ns,
                                                    const CollectionOptions& options,
                                                    bool allocateDefaultSpace) {
    MONGO_UNREACHABLE;

    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    if (ns.empty()) {
        return Status(ErrorCodes::BadValue, "Collection namespace cannot be empty");
    }

    // if (_collections.count(ns.toString())) {
    //     invariant(_collections[ns.toString()]);
    //     return Status(ErrorCodes::NamespaceExists, "collection already exists");
    // }

    KVPrefix prefix = KVPrefix::getNextPrefix(NamespaceString(ns));

    // update catalog here
    Status status =
        Status::OK();  //_engine->getCatalog()->newCollection(opCtx, ns, options, prefix);
    if (!status.isOK()) {
        return status;
    }

    string ident = _engine->getCatalog()->getCollectionIdent(ns);

    status = _engine->getEngine()->createGroupedRecordStore(opCtx, ns, ident, options, prefix);
    if (!status.isOK())
        return status;

    // Mark collation feature as in use if the collection has a non-simple default collation.
    if (!options.collation.isEmpty()) {
        const auto feature = KVCatalog::FeatureTracker::NonRepairableFeature::kCollation;
        if (_engine->getCatalog()->getFeatureTracker()->isNonRepairableFeatureInUse(opCtx,
                                                                                    feature)) {
            _engine->getCatalog()->getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx,
                                                                                        feature);
        }
    }

    opCtx->recoveryUnit()->registerChange(new AddCollectionChange(opCtx, this, ns, ident, true));

    auto rs = _engine->getEngine()->getGroupedRecordStore(opCtx, ns, ident, options, prefix);
    invariant(rs);

    // _collections[ns.toString()] = new KVCollectionCatalogEntry(
    //     _engine->getEngine(), _engine->getCatalog(), ns, ident, std::move(rs));

    return Status::OK();
}

Status KVDatabaseCatalogEntryBase::createCollection(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const CollectionOptions& options,
                                                    const BSONObj& idIndexSpec) {
    if (nss.isEmpty()) {
        return {ErrorCodes::BadValue, "Collection namespace cannot be empty"};
    }
    auto status = _engine->getCatalog()->newCollection(opCtx, nss, options, idIndexSpec);

    if (status.isOK()) {
        // Transaction which has created successfully in Eloq
        // create KVCollectionCatalogEntry directly here.
        if (auto iter = _collections.find(nss.toStringData()); iter == _collections.end()) {
            // Create corresponding KVCollectionCatalogEntry on this node
            KVPrefix prefix = KVPrefix::getNextPrefix(nss);
            auto rs = _engine->getEngine()->getGroupedRecordStore(
                opCtx, nss.toStringData(), nss.toStringData(), options, prefix);
            _collections.try_emplace(
                nss.toString(),
                std::make_unique<KVCollectionCatalogEntry>(_engine->getEngine(),
                                                           _engine->getCatalog(),
                                                           nss.toStringData(),
                                                           nss.toStringData(),
                                                           std::move(rs)));
        }
        return Status::OK();
    }
    return status;
}

CollectionCatalogEntry* KVDatabaseCatalogEntryBase::createKVCollectionCatalogEntry(
    OperationContext* opCtx, StringData ns) {
    MONGO_LOG(1) << "KVDatabaseCatalogEntryBase::createKVCollectionCatalogEntry";
    if (auto iter = _collections.find(ns); iter != _collections.end()) {
        return iter->second.get();
    }

    BSONObj obj = _engine->getCatalog()->findEntry(opCtx, ns);
    if (obj.isEmpty()) {
        return nullptr;
    }
    LOG(1) << " fetched CCE metadata: " << obj;

    if (KVCatalog::FeatureTracker::isFeatureDocument(obj)) {
        return nullptr;
    }

    BSONCollectionCatalogEntry::MetaData md;
    const BSONElement mdElement = obj["md"];
    if (mdElement.isABSONObj()) {
        MONGO_LOG(1) << "returning metadata: " << mdElement;
        md.parse(mdElement.Obj());
    }

    auto ident = obj["ident"].checkAndGetStringData();
    auto rs = _engine->getEngine()->getGroupedRecordStore(opCtx, ns, ident, md.options, md.prefix);

    auto [iter, success] = _collections.try_emplace(
        ns.toString(),
        std::make_unique<KVCollectionCatalogEntry>(
            _engine->getEngine(), _engine->getCatalog(), ns, ns, std::move(rs)));

    return iter->second.get();
}

void KVDatabaseCatalogEntryBase::initCollection(OperationContext* opCtx,
                                                const std::string& ns,
                                                bool forRepair) {
    assert(forRepair == false);

    StringData ident{ns};

    BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData(opCtx, ns);

    auto rs = _engine->getEngine()->getGroupedRecordStore(opCtx, ns, ident, md.options, md.prefix);
    invariant(rs);

    // std::scoped_lock<std::mutex> lk{_collectionsMutex};
    invariant(!_collections.count(ns));
    _collections.try_emplace(
        ns,
        std::make_unique<KVCollectionCatalogEntry>(
            _engine->getEngine(), _engine->getCatalog(), ns, ident, std::move(rs)));
}

void KVDatabaseCatalogEntryBase::reinitCollectionAfterRepair(OperationContext* opCtx,
                                                             const std::string& ns) {
    MONGO_UNREACHABLE;
    // Get rid of the old entry.
    CollectionCatalogMap::iterator it = _collections.find(ns);
    invariant(it != _collections.end());
    // delete it->second;
    _collections.erase(it);

    // Now reopen fully initialized.
    initCollection(opCtx, ns, false);
}

Status KVDatabaseCatalogEntryBase::renameCollection(OperationContext* opCtx,
                                                    StringData fromNS,
                                                    StringData toNS,
                                                    bool stayTemp) {
    MONGO_UNREACHABLE;
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    RecordStore* originalRS = NULL;

    CollectionCatalogMap::const_iterator it = _collections.find(fromNS.toString());
    if (it == _collections.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "rename cannot find collection");
    }

    originalRS = it->second->getRecordStore();

    it = _collections.find(toNS.toString());
    if (it != _collections.end()) {
        return Status(ErrorCodes::NamespaceExists, "for rename to already exists");
    }

    const std::string identFrom = _engine->getCatalog()->getCollectionIdent(fromNS);

    Status status = _engine->getEngine()->okToRename(opCtx, fromNS, toNS, identFrom, originalRS);
    if (!status.isOK())
        return status;

    status = _engine->getCatalog()->renameCollection(opCtx, fromNS, toNS, stayTemp);
    if (!status.isOK())
        return status;

    const std::string identTo = _engine->getCatalog()->getCollectionIdent(toNS);

    invariant(identFrom == identTo);

    BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData(opCtx, toNS);

    opCtx->recoveryUnit()->registerChange(
        new AddCollectionChange(opCtx, this, toNS, identTo, false));

    auto rs =
        _engine->getEngine()->getGroupedRecordStore(opCtx, toNS, identTo, md.options, md.prefix);

    // Add the destination collection to _collections before erasing the source collection. This
    // is to ensure that _collections doesn't erroneously appear empty during listDatabases if
    // a database consists of a single collection and that collection gets renamed (see
    // SERVER-34531). There is no locking to prevent listDatabases from looking into
    // _collections as a rename is taking place.
    _collections.try_emplace(
        toNS.toString(),
        std::make_unique<KVCollectionCatalogEntry>(
            _engine->getEngine(), _engine->getCatalog(), toNS, identTo, std::move(rs)));

    const CollectionCatalogMap::iterator itFrom = _collections.find(fromNS.toString());
    invariant(itFrom != _collections.end());
    // opCtx->recoveryUnit()->registerChange(
    //     new RemoveCollectionChange(opCtx, this, fromNS, identFrom, itFrom->second, false));
    _collections.erase(itFrom);

    return Status::OK();
}

Status KVDatabaseCatalogEntryBase::dropCollection(OperationContext* opCtx, StringData ns) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    _collections.erase(ns.toString());

    Status status = _engine->getCatalog()->dropCollection(opCtx, ns);
    // always Status::OK();
    return status;
    // CollectionMap::const_iterator it = _collections.find(ns.toString());
    // if (it == _collections.end()) {
    //     return Status(ErrorCodes::NamespaceNotFound, "cannnot find collection to drop");
    // }

    // KVCollectionCatalogEntry* const entry = it->second.;

    // invariant(entry->getTotalIndexCount(opCtx) == entry->getCompletedIndexCount(opCtx));

    // {
    //     std::vector<std::string> indexNames;
    //     entry->getAllIndexes(opCtx, &indexNames);
    //     for (size_t i = 0; i < indexNames.size(); i++) {
    //         entry->removeIndex(opCtx, indexNames[i]).transitional_ignore();
    //     }
    // }

    // invariant(entry->getTotalIndexCount(opCtx) == 0);

    // const std::string ident = _engine->getCatalog()->getCollectionIdent(ns);

    // Status status = _engine->getCatalog()->dropCollection(opCtx, ns);
    // if (!status.isOK()) {
    //     return status;
    // }

    // // This will lazily delete the KVCollectionCatalogEntry and notify the storageEngine to
    // // drop the collection only on WUOW::commit().
    // // opCtx->recoveryUnit()->registerChange(
    // //     new RemoveCollectionChange(opCtx, this, ns, ident, it->second, true));

    // _collections.erase(ns.toString());
}
}  // namespace mongo
