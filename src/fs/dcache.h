/*
 * icache.h
 *
 *  Created on: Aug 15, 2011
 *      Author: kair
 */

#ifndef _DCACHE_H_
#define _DCACHE_H_

#include "fs/tfs_inode.h"
#include "port/port.h"
#include "util/hash.h"
#include <unordered_map>
#include <list>

namespace tablefs {

struct DentryCacheComp {
  bool operator() (const tfs_meta_key_t &lhs, const tfs_meta_key_t &rhs) const {
    return (lhs.hash_id == rhs.hash_id) && (lhs.inode_id == rhs.inode_id);
  }
};

struct DentryCacheHash {
  std::size_t operator()(const tfs_meta_key_t& x) const
  {
    std::size_t seed = 0;
    seed ^= x.inode_id + 0x9e3779b9 + (seed<<6) + (seed>>2);
    seed ^= x.hash_id + 0x9e3779b9 + (seed<<6) + (seed>>2);
    return seed;
  }
};

class DentryCache {
public:
  typedef std::pair<tfs_meta_key_t, tfs_inode_t> Entry;
  typedef std::list<Entry> CacheList;
  typedef std::unordered_map<tfs_meta_key_t, std::list<Entry>::iterator,
            DentryCacheHash, DentryCacheComp> CacheMap;

  DentryCache(size_t size) : maxsize(size) {}

  bool Find(tfs_meta_key_t &key, tfs_inode_t &value);

  void Insert(tfs_meta_key_t &key, const tfs_inode_t &value);

  void Evict(tfs_meta_key_t &key);

  void GetSize(size_t &size_cache_list, size_t &size_cache_map) {
    size_cache_map = cache.size();
    size_cache_list = lookup.size();
  }

  virtual ~DentryCache();

private:
  size_t maxsize;
  CacheList cache;
  CacheMap lookup;

  leveldb::port::Mutex cache_mutex;
};

}
#endif /* ICACHE_H_ */
