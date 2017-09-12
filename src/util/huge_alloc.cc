#include "huge_alloc.h"
#include <iostream>
#include "util/logger.h"

namespace ERpc {

HugeAlloc::HugeAlloc(size_t initial_size, size_t numa_node,
                     Transport::reg_mr_func_t reg_mr_func,
                     Transport::dereg_mr_func_t dereg_mr_func)
    : numa_node(numa_node),
      reg_mr_func(reg_mr_func),
      dereg_mr_func(dereg_mr_func) {
  assert(numa_node <= kMaxNumaNodes);

  if (initial_size < kMaxClassSize) initial_size = kMaxClassSize;
  prev_allocation_size = initial_size;

  // Reserve initial hugepages. This throws exception if reservation fails.
  reserve_hugepages(initial_size, numa_node);
}

HugeAlloc::~HugeAlloc() {
  // Deregister and delete the created SHM regions
  for (shm_region_t &shm_region : shm_list) {
    dereg_mr_func(shm_region.mem_reg_info);
    delete_shm(shm_region.shm_key, shm_region.buf);
  }
}

// To create a cache of Buffers, we first allocate the required number of
// Buffers and then free them.
bool HugeAlloc::create_cache(size_t size, size_t num_buffers) {
  size_t size_class = get_class(size);
  size_t reqd_buffers = num_buffers - freelist[size_class].size();
  if (reqd_buffers <= 0) return true;

  std::vector<Buffer> free_buffer_vec;

  for (size_t i = 0; i < reqd_buffers; i++) {
    Buffer buffer = alloc(size);
    if (buffer.buf == nullptr) return false;

    free_buffer_vec.push_back(buffer);
  }

  for (size_t i = 0; i < reqd_buffers; i++) {
    free_buf(free_buffer_vec[i]);
  }

  return true;
}

void HugeAlloc::print_stats() {
  fprintf(stderr, "eRPC HugeAlloc stats:\n");
  fprintf(stderr, "Total reserved SHM = %zu bytes (%.2f MB)\n",
          stats.shm_reserved, 1.0 * stats.shm_reserved / MB(1));
  fprintf(stderr, "Total memory allocated to user = %zu bytes (%.2f MB)\n",
          stats.user_alloc_tot, 1.0 * stats.user_alloc_tot / MB(1));

  fprintf(stderr, "%zu SHM regions\n", shm_list.size());
  size_t shm_region_index = 0;
  for (shm_region_t &shm_region : shm_list) {
    fprintf(stderr, "Region %zu, size %zu MB\n", shm_region_index,
            shm_region.size / MB(1));
    shm_region_index++;
  }

  fprintf(stderr, "Size classes:\n");
  for (size_t i = 0; i < kNumClasses; i++) {
    size_t class_size = class_max_size(i);
    if (class_size < KB(1)) {
      fprintf(stderr, "\t%zu B: %zu Buffers\n", class_size, freelist[i].size());
    } else if (class_size < MB(1)) {
      fprintf(stderr, "\t%zu KB: %zu Buffers\n", class_size / KB(1),
              freelist[i].size());
    } else {
      fprintf(stderr, "\t%zu MB: %zu Buffers\n", class_size / MB(1),
              freelist[i].size());
    }
  }
}

uint8_t *HugeAlloc::alloc_raw(size_t size, size_t numa_node) {
  std::ostringstream xmsg;  // The exception message
  size = round_up<kHugepageSize>(size);
  int shm_key, shm_id;

  while (true) {
    // Choose a positive SHM key. Negative is fine but it looks scary in the
    // error message.
    shm_key = static_cast<int>(slow_rand.next_u64());
    shm_key = std::abs(shm_key);

    // Try to get an SHM region
    shm_id = shmget(shm_key, size, IPC_CREAT | IPC_EXCL | 0666 | SHM_HUGETLB);

    if (shm_id == -1) {
      switch (errno) {
        case EEXIST:
          // shm_key already exists. Try again.
          break;

        case EACCES:
          xmsg << "eRPC HugeAlloc: SHM allocation error. "
               << "Insufficient permissions.";
          throw std::runtime_error(xmsg.str());

        case EINVAL:
          xmsg << "eRPC HugeAlloc: SHM allocation error: SHMMAX/SHMIN "
               << "mismatch. size = " << std::to_string(size) << " ("
               << std::to_string(size / MB(1)) << " MB)";
          throw std::runtime_error(xmsg.str());

        case ENOMEM:
          // Out of memory - this is OK
          LOG_WARN(
              "eRPC HugeAlloc: Insufficient memory. Can't reserve %lu MB\n",
              size / MB(1));
          return nullptr;

        default:
          xmsg << "eRPC HugeAlloc: Unexpected SHM malloc error "
               << strerror(errno);
          throw std::runtime_error(xmsg.str());
      }
    } else {
      // shm_key worked. Break out of the while loop.
      break;
    }
  }

  uint8_t *shm_buf = static_cast<uint8_t *>(shmat(shm_id, nullptr, 0));
  rt_assert(shm_buf != nullptr,
            "eRPC HugeAlloc: shmat() failed. Key = " + std::to_string(shm_key));

  // Bind the buffer to the NUMA node
  const unsigned long nodemask = (1ul << static_cast<unsigned long>(numa_node));
  long ret = mbind(shm_buf, size, MPOL_BIND, &nodemask, 32, 0);
  rt_assert(ret == 0,
            "eRPC HugeAlloc: mbind() failed. Key " + std::to_string(shm_key));

  // If we are here, the allocation succeeded.
  memset(shm_buf, 0, size);

  // Register the allocated buffer. This may throw, which is OK.
  Transport::MemRegInfo reg_info = reg_mr_func(shm_buf, size);

  // Save the SHM region so we can free it later
  shm_list.push_back(shm_region_t(shm_key, shm_buf, size, reg_info));
  stats.shm_reserved += size;
  return shm_buf;
}

bool HugeAlloc::reserve_hugepages(size_t size, size_t numa_node) {
  assert(size >= kMaxClassSize);  // We need at least one max-sized buffer
  uint8_t *shm_buf = alloc_raw(size, numa_node);
  if (shm_buf == nullptr) return false;

  // The caller must hold the allocator lock, so we can peek at shm_list's back
  Transport::MemRegInfo &reg_info = shm_list.back().mem_reg_info;

  // Add Buffers to the largest class
  size_t num_buffers = size / kMaxClassSize;
  assert(num_buffers >= 1);
  for (size_t i = 0; i < num_buffers; i++) {
    uint8_t *buf = shm_buf + (i * kMaxClassSize);
    uint32_t lkey = reg_info.lkey;

    freelist[kNumClasses - 1].push_back(Buffer(buf, kMaxClassSize, lkey));
  }

  return true;
}

void HugeAlloc::delete_shm(int shm_key, const uint8_t *shm_buf) {
  int shmid = shmget(shm_key, 0, 0);
  if (shmid == -1) {
    switch (errno) {
      case EACCES:
        fprintf(stderr,
                "eRPC HugeAlloc: SHM free error: "
                "Insufficient permissions. SHM key = %d.\n",
                shm_key);
        break;
      case ENOENT:
        fprintf(stderr,
                "eRPC HugeAlloc: SHM free error: No such SHM key."
                "SHM key = %d.\n",
                shm_key);
        break;
      default:
        fprintf(stderr,
                "eRPC HugeAlloc: SHM free error: A wild SHM error: "
                "%s\n",
                strerror(errno));
        break;
    }

    exit(-1);
  }

  int ret = shmctl(shmid, IPC_RMID, nullptr);  // Please don't fail
  if (ret != 0) {
    fprintf(stderr, "eRPC HugeAlloc: Error freeing SHM ID %d\n", shmid);
    exit(-1);
  }

  ret = shmdt(static_cast<void *>(const_cast<uint8_t *>(shm_buf)));
  if (ret != 0) {
    fprintf(stderr, "HugeAlloc: Error freeing SHM buf %p. (SHM key = %d)\n",
            shm_buf, shm_key);
    exit(-1);
  }
}
}  // End ERpc
