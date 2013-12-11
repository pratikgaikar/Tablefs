/*
 *
 *  Created on: Jul 19, 2011
 *      Author: kair
 */

#ifndef LEVELDBADAPTOR_H_
#define LEVELDBADAPTOR_H_

#include <vector>
#include "util/properties.h"
#include "util/logging.h"
#include "leveldb/db.h"
#include "leveldb/slice.h"
#include "leveldb/iterator.h"
#include "leveldb/write_batch.h"

namespace tablefs {

class LevelDBIterator;

class LevelDBAdaptor {
public:
  LevelDBAdaptor();

  virtual ~LevelDBAdaptor();

  void SetProperties(const Properties &p) {
    p_ = p;
  }

  void SetLogging(Logging *logs_) {
    logs = logs_;
  }

  int Init();

  void Cleanup();

  int Get(const leveldb::Slice &key,
          std::string &result);

  int Put(const leveldb::Slice &key,
          const leveldb::Slice &values);

  int Sync();

  int Write(leveldb::WriteBatch &batch);

  int Delete(const leveldb::Slice &key);

  LevelDBIterator* GetNewIterator();

  void Compact();

  void Report();

  bool GetStat(std::string stat, std::string* value) {
    return db_->GetProperty(stat, value);
  }

  bool GetMetric(std::string* value);

public:
  std::string db_name;
  leveldb::DB *db_;
  leveldb::Cache *cache_;
  Logging* logs;
  Properties p_;
  bool logon;
  bool writeahead;
  time_t last_sync_time;
  time_t sync_time_limit;
  int async_data_size;
  int sync_size_limit;
};

class LevelDBIterator {
private:
  leveldb::Iterator *iter_;

public:
  LevelDBIterator(leveldb::Iterator *iter) : iter_(iter) {}

  virtual ~LevelDBIterator() { delete iter_; }

  bool Valid() const { return iter_->Valid(); };

  void SeekToFirst() { iter_->SeekToFirst(); }

  void SeekToLast() { iter_->SeekToLast(); }

  void Seek(const leveldb::Slice &target) { iter_->Seek(target); }

  void Next() { iter_->Next(); }

  void Prev() { iter_->Next(); }


  leveldb::Slice key() const { return iter_->key(); }

  leveldb::Slice value() const { return iter_->value(); }
};

}

#endif /* LEVELDBADAPTOR_H_ */
