// 状态机全矩阵测试（DEC-006 更新，验证代理）：
// 1) isAllowed 全 7x7 矩阵对照独立期望表（含新增 Waiting 态）；
// 2) transitionTo 与 isAllowed 一致性：接受时状态迁移/同状态幂等不变，
//    拒绝时状态不变且 error 文本含 from/to 状态名；
// 3) Waiting 相关转换：Opening→Waiting（启动无设备稳态）、Streaming/Restreaming→Waiting
//   （设备移除）、Waiting→Opening（设备到达/选定）、Waiting→Stopping；Failed 不可直达 Waiting；
// 4) Failed→Stopping→Idle 收敛与恢复；Opening 中途 Stopping。
#include "test_util.hpp"

#include <string>

#include "camera_state_machine.hpp"

namespace {

using rsv::CameraServiceState;
using rsv::detail::CameraStateMachine;

constexpr int kStateCount = 7;

// 行 = from，列 = to；行列顺序与 kStates 一致。
// 期望表独立于实现推导：同状态幂等 + 显式转换边
//（Idle->Opening；Opening->{Streaming,Waiting,Failed,Stopping}；
//  Streaming->{Restreaming,Waiting,Stopping,Failed}；
//  Restreaming->{Streaming,Waiting,Failed,Stopping}；
//  Waiting->{Opening,Stopping}；Stopping->Idle；Failed->Stopping）。
// 注：Failed 无直达 Waiting（失败保持可见，须经理 Stopping 收敛，DEC-006）。
constexpr bool kExpectedAllowed[kStateCount][kStateCount] = {
    // to:                                 Idle Opening Stream Restream Waiting Stop Failed
    /* Idle        */ {true, true, false, false, false, false, false},
    /* Opening     */ {false, true, true, false, true, true, true},
    /* Streaming   */ {false, false, true, true, true, true, true},
    /* Restreaming */ {false, false, true, true, true, true, true},
    /* Waiting     */ {false, true, false, false, true, true, false},
    /* Stopping    */ {true, false, false, false, false, true, false},
    /* Failed      */ {false, false, false, false, false, true, true},
};

constexpr CameraServiceState kStates[kStateCount] = {
    CameraServiceState::Idle,       CameraServiceState::Opening,
    CameraServiceState::Streaming,  CameraServiceState::Restreaming,
    CameraServiceState::Waiting,    CameraServiceState::Stopping,
    CameraServiceState::Failed};

/// 把全新机器沿合法路径驱动到目标状态；返回是否全部成功。
bool driveTo(CameraStateMachine& machine, CameraServiceState target) {
    switch (target) {
        case CameraServiceState::Idle:
            return true;
        case CameraServiceState::Opening:
            return machine.transitionTo(CameraServiceState::Opening);
        case CameraServiceState::Streaming:
            return machine.transitionTo(CameraServiceState::Opening) &&
                   machine.transitionTo(CameraServiceState::Streaming);
        case CameraServiceState::Restreaming:
            return machine.transitionTo(CameraServiceState::Opening) &&
                   machine.transitionTo(CameraServiceState::Streaming) &&
                   machine.transitionTo(CameraServiceState::Restreaming);
        case CameraServiceState::Waiting:
            // 启动时无设备：Opening -> Waiting（设计稳态，DEC-006）。
            return machine.transitionTo(CameraServiceState::Opening) &&
                   machine.transitionTo(CameraServiceState::Waiting);
        case CameraServiceState::Stopping:
            return machine.transitionTo(CameraServiceState::Opening) &&
                   machine.transitionTo(CameraServiceState::Streaming) &&
                   machine.transitionTo(CameraServiceState::Stopping);
        case CameraServiceState::Failed:
            return machine.transitionTo(CameraServiceState::Opening) &&
                   machine.transitionTo(CameraServiceState::Failed);
    }
    return false;
}

}  // namespace

int main() {
    // --- 1) isAllowed 全矩阵（纯函数，49 组合）---
    for (int from = 0; from < kStateCount; ++from) {
        for (int to = 0; to < kStateCount; ++to) {
            const bool expected = kExpectedAllowed[from][to];
            RSV_CHECK_EQ(CameraStateMachine::isAllowed(kStates[from], kStates[to]), expected);
        }
    }

    // --- 2) transitionTo 与 isAllowed 一致性（每组合全新机器）---
    for (int from = 0; from < kStateCount; ++from) {
        for (int to = 0; to < kStateCount; ++to) {
            const bool expected = kExpectedAllowed[from][to];
            CameraStateMachine machine;
            RSV_CHECK(driveTo(machine, kStates[from]));
            RSV_CHECK_EQ(machine.state(), kStates[from]);

            std::string error = "<untouched>";
            const bool ok = machine.transitionTo(kStates[to], &error);
            RSV_CHECK_EQ(ok, expected);

            if (expected && from != to) {
                RSV_CHECK_EQ(machine.state(), kStates[to]);
            } else {
                // 拒绝：状态不变，error 含 from/to 状态名。
                // 同状态幂等接受：状态保持不变。
                RSV_CHECK_EQ(machine.state(), kStates[from]);
                if (!expected) {
                    const std::string fromName = rsv::toString(kStates[from]);
                    const std::string toName = rsv::toString(kStates[to]);
                    RSV_CHECK(error.find(fromName) != std::string::npos);
                    RSV_CHECK(error.find(toName) != std::string::npos);
                }
            }
        }
    }

    // --- 3) 默认构造为 Idle；拒绝路径 error 文本含两端状态名 ---
    {
        CameraStateMachine machine;
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Idle);
        std::string error;
        RSV_CHECK(!machine.transitionTo(CameraServiceState::Streaming, &error));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Idle);
        RSV_CHECK(error.find("Idle") != std::string::npos);
        RSV_CHECK(error.find("Streaming") != std::string::npos);
        // nullptr error 出参不崩溃。
        RSV_CHECK(!machine.transitionTo(CameraServiceState::Failed, nullptr));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Idle);
    }

    // --- 4) Failed 不可直达 Waiting；须经理 Stopping 收敛后再恢复 ---
    {
        CameraStateMachine machine;
        RSV_CHECK(driveTo(machine, CameraServiceState::Failed));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Failed);
        std::string error;
        RSV_CHECK(!machine.transitionTo(CameraServiceState::Waiting, &error));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Failed);
        RSV_CHECK(error.find("Failed") != std::string::npos);
        RSV_CHECK(error.find("Waiting") != std::string::npos);
        // Failed -> Stopping -> Idle 收敛，收敛后可重新进入 Opening（恢复）。
        RSV_CHECK(machine.transitionTo(CameraServiceState::Stopping));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Stopping);
        RSV_CHECK(machine.transitionTo(CameraServiceState::Idle));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Idle);
        RSV_CHECK(machine.transitionTo(CameraServiceState::Opening));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Opening);
    }

    // --- 5) Waiting 相关场景（DEC-006 热插拔语义）---
    {
        // 启动无设备：Opening -> Waiting 稳态；设备到达 -> Opening -> Streaming。
        CameraStateMachine machine;
        RSV_CHECK(machine.transitionTo(CameraServiceState::Opening));
        RSV_CHECK(machine.transitionTo(CameraServiceState::Waiting));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Waiting);
        RSV_CHECK(machine.transitionTo(CameraServiceState::Opening));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Opening);
        RSV_CHECK(machine.transitionTo(CameraServiceState::Streaming));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Streaming);
    }
    {
        // 活动设备被移除：Streaming -> Waiting，随后恢复 Opening。
        CameraStateMachine machine;
        RSV_CHECK(driveTo(machine, CameraServiceState::Streaming));
        RSV_CHECK(machine.transitionTo(CameraServiceState::Waiting));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Waiting);
        RSV_CHECK(machine.transitionTo(CameraServiceState::Opening));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Opening);
    }
    {
        // 切换目标设备被移除：Restreaming -> Waiting。
        CameraStateMachine machine;
        RSV_CHECK(driveTo(machine, CameraServiceState::Restreaming));
        RSV_CHECK(machine.transitionTo(CameraServiceState::Waiting));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Waiting);
    }
    {
        // 等待中直接停止：Waiting -> Stopping -> Idle（设计稳态可取消）。
        CameraStateMachine machine;
        RSV_CHECK(driveTo(machine, CameraServiceState::Waiting));
        RSV_CHECK(machine.transitionTo(CameraServiceState::Stopping));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Stopping);
        RSV_CHECK(machine.transitionTo(CameraServiceState::Idle));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Idle);
    }

    // --- 6) Opening 中途 Stopping -> Idle（start 阶段取消路径）---
    {
        CameraStateMachine machine;
        RSV_CHECK(machine.transitionTo(CameraServiceState::Opening));
        RSV_CHECK(machine.transitionTo(CameraServiceState::Stopping));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Stopping);
        RSV_CHECK(machine.transitionTo(CameraServiceState::Idle));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Idle);
        // Idle 上的重复 transitionTo(Idle) 幂等。
        RSV_CHECK(machine.transitionTo(CameraServiceState::Idle));
        RSV_CHECK_EQ(machine.state(), CameraServiceState::Idle);
    }
    return rsv_test::exitStatus();
}
