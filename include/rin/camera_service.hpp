#pragma once

#include "rin/camera_types.hpp"

namespace rin {

/// 相机服务契约：生命周期入口 + 有界最新态数据通道。
///
/// 公开头零第三方类型（RULE-01）；实现方（adapter）内部用 executor::comm 邮箱承载，
/// 以 "上次已见序号" 语义向调用方暴露最新快照。
///
/// 线程模型：start/stop/requestResolution 供 owner 线程（主线程）调用；
/// tryLoad* 供 UI 线程非阻塞调用；两者与 worker 线程之间全部经无锁/有界通道。
class ICameraService {
public:
    virtual ~ICameraService() = default;

    /// Idle -> Opening。返回 admitted=false 时不改变状态。设备枚举在调用线程同步执行。
    [[nodiscard]] virtual StartOutcome start(const StreamRequest& request) = 0;

    /// Streaming/Restreaming 中请求切换分辨率；入队成功返回 true，worker 在循环边界应用。
    virtual bool requestResolution(const StreamRequest& request, std::string* error = nullptr) = 0;

    /// 请求切换到指定序列号的设备（Waiting/Streaming/Restreaming 下有效）；
    /// 设备当前不在线时同样入队，worker 在其到达后自动应用。
    virtual bool requestDevice(const std::string& serial, std::string* error = nullptr) = 0;

    /// 请求切换深度图输出配色（DEC-007）；Waiting/Opening/Streaming/Restreaming 下
    /// 有效且粘性（跨插拔/设备切换保留）。仅影响后续帧转换，不触发重流；worker 在
    /// 下一帧边界应用。
    virtual bool requestDepthColorScheme(DepthColorScheme scheme,
                                         std::string* error = nullptr) = 0;

    /// 任意状态 -> Stopping -> Idle；幂等。阻塞至 worker 回收完成。
    ///
    /// 排空语义（M3-07 关闭回归）：返回后采集 worker 已回收，全部数据通道不再有
    /// 新发布；通道内保留的最新值对既有消费方保持 stale 语义（序号不回退），调用方
    /// 不得据此恢复活动状态——消费侧派生状态（如 viewer 姿态面板/3D 视图）应在
    /// 关闭路径显式排空（回空态），不得跨 shutdown 存活。
    virtual void stop() = 0;

    [[nodiscard]] virtual CameraServiceState state() const = 0;
    [[nodiscard]] virtual std::string lastError() const = 0;

    /// 取 kind 流中 sequence > lastSeenSequence 的最新帧；无新帧返回 false。
    [[nodiscard]] virtual bool tryLoadFrame(FrameKind kind,
                                            std::uint64_t& lastSeenSequence,
                                            Frame& out) = 0;

    [[nodiscard]] virtual bool tryLoadIntrinsics(std::uint64_t& lastSeenSequence,
                                                 IntrinsicsSnapshot& out) = 0;

    /// 取运动通道中 sequence > lastSeenSequence 的最新原始 IMU 采样（ACCEL/GYRO
    /// 共用通道，最新态语义）；无新采样返回 false。未使能运动流或设备无 IMU 时恒为
    /// false；原始采样仅作诊断，姿态消费方请使用 tryLoadPose()。
    [[nodiscard]] virtual bool tryLoadMotion(std::uint64_t& lastSeenSequence,
                                             MotionSample& out) = 0;

    /// 取姿态通道中 sequence > lastSeenSequence 的最新融合姿态快照（最新态语义，
    /// DEC-010/EXEC-06：采集 worker 内融合并发布）；无新快照返回 false。运动流未
    /// 使能或姿态尚未首次收敛时可能长期为 false。
    [[nodiscard]] virtual bool tryLoadPose(std::uint64_t& lastSeenSequence,
                                           ImuSnapshot& out) = 0;

    /// 取在线设备目录（含各设备能力与活动设备）；无更新返回 false。
    [[nodiscard]] virtual bool tryLoadCatalog(std::uint64_t& lastSeenSequence,
                                              DeviceCatalog& out) = 0;

    /// 最新事件快照（无序号语义；调用方只关心最近一条）。
    [[nodiscard]] virtual bool tryLoadEvent(ServiceEvent& out) = 0;
};

}  // namespace rin
