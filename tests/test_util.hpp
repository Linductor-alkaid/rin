#pragma once

// 轻量测试壳：无第三方依赖（executor 同款 standalone 纪律）。
#include <cstdio>
#include <string>

namespace rsv_test {

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

}  // namespace rsv_test

#define RSV_CHECK(cond)                                              \
    do {                                                             \
        ++rsv_test::checkCount();                                    \
        if (!(cond)) {                                               \
            rsv_test::recordFailure(__FILE__, __LINE__, #cond);      \
        }                                                            \
    } while (0)

#define RSV_CHECK_EQ(a, b)                                                              \
    do {                                                                                \
        ++rsv_test::checkCount();                                                       \
        if (!((a) == (b))) {                                                            \
            rsv_test::recordFailure(__FILE__, __LINE__, std::string(#a " == ") + #b);   \
        }                                                                               \
    } while (0)
