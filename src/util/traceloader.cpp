/*
 * TraceLoader.cpp
 *
 *  Created on: Aug 30, 2011
 *      Author: kair
 */

#include "util/traceloader.h"
#include <cstring>

namespace tablefs {

static const int MemoryPageSize = 1024 * 1024;

void TraceLoader::LoadTrace(const char* filename) {
  FILE* file = fopen(filename, "r");
  if (file == NULL) {
    printf("no trace file\n");
    return;
  }
  char pathname[4096];
  if (fscanf(file, "%ld %ld\n", &num_paths, &num_file_paths) != 2) {
    printf("the format of trace file is wrong: %s\n", filename);
    return;
  }
  paths = (char**) allocator_->Allocate(num_paths * sizeof(char *));
  if (paths == NULL) {
    printf("Cannot allocate memory: %d\n", __LINE__);
    exit(1);
  }
  filetypes = (char *) allocator_->Allocate(num_paths * sizeof(char));
  if (filetypes == NULL) {
    printf("Cannot allocate memory: %d\n", __LINE__);
    exit(1);
  }
  FileRecord rec;
  for (int i = 0; i < num_paths; ++i) {
    int len = 0;
    char c;
    char* target = pathname;
    do {
      c = fgetc(file);
      *target = c;
      len++;
      target++;
    } while (c != '\n' && c != EOF);
    if (c == EOF)
      break;
    pathname[len-3]='\0';
    filetypes[i] = pathname[len-2];
    paths[i] = AddEntry(pathname, len+1);
  }
  num_dir_paths = num_paths - num_file_paths;
  fclose(file);
}

char* TraceLoader::AddEntry(const char *filename, int size) {
  char* new_path = (char *) allocator_->Allocate(size);
  strncpy(new_path, filename, size);
  return new_path;
}

TraceLoader::TraceLoader(Allocator* allocator) :
  allocator_(allocator)
{
  num_paths = 0;
  paths = NULL;
  filetypes = NULL;
}

TraceLoader::~TraceLoader() {
}

char* TraceLoader::GetPath(int index, char &filetype) {
   if (index < num_paths) {
        filetype = filetypes[index];
        return paths[index];
   } else {
        return NULL;
   }
}

char* TraceLoader::GetPath(int index) {
   if (index < num_paths) {
        return paths[index];
   } else {
        return NULL;
   }
}

char* TraceLoader::GetDirPath(int dindex) {
  if (dindex >= 0 && dindex < num_dir_paths) {
    return paths[dindex];
  } else {
    return NULL;
  }
}

char* TraceLoader::GetFilePath(int findex) {
  if (findex >= 0 && findex < num_file_paths) {
    return paths[findex + (int) num_dir_paths];
  } else {
    return NULL;
  }
}

}
