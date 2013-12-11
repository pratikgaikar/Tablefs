#include <vector>
#include <string>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <utime.h>
#include <string.h>
#include <unistd.h>
#include "port/port.h"
#include "leveldb/env.h"
#include "fs/fswrapper.h"
#include "util/monitor.h"
#include "util/allocator.h"
#include "util/traceloader.h"
#include "util/properties.h"
#include "util/command.h"
#include "util/random.h"
#include "util/mutexlock.h"
#include "util/socket.h"

namespace tablefs {

const int MONITOR_INTERVAL = 10000000;

class RandomPicker {
public:
  enum Mode {
    Uniform,
    Skewed,
    Default
  };

  RandomPicker(uint32_t seed, std::string mode, int range) :
    rand(seed), range_(range) {
      if (mode.compare("uniform") == 0) {
        mode_ = Uniform;
      } else if (mode.compare("skewed") == 0) {
        mode_ = Skewed;
      } else {
        mode_ = Uniform;
      }
      if (mode_ == Skewed) {
        max_log_ = 0;
        uint32_t max_exp = 1;
        while (max_exp < range_) {
          max_log_ += 1;
          max_exp = max_exp << 1;
        }
      } else {
        max_log_ = 0;
      }
  }

  uint32_t Next() {
    switch (mode_) {
      case Uniform: return rand.Uniform(range_);
      case Skewed: return rand.Skewed(max_log_) % range_;
      default: return 0;
    }
  }

private:
  leveldb::Random rand;
  Mode mode_;
  int range_;
  int max_log_;
};

struct SharedState {
    leveldb::port::Mutex mu;
    leveldb::port::CondVar cv;
    int total;

    int num_initialized;
    int num_done;
    bool start;
    SharedState() : cv(&mu) {}
};

struct ThreadState {
    int tid;
    int step;
    int op_count_;
    SharedState* shared;

    ThreadState(int index, int numthreads) :
      tid(index), step(numthreads), op_count_(0) { }

    void IncreaseCount() {
      ++op_count_;
    }

    void IncreaseCount(int num_ops) {
      op_count_ += num_ops;
    }

    int GetCount() {
      return op_count_;
    }
};

void SendMetric(const char* metricString) {
  UDPSocket sock;
  try {
    sock.sendTo(metricString, strlen(metricString),
                std::string("127.0.0.1"), 10600);
  } catch (SocketException &e) {
  }
}

class OperationStat: public MetricStat {
private:
  int num_threads;
  ThreadState** targs;
  bool enabled;
  time_t last;

public:
  OperationStat() : enabled(false), num_threads(0), targs(NULL), last(0) {}

  virtual void GetMetric(TMetList &metlist, time_t now) {
    if (enabled) {
      char metname[36] = "Opcount_thread_";
      char* metname_end = metname + strlen(metname);
      int tot_count = 0;
      for (int i = 0; i < num_threads; ++i) {
        sprintf(metname_end, "%08d", i);
        int op_count = targs[i]->GetCount();
        tot_count += op_count;
        AddMetric(metlist, std::string(metname), now, op_count);
      }
      if (now - last > 5) {
        printf("opcount %d\n", tot_count);
        last = now;
        char metricString[128];
        sprintf(metricString, "fsbench.opcount %d %d\n", now, tot_count);
        SendMetric(metricString);
      }
      AddMetric(metlist, std::string("Opcount"), now, tot_count);
    }
  }

  void SetThreadArgs(int n, ThreadState** args) {
    targs = args;
    num_threads = n;
    enabled = true;
  }

  void Disable() {
    targs = NULL;
    num_threads = 0;
    enabled = false;
  }
};

class FileSystemBenchmark {
private:
  Properties prop;

  int mem_lock_size;
  Allocator *allocator;
  TraceLoader *loader;
  int* path_order;

  std::string mon_part;
  std::string mon_fs;
  std::string mon_mount;
  FileSystemWrapper *fs;

  int num_threads;
  std::string benchmarks;

  OperationStat* opstat;
  Monitor* tmon;
  Monitor* emon;
  FILE* logf;


public:

  FileSystemBenchmark(Properties &properties) :
    prop(properties), tmon(NULL), emon(NULL), fs(NULL),
    logf(NULL), allocator(NULL), path_order(NULL) {

    std::string filename = prop.getProperty("monitor_logfile");
    logf = fopen(filename.c_str(), "w");
    if (logf == NULL) {
      fprintf(stderr, "Cannot open monitor logfile!\n");
      exit(1);
    } else {
      printf("open log file: %s\n", filename.c_str());
    }
    prop.Report(logf);

    mem_lock_size = prop.getPropertyInt("mem_lock_size", 400);
    if (mem_lock_size < 0)
      mem_lock_size = 0;
    if (prop.getPropertyBool("loadtrace", true)) {
      allocator = new Allocator(mem_lock_size);
      loader = new TraceLoader(allocator);
      loader->LoadTrace(prop.getProperty("pathfile").c_str());

      int npath = prop.getPropertyInt("create_numpaths");
      path_order = new int[npath];
      path_order[0] = 0;
      for (int i = 1; i < npath; ++i) {
        int j = rand() % i;
        path_order[i] = path_order[j];
        path_order[j] = i;
      }
    } else {
      loader = NULL;
    }

    mon_part = prop.getProperty("mon_partition");
    mon_fs = prop.getProperty("mon_filesystem");
    mon_mount = prop.getProperty("mon_mountpoint");
    if (mon_part.size() == 0 || mon_fs.size() == 0 || mon_mount.size() == 0) {
      fprintf(stderr, "No partition, filesystem, or mount point for monitoring");
      exit(1);
    }

    emon = new Monitor(mon_part);
    tmon = new Monitor();
    opstat = new OperationStat();
    tmon->AddMetricStat(opstat);

    num_threads = prop.getPropertyInt("numthreads", 1);
    benchmarks = prop.getProperty("benchmarks");
    if (benchmarks.size() == 0) {
      fprintf(stderr, "No benchmark is found.");
    }

    std::string benchfs = prop.getProperty("filesystem");
    if (benchfs == "tablefs_user" || benchfs == "tablefs_pred") {
      fs = new TableFSWrapper();
      if (fs->Setup(prop) < 0) {
        fprintf(stderr, "Fail to open tablefs_user\n");
        exit(1);
      }
      prop.setProperty("target", "/");
    } else if (benchfs == "tablefs_debug") {
      fs = new TableFSTestWrapper();
      if (fs->Setup(prop) < 0) {
        fprintf(stderr, "Fail to open tablefs_user\n");
        exit(1);
      }
    } else {
      fs = new FileSystemWrapper();
    }
    emon->AddMetricStat(fs->GetMetricStat());
  }

  ~FileSystemBenchmark() {
    if (logf != NULL) {
      if (tmon != NULL)
        tmon->Report(logf);
      if (emon != NULL)
        emon->Report(logf);
      fclose(logf);
    }
    delete fs;
    if (path_order != NULL)
      delete [] path_order;
    if (tmon != NULL)
      delete tmon;
    if (emon != NULL)
      delete emon;
    if (loader != NULL)
      delete loader;
    if (allocator != NULL)
      delete allocator;
  }

private:
  void MetadataCreate(ThreadState* thread) {
    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    char* end_fpath = fpath + strlen(fpath);
    int cnt = 0;
    int numdirs = loader->GetNumDirectoryPaths();
    printf("number dir %d\n", numdirs);
    for (int i = 0; i < numdirs; ++i) {
      char* path = loader->GetDirPath(i);
      if (path != NULL) {
        strcpy(end_fpath, path);
        if (fs->Mkdir(fpath, S_IRWXU | S_IRWXG | S_IRWXO) < 0) {
          perror(fpath);
        }
        thread->IncreaseCount();
      }
    }
    int numpaths = prop.getPropertyInt("create_numpaths");
    for (int i = thread->tid; i < numpaths; i += thread->step) {
      char* path = loader->GetFilePath(i);
      if (path != NULL) {
        strcpy(end_fpath, path);
        if (fs->Mknod(fpath, S_IRWXU | S_IRWXG | S_IRWXO, 0) < 0) {
          perror(fpath);
          ++cnt;
        }
        thread->IncreaseCount();
      }
    }
    printf("Bad count = %d\n", cnt);
  }

  void MetadataCreateWithCompact(ThreadState* thread) {
    MetadataCreate(thread);
    fs->Compact();
  }

  void MetadataQueryTest(ThreadState* thread) {
    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    char* end_fpath = fpath + strlen(fpath);
    int nquery = prop.getPropertyInt("query_number");
    int ratio = prop.getPropertyInt("readwrite_readratio");
    int npath = prop.getPropertyInt("create_numpaths")
               + loader->GetNumDirectoryPaths();

    struct stat statbuf;
    srand(100);
    for (int i = 0; i < nquery; ++i) {
      int pi = rand() % npath;
      char* path = loader->GetPath(pi);
      if (path != NULL) {
        strcpy(end_fpath, path);
        int qi = rand() % 100;
        int ret;
        if ((qi % 100) < ratio/2) {
          ret = fs->Chmod(fpath, S_IRWXU | S_IRWXG );
        } else
        if ((qi % 100) < ratio) {
          struct utimbuf timbuf;
          timbuf.actime = time(NULL);
          timbuf.modtime = time(NULL);
          ret = fs->Utime(fpath, &timbuf);
        } else {
          ret = fs->Stat(fpath, &statbuf);
        }
        if (ret < 0) {
          printf("Error %s\n", fpath);
        }
        thread->IncreaseCount();
      }
    }
  }

  void OnedirCreate(ThreadState* thread) {
    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    char* end_fpath = fpath + strlen(fpath);
    int numpaths = prop.getPropertyInt("create_numpaths");
    srand(100);
    int bad_count = 0;
    for (int i = thread->tid; i < numpaths; i += thread->step) {
      sprintf(end_fpath, "/f%016d", rand() % numpaths);
      if (fs->Mknod(fpath, S_IRWXU | S_IRWXG | S_IRWXO, 0) < 0) {
        bad_count ++;
      }
      thread->IncreaseCount();
    }
    printf("BadCount = %d\n", bad_count);
  }

  void OnedirQueryTest(ThreadState* thread) {
    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    char* end_fpath = fpath + strlen(fpath);
    int nquery = prop.getPropertyInt("query_number");
    int ratio = prop.getPropertyInt("readwrite_readratio");
    int npath = prop.getPropertyInt("create_numpaths");

    struct stat statbuf;
    srand(100);
    int bad_count = 0;
    for (int i = 0; i < nquery; ++i) {
      int pi = rand() % npath;
      sprintf(end_fpath, "/f%016d", pi);
      int qi = rand() % 100;
      int ret;
      if ((qi % 100) < ratio/2) {
        ret = fs->Chmod(fpath, S_IRWXU | S_IRWXG );
      } else
      if ((qi % 100) < ratio) {
        struct utimbuf timbuf;
        timbuf.actime = time(NULL);
        timbuf.modtime = time(NULL);
        ret = fs->Utime(fpath, &timbuf);
      } else {
        ret = fs->Stat(fpath, &statbuf);
      }
      if (ret < 0) {
        bad_count ++;
      }
      thread->IncreaseCount();
    }
    printf("BadCount = %d\n", bad_count);
  }

  void SmallFileCreate(ThreadState* thread) {
    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    char* end_fpath = fpath + strlen(fpath);
    int len_fpath = strlen(fpath);
    int numpaths = prop.getPropertyInt("create_numpaths") +
                   loader->GetNumDirectoryPaths();
    int content_size = prop.getPropertyInt("filesize");
    char *content = new char[content_size];
    for (int i = 0; i < content_size; ++i)
      content[i] = rand() % 26 + 97;
    for (int i = thread->tid; i < numpaths; i += thread->step) {
      char filetype;
      char* path = loader->GetPath(i, filetype);
      if (path != NULL) {
        strcpy(end_fpath, path);
        if (filetype == 'f') {
          fs->Mknod(fpath, S_IRWXU | S_IRWXG | S_IRWXO, 0);
          int fd = fs->Open(fpath, O_WRONLY);
          if (fd >= 0) {
            fs->Write(fd, content, content_size);
            fs->Close(fd);
          } else {
            perror(fpath);
            return;
          }
        } else
        if (filetype == 'd') {
            if (fs->Mkdir(fpath, S_IRWXU | S_IRWXG | S_IRWXO) < 0)  {
              perror(fpath);
            }
        }
        thread->IncreaseCount();
      }
    }
  }

  void SmallFileQueryTest(ThreadState* thread) {
    size_t content_size = prop.getPropertyInt("filesize");
    char *content = new char[content_size];
    for (int i = 0; i < content_size; ++i)
      content[i] = rand() % 26 + 97;

    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    int len_fpath = strlen(fpath);

    int n = prop.getPropertyInt("query_number");
    int nfilepath = prop.getPropertyInt("create_numpaths");
    int ratio = prop.getPropertyInt("readwrite_readratio", 0);

    srand(100);
    for (int j = 0; j < n; ++j) {
      int pi = rand() % nfilepath;
      char* path = loader->GetFilePath(pi);
      sprintf(fpath+len_fpath, "%s\0", path);

      int qi = rand() % 100;
      if (qi % 100 < ratio) {
        int fd = fs->Open(fpath, O_RDONLY);
        if (fd >= 0) {
          fs->Read(fd, content, content_size);
          fs->Close(fd);
        }
      } else {
        int fd = fs->Open(fpath, O_WRONLY);
        if (fd >= 0) {
          fs->Write(fd, content, content_size);
          fs->Close(fd);
        }
      }
      thread->IncreaseCount();
    }
    delete [] content;
  }

  void DeleteQueryTest(ThreadState* thread) {
    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    char* end_fpath = fpath + strlen(fpath);

    int err_cnt = 0;
    int numquery = prop.getPropertyInt("query_number");
    int numfiles = prop.getPropertyInt("create_numpaths");
    RandomPicker picker(thread->tid+1, "uniform", numfiles);

    for (int i = thread->tid; i < numquery; i += thread->step) {
      char* path = loader->GetFilePath(path_order[i % numfiles]);
      if (path != NULL) {
        strcpy(end_fpath, path);
        if (fs->Unlink(fpath) < 0)  {
          ++err_cnt;
        } else {
          thread->IncreaseCount();
        }
      }
    }
    printf("Error count = %d\n", err_cnt);
  }

  void ScanQueryTest(ThreadState* thread) {
    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    char* end_fpath = fpath + strlen(fpath);
    int err_cnt = 0;
    int numquery = prop.getPropertyInt("query_number");
    int numdirs = loader->GetNumDirectoryPaths();
    RandomPicker picker(thread->tid+1, "uniform", numdirs);
    for (int i = thread->tid; i < numquery; i += thread->step) {
      char* path = loader->GetDirPath(picker.Next());
      if (path != NULL) {
        strcpy(end_fpath, path);
        int ops;
        if ((ops = fs->Listdir(fpath)) < 0)  {
          ++err_cnt;
        } else {
          thread->IncreaseCount(ops);
        }
      }
    }
    printf("Error count = %d\n", err_cnt);
  }

  void LsstatQueryTest(ThreadState* thread) {
    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    char* end_fpath = fpath + strlen(fpath);
    int err_cnt = 0;
    int numquery = prop.getPropertyInt("query_number");
    int numdirs = loader->GetNumDirectoryPaths();
    RandomPicker picker(thread->tid+1, "uniform", numdirs);
    for (int i = thread->tid; i < numquery; i += thread->step) {
      char* path = loader->GetDirPath(picker.Next());
      if (path != NULL) {
        strcpy(end_fpath, path);
        int ops;
        if ((ops = fs->Lsstat(fpath)) < 0)  {
          ++err_cnt;
        } else {
          thread->IncreaseCount(ops);
        }
      }
    }
    printf("Error count = %d\n", err_cnt);
  }

  void ScanfileQueryTest(ThreadState* thread) {
    char fpath[4096];
    sprintf(fpath, "%s/", prop.getProperty("target").c_str());
    char* end_fpath = fpath + strlen(fpath);
    int err_cnt = 0;
    int numquery = prop.getPropertyInt("query_number");
    int numdirs = loader->GetNumDirectoryPaths();
    RandomPicker picker(thread->tid+1, "uniform", numdirs);
    for (int i = thread->tid; i < numquery; i += thread->step) {
      char* path = loader->GetDirPath(picker.Next());
      if (path != NULL) {
        strcpy(end_fpath, path);
        int ops;
        if ((ops = fs->Scanfile(fpath)) < 0)  {
          ++err_cnt;
        } else {
          thread->IncreaseCount(ops);
        }
      }
    }
    printf("Error count = %d\n", err_cnt);
  }

  void RenameQueryTest(ThreadState* thread) {
    char src_path[4096];
    char dst_path[4096];
    sprintf(src_path, "%s/", prop.getProperty("target").c_str());
    sprintf(dst_path, "%s/", prop.getProperty("target").c_str());
    char* end_src_path = src_path + strlen(src_path);
    char* end_dst_path = dst_path + strlen(dst_path);

    int err_cnt = 0;
    int numquery = prop.getPropertyInt("query_number");
    int numdirs = loader->GetNumDirectoryPaths();
    int numfiles = prop.getPropertyInt("create_numpaths");
    int rename_type = prop.getPropertyInt("rename_type", 0);

    RandomPicker dir_picker(thread->tid+1, "uniform", numdirs);

    for (int i = thread->tid; i < numquery; i += thread->step) {
      char* path = loader->GetFilePath(path_order[i % numfiles]);
      if (path != NULL) {
        strcpy(end_src_path, path);
        if (rename_type == 0) {
          strcpy(end_dst_path, path);
          strcpy(dst_path + strlen(dst_path), ".rename");
        } else {
          path = loader->GetDirPath(dir_picker.Next());
          strcpy(end_dst_path, path);
          sprintf(dst_path + strlen(dst_path), "%016d", i);
        }

        if (fs->Rename(src_path, dst_path) < 0)  {
          ++err_cnt;
        } else {
          thread->IncreaseCount();
        }
      }
    }
    printf("Error count = %d\n", err_cnt);
  }

  struct ThreadArg {
      FileSystemBenchmark* fsb;
      SharedState* shared;
      ThreadState* thread;
      void (FileSystemBenchmark::*method) (ThreadState*);
  };

  static void ThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    ThreadState* thread = arg->thread;
    {
      leveldb::MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    (arg->fsb->*(arg->method))(thread);

     {
      leveldb::MutexLock l(&shared->mu);
      shared->num_done++;
      if (shared->num_done >= shared->total) {
        shared->cv.SignalAll();
      }
    }
  }

  static void MonitorThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    {
      leveldb::MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    int count = 0;
    int alpha = 1;
    while (shared->num_done < shared->total - 1) {
      arg->fsb->tmon->DoMonitor();
      count ++;
      if (count > 10000) {
        alpha ++;
        count = 0;
      }
      leveldb::Env::Default()->SleepForMicroseconds(MONITOR_INTERVAL * alpha);
    }
    arg->fsb->tmon->DoMonitor();
    shared->num_done ++;
    shared->cv.SignalAll();
  }

  void RunBenchmark(int n,
                    void (FileSystemBenchmark::*method)(ThreadState*)) {
    SharedState shared;
    shared.total = n+1;
    shared.num_initialized = 0;
    shared.num_done = 0;
    shared.start = false;

    ThreadArg* arg = new ThreadArg[n+1];
    ThreadState** thread_states = new ThreadState*[n];
    for (int i = 0; i < n; i++) {
      arg[i].fsb = this;
      arg[i].method = method;
      arg[i].shared = &shared;
      arg[i].thread = new ThreadState(i, n);
      thread_states[i] = arg[i].thread;
      arg[i].thread->shared = &shared;
    }
    arg[n].fsb = this;
    arg[n].shared = &shared;
    opstat->SetThreadArgs(n, thread_states);

    for (int i = 0; i < n; i++) {
      leveldb::Env::Default()->StartThread(ThreadBody, &arg[i]);
    }
    //TODO: Add monitor thread (change n to n + 1, < n to <=n )
    leveldb::Env::Default()->StartThread(MonitorThreadBody, &arg[n]);

    shared.mu.Lock();
    while (shared.num_initialized <= n) {
      shared.cv.Wait();
    }


    emon->DoMonitor();
    tmon->DoMonitor();

    char metricString[128];
    sprintf(metricString, "fsbench.stage %d 100\n", time(NULL));
    SendMetric(metricString);

    shared.start = true;
    shared.cv.SignalAll();
    while (shared.num_done <= n) {
      shared.cv.Wait();
    }
    shared.mu.Unlock();

    sprintf(metricString, "fsbench.stage %d 200\n", time(NULL));
    SendMetric(metricString);

    emon->DoMonitor();
    tmon->DoMonitor();

    opstat->Disable();
    for (int i = 0; i < n; i++) {
      delete arg[i].thread;
    }
    delete[] arg;
    delete[] thread_states;
  }

public:
  void Run() {
    size_t lastbench = 0;
    while (lastbench < benchmarks.size()) {
      size_t sep = benchmarks.find(',', lastbench);
      std::string bench;
      if (sep == std::string::npos) {
        bench = benchmarks.substr(lastbench, benchmarks.size() - lastbench);
        lastbench = benchmarks.size();
      } else {
        bench = benchmarks.substr(lastbench, sep - lastbench);
        lastbench = sep + 1;
      }

      void (FileSystemBenchmark::*method)(ThreadState*) = NULL;
      if (bench == std::string("metadatacreate")) {
        method = &FileSystemBenchmark::MetadataCreate;
      } else if (bench == std::string("metadatacreatecompact")) {
        method = &FileSystemBenchmark::MetadataCreateWithCompact;
      } else if (bench == std::string("metadataquery")) {
        method = &FileSystemBenchmark::MetadataQueryTest;
      } else if (bench == std::string("onedircreate")) {
        method = &FileSystemBenchmark::OnedirCreate;
      } else if (bench == std::string("onedirquery")) {
        method = &FileSystemBenchmark::OnedirQueryTest;
      }else if (bench == std::string("smallfilecreate")) {
        method = &FileSystemBenchmark::SmallFileCreate;
      } else if (bench == std::string("smallfilequery")) {
        method = &FileSystemBenchmark::SmallFileQueryTest;
      } else if (bench == std::string("scanquery")) {
        method = &FileSystemBenchmark::ScanQueryTest;
      } else if (bench == std::string("lsstatquery")) {
        method = &FileSystemBenchmark::LsstatQueryTest;
      } else if (bench == std::string("scanfilequery")) {
        method = &FileSystemBenchmark::ScanfileQueryTest;
      } else if (bench == std::string("renamequery")) {
        method = &FileSystemBenchmark::RenameQueryTest;
      } else if (bench == std::string("deletequery")) {
        method = &FileSystemBenchmark::DeleteQueryTest;
      } else {
        fprintf(stderr, "Cannot find the benchmark %s", bench.c_str());
        continue;
      }

      time_t start = time(NULL);
      RunBenchmark(num_threads, method);
      time_t end = time(NULL);

      fprintf(logf, "%s: %ld\n", bench.c_str(), end-start);

      sync();
      command::DropBufferCache();
    }
  }
};

}  // namespace leveldb

int main(int argc, char *argv[]) {
  tablefs::Properties prop;
  prop.parseOpts(argc, argv);
  std::string config_filename = prop.getProperty("configfile");
  if (config_filename.size() > 0) {
    prop.load(config_filename);
  }
  tablefs::FileSystemBenchmark bench = tablefs::FileSystemBenchmark(prop);
  bench.Run();
  return 0;
}

