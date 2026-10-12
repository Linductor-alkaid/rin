#pragma once

// 官方推荐显示路径（EUI-20260923-003 绕行移除）：eui-neo v0.6.1 修复
// convertToRgba8 的 RGBA8/BGRA8 目标指针推进（f8caff1 / 上游 PR #97）后，
// 真机回归确认 ImageStream 全表面正常更新（2026-10-12，见依赖台账升级会话），
// GpuFrameView（importGpuImage 外部导入绕行）按移除条件删除。
// UI/渲染线程 pump 中以最新帧 submit；容量 2 有界最新帧邮箱（DEC-002 集成决策）。

#include <eui_neo.h>

#include <rin/camera_types.hpp>

#include <memory>

namespace viewer {

class FrameStreamView {
public:
    FrameStreamView() : stream_(std::make_shared<eui::ImageStream>(2)) {}

    /// 提交一帧。必须且只能在 UI/渲染线程（pump/compose 回调）调用：stream 的
    /// 消费端（渲染原语 updateTexture）运行在同一上下文，提交侧仅做有界入队。
    void update(const rin::Frame& frame) {
        if (!frame.valid()) {
            return;
        }
        const eui::ImageFrame imageFrame{frame.pixels, frame.width, frame.height,
                                         frame.stride, eui::ImagePixelFormat::RGBA8,
                                         frame.sequence};
        if (stream_->submit(imageFrame)) {
            hasFrames_ = true;
        }
    }

    [[nodiscard]] const std::shared_ptr<eui::ImageStream>& stream() const { return stream_; }
    [[nodiscard]] bool valid() const { return hasFrames_; }

    /// Waiting/Failed 排空与 shutdown（DEC-006 稳态语义）：换入空流。运行时按
    /// sourceChanged（instance.stream != element.imageStream）检测到流更换后
    /// releaseTexture，画面回空态而非保留陈旧末帧。
    void release() {
        stream_ = std::make_shared<eui::ImageStream>(2);
        hasFrames_ = false;
    }

private:
    std::shared_ptr<eui::ImageStream> stream_;
    bool hasFrames_ = false;
};

}  // namespace viewer
