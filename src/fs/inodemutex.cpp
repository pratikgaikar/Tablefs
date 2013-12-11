/*************************************************************************
* Author: Kai Ren
* Created Time: 2012-09-10 17:04:58
* File Name: inodemutex.cpp
* Description:
 ************************************************************************/
#include "fs/inodemutex.h"
#include "util/hash.h"
#include "util/logging.h"

namespace tablefs {

RWLock::RWLock() { pthread_rwlock_init(&rw_, NULL); }

RWLock::~RWLock() { pthread_rwlock_destroy(&rw_); }

void RWLock::ReadLock() { pthread_rwlock_rdlock(&rw_); }

void RWLock::WriteLock() { pthread_rwlock_wrlock(&rw_); }

void RWLock::Unlock() { pthread_rwlock_unlock(&rw_); }

void InodeMutex::ReadLock(const tfs_meta_key_t &key) {
  uint32_t lock_id = (key.inode_id + key.hash_id) & ILOCK_BASE;
//  uint32_t lock_id = leveldb::Hash(key.str, 16, 0) & ILOCK_BASE;
//  ilock[lock_id].ReadLock();
//  Logging::Default()->LogMsg("ReadLock [%d, %d]\n", key.inode_id, key.hash_id);
}

void InodeMutex::WriteLock(const tfs_meta_key_t &key) {
  uint32_t lock_id = (key.inode_id + key.hash_id) & ILOCK_BASE;
//  uint32_t lock_id = leveldb::Hash(key.str, 16, 0) & ILOCK_BASE;
//  ilock[lock_id].WriteLock();
//  Logging::Default()->LogMsg("WriteLock [%d, %d]\n", key.inode_id, key.hash_id);
}

void InodeMutex::Unlock(const tfs_meta_key_t &key) {
  uint32_t lock_id = (key.inode_id + key.hash_id) & ILOCK_BASE;
//  uint32_t lock_id = leveldb::Hash(key.str, 16, 0) & ILOCK_BASE;
//  ilock[lock_id].Unlock();
//  Logging::Default()->LogMsg("Unlock [%d, %d]\n", key.inode_id, key.hash_id);
}

} // namespace tablefs
