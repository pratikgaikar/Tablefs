#include "fs/icache.h"
#include "util/logging.h"
#include "leveldb/write_batch.h"

namespace tablefs {

LevelDBAdaptor* InodeCache::metadb_ = NULL;
int InodeCacheHandle::object_count = 0;

void CleanInodeHandle(const leveldb::Slice &key, void* value) {
  InodeCacheHandle* handle = reinterpret_cast<InodeCacheHandle*>(value);
  if (handle->mode_ == INODE_WRITE) {
    InodeCache::metadb_->Put(handle->key_.ToSlice(), handle->value_);
  } else if (handle->mode_ == INODE_DELETE) {
    InodeCache::metadb_->Delete(handle->key_.ToSlice());
  }
#ifdef TABLEFS_DEBUG
  if (Logging::Default() != NULL) {
    const tfs_inode_header* header = reinterpret_cast<const tfs_inode_header*> (handle->value_.data());
    Logging::Default()->LogMsg("Clean [write back]: inode_id(%08x) hash_id(%08x) mode_(%d) size(%d)\n",
                                handle->key_.inode_id,
                                handle->key_.hash_id,
                                handle->mode_,
                                header->fstat.st_size);
  }
#endif
  delete handle;
}

InodeCacheHandle* InodeCache::Insert(const tfs_meta_key_t key,
                                     const std::string &value) {
  InodeCacheHandle* handle = new InodeCacheHandle(key, value, INODE_WRITE);
  leveldb::Cache::Handle* ch = cache->Insert(key.ToSlice(), (void *) handle, 1, CleanInodeHandle);
  handle->pointer = ch;
  return handle;
}

InodeCacheHandle* InodeCache::Get(const tfs_meta_key_t key,
                                  const InodeAccessMode mode) {
  leveldb::Cache::Handle* ch = cache->Lookup(key.ToSlice());
  InodeCacheHandle* handle = NULL;
  if (ch == NULL) {
    std::string value;
    if (metadb_->Get(key.ToSlice(), value) > 0) {
      handle = new InodeCacheHandle(key, value, mode);
      ch = cache->Insert(key.ToSlice(), (void *) handle, 1, CleanInodeHandle);
      handle->pointer = ch;
    }
  } else {
    handle = (InodeCacheHandle*) cache->Value(ch);
    if (mode > INODE_READ) {
      handle->mode_ = mode;
    }
  }
  return handle;
}

void InodeCache::Release(InodeCacheHandle* handle) {
  cache->Release(handle->pointer);
}

void InodeCache::WriteBack(InodeCacheHandle* handle) {
#ifdef TABLEFS_DEBUG
  if (Logging::Default() != NULL) {
    const tfs_inode_header* header = reinterpret_cast<const tfs_inode_header*> (handle->value_.data());
    Logging::Default()->LogMsg("Write Back: inode_id(%08x) hash_id(%08x) mode_(%d) size(%d)\n",
                                handle->key_.inode_id,
                                handle->key_.hash_id,
                                handle->mode_,
                                header->fstat.st_size);
  }
#endif

  if (handle->mode_ == INODE_WRITE) {
    InodeCache::metadb_->Put(handle->key_.ToSlice(), handle->value_);
    handle->mode_ = INODE_READ;
  } else if (handle->mode_ == INODE_DELETE) {
    InodeCache::metadb_->Delete(handle->key_.ToSlice());
    Evict(handle->key_);
  }
}

void InodeCache::BatchCommit(InodeCacheHandle* handle1,
                             InodeCacheHandle* handle2) {
#ifdef TABLEFS_DEBUG
  if (Logging::Default() != NULL) {
    const tfs_inode_header* header = reinterpret_cast<const tfs_inode_header*> (handle1->value_.data());
    Logging::Default()->LogMsg("Batch commit [write back]: inode_id(%08x) hash_id(%08x) mode_(%d) size(%d)\n",
                                handle1->key_.inode_id,
                                handle1->key_.hash_id,
                                handle1->mode_,
                                header->fstat.st_size);
    header = reinterpret_cast<const tfs_inode_header*> (handle2->value_.data());
    Logging::Default()->LogMsg("Batch commit [write back]: inode_id(%08x) hash_id(%08x) mode_(%d) size(%d)\n",
                                handle2->key_.inode_id,
                                handle2->key_.hash_id,
                                handle2->mode_,
                                header->fstat.st_size);

  }
#endif

  leveldb::WriteBatch batch;
  batch.Clear();
  if (handle1->mode_ == INODE_WRITE) {
    batch.Put(handle1->key_.ToSlice(), handle1->value_);
    handle1->mode_ = INODE_READ;
  } else if (handle1->mode_ == INODE_DELETE) {
    batch.Delete(handle1->key_.ToSlice());
    Evict(handle1->key_);
  }
  if (handle2->mode_ == INODE_WRITE) {
    batch.Put(handle2->key_.ToSlice(), handle2->value_);
    handle2->mode_ = INODE_READ;
  } else if (handle2->mode_ == INODE_DELETE) {
    batch.Delete(handle2->key_.ToSlice());
    Evict(handle2->key_);
  }
  metadb_->Write(batch);
}

void InodeCache::Evict(const tfs_meta_key_t key) {
  cache->Erase(key.ToSlice());
}

} // namespace tablefs
