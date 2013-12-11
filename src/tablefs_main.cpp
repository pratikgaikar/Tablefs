#define FUSE_USE_VERSION 26

#include <fuse.h>
#include "fs/tfs_state.h"
#include "fs/tablefs.h"
#include "util/properties.h"
#include "leveldb/env.h"

static void usage()
{
    fprintf(stderr,
            "USAGE:  tablefs <FUSEmount> <threshold> <METADIR> <DATADIR> <LOGFILE>\n");
    abort();
}

static tablefs::TableFS *fs;

int wrap_getattr(const char *path, struct stat *statbuf) {
  return fs->GetAttr(path, statbuf);
}
int wrap_readlink(const char *path, char *link, size_t size) {
  return fs->Readlink(path, link, size);
}
int wrap_mknod(const char *path, mode_t mode, dev_t dev) {
  return fs->MakeNode(path, mode, dev);
}
int wrap_mkdir(const char *path, mode_t mode) {
  return fs->MakeDir(path, mode);
}
int wrap_unlink(const char *path) {
  return fs->Unlink(path);
}
int wrap_rmdir(const char *path) {
  return fs->RemoveDir(path);
}
int wrap_symlink(const char *path, const char *link) {
  return fs->Symlink(path, link);
}
int wrap_rename(const char *path, const char *newpath) {
  return fs->Rename(path, newpath);
}
/*
int wrap_link(const char *path, const char *newpath) {
  return fs->Link(path, newpath);
}
*/
int wrap_chmod(const char *path, mode_t mode) {
  return fs->Chmod(path, mode);
}
int wrap_chown(const char *path, uid_t uid, gid_t gid) {
  return fs->Chown(path, uid, gid);
}
int wrap_truncate(const char *path, off_t newSize) {
  return fs->Truncate(path, newSize);
}
int wrap_open(const char *path, struct fuse_file_info *fileInfo) {
  return fs->Open(path, fileInfo);
}
int wrap_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fileInfo) {
  return fs->Read(path, buf, size, offset, fileInfo);
}
int wrap_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fileInfo) {
  return fs->Write(path, buf, size, offset, fileInfo);
}
int wrap_release(const char *path, struct fuse_file_info *fileInfo) {
  return fs->Release(path, fileInfo);
}
/*
int wrap_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
  return fs->Fsync(path, datasync, fi);
}
*/
int wrap_opendir(const char *path, struct fuse_file_info *fileInfo) {
  return fs->OpenDir(path, fileInfo);
}
int wrap_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fileInfo) {
  return fs->ReadDir(path, buf, filler, offset, fileInfo);
}
int wrap_releasedir(const char *path, struct fuse_file_info *fileInfo) {
  return fs->ReleaseDir(path, fileInfo);
}
void* wrap_init(struct fuse_conn_info *conn) {
  return fs->Init(conn);
}
int wrap_access(const char *path, int mask) {
  return fs->Access(path, mask);
}
int wrap_utimens(const char *path, const struct timespec tv[2]) {
  return fs->UpdateTimens(path, tv);
}
void wrap_destroy(void * data) {
  fs->Destroy(data);
}

static struct fuse_operations tablefs_operations;

int main(int argc, char *argv[])
{
  tablefs::Properties prop;
  prop.parseOpts(argc, argv);

  std::string mountdir = prop.getProperty("mountdir");
  std::string datadir = prop.getProperty("datadir");
  std::string metadir = prop.getProperty("metadir");

  leveldb::Env* env = leveldb::Env::Default();
  if (!env->FileExists(mountdir) || !env->FileExists(datadir) || !env->FileExists(metadir)) {
      fprintf(stdout, "Some input directories cannot be found.\n");
  }

  int fuse_stat;
  tablefs::FileSystemState *tablefs_data;
  tablefs_data = new tablefs::FileSystemState();
  if (tablefs_data == NULL || tablefs_data->Setup(prop) < 0) {
      fprintf(stdout, "Error allocate tablefs_data: %s\n", strerror(errno));
      return -1;
  }

  char *fuse_argv[20];
  int fuse_argc = 0;
  fuse_argv[fuse_argc++] = argv[0];
  char fuse_mount_dir[100];
  strcpy(fuse_mount_dir, mountdir.c_str());
  fuse_argv[fuse_argc++] = fuse_mount_dir;
//  fuse_argv[fuse_argc++] = "-f";
  fuse_argv[fuse_argc++] = "-s";

  fs = new tablefs::TableFS();
  fs->SetState(tablefs_data);

  tablefs_operations.init       = wrap_init;
  tablefs_operations.getattr    = wrap_getattr;
  tablefs_operations.opendir    = wrap_opendir;
  tablefs_operations.readdir    = wrap_readdir;
  tablefs_operations.releasedir = wrap_releasedir;
  tablefs_operations.mkdir      = wrap_mkdir;
  tablefs_operations.rmdir      = wrap_rmdir;
  tablefs_operations.rename     = wrap_rename;

  tablefs_operations.symlink    = wrap_symlink;
  tablefs_operations.readlink   = wrap_readlink;

  tablefs_operations.open       = wrap_open;
  tablefs_operations.read       = wrap_read;
  tablefs_operations.write      = wrap_write;
  tablefs_operations.mknod      = wrap_mknod;
  tablefs_operations.unlink     = wrap_unlink;
  tablefs_operations.release    = wrap_release;
  tablefs_operations.chmod      = wrap_chmod;
  tablefs_operations.chown      = wrap_chown;

  tablefs_operations.truncate   = wrap_truncate;
  tablefs_operations.access     = wrap_access;
  tablefs_operations.utimens    = wrap_utimens;
  tablefs_operations.destroy    = wrap_destroy;

  fprintf(stdout, "start to run fuse_main at %s %s\n", argv[0], fuse_mount_dir);

  fuse_stat = fuse_main(fuse_argc, fuse_argv, &tablefs_operations, tablefs_data);

  return fuse_stat;
}
