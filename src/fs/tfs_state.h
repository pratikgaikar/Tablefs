#ifndef TABLEFS_STATE_H_
#define TABLEFS_STATE_H_

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <errno.h>
#include "fs/tfs_inode.h"
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/iterator.h"
#include "adaptor/leveldb_adaptor.h"
#include "util/properties.h"
#include "util/logging.h"

namespace tablefs {

struct FileSystemState {
  std::string metadir_;
  std::string datadir_;
  std::string mountdir_;

  LevelDBAdaptor* metadb;

  tfs_inode_t max_inode_num;
  int threshold_;

  Logging* logs;

  FileSystemState();

  ~FileSystemState();

  int Setup(Properties& prop);

  void Destroy();

  LevelDBAdaptor* GetMetaDB() {
      return metadb;
  }

  Logging* GetLog() {
      return logs;
  }

  const std::string& GetDataDir() {
    return datadir_;
  }

  const int GetThreshold() {
    return threshold_;
  }

  bool IsEmpty() {
      return (max_inode_num == 0);
  }

  tfs_inode_t NewInode();
};

}

#endif
