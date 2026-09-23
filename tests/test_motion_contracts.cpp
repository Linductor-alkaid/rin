// M3-03 IMU 运动数据契约测试（独立验证）：
// 1) MotionSample：kind（Accel/Gyro）决定 axes 物理量与单位（比力 m/s² 含重力 /
//    角速度 rad/s，设备坐标系），设备时间戳与通道序号可携带；valid() 要求 kind
//    合法且三轴全部有限（NaN/Inf 任一轴即无效）；
// 2) MotionSourceStats：默认零值；实测 Hz 与会话累计样本数可载入；
// 3) ImuSnapshot（DEC-010 锁定方向约定）：姿态为单位四元数且标量在前
//    (w,x,y,z)——默认恒等 {1,0,0,0}；valid() 校验分量有限、单位范数（1e-3 容差，
//    覆盖 float 归一化舍入）与源频率非负有限；序号不影响 valid()；
// 4) StreamRequest::enableMotion 使能位参与相等比较——restream 去重以
//    request == 判定（realsense_camera_service.cpp:553），仅翻转运动使能的请求
//    必须被视为真实变更；
// 5) DeviceInfo IMU 能力字段：默认无 IMU；imuSupported 与 ACCEL/GYRO 速率档位
//    （Hz，升序去重约定）可经 DeviceCatalog 携带。
//
// DOD-02 适用性说明：M3-03 仅新增公开契约类型与两条通道声明；适配器通道为
// 无生产者桩（发布由 M3-04 接入），本项未引入新 Executor 任务/线程/队列，并发
// 矩阵（异常/提交拒绝/执行中取消/超时/shutdown）不适用；通道"无新数据返回
// false 且出参不动"的空闲行为在 test_public_boundary.cpp 扩展中覆盖。
#include "test_util.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include <rin/camera_types.hpp>

namespace {

using rin::DeviceCatalog;
using rin::DeviceInfo;
using rin::ImuSnapshot;
using rin::MotionSample;
using rin::MotionSourceStats;
using rin::MotionStreamKind;
using rin::ResolutionOption;
using rin::StreamRequest;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

}  // namespace

int main() {
    // --- 1) MotionSample ---
    {
        // 默认构造：Accel、零轴、零时间戳/序号，且为有效采样。
        const MotionSample sample;
        RIN_CHECK(sample.kind == MotionStreamKind::Accel);
        RIN_CHECK_EQ(sample.axes[0], 0.0f);
        RIN_CHECK_EQ(sample.axes[1], 0.0f);
        RIN_CHECK_EQ(sample.axes[2], 0.0f);
        RIN_CHECK_EQ(sample.deviceTimestampMs, 0.0);
        RIN_CHECK_EQ(sample.sequence, std::uint64_t{0});
        RIN_CHECK(sample.valid());
    }
    {
        // kind 决定 axes 单位：Accel 为比力 m/s²（含重力），Gyro 为角速度 rad/s。
        MotionSample accel;
        accel.kind = MotionStreamKind::Accel;
        accel.axes = {0.0f, 0.0f, 9.81f};  // 静止水平放置：上指轴 +1g
        accel.deviceTimestampMs = 1234.5;
        accel.sequence = 7;
        RIN_CHECK(accel.valid());

        MotionSample gyro;
        gyro.kind = MotionStreamKind::Gyro;
        gyro.axes = {0.01f, -0.02f, 0.5f};  // rad/s
        gyro.deviceTimestampMs = 1235.0;
        gyro.sequence = 8;
        RIN_CHECK(gyro.valid());

        // 字段可携带：写入值原样读回（契约面，非行为）。
        RIN_CHECK_EQ(accel.axes[2], 9.81f);
        RIN_CHECK_EQ(accel.deviceTimestampMs, 1234.5);
        RIN_CHECK_EQ(accel.sequence, std::uint64_t{7});
        RIN_CHECK_EQ(gyro.sequence, std::uint64_t{8});
    }
    {
        // 任一轴非有限（NaN / ±Inf，逐轴位）→ invalid。
        for (int axis = 0; axis < 3; ++axis) {
            MotionSample nanSample;
            nanSample.kind = MotionStreamKind::Accel;
            nanSample.axes = {1.0f, 2.0f, 3.0f};
            nanSample.axes[static_cast<std::size_t>(axis)] = kNaN;
            RIN_CHECK(!nanSample.valid());

            MotionSample infSample;
            infSample.kind = MotionStreamKind::Gyro;
            infSample.axes = {0.0f, 0.0f, 0.0f};
            infSample.axes[static_cast<std::size_t>(axis)] = kInf;
            RIN_CHECK(!infSample.valid());

            MotionSample negInfSample;
            negInfSample.kind = MotionStreamKind::Gyro;
            negInfSample.axes = {0.0f, 0.0f, 0.0f};
            negInfSample.axes[static_cast<std::size_t>(axis)] = -kInf;
            RIN_CHECK(!negInfSample.valid());
        }
    }
    {
        // kind 非法（枚举损坏/越界值）→ invalid（实现显式防御）。
        MotionSample corrupted;
        corrupted.axes = {0.0f, 0.0f, 9.81f};
        corrupted.kind = static_cast<MotionStreamKind>(99);
        RIN_CHECK(!corrupted.valid());
    }

    // --- 2) MotionSourceStats ---
    {
        const MotionSourceStats stats;
        RIN_CHECK_EQ(stats.gyroHz, 0.0f);
        RIN_CHECK_EQ(stats.accelHz, 0.0f);
        RIN_CHECK_EQ(stats.gyroSamples, std::uint64_t{0});
        RIN_CHECK_EQ(stats.accelSamples, std::uint64_t{0});
    }
    {
        // 实测频率 + 会话累计样本数可载入（随 ImuSnapshot 发布的载体）。
        MotionSourceStats stats;
        stats.gyroHz = 199.8f;
        stats.accelHz = 249.5f;
        stats.gyroSamples = 1000;
        stats.accelSamples = 1250;
        RIN_CHECK_EQ(stats.gyroHz, 199.8f);
        RIN_CHECK_EQ(stats.accelHz, 249.5f);
        RIN_CHECK_EQ(stats.gyroSamples, std::uint64_t{1000});
        RIN_CHECK_EQ(stats.accelSamples, std::uint64_t{1250});
    }

    // --- 3) ImuSnapshot（DEC-010：单位四元数标量在前，传感器系→世界系）---
    {
        // 默认 = 恒等姿态 {1,0,0,0}：w 在前（标量在前约定），源统计零、序号零。
        const ImuSnapshot snapshot;
        RIN_CHECK_EQ(snapshot.orientation[0], 1.0f);
        RIN_CHECK_EQ(snapshot.orientation[1], 0.0f);
        RIN_CHECK_EQ(snapshot.orientation[2], 0.0f);
        RIN_CHECK_EQ(snapshot.orientation[3], 0.0f);
        RIN_CHECK_EQ(snapshot.sources.gyroHz, 0.0f);
        RIN_CHECK_EQ(snapshot.sources.accelHz, 0.0f);
        RIN_CHECK_EQ(snapshot.sequence, std::uint64_t{0});
        RIN_CHECK(snapshot.valid());
    }
    {
        // 非平凡单位四元数（绕 Z 90°：w=cos45°, z=sin45°，标量在前）→ 有效。
        ImuSnapshot rotated;
        rotated.orientation = {0.70710678f, 0.0f, 0.0f, 0.70710678f};
        rotated.sources = MotionSourceStats{199.8f, 249.5f, 1000, 1250};
        rotated.sequence = 42;
        RIN_CHECK(rotated.valid());
        RIN_CHECK_EQ(rotated.sequence, std::uint64_t{42});
    }
    {
        // 范数偏离 1 超出 1e-3 容差 → invalid（含全零：未初始化/损坏值拦截）。
        ImuSnapshot scaled;
        scaled.orientation = {2.0f, 0.0f, 0.0f, 0.0f};
        RIN_CHECK(!scaled.valid());

        ImuSnapshot zeroed;
        zeroed.orientation = {0.0f, 0.0f, 0.0f, 0.0f};
        RIN_CHECK(!zeroed.valid());

        ImuSnapshot outside;
        outside.orientation = {1.001f, 0.0f, 0.0f, 0.0f};  // norm² ≈ 1.002001
        RIN_CHECK(!outside.valid());
    }
    {
        // 容差内（float 归一化舍入量级）→ 有效：norm² 偏差 ≈ 1.0001e-4 ≤ 1e-3。
        ImuSnapshot within;
        within.orientation = {1.00005f, 0.0f, 0.0f, 0.0f};
        RIN_CHECK(within.valid());
    }
    {
        // 任一分量非有限 → invalid；源频率非有限或负值 → invalid；序号不影响。
        ImuSnapshot nanW;
        nanW.orientation = {kNaN, 0.0f, 0.0f, 0.0f};
        RIN_CHECK(!nanW.valid());

        ImuSnapshot infZ;
        infZ.orientation = {0.0f, 0.0f, 0.0f, kInf};
        RIN_CHECK(!infZ.valid());

        ImuSnapshot negGyro;
        negGyro.sources.gyroHz = -1.0f;
        RIN_CHECK(!negGyro.valid());

        ImuSnapshot nanAccel;
        nanAccel.sources.accelHz = kNaN;
        RIN_CHECK(!nanAccel.valid());

        ImuSnapshot infGyro;
        infGyro.sources.gyroHz = kInf;
        RIN_CHECK(!infGyro.valid());

        ImuSnapshot seqIrrelevant;
        seqIrrelevant.sequence = 123456789;
        RIN_CHECK(seqIrrelevant.valid());
    }

    // --- 4) StreamRequest::enableMotion（restream 去重语义）---
    {
        // 默认：双流 848x480@30、运动流关闭；两份默认请求相等。
        const StreamRequest defaults;
        RIN_CHECK_EQ(defaults.colorWidth, std::uint32_t{848});
        RIN_CHECK_EQ(defaults.colorFps, std::uint32_t{30});
        RIN_CHECK_EQ(defaults.depthHeight, std::uint32_t{480});
        RIN_CHECK(!defaults.enableMotion);
        RIN_CHECK(defaults == StreamRequest{});
        RIN_CHECK(!(defaults != StreamRequest{}));
    }
    {
        // 仅翻转 enableMotion → 不再相等（worker 以 request == 去重，运动使能
        // 切换必须触发真实 restream 而不是被当作重复请求吞掉）。
        StreamRequest withMotion;
        withMotion.enableMotion = true;
        const StreamRequest withoutMotion;
        RIN_CHECK(withMotion != withoutMotion);
        RIN_CHECK(!(withMotion == withoutMotion));

        // 两份 enableMotion=true 的相同请求相等（重复请求可被去重）。
        StreamRequest withMotionAgain;
        withMotionAgain.enableMotion = true;
        RIN_CHECK(withMotion == withMotionAgain);
        RIN_CHECK(!(withMotion != withMotionAgain));
    }
    {
        // 任一分辨率/fps 字段不同仍不相等；operator!= 与 !(operator==) 一致。
        StreamRequest a;
        StreamRequest b;
        b.colorWidth = 640;
        RIN_CHECK(a != b);
        StreamRequest c;
        c.depthFps = 15;
        RIN_CHECK(a != c);
        StreamRequest d;
        d.enableMotion = true;
        d.colorWidth = 640;
        RIN_CHECK(a != d);
        RIN_CHECK(!(a == d));
    }

    // --- 5) DeviceInfo / DeviceCatalog IMU 能力字段 ---
    {
        // 默认：无 IMU、无速率档位。
        const DeviceInfo info;
        RIN_CHECK(!info.imuSupported);
        RIN_CHECK(info.imuAccelRatesHz.empty());
        RIN_CHECK(info.imuGyroRatesHz.empty());
        RIN_CHECK(info.colorOptions.empty());
        RIN_CHECK(info.depthOptions.empty());
    }
    {
        // D435if 形态的 IMU 能力上报：imuSupported + 升序去重速率档位随
        // DeviceCatalog 携带（枚举填充本身由适配器/真机路径覆盖）。
        DeviceInfo info;
        info.name = "Intel RealSense Depth Camera 435if";
        info.serial = "342622300000";
        info.firmwareVersion = "5.17.1";
        info.colorOptions.push_back(ResolutionOption{848, 480, 30});
        info.depthOptions.push_back(ResolutionOption{848, 480, 30});
        info.imuSupported = true;
        info.imuAccelRatesHz = {63, 250};
        info.imuGyroRatesHz = {200, 400};

        DeviceCatalog catalog;
        catalog.devices.push_back(info);
        catalog.activeSerial = info.serial;
        catalog.activeIsAuto = true;

        RIN_CHECK_EQ(catalog.devices.size(), std::size_t{1});
        const DeviceInfo& reported = catalog.devices.front();
        RIN_CHECK(reported.imuSupported);
        RIN_CHECK_EQ(reported.imuAccelRatesHz.size(), std::size_t{2});
        RIN_CHECK_EQ(reported.imuAccelRatesHz[0], std::uint32_t{63});
        RIN_CHECK_EQ(reported.imuAccelRatesHz[1], std::uint32_t{250});
        RIN_CHECK_EQ(reported.imuGyroRatesHz.size(), std::size_t{2});
        RIN_CHECK_EQ(reported.imuGyroRatesHz[0], std::uint32_t{200});
        RIN_CHECK_EQ(reported.imuGyroRatesHz[1], std::uint32_t{400});
        RIN_CHECK_EQ(catalog.activeSerial, info.serial);
        RIN_CHECK(catalog.activeIsAuto);
    }
    return rin_test::exitStatus();
}
