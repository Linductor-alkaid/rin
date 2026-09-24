// M3-04 ImuFuser 接缝测试（独立验证，离线纯逻辑）：
// 1) IdentityImuFuser 假实现契约（src/core/imu_fuser.hpp:43-56）：
//    - 构造初态 hasPose()==false，orientation() 为恒等 {1,0,0,0}（DEC-010 严格
//      恒等约定）；
//    - 任一 advance 后 hasPose()==true 且姿态恒为恒等（假实现不消费采样内容，
//      真实融合由 M3-05 提供）；
//    - reset() 回到初态且幂等；复位后可重新推进。
// 2) 基类多态接缝（POST-05 可替换边界）：经 ImuFuser& / unique_ptr<ImuFuser>
//    使用；不可拷贝（显式删除拷贝）；经基类指针析构安全。
// 3) createImuFuser() 工厂：返回非空可用实例、满足同一接缝契约。刻意不锁定
//    返回的具体实现类型（M3-05 按 DEC-010 切换 Mahony 真身时不破坏本测试）。
//
// DOD-02 适用性说明：IdentityImuFuser 为单线程纯逻辑（无任务提交/取消/等待/
// 关闭语义），并发矩阵不适用；MotionIngest 驱动融合器的跨上下文行为见
// motion_ingest 测试（含 tsan 预设复跑）。
#include "test_util.hpp"

#include <array>
#include <memory>
#include <type_traits>

#include "imu_fuser.hpp"

namespace {

using rin::detail::createImuFuser;
using rin::detail::IdentityImuFuser;
using rin::detail::ImuFuser;
using rin::MotionSample;
using rin::MotionStreamKind;

constexpr std::array<float, 4> kIdentity{1.0f, 0.0f, 0.0f, 0.0f};

MotionSample validGyro(double timestampMs) {
    MotionSample sample;
    sample.kind = MotionStreamKind::Gyro;
    sample.axes = {0.01f, -0.02f, 0.03f};
    sample.deviceTimestampMs = timestampMs;
    return sample;
}

/// 经基类引用驱动并断言假实现契约（多态路径与具体类型路径行为一致）。
void checkIdentityContract(ImuFuser& fuser) {
    RIN_CHECK(!fuser.hasPose());
    RIN_CHECK_EQ(fuser.orientation()[0], kIdentity[0]);
    RIN_CHECK_EQ(fuser.orientation()[1], kIdentity[1]);
    RIN_CHECK_EQ(fuser.orientation()[2], kIdentity[2]);
    RIN_CHECK_EQ(fuser.orientation()[3], kIdentity[3]);

    fuser.advance(validGyro(10.0));
    RIN_CHECK(fuser.hasPose());
    RIN_CHECK_EQ(fuser.orientation()[0], kIdentity[0]);
    RIN_CHECK_EQ(fuser.orientation()[1], kIdentity[1]);
    RIN_CHECK_EQ(fuser.orientation()[2], kIdentity[2]);
    RIN_CHECK_EQ(fuser.orientation()[3], kIdentity[3]);

    // 假实现不消费采样内容：Accel 采样同样推进，姿态仍恒等。
    MotionSample accel;
    accel.kind = MotionStreamKind::Accel;
    accel.axes = {0.0f, 0.0f, 9.81f};
    accel.deviceTimestampMs = 14.0;
    fuser.advance(accel);
    RIN_CHECK(fuser.hasPose());
    RIN_CHECK_EQ(fuser.orientation()[0], kIdentity[0]);

    // reset() 回到初态且幂等；复位后可重新推进。
    fuser.reset();
    fuser.reset();
    RIN_CHECK(!fuser.hasPose());
    RIN_CHECK_EQ(fuser.orientation()[0], kIdentity[0]);
    fuser.advance(validGyro(20.0));
    RIN_CHECK(fuser.hasPose());
}

}  // namespace

int main() {
    // 契约面：假实现不可拷贝（基类显式删除拷贝，独占接缝语义）。
    RIN_CHECK(!std::is_copy_constructible_v<IdentityImuFuser>);
    RIN_CHECK(!std::is_copy_constructible_v<ImuFuser>);

    // --- 1) 具体类型直接使用 ---
    {
        IdentityImuFuser fuser;
        checkIdentityContract(fuser);
    }

    // --- 2) 经基类引用（适配器实际持有的形态）---
    {
        IdentityImuFuser concrete;
        ImuFuser& fuser = concrete;
        checkIdentityContract(fuser);
    }

    // --- 3) 工厂：非空、经基类 unique_ptr 的同一契约、基类指针析构安全 ---
    {
        const std::unique_ptr<ImuFuser> fuser = createImuFuser();
        RIN_CHECK(fuser != nullptr);
        checkIdentityContract(*fuser);
    }
    {
        // 工厂每次返回独立实例（两次取用互不共享状态）。
        const std::unique_ptr<ImuFuser> first = createImuFuser();
        const std::unique_ptr<ImuFuser> second = createImuFuser();
        RIN_CHECK(first != nullptr);
        RIN_CHECK(second != nullptr);
        RIN_CHECK(first.get() != second.get());
        first->advance(validGyro(1.0));
        RIN_CHECK(first->hasPose());
        RIN_CHECK(!second->hasPose());  // 独立实例推进互不影响
    }
    return rin_test::exitStatus();
}
