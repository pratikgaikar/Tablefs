#ifndef ALLOCATOR_H_
#define ALLOCATOR_H_
#include <string>
#include <vector>
#include <stdint.h>
#include <assert.h>

namespace tablefs {

class Allocator {
private:
  std::vector<char*> blocks;
  size_t remaining_bytes;
  size_t current_block;
  bool flag_using_lock_memory;

public:
  Allocator(size_t lock_size);

  ~Allocator();

  void* Allocate(size_t bytes);
};

}

#endif /* ALLOCATOR_H_ */
