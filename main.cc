#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <vector>

#include "logging.h"
#include "userfaultfd.h"

namespace {

const int kPageSize = sysconf(_SC_PAGE_SIZE);

class Shmem {
 public:
  Shmem(size_t size)
      : size_(size), memfd_(memfd_create("userfaultfd-memfd", /*flags=*/0)) {
    // `size_` should be a multiple of the page size to keep things simple.
    CHECK_EQ(size_ % kPageSize, 0);
    CHECK_GE(memfd_, 0);
    // Extend the memfd anonymous file to have a size equal to `size_`.
    CHECK_EQ(ftruncate(memfd_, size_), 0);
  }

  ~Shmem() {
    for (void* region : regions_) {
      CHECK_EQ(munmap(region, size_), 0);
    }
    CHECK_EQ(close(memfd_), 0);
  }

  size_t size() const { return size_; }

  void* GetNewRegion() {
    void* region = mmap(/*addr=*/nullptr, size_, PROT_READ | PROT_WRITE,
                        /*flags=*/MAP_SHARED, memfd_, /*offset=*/0);
    CHECK_NE(region, MAP_FAILED);
    regions_.push_back(region);
    return region;
  }

 private:
  // The size of the memfd anonymous file.
  const size_t size_;
  // The memfd.
  const int memfd_;

  // The regions mmap'd for the memfd.
  std::vector<void*> regions_;
};

struct Init {
  long uffd;
  void* region0;
  void* region1;
};

volatile Init init = {.uffd = -1, .region0 = nullptr, .region1 = nullptr};

void* FaultHandlerThread(void* arg) {
  // It is safe to access `init` because thread creation is a synchronizing
  // event.
  long uffd = init.uffd;
  CHECK_NE(uffd, -1);
  char* region0 = static_cast<char*>(init.region0);
  CHECK_NE(region0, nullptr);
  char* region1 = static_cast<char*>(init.region1);
  CHECK_NE(region1, nullptr);
  CHECK_NE(region0, region1);

  pollfd pollfd;
  pollfd.fd = uffd;
  pollfd.events = POLLIN;
  uffd_msg msg;
  while (true) {
    int num_ready = poll(&pollfd, 1, -1);
    CHECK_NE(num_ready, -1);

    int num_read = read(uffd, &msg, sizeof(msg));
    CHECK_EQ(num_read, sizeof(msg));
    CHECK_EQ(msg.event, UFFD_EVENT_PAGEFAULT);

    printf("UFFD_EVENT_PAGEFAULT event: ");
    printf("flags = %llx; ", msg.arg.pagefault.flags);
    printf("address = %llx\n", msg.arg.pagefault.address);

    uint64_t offset =
        msg.arg.pagefault.address - reinterpret_cast<uint64_t>(region1);
    CHECK_EQ(offset % kPageSize, 0);
    char* page = region0 + offset;
    page[0] = 'c' + (offset / kPageSize);

    uffdio_continue uffdio_continue;
    uffdio_continue.range.start = msg.arg.pagefault.address;
    uffdio_continue.range.len = kPageSize;
    CHECK_NE(ioctl(uffd, UFFDIO_CONTINUE, &uffdio_continue), -1);
  }

  return nullptr;
}

}  // namespace

int main(int argc, char* argv[]) {
  constexpr size_t kNumPages = 10;
  const size_t kShmemSize = kPageSize * kNumPages;
  Shmem shmem(/*size=*/kShmemSize);
  char* region0 = static_cast<char*>(shmem.GetNewRegion());
  char* region1 = static_cast<char*>(shmem.GetNewRegion());
  CHECK_NE(region0, region1);
  printf("region0: %p, region1: %p\n", region0, region1);

  // Important: We must touch each page in `region0` first before accessing
  // `region1` below. Touching each page causes each page to be allocated and
  // mappings to be created for it in both `region0` and `region1` (which, as a
  // reminder, are both virtual addresses). Minor mode userfaultfd page faults
  // do not seem to be generated for `region1` if we do not touch each page
  // first.
  for (size_t i = 0; i < kNumPages; i++) {
    char* ch = region0 + (i * kPageSize);
    ch[0] = 'a';
  }

  long uffd =
      syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
  CHECK_GE(uffd, 0);

  init.uffd = uffd;
  init.region0 = region0;
  init.region1 = region1;

  uffdio_api uffdio_api;
  uffdio_api.api = UFFD_API;
  uffdio_api.features = UFFD_FEATURE_MINOR_SHMEM;
  CHECK_NE(ioctl(uffd, UFFDIO_API, &uffdio_api), -1);
  CHECK_EQ(uffdio_api.api, UFFD_API);
  // Make sure we are running on a kernel that supports minor faults for shared
  // memory. This is a new kernel feature and is only turned on when
  // CONFIG_HAVE_ARCH_USERFAULTFD_MINOR is set, so the running kernel is
  // unlikely to have it unless we deliberately ensured it does.
  CHECK_NE(uffdio_api.features & UFFD_FEATURE_MINOR_SHMEM, 0);

  // Register `region1` with userfaultfd so we receive minor faults on accesses
  // to it.
  uffdio_register uffdio_register;
  uffdio_register.range.start = reinterpret_cast<uint64_t>(region1);
  uffdio_register.range.len = kShmemSize;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MINOR;
  CHECK_NE(ioctl(uffd, UFFDIO_REGISTER, &uffdio_register), -1);
  // Make sure again that we can definitely use the UFFDIO_CONTINUE command.
  CHECK_NE(uffdio_register.ioctls & ((uint64_t)1 << _UFFDIO_CONTINUE), 0);

  pthread_t thr;
  CHECK_EQ(pthread_create(&thr, NULL, FaultHandlerThread, nullptr), 0);

  // Now access each page in `region1`.
  for (size_t page_idx = 0; page_idx < kNumPages; page_idx++) {
    char* page = region1 + (page_idx * kPageSize);
    char val = page[0];
    printf("Page %lu (%p) has value %c.\n", page_idx, page, val);
    CHECK_EQ(val, 'c' + page_idx);
  }

  return 0;
}
