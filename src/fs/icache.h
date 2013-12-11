#ifndef _META_MANAGER_H_
#define _META_MANAGER_H_

#include "adaptor/leveldb_adaptor.h"
#include "fs/inodemutex.h"
#include "fs/tfs_inode.h"
#include "leveldb/cache.h"
#include "port/port.h"

namespace tablefs {

enum InodeAccessMode {
  INODE_READ = 0,
  INODE_DELETE = 1,
  INODE_WRITE = 2,
};

enum InodeState {
  CLEAN = 0,
  DELETED = 1,
  DIRTY = 2,
};

struct InodeCacheHandle {
  tfs_meta_key_t key_;
  std::string value_;
  InodeAccessMode mode_;

  leveldb::Cache::Handle* pointer;
  static int object_count;

  InodeCacheHandle():
    mode_(INODE_READ) {
    ++object_count;
  }

  InodeCacheHandle(const tfs_meta_key_t &key,
                   const std::string &value,
                   InodeAccessMode mode):
    key_(key), value_(value), mode_(mode) {
    ++object_count;
  }

  ~InodeCacheHandle() {
    --object_count;
  }
};

class InodeCache {
public:
  static LevelDBAdaptor* metadb_;
  leveldb::Cache* cache;

  InodeCache(LevelDBAdaptor *metadb) {
    cache = leveldb::NewLRUCache(MAX_OPEN_FILES);
    metadb_ = metadb;
  }

  ~InodeCache() {
    delete cache;
  }

  InodeCacheHandle* Insert(const tfs_meta_key_t key,
                           const std::string &value);

  InodeCacheHandle* Get(const tfs_meta_key_t key,
                        const InodeAccessMode mode);

  void Release(InodeCacheHandle* handle);

  void WriteBack(InodeCacheHandle* handle);

  void BatchCommit(InodeCacheHandle* handle1,
                   InodeCacheHandle* handle2);

  void Evict(const tfs_meta_key_t key);

  friend void CleanInodeHandle(const leveldb::Slice key, void* value);

};

}

#endif
