#ifndef _INODE_LOCK_H_
#define _INODE_LOCK_H_

#include <stdint.h>
#include <pthread.h>
#include "fs/tfs_inode.h"

namespace tablefs {

class RWLock{
 public:
  RWLock();
  ~RWLock();

  void ReadLock();
  void WriteLock();
  void Unlock();

 private:
  pthread_rwlock_t rw_;

  // No copying
  RWLock(const RWLock&);
  void operator=(const RWLock&);
};

class InodeMutex {
public:
  InodeMutex() {}
  ~InodeMutex() {}

  void ReadLock(const tfs_meta_key_t &key);

  void WriteLock(const tfs_meta_key_t &key);

  void Unlock(const tfs_meta_key_t &key);

private:
  const static unsigned int NUM_ILOCK = 1024;
  const static unsigned int ILOCK_BASE = 0x3ff;
  RWLock ilock[NUM_ILOCK];

  //no copying
  InodeMutex(const InodeMutex &);
  void operator=(const InodeMutex &);
};

}

#endif
