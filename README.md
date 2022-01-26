This project shows a simple example of using userfaultfd with minor faults on shared memory (`UFFD_FEATURE_MINOR_SHMEM`). I create two virtual memory ranges that both point to the same physical memory (backed by memfd) and register one of the virtual memory ranges with userfaultfd (`UFFDIO_REGISTER_MODE_MINOR`). I then access the registered virtual memory range and handle the generated userfaultfd page faults with the `UFFDIO_CONTINUE` ioctl.

Compile:
```
g++ main.cc -pthread -o main
```

Run:
```
./main
```

---

The `userfaultfd.h` file included in this repository is the userfaultfd UAPI header from Linux v5.16. This is directly included in `main.cc` rather than `<linux/userfaultfd.h>`, which makes it easier to compile this project on a kernel that has out-of-date kernel headers.
