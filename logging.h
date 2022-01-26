#ifndef LOGGING_H_
#define LOGGING_H_

#include <cstring>
#include <iostream>

static inline pid_t GetTid() {
  static thread_local int tid = syscall(__NR_gettid);
  return tid;
}

inline std::ostream& operator<<(std::ostream& os, std::nullptr_t) {
  os << "nullptr";
  return os;
}

#define __LCHECK(op, invop, expr1, expr2)                                      \
  do {                                                                         \
    auto __val1 = expr1;                                                       \
    auto __val2 = expr2;                                                       \
    if (!(__val1 op __val2)) {                                                 \
      int __errno = errno;                                                     \
      std::cerr << __FILE__ << ":" << __LINE__ << "(" << GetTid() << ") "      \
                << "CHECK FAILED: " << #expr1 << " " #op " " << #expr2 << " [" \
                << __val1 << " " #invop " " << __val2 << "]" << std::endl;     \
      if (__errno) {                                                           \
        std::cerr << "errno: " << __errno << " [" << std::strerror(__errno)    \
                  << "]" << std::endl;                                         \
      }                                                                        \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define CHECK_EQ(val1, val2) __LCHECK(==, !=, val1, val2)
#define CHECK_NE(val1, val2) __LCHECK(!=, ==, val1, val2)
#define CHECK_LT(val1, val2) __LCHECK(<, >=, val1, val2)
#define CHECK_LE(val1, val2) __LCHECK(<=, <, val1, val2)
#define CHECK_GT(val1, val2) __LCHECK(>, <=, val1, val2)
#define CHECK_GE(val1, val2) __LCHECK(>=, >, val1, val2)
#define CHECK(val1) CHECK_NE(val1, 0)
#define CHECK_ZERO(val1) CHECK_EQ(val1, 0)

#endif  // LOGGING_H_
