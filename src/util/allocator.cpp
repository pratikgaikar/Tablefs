/*************************************************************************
* Author: Kai Ren
* Created Time: 2012-02-27 20:41:41
* File Name: ./allocator.cpp
* Description:
 ************************************************************************/

#include "util/allocator.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

namespace tablefs {


static const size_t MemoryPageSize = 128 * 1024 * 1024;

Allocator::Allocator(size_t lock_size) {
  if (lock_size > 0) {
    int num_memory_pages = (lock_size*1024*1024 - 1) / MemoryPageSize + 1;
    for (int i = 0; i < num_memory_pages; ++i) {
      void* page_start;
      int ret;
      if ((ret = posix_memalign(&page_start,sysconf(_SC_PAGESIZE),
                        MemoryPageSize)) != 0) {
        printf("memory allocation failed: need %u more bytes\n",
               lock_size);
        exit(1);
      }
      if (mlock(page_start, MemoryPageSize) != 0) {
        perror("memory locking failed");
        exit(1);
      }
      blocks.push_back((char *) page_start);
    }
    current_block = 0;
    remaining_bytes = MemoryPageSize;
    flag_using_lock_memory = true;
  } else {
    current_block = 0;
    remaining_bytes = 0;
    flag_using_lock_memory = false;
  }
}

Allocator::~Allocator() {
  if (flag_using_lock_memory) {
    for (size_t i = 0; i < blocks.size(); i++) {
      free(blocks[i]);
    }
  } else {
    for (size_t i = 0; i < blocks.size(); i++) {
      delete [] blocks[i];
    }
  }
}

void* Allocator::Allocate(size_t bytes) {
  if (bytes > MemoryPageSize) {
    fprintf(stderr, "ask for too much memory\n");
    exit(1);
  }
  if (flag_using_lock_memory) {
    if (remaining_bytes < bytes) {
      if (current_block + 1 < blocks.size()) {
        current_block ++;
        remaining_bytes = MemoryPageSize;
      } else {
        char* page_start = new char[bytes];
        blocks.push_back(page_start);
        remaining_bytes = 0;
        return (void *) page_start;
      }
    }
    char* alloc_addr = blocks[current_block]+
                       (MemoryPageSize - remaining_bytes);
    remaining_bytes -= bytes;
    return (void *) alloc_addr;
  } else {
    char* page_start = new char[bytes];
    blocks.push_back(page_start);
    return (void *) page_start;
  }
}

} // namespace tablefs
