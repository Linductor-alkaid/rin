#pragma once

// 轻量测试壳：无第三方依赖（executor 同款 standalone 纪律）。
// M12/CR-23、CR-39：有界等待与断言 helper 的唯一版本（原 workflow_engine_
// contract_suite.hpp 与各测试文件的手抄副本统一收敛于此——套件头不再承载
// 通用 helper，避免"引入契约套件符号"的顾虑）。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

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

// --- 有界等待（M12/CR-23） ---

/// 有界轮询默认死限（防悬挂）。
inline constexpr std::chrono::milliseconds kPollDeadline{5000};

/// 静默检查默认窗宽（run_control/契约套件同款 300ms）。
inline constexpr std::chrono::milliseconds kQuietWindow{300};

/// 有界轮询（kPollDeadline 死限；pred() 为真即返回；超时后最后一次 pred()
/// 定结果）。
template <typename Pred>
bool pollUntil(Pred&& pred, std::chrono::milliseconds timeout = kPollDeadline) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

/// 静默检查：window 内 hasNew() 一旦为真即返回 false（出现新发布）；全窗
/// 安静返回 true。
template <typename HasNew>
bool quietFor(HasNew&& hasNew, std::chrono::milliseconds window = kQuietWindow) {
    const auto deadline = std::chrono::steady_clock::now() + window;
    while (std::chrono::steady_clock::now() < deadline) {
        if (hasNew()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

// --- 断言 helper（M12/CR-39） ---

/// 分节输出（各测试共用的小壳）。
inline void runSection(const char* name, void (*fn)()) {
    std::printf("== %s\n", name);
    fn();
}

/// float 近等（默认容差 1e-3，原 test_node_canvas/test_monitor_any 同款 kTol）。
[[nodiscard]] inline bool nearF(float a, float b, float tol = 1e-3f) {
    return std::fabs(a - b) <= tol;
}

/// double 近等（默认容差 1e-9，原 test_param_panel 同款）。
[[nodiscard]] inline bool nearD(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

// hasKind（图校验问题存在性）见 validation_test_util.hpp——本头保持零项目
// 依赖（test_depth_metric_convert 等不链接 rin 的目标也复用本头）。

}  // namespace rin_test

#define RIN_CHECK(cond)                                              \
    do {                                                             \
        ++rin_test::checkCount();                                    \
        if (!(cond)) {                                               \
            rin_test::recordFailure(__FILE__, __LINE__, #cond);      \
        }                                                            \
    } while (0)

/// 带补充消息的检查（参数化用例失败时定位到具体输入）。
#define RIN_CHECK_MSG(cond, message)                                     \
    do {                                                                 \
        ++rin_test::checkCount();                                        \
        if (!(cond)) {                                                   \
            rin_test::recordFailure(__FILE__, __LINE__,                  \
                                    std::string(#cond) + " | " + (message)); \
        }                                                                \
    } while (0)

#define RIN_CHECK_EQ(a, b)                                                              \
    do {                                                                                \
        ++rin_test::checkCount();                                                       \
        if (!((a) == (b))) {                                                            \
            rin_test::recordFailure(__FILE__, __LINE__, std::string(#a " == ") + #b);   \
        }                                                                               \
    } while (0)
