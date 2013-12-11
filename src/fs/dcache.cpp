/*
 * InodeCache.cpp
 *
 *  Created on: Aug 15, 2011
 *      Author: kair
 */

#include "dcache.h"
#include "util/mutexlock.h"

namespace tablefs {

DentryCache::~DentryCache() {
}

bool DentryCache::Find(tfs_meta_key_t &key, tfs_inode_t &value) {
  leveldb::MutexLock lock_cache_mutex(&cache_mutex);

  CacheMap::iterator it = lookup.find(key);
  if (it == lookup.end()) {
    return false;
  } else {
    value = it->second->second;
    cache.push_front(*(it->second));
    cache.erase(it->second);
    lookup[key] = cache.begin();
    return true;
  }
}

void DentryCache::Insert(tfs_meta_key_t &key, const tfs_inode_t &value) {
  leveldb::MutexLock lock_cache_mutex(&cache_mutex);

  Entry ent(key, value);
  cache.push_front(ent);
  lookup[key] = cache.begin();
  if (cache.size() > maxsize) {
    lookup.erase(cache.back().first);
    cache.pop_back();
  }
}

void DentryCache::Evict(tfs_meta_key_t &key) {
  leveldb::MutexLock lock_cache_mutex(&cache_mutex);

  CacheMap::iterator it = lookup.find(key);
  if (it != lookup.end()) {
    lookup.erase(it);
  }
}

}
