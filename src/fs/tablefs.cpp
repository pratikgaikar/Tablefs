#define FUSE_USE_VERSION 26

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <error.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/time.h>
#include <vector>
#include <algorithm>
#include <pthread.h>
#include <sstream>
#include "fs/tablefs.h"
#include "util/myhash.h"
#include "util/mutexlock.h"
#include "util/socket.h"
#include "leveldb/env.h"
#include "leveldb/db.h"
#include "leveldb/cache.h"
#include "leveldb/write_batch.h"

namespace tablefs {

struct tfs_file_handle_t {
  InodeCacheHandle* handle_;
  int flags_;
  int fd_;

  tfs_file_handle_t(): handle_(NULL), fd_(-1) {}
};

inline static void BuildMetaKey(const tfs_inode_t inode_id,
                                const tfs_hash_t hash_id,
                                tfs_meta_key_t &key) {
  key.inode_id = inode_id;
  key.hash_id = hash_id;
}

inline static void BuildMetaKey(const char *path,
                                const int len,
                                const tfs_inode_t inode_id,
                                tfs_meta_key_t &key) {
  BuildMetaKey(inode_id, murmur64(path, len, 123), key);
}

inline static bool IsKeyInDir(const leveldb::Slice &key,
                              const tfs_meta_key_t &dirkey) {
  const tfs_meta_key_t* rkey = (const tfs_meta_key_t *) key.data();
  return rkey->inode_id == dirkey.inode_id;
}

const tfs_inode_header *GetInodeHeader(const std::string &value) {
  return reinterpret_cast<const tfs_inode_header*> (value.data());
}

const tfs_stat_t *GetAttribute(std::string &value) {
  return reinterpret_cast<const tfs_stat_t*> (value.data());
}

size_t GetInlineData(std::string &value, char* buf, size_t offset, size_t size) {
  const tfs_inode_header* header = GetInodeHeader(value);
  size_t realoffset = TFS_INODE_HEADER_SIZE + header->namelen + 1 + offset;
  if (realoffset < value.size()) {
    if (realoffset + size > value.size()) {
      size = value.size() - realoffset;
    }
    memcpy(buf, value.c_str() + realoffset , size);
    return size;
  } else {
    return 0;
  }
}

void UpdateIhandleValue(std::string &value,
                        const char* buf, size_t offset, size_t size) {
  if (offset > value.size()) {
    value.resize(offset);
  }
  value.replace(offset, size, buf, size);
}

void UpdateInodeHeader(std::string &value,
                       tfs_inode_header &new_header) {
  UpdateIhandleValue(value, (const char *) &new_header,
                     0, TFS_INODE_HEADER_SIZE);
}

void UpdateAttribute(std::string &value,
                     const tfs_stat_t &new_fstat) {
  UpdateIhandleValue(value, (const char *) &new_fstat,
                     0, TFS_INODE_ATTR_SIZE);
}

void UpdateInlineData(std::string &value,
                      const char* buf, size_t offset, size_t size) {
  const tfs_inode_header* header = GetInodeHeader(value);
  size_t realoffset = TFS_INODE_HEADER_SIZE + header->namelen + 1 + offset;
  UpdateIhandleValue(value, buf, realoffset, size);
}

void TruncateInlineData(std::string &value, size_t new_size) {
  const tfs_inode_header* header = GetInodeHeader(value);
  size_t target_size = TFS_INODE_HEADER_SIZE + header->namelen + new_size + 1;
  value.resize(target_size);
}

void DropInlineData(std::string &value) {
  const tfs_inode_header* header = GetInodeHeader(value);
  size_t target_size = TFS_INODE_HEADER_SIZE + header->namelen + 1;
  value.resize(target_size);
}

void TableFS::SetState(FileSystemState* state) {
  state_ = state;
}

int TableFS::FSError(const char *err_msg) {
  int retv = -errno;
#ifdef TABLEFS_DEBUG
  state_->GetLog()->LogMsg(err_msg);
#endif
  return retv;
}

void TableFS::InitStat(tfs_stat_t &statbuf,
                       tfs_inode_t inode,
                       mode_t mode,
                       dev_t dev) {
  statbuf.st_ino = inode;
  statbuf.st_mode = mode;
  statbuf.st_dev = dev;

  if (flag_fuse_enabled) {
    statbuf.st_gid = fuse_get_context()->gid;
    statbuf.st_uid = fuse_get_context()->uid;
  } else {
    statbuf.st_gid = 0;
    statbuf.st_uid = 0;
  }

  statbuf.st_size = 0;
  statbuf.st_blksize = 0;
  statbuf.st_blocks = 0;
  if S_ISREG(mode) {
    statbuf.st_nlink = 1;
  } else {
    statbuf.st_nlink = 2;
  }
  time_t now = time(NULL);
  statbuf.st_atim.tv_sec = now;
  statbuf.st_ctim.tv_sec = now;
  statbuf.st_mtim.tv_sec = now;
}

tfs_inode_val_t TableFS::InitInodeValue(tfs_inode_t inum,
                                        mode_t mode,
                                        dev_t dev,
                                        leveldb::Slice filename) {
  tfs_inode_val_t ival;
  ival.size = TFS_INODE_HEADER_SIZE + filename.size() + 1;
  ival.value = new char[ival.size];
  tfs_inode_header* header = reinterpret_cast<tfs_inode_header*>(ival.value);
  InitStat(header->fstat, inum, mode, dev);
  header->has_blob = 0;
  header->namelen = filename.size();
  char* name_buffer = ival.value + TFS_INODE_HEADER_SIZE;
  memcpy(name_buffer, filename.data(), filename.size());
  name_buffer[header->namelen] = '\0';
  return ival;
}

std::string TableFS::InitInodeValue(const std::string& old_value,
                                    leveldb::Slice filename) {
  //TODO: Optimize avoid too many copies
  std::string new_value = old_value;
  tfs_inode_header header = *GetInodeHeader(old_value);
  new_value.replace(TFS_INODE_HEADER_SIZE, header.namelen+1,
                    filename.data(), filename.size()+1);
  header.namelen = filename.size();
  UpdateInodeHeader(new_value, header);

  return new_value;
}

void TableFS::FreeInodeValue(tfs_inode_val_t &ival) {
  if (ival.value != NULL) {
    delete [] ival.value;
    ival.value = NULL;
  }
}

bool TableFS::ParentPathLookup(const char *path,
                               tfs_meta_key_t &key,
                               tfs_inode_t &inode_in_search,
                               const char* &lastdelimiter) {
  const char* lpos = path;
  const char* rpos;
  bool flag_found = true;
  std::string item;
  inode_in_search = ROOT_INODE_ID;
  while ((rpos = strchr(lpos+1, PATH_DELIMITER)) != NULL) {
    if (rpos - lpos > 0) {
      BuildMetaKey(lpos+1, rpos-lpos-1, inode_in_search, key);
      if (!dentry_cache->Find(key, inode_in_search)) {
        {
          fstree_lock.ReadLock(key);
          std::string result;
          int ret = metadb->Get(key.ToSlice(), result);
          if (ret == 1) {
            inode_in_search = GetAttribute(result)->st_ino;
            dentry_cache->Insert(key, inode_in_search);
          } else {
            errno = ENOENT;
            flag_found = false;
          }
          fstree_lock.Unlock(key);
          if (!flag_found) {
            return false;
          }
        }
      }
    }
    lpos = rpos;
  }
  if (lpos == path) {
    BuildMetaKey(NULL, 0, ROOT_INODE_ID, key);
  }
  lastdelimiter = lpos;
  return flag_found;
}

bool TableFS::PathLookup(const char *path,
                         tfs_meta_key_t &key) {
  const char* lpos;
  tfs_inode_t inode_in_search;
  if (ParentPathLookup(path, key, inode_in_search, lpos)) {
    const char* rpos = strchr(lpos, '\0');
    if (rpos != NULL && rpos-lpos > 1) {
      BuildMetaKey(lpos+1, rpos-lpos-1, inode_in_search, key);
    }
    return true;
  } else {
    errno = ENOENT;
    return false;
  }
}

bool TableFS::PathLookup(const char *path,
                         tfs_meta_key_t &key,
                         leveldb::Slice &filename) {
  const char* lpos;
  tfs_inode_t inode_in_search;
  if (ParentPathLookup(path, key, inode_in_search, lpos)) {
    const char* rpos = strchr(lpos, '\0');
    if (rpos != NULL && rpos-lpos > 1) {
      BuildMetaKey(lpos+1, rpos-lpos-1, inode_in_search, key);
      filename = leveldb::Slice(lpos+1, rpos-lpos-1);
    } else {
      filename = leveldb::Slice(lpos, 1);
    }
    return true;
  } else {
    errno = ENOENT;
    return false;
  }
}

const int DEFAULT_SYNC_INTERVAL = 5;
volatile int stop_monitor_thread;
volatile int flag_monitor_thread_finish;
pthread_mutex_t     mtx_monitor;
pthread_cond_t      cv_monitor;

void do_monitor(LevelDBAdaptor* metadb) {
  std::string metric;
  if (metadb->GetMetric(&metric)) {
    const int metric_cnt = 13;
    int r[metric_cnt];
    std::stringstream ssmetric(metric);
    for (int i = 0; i < metric_cnt; ++i)
      ssmetric >> r[i];
    const char* metname[metric_cnt] = {
        "num_files", "num_size", "num_compact",
        "tot_comp_time", "tot_comp_read", "tot_comp_write",
        "num_write_op", "num_get_op", "filterhit",
        "minor_count", "log_writes", "log_syncs", "desc_syncs"
    };
    int now_time = (int) time(NULL);
    for (int ri = 0; ri < metric_cnt; ++ri) {
      char metricString[128];
      sprintf(metricString,
             "tablefs.%s %d %d\n", metname[ri], now_time, r[ri]);
      UDPSocket sock;
      try {
        sock.sendTo(metricString, strlen(metricString),
                    std::string("127.0.0.1"), 10600);
      } catch (SocketException &e) {
      }
    }
  }
}

void report_get_count(LevelDBAdaptor* metadb) {
  std::string metric;
  if (metadb->GetStat("leveldb.get_count_by_num_files", &metric)) {
    const int metric_cnt = 20;
    int r[metric_cnt];
    std::stringstream ssmetric(metric);
    for (int i = 0; i < metric_cnt; ++i)
      ssmetric >> r[i];
    for (int i = 0; i < metric_cnt; ++i) {
      int now_time = (int) time(NULL);
      char metricString[128];
      sprintf(metricString,
             "tablefs.get_count_%d %d %d\n", i, now_time, r[i]);
      UDPSocket sock;
      try {
        sock.sendTo(metricString, strlen(metricString),
                    std::string("127.0.0.1"), 10600);
      } catch (SocketException &e) {
      }
    }
  }
  if (metadb->GetStat("leveldb.false_get_count_by_num_files", &metric)) {
    const int metric_cnt = 20;
    int r[metric_cnt];
    std::stringstream ssmetric(metric);
    for (int i = 0; i < metric_cnt; ++i)
      ssmetric >> r[i];
    for (int i = 0; i < metric_cnt; ++i) {
      int now_time = (int) time(NULL);
      char metricString[128];
      sprintf(metricString,
             "tablefs.false_get_count_%d %d %d\n", i, now_time, r[i]);
      UDPSocket sock;
      try {
        sock.sendTo(metricString, strlen(metricString),
                    std::string("127.0.0.1"), 10600);
      } catch (SocketException &e) {
      }
    }
  }
}

void* monitor_thread(void *v) {
    LevelDBAdaptor* metadb= (LevelDBAdaptor*) v;

    struct timespec wait;
    memset(&wait, 0, sizeof(wait)); /* ensure it's initialized */
    struct timeval now;
    char* err;

    pthread_mutex_lock(&(mtx_monitor));
    int ret = ETIMEDOUT;
    while (stop_monitor_thread == 0) {
        if (ret == ETIMEDOUT) {
          do_monitor(metadb);
        } else {
          if (stop_monitor_thread == 0) {
            fprintf(stderr, "Unexpected interrupt for monitor thread\n");
            stop_monitor_thread = 1;
          }
        }
        gettimeofday(&now, NULL);
        wait.tv_sec = now.tv_sec + DEFAULT_SYNC_INTERVAL;
        pthread_cond_timedwait(&(cv_monitor), &(mtx_monitor), &wait);
    }
    flag_monitor_thread_finish = 1;
    pthread_cond_broadcast(&(cv_monitor));
    pthread_mutex_unlock(&(mtx_monitor));
    return NULL;
}

void monitor_init(LevelDBAdaptor *mdb) {
    stop_monitor_thread = 0;
    flag_monitor_thread_finish = 0;
    pthread_mutex_init(&(mtx_monitor), NULL);
    pthread_cond_init(&(cv_monitor), NULL);

    int ret;
    pthread_t tid;
    if ((ret = pthread_create(&tid, NULL, monitor_thread, mdb))) {
        fprintf(stderr, "pthread_create() error: %d", ret);
        exit(1);
    }
    if ((ret = pthread_detach(tid))) {
        fprintf(stderr, "pthread_detach() error: %d", ret);
        exit(1);
    }
}

void monitor_destroy() {
  pthread_mutex_lock(&(mtx_monitor));
  stop_monitor_thread = 1;
  pthread_cond_signal(&(cv_monitor));
  while (flag_monitor_thread_finish == 0) {
    pthread_cond_wait(&(cv_monitor), &(mtx_monitor));
  }
  pthread_mutex_unlock(&(mtx_monitor));

  pthread_mutex_destroy(&(mtx_monitor));
  pthread_cond_destroy(&(cv_monitor));
}

void* TableFS::Init(struct fuse_conn_info *conn) {
  state_->GetLog()->LogMsg("TableFS initialized.\n");
  if (conn != NULL) {
    flag_fuse_enabled = true;
  } else {
    flag_fuse_enabled = false;
  }
  metadb = state_->GetMetaDB();
  if (state_->IsEmpty()) {
    state_->GetLog()->LogMsg("TableFS create root inode.\n");
    tfs_meta_key_t key;
    BuildMetaKey(NULL, 0, ROOT_INODE_ID, key);
    struct stat statbuf;
    lstat(ROOT_INODE_STAT, &statbuf);
    tfs_inode_val_t value = InitInodeValue(ROOT_INODE_ID,
          statbuf.st_mode, statbuf.st_dev, leveldb::Slice("\0"));
    if (metadb->Put(key.ToSlice(), value.ToSlice()) < 0) {
      state_->GetLog()->LogMsg("TableFS create root directory failed.\n");
    }
    FreeInodeValue(value);
  }
  inode_cache = new InodeCache(metadb);
  dentry_cache = new DentryCache(16384);
  //monitor_init(metadb);
  return state_;
}

void TableFS::Destroy(void * data) {
  monitor_destroy();
  do_monitor(metadb);
  report_get_count(metadb);
  if (dentry_cache != NULL)
    delete dentry_cache;
  if (inode_cache != NULL) {
    delete inode_cache;
  }
  state_->GetLog()->LogMsg("Clean write back cache.\n");
  state_->Destroy();
  delete state_;
}

int TableFS::GetAttr(const char *path, struct stat *statbuf) {
  tfs_meta_key_t key;
  if (!PathLookup(path, key)) {
     return FSError("GetAttr Path Lookup: No such file or directory: %s\n");
  }
  int ret = 0;
  fstree_lock.ReadLock(key);
  InodeCacheHandle* handle = inode_cache->Get(key, INODE_READ);
  if (handle != NULL) {
    *statbuf = *(GetAttribute(handle->value_));
#ifdef TABLEFS_DEBUG
  state_->GetLog()->LogMsg("GetAttr: %s, Handle: %x HandleMode: %d\n",
                            path, handle, handle->mode_);
  state_->GetLog()->LogMsg("GetAttr DBKey: [%d,%d]\n", key.inode_id, key.hash_id);
  state_->GetLog()->LogStat(path, statbuf);
#endif
    inode_cache->Release(handle);
  } else {
    ret = -ENOENT;
  }
  fstree_lock.Unlock(key);
  return ret;
}

void TableFS::GetDiskFilePath(char *path, tfs_inode_t inode_id) {
  sprintf(path, "%s/%d/%d",
          state_->GetDataDir().data(),
          (int) inode_id >> NUM_FILES_IN_DATADIR_BITS,
          (int) inode_id % (NUM_FILES_IN_DATADIR));
}

int TableFS::OpenDiskFile(const tfs_inode_header* iheader, int flags) {
  char fpath[128];
  GetDiskFilePath(fpath, iheader->fstat.st_ino);
  int fd = open(fpath, flags | O_CREAT, iheader->fstat.st_mode);
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("OpenDiskFile: %s InodeID: %d FD: %d\n",
                           fpath, iheader->fstat.st_ino, fd);
#endif
  return fd;
}

int TableFS::TruncateDiskFile(tfs_inode_t inode_id, off_t new_size) {
  char fpath[128];
  GetDiskFilePath(fpath, inode_id);
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("TruncateDiskFile: %s, InodeID: %d, NewSize: %d\n",
                          fpath, inode_id, new_size);
#endif
  return truncate(fpath, new_size);
}

ssize_t TableFS::MigrateDiskFileToBuffer(tfs_inode_t inode_id,
                                         char* buffer,
                                         size_t size) {
  char fpath[128];
  GetDiskFilePath(fpath, inode_id);
  int fd = open(fpath, O_RDONLY);
  ssize_t ret = pread(fd, buffer, size, 0);
  close(fd);
  unlink(fpath);
  return ret;
}

int TableFS::MigrateToDiskFile(InodeCacheHandle* handle, int &fd, int flags) {
  const tfs_inode_header* iheader = GetInodeHeader(handle->value_);
  if (fd >= 0) {
    close(fd);
  }
  fd = OpenDiskFile(iheader, flags);
  if (fd < 0) {
    fd = -1;
    return -errno;
  }
  int ret = 0;
  if (iheader->fstat.st_size > 0 ) {
    const char* buffer = (const char *) iheader +
                         (TFS_INODE_HEADER_SIZE + iheader->namelen + 1);
    if (pwrite(fd, buffer, iheader->fstat.st_size, 0) !=
        iheader->fstat.st_size) {
      ret = -errno;
    }
    DropInlineData(handle->value_);
  }
  return ret;
}

void TableFS::CloseDiskFile(int& fd_) {
  close(fd_);
  fd_ = -1;
}

int TableFS::Open(const char *path, struct fuse_file_info *fi) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Open: %s, Flags: %d\n", path, fi->flags);
#endif

  tfs_meta_key_t key;
  if (!PathLookup(path, key)) {
    return FSError("Open: No such file or directory\n");
  }
  fstree_lock.WriteLock(key);
  InodeCacheHandle* handle = NULL;
  if ((fi->flags & O_RDWR) > 0 ||
      (fi->flags & O_WRONLY) > 0 ||
      (fi->flags & O_TRUNC) > 0) {
    handle = inode_cache->Get(key, INODE_WRITE);
  } else {
    handle = inode_cache->Get(key, INODE_READ);
  }

  int ret = 0;
  if (handle != NULL) {
    tfs_file_handle_t* fh = new tfs_file_handle_t();
    fh->handle_ = handle;
    fh->flags_ = fi->flags;
    const tfs_inode_header *iheader= GetInodeHeader(handle->value_);

    if (iheader->has_blob > 0) {
        fh->flags_ = fi->flags;
        fh->fd_ = OpenDiskFile(iheader, fh->flags_);
        if (fh->fd_ < 0) {
          inode_cache->Release(handle);
          ret = -errno;
        }
    }
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Open: %s, Handle: %x, HandleMode: %d FD: %d\n",
                           path, handle, handle->mode_, fh->fd_);
#endif
    if (ret == 0) {
      fi->fh = (uint64_t) fh;
    } else {
      delete fh;
    }
  } else {
    ret = -ENOENT;
  }
  fstree_lock.Unlock(key);


  return ret;
}

int TableFS::Read(const char* path, char *buf, size_t size,
                  off_t offset, struct fuse_file_info *fi) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Read: %s\n", path);
#endif
  tfs_file_handle_t* fh = reinterpret_cast<tfs_file_handle_t*>(fi->fh);
  InodeCacheHandle* handle = fh->handle_;
  fstree_lock.ReadLock(handle->key_);
  const tfs_inode_header* iheader = GetInodeHeader(handle->value_);
  int ret;
  if (iheader->has_blob > 0) {
    if (fh->fd_ < 0) {
      fh->fd_ = OpenDiskFile(iheader, fh->flags_);
      if (fh->fd_ < 0)
        ret = -EBADF;
    }
    if (fh->fd_ >= 0) {
      ret = pread(fh->fd_, buf, size, offset);
    }
  } else {
    ret = GetInlineData(handle->value_, buf, offset, size);
  }
  fstree_lock.Unlock(handle->key_);
  return ret;
}

int TableFS::Write(const char* path, const char *buf, size_t size,
                   off_t offset, struct fuse_file_info *fi) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Write: %s %lld %d\n", path, offset, size);
#endif

  tfs_file_handle_t* fh = reinterpret_cast<tfs_file_handle_t*>(fi->fh);
  InodeCacheHandle* handle = fh->handle_;
  handle->mode_ = INODE_WRITE;
  fstree_lock.ReadLock(handle->key_);
  const tfs_inode_header* iheader = GetInodeHeader(handle->value_);
  int ret = 0, has_imgrated = 0;
  int has_larger_size = (iheader->fstat.st_size < offset + size) ? 1 : 0;

#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Write: %s has_larger_size %d old: %d new: %lld\n", 
      path, has_larger_size, iheader->fstat.st_size, offset + size);
#endif

  if (iheader->has_blob > 0) {
    if (fh->fd_ < 0) {
      fh->fd_ = OpenDiskFile(iheader, fh->flags_);
      if (fh->fd_ < 0)
        ret = -EBADF;
    }
    if (fh->fd_ >= 0) {
      ret = pwrite(fh->fd_, buf, size, offset);
    }
  } else {
    if (offset + size > state_->GetThreshold()) {
      size_t cursize = iheader->fstat.st_size;
      ret = MigrateToDiskFile(handle, fh->fd_, fi->flags);
      if (ret == 0) {
        tfs_inode_header new_iheader = *GetInodeHeader(handle->value_);
        new_iheader.fstat.st_size = offset + size;
        new_iheader.has_blob = 1;
        UpdateInodeHeader(handle->value_, new_iheader);
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Write: UpdateInodeHeader %d\n", GetInodeHeader(handle->value_)->has_blob);
#endif
        has_imgrated = 1;
        ret = pwrite(fh->fd_, buf, size, offset);
      }
    } else {
      UpdateInlineData(handle->value_, buf, offset, size);
      ret = size;
    }
  }
  if (ret >= 0) {
    if (has_larger_size > 0 && has_imgrated == 0) {
      tfs_inode_header new_iheader = *GetInodeHeader(handle->value_);
      new_iheader.fstat.st_size = offset + size;
      UpdateInodeHeader(handle->value_, new_iheader);
    }
  }
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Write: %s, Handle: %x HandleMode: %d\n",
                           path, handle, handle->mode_);
#endif
  fstree_lock.Unlock(handle->key_);
  return ret;
}

int TableFS::Fsync(const char *path, int datasync, struct fuse_file_info *fi) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Fsync: %s\n", path);
#endif

  tfs_file_handle_t* fh = reinterpret_cast<tfs_file_handle_t*>(fi->fh);
  InodeCacheHandle* handle = fh->handle_;
  fstree_lock.WriteLock(handle->key_);

  const tfs_inode_header* iheader = GetInodeHeader(handle->value_);
  int ret = 0;
  if (handle->mode_ == INODE_WRITE) {
    if (iheader->has_blob > 0) {
      ret = fsync(fh->fd_);
    }
    if (datasync == 0) {
      ret = metadb->Sync();
    }
  }

  fstree_lock.Unlock(handle->key_);
  return -ret;
}

int TableFS::Release(const char *path, struct fuse_file_info *fi) {
  tfs_file_handle_t* fh = reinterpret_cast<tfs_file_handle_t*>(fi->fh);
  InodeCacheHandle* handle = fh->handle_;
  fstree_lock.WriteLock(handle->key_);

  if (handle->mode_ == INODE_WRITE) {
    const tfs_stat_t *value = GetAttribute(handle->value_);
    tfs_stat_t new_value = *value;
    new_value.st_atim.tv_sec = time(NULL);
    new_value.st_mtim.tv_sec = time(NULL);
    UpdateAttribute(handle->value_, new_value);
  }

#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Release: %s, FD: %d\n",
                           path, fh->fd_);
#endif

  int ret = 0;
  if (fh->fd_ != -1) {
    ret = close(fh->fd_);
  }
  inode_cache->WriteBack(handle);
  tfs_meta_key_t key = handle->key_;
  inode_cache->Release(handle);
  inode_cache->Evict(key);
  delete fh;

  fstree_lock.Unlock(key);
  if (ret != 0) {
    return -errno;
  } else {
    return 0;
  }
}

int TableFS::Truncate(const char *path, off_t new_size) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Truncate: %s\n", path);
#endif

  tfs_meta_key_t key;
  if (!PathLookup(path, key)) {
    return FSError("Open: No such file or directory\n");
  }
  InodeCacheHandle* handle =
    inode_cache->Get(key, INODE_WRITE);
  fstree_lock.WriteLock(key);

  int ret = 0;
  if (handle != NULL) {
    const tfs_inode_header *iheader = GetInodeHeader(handle->value_);

    if (iheader->has_blob > 0) {
      if (new_size > state_->GetThreshold()) {
        TruncateDiskFile(iheader->fstat.st_ino, new_size);
      } else {
        char* buffer = new char[new_size];
        MigrateDiskFileToBuffer(iheader->fstat.st_ino, buffer, new_size);
        UpdateInlineData(handle->value_, buffer, 0, new_size);
        delete [] buffer;
      }
    } else {
      if (new_size > state_->GetThreshold()) {
        int fd;
        if (MigrateToDiskFile(handle, fd, O_TRUNC|O_WRONLY) == 0) {
          if ((ret = ftruncate(fd, new_size)) == 0) {
            fsync(fd);
          }
          close(fd);
        }
      } else {
        TruncateInlineData(handle->value_, new_size);
      }
    }
    if (new_size != iheader->fstat.st_size) {
      tfs_inode_header new_iheader = *GetInodeHeader(handle->value_);
      new_iheader.fstat.st_size = new_size;
      if (new_size > state_->GetThreshold()) {
        new_iheader.has_blob = 1;
      } else {
        new_iheader.has_blob = 0;
      }
      UpdateInodeHeader(handle->value_, new_iheader);
    }
    inode_cache->WriteBack(handle);
    inode_cache->Release(handle);
  } else {
    ret = -ENOENT;
  }

  fstree_lock.Unlock(key);
  return ret;
}

int TableFS::Readlink(const char *path, char *buf, size_t size) {
  tfs_meta_key_t key;
  if (!PathLookup(path, key)) {
    return FSError("Open: No such file or directory\n");
  }
  fstree_lock.ReadLock(key);
  std::string result;
  int ret = 0;
  if (metadb->Get(key.ToSlice(), result) > 0) {
    size_t data_size = GetInlineData(result, buf, 0, size-1);
    buf[data_size] = '\0';
  } else {
    errno = ENOENT;
    ret = -1;
  }
  fstree_lock.Unlock(key);
  if (ret < 0) {
    return FSError("Open: No such file or directory\n");
  } else {
    return 0;
  }
}

int TableFS::Symlink(const char *target, const char *path) {
  tfs_meta_key_t key;
  leveldb::Slice filename;
  if (!PathLookup(path, key, filename)) {
#ifdef  TABLEFS_DEBUG
    state_->GetLog()->LogMsg("Symlink: %s %s\n", path, target);
#endif
    return FSError("Symlink: No such parent file or directory\n");
  }

  size_t val_size = TFS_INODE_HEADER_SIZE+filename.size()+1+strlen(target);
  char* value = new char[val_size];
  tfs_inode_header* header = reinterpret_cast<tfs_inode_header*>(value);
  InitStat(header->fstat, state_->NewInode(), S_IFLNK, 0);
  header->has_blob = 0;
  header->namelen = filename.size();
  char* name_buffer = value + TFS_INODE_HEADER_SIZE;
  memcpy(name_buffer, filename.data(), filename.size());
  name_buffer[header->namelen] = '\0';
  strncpy(name_buffer+filename.size()+1, target, strlen(target));

  fstree_lock.WriteLock(key);
  metadb->Put(key.ToSlice(), std::string(value, val_size));
  fstree_lock.Unlock(key);
  delete [] value;
  return 0;
}

int TableFS::Unlink(const char *path) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Unlink: %s\n", path);
#endif
  tfs_meta_key_t key;
  if (!PathLookup(path, key)) {
    return FSError("Unlink: No such file or directory\n");
  }

  int ret = 0;
  fstree_lock.WriteLock(key);
  InodeCacheHandle* handle = inode_cache->Get(key, INODE_DELETE);
  if (handle != NULL) {
    const tfs_inode_header *value = GetInodeHeader(handle->value_);
    if (value->fstat.st_size > state_->GetThreshold()) {
      char fpath[128];
      GetDiskFilePath(fpath, value->fstat.st_ino);
      unlink(fpath);
    }
    dentry_cache->Evict(key);
    inode_cache->Release(handle);
    inode_cache->Evict(key);
  } else {
    ret = -ENOENT;
  }
  fstree_lock.Unlock(key);

  return ret;
}

int TableFS::MakeNode(const char *path, mode_t mode, dev_t dev) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("MakeNode: %s\n", path);
#endif
  tfs_meta_key_t key;
  leveldb::Slice filename;
  if (!PathLookup(path, key, filename)) {
    return FSError("MakeNode: No such parent file or directory\n");
  }

  tfs_inode_val_t value =
    InitInodeValue(state_->NewInode(), mode | S_IFREG, dev, filename);

  int ret = 0;
  {
    fstree_lock.WriteLock(key);
    ret=metadb->Put(key.ToSlice(), value.ToSlice());
    fstree_lock.Unlock(key);
  }

  FreeInodeValue(value);

  if (ret == 0) {
    return 0;
  } else {
    errno = ENOENT;
    return FSError("MakeNode failed\n");
  }
}

int TableFS::MakeDir(const char *path, mode_t mode) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("MakeDir: %s\n", path);
#endif
  tfs_meta_key_t key;
  leveldb::Slice filename;
  if (!PathLookup(path, key, filename)) {
    return FSError("MakeDir: No such parent file or directory\n");
  }

  tfs_inode_val_t value =
    InitInodeValue(state_->NewInode(), mode | S_IFDIR, 0, filename);

  int ret = 0;
  {
    fstree_lock.WriteLock(key);
    ret=metadb->Put(key.ToSlice(), value.ToSlice());
    fstree_lock.Unlock(key);
  }

  FreeInodeValue(value);

  if (ret == 0) {
    return 0;
  } else {
    errno = ENOENT;
    return FSError("MakeDir failed\n");
  }
}

int TableFS::OpenDir(const char *path, struct fuse_file_info *fi) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("OpenDir: %s\n", path);
#endif
  tfs_meta_key_t key;
  std::string inode;
  if (!PathLookup(path, key)) {
    return FSError("OpenDir: No such file or directory\n");
  }
  fstree_lock.ReadLock(key);
  InodeCacheHandle* handle =
    inode_cache->Get(key, INODE_READ);
  if (handle != NULL) {
    fi->fh = (uint64_t) handle;
    fstree_lock.Unlock(key);
    return 0;
  } else {
    fstree_lock.Unlock(key);
    return FSError("OpenDir: No such file or directory\n");
  }
}

int TableFS::ReadDir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t offset, struct fuse_file_info *fi) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("ReadDir: %s\n", path);
#endif
  InodeCacheHandle* phandle = (InodeCacheHandle *) fi->fh;
  tfs_meta_key_t childkey;
  int ret = 0;
  tfs_inode_t child_inumber = GetAttribute(phandle->value_)->st_ino;
  BuildMetaKey(child_inumber,
              (child_inumber == ROOT_INODE_ID) ? 1 : 0,
              childkey);
  LevelDBIterator* iter = metadb->GetNewIterator();
  if (filler(buf, ".", NULL, 0) < 0) {
    return FSError("Cannot read a directory");
  }
  if (filler(buf, "..", NULL, 0) < 0) {
    return FSError("Cannot read a directory");
  }
  for (iter->Seek(childkey.ToSlice());
       iter->Valid() && IsKeyInDir(iter->key(), childkey);
       iter->Next()) {
    const char* name_buffer = iter->value().data() + TFS_INODE_HEADER_SIZE;
    if (filler(buf, name_buffer, NULL, 0) < 0) {
      ret = -1;
    }
    if (ret < 0) {
      break;
    }
  }
  delete iter;
  return ret;
}

int TableFS::ReleaseDir(const char *path, struct fuse_file_info *fi) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("ReleaseDir: %s\n", path);
#endif
  InodeCacheHandle* handle = (InodeCacheHandle *) fi->fh;
  tfs_meta_key_t key = handle->key_;
  inode_cache->Release(handle);
  return 0;
}

int TableFS::RemoveDir(const char *path) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("RemoveDir: %s\n", path);
#endif

  //TODO: Do we need to scan the whole table and delete the item?
  tfs_meta_key_t key;
  std::string inode;
  if (!PathLookup(path, key)) {
    return FSError("RemoveDir: No such file or directory\n");
  }
  int ret = 0;
  fstree_lock.WriteLock(key);
  InodeCacheHandle* handle = inode_cache->Get(key, INODE_DELETE);
  if (handle != NULL) {
    dentry_cache->Evict(key);
    inode_cache->Release(handle);
    inode_cache->Evict(key);
  } else {
    ret = -ENOENT;
  }
  fstree_lock.Unlock(key);
  return ret;
}

int TableFS::Rename(const char *old_path, const char *new_path) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Rename: %s %s\n", old_path, new_path);
#endif
  tfs_meta_key_t old_key;
  if (!PathLookup(old_path, old_key)) {
    return FSError("No such file or directory\n");
  }
  tfs_meta_key_t new_key;
  leveldb::Slice filename;
  if (!PathLookup(new_path, new_key, filename)) {
    return FSError("No such file or directory\n");
  }

#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Rename old_key: [%08x, %08x]\n", old_key.inode_id, old_key.hash_id);
  state_->GetLog()->LogMsg("Rename new_key: [%08x, %08x]\n", new_key.inode_id, new_key.hash_id);
#endif

  tfs_meta_key_t large_key;
  tfs_meta_key_t small_key;
  if (old_key.inode_id > new_key.inode_id ||
      (old_key.inode_id == new_key.inode_id) &&
      (old_key.hash_id > new_key.hash_id)) {
    large_key = old_key;
    small_key = new_key;
  } else {
    large_key = new_key;
    small_key = old_key;
  }

  fstree_lock.WriteLock(large_key);
  fstree_lock.WriteLock(small_key);

  int ret = 0;
  InodeCacheHandle* old_handle = inode_cache->Get(old_key, INODE_DELETE);
  if (old_handle != NULL) {
    const tfs_inode_header* old_iheader = GetInodeHeader(old_handle->value_);

    std::string new_value = InitInodeValue(old_handle->value_, filename);
    InodeCacheHandle* new_handle = inode_cache->Insert(new_key, new_value);

    inode_cache->BatchCommit(old_handle, new_handle);
    inode_cache->Release(new_handle);
    inode_cache->Release(old_handle);
    inode_cache->Evict(old_key);
    dentry_cache->Evict(old_key);
    dentry_cache->Evict(new_key);
  } else {
    ret = -ENOENT;
  }
  fstree_lock.Unlock(small_key);
  fstree_lock.Unlock(large_key);
  return ret;
}

int TableFS::Access(const char *path, int mask) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("Access: %s %08x\n", path, mask);
#endif
  //TODO: Implement Access
  return 0;
}

int TableFS::UpdateTimens(const char *path, const struct timespec tv[2]) {
#ifdef  TABLEFS_DEBUG
  state_->GetLog()->LogMsg("UpdateTimens: %s\n", path);
#endif
  tfs_meta_key_t key;
  if (!PathLookup(path, key)) {
    return FSError("No such file or directory\n");
  }
  int ret = 0;
  fstree_lock.WriteLock(key);
  InodeCacheHandle* handle = inode_cache->Get(key, INODE_WRITE);
  if (handle != NULL) {
    {
      const tfs_stat_t *value = GetAttribute(handle->value_);
      tfs_stat_t new_value = *value;
      new_value.st_atim.tv_sec = tv[0].tv_sec;
      new_value.st_mtim.tv_sec = tv[1].tv_sec;
      UpdateAttribute(handle->value_, new_value);
      inode_cache->WriteBack(handle);
      inode_cache->Release(handle);
    }
  } else {
    ret = -ENOENT;
  }
  fstree_lock.Unlock(key);
  return ret;
}

int TableFS::Chmod(const char *path, mode_t mode) {
  tfs_meta_key_t key;
  if (!PathLookup(path, key)) {
    return FSError("No such file or directory\n");
  }
  int ret = 0;
  fstree_lock.WriteLock(key);
  InodeCacheHandle* handle = inode_cache->Get(key, INODE_WRITE);
  if (handle != NULL) {
    const tfs_stat_t *value = GetAttribute(handle->value_);
    tfs_stat_t new_value = *value;
    new_value.st_mode = mode;
    UpdateAttribute(handle->value_, new_value);
    inode_cache->WriteBack(handle);
    inode_cache->Release(handle);
  } else {
    ret = -ENOENT;
  }
  fstree_lock.Unlock(key);
  return ret;
}

int TableFS::Chown(const char *path, uid_t uid, gid_t gid) {
  tfs_meta_key_t key;
  if (!PathLookup(path, key)) {
    return FSError("No such file or directory\n");
  }
  int ret = 0;
  fstree_lock.WriteLock(key);
  InodeCacheHandle* handle = inode_cache->Get(key, INODE_WRITE);
  if (handle != NULL) {
    const tfs_stat_t *value = GetAttribute(handle->value_);
    tfs_stat_t new_value = *value;
    new_value.st_uid = uid;
    new_value.st_gid = gid;
    UpdateAttribute(handle->value_, new_value);
    inode_cache->WriteBack(handle);
    inode_cache->Release(handle);
    return 0;
  } else {
    ret = -ENOENT;
  }
  fstree_lock.Unlock(key);
  return ret;
}

bool TableFS::GetStat(std::string stat, std::string* value) {
  return state_->GetMetaDB()->GetStat(stat, value);
}

void TableFS::Compact() {
  state_->GetMetaDB()->Compact();
}

}
