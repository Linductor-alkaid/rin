#pragma once

// 3D 位姿视图控件（M3-06，[DEC-011]）：CPU 投影（rin Core 纯逻辑 pose_math）+
// EUI-NEO `polygon` 原语组装。固定世界坐标系（重力对齐：Z 轴向上）绘制地面网格与
// 世界坐标轴；相机视锥与相机局部轴随最新融合姿态实时更新（painter 序表达遮挡，
// 线框 + 半透明面，无深度缓冲）；"Reset" 把显示姿态参考重新锚定到当前物理姿态
// （抑制六轴 yaw 漂移的视觉积累，DEC-011 风险 3）；姿态不可用时显示空态。
//
// 边界（RULE-01/05/07）：本单元只做有界投影与点集提交（≤37 个多边形、≤150 顶点，
// DEC-011 原型实测 ~40µs/帧），不做融合、不做阻塞等待；姿态快照由 app.cpp 的
// pump() 经 tryLoadPose() 消费后写入 PoseViewState（IMU 状态面板 M3-07 起消费
// 同一份快照，见 imu_panel.hpp）。投影数学全部位于 Core，此处仅在 UI 边界把
// rin::PoseVec2 转换为 eui::Vec2（DEC-011 决策 1/2）。
//
// [DEC-011]: ../../docs/decisions/DEC-011-pose-view-rendering.md

#include "viewer_theme.hpp"

#include <eui_neo.h>

#include <rin/camera_types.hpp>
#include <rin/pose_math.hpp>

#include <array>
#include <string>
#include <vector>

namespace viewer {

/// 位姿视图状态：viewer 上下文持有（地址稳定，onClick 闭包可安全引用），
/// pump() 消费最新快照，Reset 按钮重新锚定显示参考。
struct PoseViewState {
    /// 姿态是否可用（false = 空态）。
    bool available = false;
    /// 最新融合姿态快照（DEC-010 语义：传感器系→世界系，标量在前单位四元数）。
    rin::ImuSnapshot snapshot;
    /// 显示参考基准（传感器系→世界系）。显示姿态 = conj(baseline) ⊗ 最新姿态；
    /// baseline 恒等时按原始姿态显示。清空（空态）时复位为恒等。
    std::array<float, 4> baseline{1.0f, 0.0f, 0.0f, 0.0f};

    /// 消费一条新快照（pump 专用）。
    void update(const rin::ImuSnapshot& next) {
        available = true;
        snapshot = next;
    }

    /// 视图重置（M3-06）：以当前物理姿态为显示参考；幂等（重复点击显示不变）。
    void resetView() {
        if (available) {
            baseline = snapshot.orientation;
        }
    }

    /// 回空态并复位参考（流停止/restream 重建/非 IMU 设备时由 pump 调用）。
    void clear() {
        available = false;
        baseline = {1.0f, 0.0f, 0.0f, 0.0f};
    }
};

namespace {

// --- 场景比例（DEC-011 原型实测构图，观察相机固定于世界系） ---
constexpr float kOrbitAzimuth = 0.62f;
constexpr float kOrbitElevation = 0.50f;
constexpr float kOrbitDistance = 3.8f;
constexpr float kProjectorNearZ = 0.05f;
constexpr float kGridExtent = 1.0f;
constexpr float kGridStep = 0.25f;
constexpr float kGridWidthPx = 1.0f;
constexpr float kWorldAxisLength = 1.25f;
constexpr float kWorldAxisWidthPx = 2.6f;
constexpr float kFrustumImagePlane = 0.85f;
constexpr float kFrustumHalfTan = 0.42f;
constexpr float kFrustumEdgeWidthPx = 1.8f;
constexpr float kCamAxisLength = 0.5f;
constexpr float kCamAxisWidthPx = 2.2f;

/// 投影相机局部场景 → polygon 点集组装（有界：网格 18 + 世界轴 3 + 视锥面 5 +
/// 视锥边 8 + 相机轴 3）。x/y/w/h 为场景区域（相对位姿卡片），polygon 元素
/// 覆盖整个区域，点集即区域局部坐标。
inline void composePoseScene(eui::Ui& ui, const rin::PoseMat3& camToWorld, float x, float y,
                             float w, float h) {
    const theme::PoseSceneTokens& scene = theme::poseScene();
    const rin::OrbitView view =
        rin::makeOrbitView(kOrbitAzimuth, kOrbitElevation, kOrbitDistance);
    const rin::PoseProjector projector{{w * 0.5f, h * 0.52f}, h * 0.55f, kProjectorNearZ};

    // 复用点集缓冲：逐多边形 move 进元素后 clear 复用，稳态无堆分配增长。
    std::vector<eui::Vec2> points;
    const auto polygon = [&](const char* id, const eui::Color& color) {
        if (points.empty()) {
            return;
        }
        ui.polygon(id)
            .position(x, y)
            .size(w, h)
            .ignoreLayout()
            .points(std::move(points))
            .color(color)
            .build();
        points.clear();  // move 后处于有效未指定态，clear 恢复空缓冲
    };
    const auto segment = [&](const char* id, const eui::Color& color,
                             const rin::PoseVec3& aWorld, const rin::PoseVec3& bWorld,
                             float widthPx) {
        std::array<rin::PoseVec2, 4> quad{};
        if (!rin::segmentToQuad(projector, rin::worldToView(view, aWorld),
                                rin::worldToView(view, bWorld), widthPx, quad)) {
            return;  // 近平面后/退化：剔除该线段（稳定 id 集合按帧缩减，可观察）
        }
        for (const rin::PoseVec2& p : quad) {
            points.push_back({p.x, p.y});
        }
        polygon(id, color);
    };

    // 地面网格（世界 z=0 平面，±1.0，步长 0.25）——固定世界系参考。
    int gridIndex = 0;
    for (float t = -kGridExtent; t <= kGridExtent + kGridStep * 0.5f; t += kGridStep) {
        std::string id = "view.pose.grid.v" + std::to_string(gridIndex++);
        segment(id.c_str(), scene.grid, {t, -kGridExtent, 0.0f}, {t, kGridExtent, 0.0f},
                kGridWidthPx);
        id = "view.pose.grid.h" + std::to_string(gridIndex++);
        segment(id.c_str(), scene.grid, {-kGridExtent, t, 0.0f}, {kGridExtent, t, 0.0f},
                kGridWidthPx);
    }

    // 世界坐标轴（RGB 惯例 = X/Y/Z，原点起）——固定世界系参考。
    const rin::PoseVec3 origin{};
    const rin::PoseVec3 axisTips[3] = {{kWorldAxisLength, 0.0f, 0.0f},
                                       {0.0f, kWorldAxisLength, 0.0f},
                                       {0.0f, 0.0f, kWorldAxisLength}};
    const eui::Color axisColors[3] = {scene.axisX, scene.axisY, scene.axisZ};
    const char* axisIds[3] = {"view.pose.axis.x", "view.pose.axis.y", "view.pose.axis.z"};
    for (int i = 0; i < 3; ++i) {
        segment(axisIds[i], axisColors[i], origin, axisTips[i], kWorldAxisWidthPx);
    }

    // 相机视锥与相机局部轴（随姿态实时更新）：相机系光轴 +Z 前向，像平面
    // z = kFrustumImagePlane。
    const float half = kFrustumImagePlane * kFrustumHalfTan;
    const rin::PoseVec3 local[5] = {{0.0f, 0.0f, 0.0f},
                                    {-half, -half, kFrustumImagePlane},
                                    {half, -half, kFrustumImagePlane},
                                    {half, half, kFrustumImagePlane},
                                    {-half, half, kFrustumImagePlane}};
    rin::PoseVec3 world[5];
    for (int i = 0; i < 5; ++i) {
        world[i] = rin::rotateVector(camToWorld, local[i]);
    }

    // 视锥半透明面（painter 序：先面后线）：4 个侧面三角（apex + 相邻两角）+
    // 像平面四边形；顶点在近平面后整面剔除。
    const auto emitFace = [&](const char* id, const int* indices, int count) {
        rin::PoseVec2 sp[4]{};
        for (int k = 0; k < count; ++k) {
            if (!rin::projectToScreen(projector, rin::worldToView(view, world[indices[k]]),
                                      sp[k])) {
                return;
            }
        }
        for (int k = 0; k < count; ++k) {
            points.push_back({sp[k].x, sp[k].y});
        }
        if (count == 3) {
            points.push_back({sp[0].x, sp[0].y});  // 三角形闭合为 4 点
        }
        polygon(id, scene.frustumFace);
    };
    const char* faceIds[5] = {"view.pose.face.0", "view.pose.face.1", "view.pose.face.2",
                              "view.pose.face.3", "view.pose.face.plane"};
    for (int f = 0; f < 4; ++f) {
        const int indices[3] = {0, 1 + f, 1 + (f + 1) % 4};
        emitFace(faceIds[f], indices, 3);
    }
    const int planeIndices[4] = {1, 2, 3, 4};
    emitFace(faceIds[4], planeIndices, 4);

    // 视锥线框：4 条 apex→像平面角 + 4 条像平面边。
    const auto edgeId = [](int index) {
        return "view.pose.edge." + std::to_string(index);
    };
    for (int e = 0; e < 4; ++e) {
        const std::string id = edgeId(e);
        segment(id.c_str(), scene.frustumEdge, world[0], world[1 + e],
                kFrustumEdgeWidthPx);
    }
    for (int e = 0; e < 4; ++e) {
        const std::string id = edgeId(4 + e);
        segment(id.c_str(), scene.frustumEdge, world[1 + e], world[1 + (e + 1) % 4],
                kFrustumEdgeWidthPx);
    }

    // 相机局部轴（RGB 惯例，随姿态旋转，与视锥同基准点）。
    const rin::PoseVec3 camAxes[3] = {{kCamAxisLength, 0.0f, 0.0f},
                                      {0.0f, kCamAxisLength, 0.0f},
                                      {0.0f, 0.0f, kCamAxisLength}};
    const eui::Color camColors[3] = {scene.camAxisX, scene.camAxisY, scene.camAxisZ};
    const char* camIds[3] = {"view.pose.cam.x", "view.pose.cam.y", "view.pose.cam.z"};
    for (int i = 0; i < 3; ++i) {
        segment(camIds[i], camColors[i], world[0], rin::rotateVector(camToWorld, camAxes[i]),
                kCamAxisWidthPx);
    }
}

}  // namespace

/// 合成 3D 位姿视图卡片（与 RGB/Depth 画面卡同卡片语汇）。intrinsics 为当前
/// 内参快照（可空）：携带有效 gyro→color 外参时把融合姿态（IMU 传感器系）换算
/// 到彩色相机系（camera_types.hpp 外参契约用途），否则按传感器系直接显示。
inline void composePoseViewCard(eui::Ui& ui, PoseViewState& state,
                                const rin::IntrinsicsSnapshot* intrinsics, float width,
                                float height) {
    using namespace viewer::theme;  // 语义令牌（DEC-005）；函数内引入，不泄漏头文件作用域
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace3;
    const float labelHeight = kFontCaption + kSpace1;
    const float areaY = pad + labelHeight + kSpace1;
    const float areaWidth = width - pad * 2.0f;
    const float areaHeight = height - areaY - pad;
    const bool hasPose = state.available && state.snapshot.valid();

    ui.stack("view.pose")
        .size(width, height)
        .clip()
        .content([&] {
            ui.rect("view.pose.card")
                .size(width, height)
                .radius(kRadiusXl)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.rect("view.pose.area")
                .position(pad, areaY)
                .size(areaWidth, areaHeight)
                .radius(kRadiusMd)
                .color(tokens.surface)
                .build();

            if (!hasPose) {
                // 空态（M3-06）：姿态不可用——未使能运动流、设备无 IMU、流重建
                // 或融合尚未收敛；仅背景板 + 提示，不合成场景多边形。
                ui.text("view.pose.empty")
                    .position(pad, areaY)
                    .size(areaWidth, areaHeight)
                    .text("no IMU pose - waiting for motion stream")
                    .fontSize(kFontSm)
                    .color(tokens.fgSubtlest)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            } else {
                // 显示姿态 = conj(baseline) ⊗ 最新姿态（Reset 重锚定参考）；有效
                // 外参时换算到彩色相机系：R_cam←world = R_sensor←world · R_cam←sensor。
                const std::array<float, 4> relative = rin::quatMultiply(
                    rin::quatConjugate(state.baseline), state.snapshot.orientation);
                rin::PoseMat3 camToWorld = rin::orientationToMatrix(relative);
                if (intrinsics != nullptr && intrinsics->gyroToColor.valid()) {
                    camToWorld = rin::multiplyMatrix3(
                        camToWorld,
                        rin::transposeMatrix3(
                            rin::matrix3FromColumnMajor(intrinsics->gyroToColor.rotation)));
                }
                composePoseScene(ui, camToWorld, pad, areaY, areaWidth, areaHeight);

                // 源频率与姿态数值读数自 M3-07 起由 IMU 状态面板（imu_panel.hpp）
                // 统一呈现，本卡不再重复叠加元数据。
            }

            ui.text("view.pose.label")
                .position(pad, pad)
                .size(areaWidth, labelHeight)
                .text("Pose")
                .fontSize(kFontCaption)
                .fontWeight(kWeightMedium)
                .color(tokens.fgSubtle)
                .build();

            if (hasPose) {
                // 视图重置（DEC-011 风险 3）：以当前物理姿态重新锚定显示参考。
                const float resetWidth = 56.0f;
                const float resetHeight = labelHeight + kSpace1;
                ui.rect("view.pose.reset")
                    .position(width - pad - resetWidth, pad - 2.0f)
                    .size(resetWidth, resetHeight)
                    .radius(kRadiusSm)
                    .color(tokens.input)
                    .border(kBorderHairline, tokens.inputBorder)
                    .onClick([&state] { state.resetView(); })
                    .build();
                ui.text("view.pose.resetText")
                    .position(width - pad - resetWidth, pad - 2.0f)
                    .size(resetWidth, resetHeight)
                    .text("Reset")
                    .fontSize(kFontCaption)
                    .color(tokens.fgSubtle)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }
        })
        .build();
}

}  // namespace viewer
