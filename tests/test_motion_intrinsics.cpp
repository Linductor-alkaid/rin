// M3-04 IntrinsicsSnapshot 契约扩展测试（独立验证）：
// 1) Extrinsics（gyro→color 刚体外参，SDK rs2_extrinsics 语义）：默认构造为全零，
//    契约（include/rin/camera_types.hpp:127-128 "运动流未使能、设备无 IMU 或读取
//    失败时保持全零无效值（valid() == false）可观察"）要求全零 → invalid；有限
//    非零旋转 + 有限平移 → valid；任一旋转/平移分量非有限 → invalid。
// 2) MotionIntrinsics（ACCEL/GYRO 出厂运动内参）：默认全零 → invalid（同上契约，
//    camera_types.hpp:130-133）；对角刻度非零的出厂标定值 → valid；scale/bias/
//    noiseVariances/biasVariances 任一分量非有限 → invalid。
// 3) IntrinsicsSnapshot 装配语义：默认快照（无 IMU/读取失败形态）运动字段必须
//    可观察为无效（全零 valid()==false）；纯视频发布形态（motion=false）不携带
//    运动数据；完整快照各字段同时成立。
//
// DOD-02 适用性说明：本文件只覆盖公开契约类型的纯值语义（无 Executor 任务、
// 无通道、无并发），并发矩阵（正常完成/异常/提交拒绝/执行中取消/超时/shutdown）
// 不适用；行为路径（读取/发布）由 motion_ingest 与 realsense_hardware 覆盖。
#include "test_util.hpp"

#include <cmath>
#include <limits>

#include <rin/camera_types.hpp>

namespace {

using rin::Extrinsics;
using rin::IntrinsicsSnapshot;
using rin::MotionIntrinsics;
using rin::StreamIntrinsics;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// 列主序恒等旋转（v_target = R * v_source + t 的 R = I 形态）。
constexpr std::array<float, 9> kIdentityRotation = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                                    0.0f, 0.0f, 0.0f, 1.0f};

}  // namespace

int main() {
    // --- 1) Extrinsics ---
    {
        // 默认构造 = 全零。契约：这是"运动流未使能/无 IMU/读取失败"的可观察
        // 无效标记（valid() == false），消费方据此区分"没有外参"与"外参可用"。
        const Extrinsics zero;
        RIN_CHECK(!zero.valid());
    }
    {
        // 恒等旋转 + 非零平移（真实标定形态）→ valid。
        Extrinsics identity;
        identity.rotation = kIdentityRotation;
        identity.translation = {0.01f, -0.02f, 0.003f};
        RIN_CHECK(identity.valid());
    }
    {
        // 全零平移 + 非零旋转仍有效（共面安装平移可近似 0，旋转不可为全零）。
        Extrinsics rotationOnly;
        rotationOnly.rotation = kIdentityRotation;
        RIN_CHECK(rotationOnly.valid());
    }
    {
        // 任一旋转分量非有限 → invalid（位次覆盖 9 元素）。
        for (std::size_t index = 0; index < 9; ++index) {
            Extrinsics nanRotation;
            nanRotation.rotation = kIdentityRotation;
            nanRotation.rotation[index] = kNaN;
            RIN_CHECK(!nanRotation.valid());

            Extrinsics infRotation;
            infRotation.rotation = kIdentityRotation;
            infRotation.rotation[index] = kInf;
            RIN_CHECK(!infRotation.valid());
        }
    }
    {
        // 任一平移分量非有限 → invalid（位次覆盖 3 元素）。
        for (std::size_t index = 0; index < 3; ++index) {
            Extrinsics nanTranslation;
            nanTranslation.rotation = kIdentityRotation;
            nanTranslation.translation = {0.01f, 0.02f, 0.03f};
            nanTranslation.translation[index] = kNaN;
            RIN_CHECK(!nanTranslation.valid());
        }
    }

    // --- 2) MotionIntrinsics ---
    {
        // 默认全零 → invalid（读取失败保持全零可观察，契约同 gyroToColor）。
        const MotionIntrinsics zero;
        RIN_CHECK(!zero.valid());
    }
    {
        // 出厂标定形态：对角刻度 ~1 + 小零偏 + 正方差 → valid。
        MotionIntrinsics factory;
        factory.scale = {1.0f, 0.01f, 0.02f, -0.01f, 1.0f, 0.01f, -0.02f, 0.01f, 1.0f};
        factory.bias = {0.002f, -0.001f, 0.003f};
        factory.noiseVariances = {1e-4f, 1e-4f, 1e-4f};
        factory.biasVariances = {1e-6f, 1e-6f, 1e-6f};
        RIN_CHECK(factory.valid());
    }
    {
        // scale / bias / noiseVariances / biasVariances 任一分量非有限 → invalid。
        MotionIntrinsics nanScale;
        nanScale.scale = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, kNaN};
        RIN_CHECK(!nanScale.valid());

        MotionIntrinsics infBias;
        infBias.scale = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        infBias.bias = {0.0f, kInf, 0.0f};
        RIN_CHECK(!infBias.valid());

        MotionIntrinsics nanNoise;
        nanNoise.scale = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        nanNoise.noiseVariances = {1e-4f, kNaN, 1e-4f};
        RIN_CHECK(!nanNoise.valid());

        MotionIntrinsics infBiasVar;
        infBiasVar.scale = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        infBiasVar.biasVariances = {1e-6f, 1e-6f, -kInf};
        RIN_CHECK(!infBiasVar.valid());
    }

    // --- 3) IntrinsicsSnapshot 装配语义 ---
    {
        // 默认快照：视频内参全零无效、运动字段全零无效（消费方在首份快照到达前
        // 与"无 IMU/读取失败"形态下必须能观察运动字段不可用）、序号 0。
        const IntrinsicsSnapshot snapshot;
        RIN_CHECK(!snapshot.color.valid());
        RIN_CHECK(!snapshot.depth.valid());
        RIN_CHECK(!snapshot.gyroToColor.valid());
        RIN_CHECK(!snapshot.accelIntrinsics.valid());
        RIN_CHECK(!snapshot.gyroIntrinsics.valid());
        RIN_CHECK_EQ(snapshot.sequence, std::uint64_t{0});
    }
    {
        // 纯视频发布形态（motion=false，无 IMU 设备退化）：color/depth 有效 +
        // 运动字段保持全零无效——消费方以此判定"本流不带运动数据"。
        IntrinsicsSnapshot videoOnly;
        videoOnly.sequence = 3;
        videoOnly.color = StreamIntrinsics{640, 480, 382.5f, 382.5f, 320.0f, 240.0f,
                                           {}, rin::DistortionModel::ModifiedBrownConrady};
        videoOnly.depth = StreamIntrinsics{640, 480, 382.0f, 382.0f, 319.5f, 239.5f,
                                           {}, rin::DistortionModel::InverseBrownConrady};
        RIN_CHECK(videoOnly.color.valid());
        RIN_CHECK(videoOnly.depth.valid());
        RIN_CHECK_EQ(videoOnly.sequence, std::uint64_t{3});
        RIN_CHECK(!videoOnly.gyroToColor.valid());
        RIN_CHECK(!videoOnly.accelIntrinsics.valid());
        RIN_CHECK(!videoOnly.gyroIntrinsics.valid());
    }
    {
        // 完整形态（motion=true 且读取成功）：内参 + 外参 + 双运动内参全部有效。
        IntrinsicsSnapshot full;
        full.sequence = 9;
        full.color = StreamIntrinsics{848, 480, 697.0f, 697.0f, 424.0f, 240.0f,
                                      {}, rin::DistortionModel::None};
        full.depth = StreamIntrinsics{848, 480, 696.0f, 696.0f, 423.5f, 239.0f,
                                      {}, rin::DistortionModel::InverseBrownConrady};
        full.gyroToColor.rotation = kIdentityRotation;
        full.gyroToColor.translation = {0.012f, -0.001f, 0.004f};
        for (MotionIntrinsics* motion : {&full.accelIntrinsics, &full.gyroIntrinsics}) {
            motion->scale = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
            motion->noiseVariances = {1e-4f, 1e-4f, 1e-4f};
            motion->biasVariances = {1e-6f, 1e-6f, 1e-6f};
        }
        RIN_CHECK(full.color.valid());
        RIN_CHECK(full.depth.valid());
        RIN_CHECK(full.gyroToColor.valid());
        RIN_CHECK(full.accelIntrinsics.valid());
        RIN_CHECK(full.gyroIntrinsics.valid());
    }
    return rin_test::exitStatus();
}
