/*
 * TraceLoader.h
 *
 *  Created on: Aug 30, 2011
 *      Author: kair
 */

#ifndef TRACELOADER_H_
#define TRACELOADER_H_
#include <string>
#include <vector>
#include "util/allocator.h"

namespace tablefs {

struct FileRecord {
  char* filename;
  char filetype;
};

class TraceLoader {
private:
  char** paths;
  char* filetypes;
  Allocator* allocator_;
  size_t num_paths;
  size_t num_dir_paths;
  size_t num_file_paths;
  char* AddEntry(const char* filename, int size);

public:
  TraceLoader(Allocator* allocator);

  virtual ~TraceLoader();

  void LoadTrace(const char* filename);

  int GetNumPaths() {
    return num_paths;
  }

  int GetNumFilePaths() {
    return num_file_paths;
  }

  int GetNumDirectoryPaths() {
    return num_paths - num_file_paths;
  }

  char* GetPath(int index, char &filetype);

  char* GetPath(int index);

  char* GetDirPath(int dindex);

  char* GetFilePath(int findex);
};

}

#endif /* TRACELOADER_H_ */
