#pragma once

// 轻量测试壳：无第三方依赖（executor 同款 standalone 纪律）。
#include <cstdio>
#include <string>

namespace rin_test {

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline int& checkCount() {
    static int count = 0;
    return count;
}

inline void recordFailure(const char* file, int line, const std::string& what) {
    ++failureCount();
    std::printf("FAIL %s:%d %s\n", file, line, what.c_str());
}

inline int exitStatus() {
    const int failures = failureCount();
    std::printf("%s: %d checks, %d failures\n", failures == 0 ? "OK" : "FAILED", checkCount(),
                failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace rin_test

#define RIN_CHECK(cond)                                              \
    do {                                                             \
        ++rin_test::checkCount();                                    \
        if (!(cond)) {                                               \
            rin_test::recordFailure(__FILE__, __LINE__, #cond);      \
        }                                                            \
    } while (0)

#define RIN_CHECK_EQ(a, b)                                                              \
    do {                                                                                \
        ++rin_test::checkCount();                                                       \
        if (!((a) == (b))) {                                                            \
            rin_test::recordFailure(__FILE__, __LINE__, std::string(#a " == ") + #b);   \
        }                                                                               \
    } while (0)
