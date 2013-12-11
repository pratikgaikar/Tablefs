#include "fs/tfs_state.h"
#include <unistd.h>

namespace tablefs {

FileSystemState::FileSystemState() :
  metadb(NULL),  max_inode_num(0), threshold_(0), logs(NULL) {

}

FileSystemState::~FileSystemState() {
}

int FileSystemState::Setup(Properties& prop) {
  char resolved_path[4096];
  char* ret;
  ret = realpath(prop.getProperty("metadir").c_str(), resolved_path);
  metadir_ = std::string(resolved_path);
  ret = realpath(prop.getProperty("datadir").c_str(), resolved_path);
  datadir_ = std::string(resolved_path);
  ret = realpath(prop.getProperty("mountdir").c_str(), resolved_path);
  mountdir_= std::string(resolved_path);

  threshold_ = prop.getPropertyInt("threshold", 4096);
  if (access(datadir_.c_str(), W_OK)>0 || access(metadir_.c_str(), W_OK)>0) {
     fprintf(stderr, "cannot open directory!\n");
     exit(1);
  }

  logs = new Logging(prop.getProperty("logfile", ""));
  logs->SetDefault(logs);
  logs->Open();

  Properties prop_ = prop;
  prop_.setProperty("leveldb.db", metadir_+std::string("/meta"));
  prop_.setProperty("leveldb.create.if.missing.db", "true");

  metadb = new LevelDBAdaptor();
  metadb->SetProperties(prop_);
  metadb->SetLogging(logs);
  if (metadb->Init() < 0) {
    printf("failed to open metadb %s\n", prop_.getProperty("leveldb.db").c_str());
    return -1;
  } else {
    printf("open metadb successfully %s\n", metadir_.c_str());
  }

  logs->LogMsg("Initialized two databases.\n");

  char fpath[256];
  sprintf(fpath, "%s/root.dat", datadir_.data());
  FILE *f = fopen(fpath, "r");
  if (f == NULL) {
    f = fopen(fpath, "w");
    max_inode_num = 0;
    fprintf(f, "%u\n", max_inode_num);
    fclose(f);
    char fpath[512];
    sprintf(fpath, "%s/%d", datadir_.data(), 0);
    mkdir(fpath, 0777);
  } else {
    if (fscanf(f, "%u", &max_inode_num) == 0) {
      max_inode_num = 0;
    }
    fclose(f);
  }

  return 0;
}

void FileSystemState::Destroy() {
  char fpath[256];
  sprintf(fpath, "%s/root.dat", datadir_.data());
  FILE* f = fopen(fpath, "w");
  if (f != NULL) {
    fprintf(f, "%u\n", max_inode_num);
    fclose(f);
    logs->LogMsg("fpath: %s\n", fpath);
  } else {
    logs->LogMsg("Cannot write the max inode num: %s %s\n", 
                 fpath, strerror(errno));
  }
  if (metadb != NULL) {
    metadb->Cleanup();
    delete metadb;
  }
  if (logs != NULL)
    delete logs;
}

tfs_inode_t FileSystemState::NewInode() {
  ++max_inode_num;
  if (max_inode_num % (NUM_FILES_IN_DATADIR) == 0) {
    char fpath[512];
    sprintf(fpath, "%s/%d", datadir_.data(),
            (int) max_inode_num >> 14);
    mkdir(fpath, 0777);
  }
  return max_inode_num;
}

}
