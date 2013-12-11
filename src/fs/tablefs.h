#ifndef TABLE_FS_H
#define TABLE_FS_H

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include "fs/tfs_state.h"
#include "fs/inodemutex.h"
#include "fs/dcache.h"
#include "fs/icache.h"

namespace tablefs {

class TableFS {
public:
  ~TableFS() {
  }

  void SetState(FileSystemState* state);

  void* Init(struct fuse_conn_info *conn);

  void Destroy(void * data);

  int GetAttr(const char *path, struct stat *statbuf);

  int Open(const char *path, struct fuse_file_info *fi);

  int Read(const char* path, char *buf, size_t size,
           off_t offset, struct fuse_file_info *fi);

  int Write(const char* path, const char *buf, size_t size,
            off_t offset, struct fuse_file_info *fi);

  int Truncate(const char *path, off_t offset);

  int Fsync(const char *path, int datasync, struct fuse_file_info *fi);

  int Release(const char *path, struct fuse_file_info *fi);

  int Readlink(const char *path, char *buf, size_t size);

  int Symlink(const char *target, const char *path);

  int Unlink(const char *path);

  int MakeNode(const char *path, mode_t mode, dev_t dev);

  int MakeDir(const char *path, mode_t mode);

  int OpenDir(const char *path, struct fuse_file_info *fi);

  int ReadDir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi);

  int ReleaseDir(const char *path, struct fuse_file_info *fi);

  int RemoveDir(const char *path);

  int Rename(const char *new_path, const char *old_path);

  int Access(const char *path, int mask);

  int UpdateTimens(const char *path, const struct timespec tv[2]);

  int Chmod(const char *path, mode_t mode);

  int Chown(const char *path, uid_t uid, gid_t gid);

  void Compact();

  bool GetStat(std::string stat, std::string* value);

private:
  FileSystemState *state_;
  LevelDBAdaptor* metadb;
  InodeCache *inode_cache;
  DentryCache *dentry_cache;
  InodeMutex fstree_lock;
  bool flag_fuse_enabled;

  inline int FSError(const char *error_message);

  inline void DeleteDBFile(tfs_inode_t inode_id, int filesize);

  inline void GetDiskFilePath(char *path, tfs_inode_t inode_id);

  inline int OpenDiskFile(const tfs_inode_header* iheader, int flags);

  inline int TruncateDiskFile(tfs_inode_t inode_id, off_t new_size);

  inline ssize_t MigrateDiskFileToBuffer(tfs_inode_t inode_it,
                                         char* buffer, size_t size);

  int MigrateToDiskFile(InodeCacheHandle* handle, int &fd, int flags);

  inline void CloseDiskFile(int& fd_);

  inline void InitStat(struct stat &statbuf,
                       tfs_inode_t inode,
                       mode_t mode,
                       dev_t dev);

  tfs_inode_val_t InitInodeValue(tfs_inode_t inum,
                                 mode_t mode,
                                 dev_t dev,
                                 leveldb::Slice filename);

  std::string InitInodeValue(const std::string& old_value,
                             leveldb::Slice filename);

  void FreeInodeValue(tfs_inode_val_t &ival);

  bool ParentPathLookup(const char* path,
                        tfs_meta_key_t &key,
                        tfs_inode_t &inode_in_search,
                        const char* &lastdelimiter);

  inline bool PathLookup(const char *path,
                         tfs_meta_key_t &key,
                         leveldb::Slice &filename);

  inline bool PathLookup(const char *path,
                         tfs_meta_key_t &key);

  friend class TableFSTestWrapper;
};

}
#endif
