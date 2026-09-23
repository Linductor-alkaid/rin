#pragma once

// 绕行方案（EUI-20260923-003）：EUI-NEO 的 ImageStream 动态纹理上传在当前 GL 栈上
// 渲染异常（官方 examples/dynamic_texture.cpp 在本机同样复现竖向色带）。改用官方
// 外部 GPU 图像接口：UI/渲染线程上传自有 GL 纹理（RGBA8），经 importGpuImage 导入
// 绘制。仅限 viewer 应用边界内，不触碰 pinned 依赖。

#include <GL/gl.h>

#include <eui_neo.h>

#include <rs_vision/camera_types.hpp>

#include <cstdio>
#include <memory>

namespace viewer {

class GpuFrameView {
public:
    /// 上传一帧并按需（重新）导入。必须且只能在 UI/渲染线程（compose/onFrame 回调）
    /// 调用：GL 上下文仅在该线程 current。
    void update(const rsv::Frame& frame) {
        if (!frame.valid()) {
            return;
        }
        if (!ensureImported(frame.width, frame.height)) {
            return;
        }
        glBindTexture(GL_TEXTURE_2D, texture_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,
                      static_cast<GLint>(frame.stride / 4u));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(frame.width),
                        static_cast<GLsizei>(frame.height), GL_RGBA, GL_UNSIGNED_BYTE,
                        frame.pixels->data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        ++revision_;
    }

    [[nodiscard]] std::shared_ptr<const eui::GpuImage> image() const { return imported_; }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    [[nodiscard]] bool valid() const { return imported_ != nullptr; }

    /// onShutdown（GPU 设备销毁前、主线程）释放应用持有的导入引用。
    void release() {
        imported_.reset();
        texture_ = 0;
        width_ = 0;
        height_ = 0;
    }

private:
    /// 创建纹理并导入；分辨率变化时换新纹理（旧纹理由框架 retirement 队列经
    /// owner deleter 在 GPU 读取结束后删除，不在此处立即删除）。
    bool ensureImported(std::uint32_t width, std::uint32_t height) {
        if (imported_ != nullptr && width_ == width && height_ == height) {
            return true;
        }
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        const GLuint* owned = new GLuint(texture_);
        std::shared_ptr<const void> lifetime(
            owned, [](const void* p) {
                const GLuint texture = *static_cast<const GLuint*>(p);
                glDeleteTextures(1, &texture);
                delete static_cast<const GLuint*>(p);
            });

        eui::GpuImageDescriptor descriptor;
        descriptor.device = eui::image::gpuDevice();
        descriptor.width = static_cast<int>(width);
        descriptor.height = static_cast<int>(height);
        descriptor.texture = texture_;
        imported_ = eui::image::importGpuImage(descriptor, lifetime);
        if (imported_ == nullptr) {
            std::fprintf(stderr, "importGpuImage failed\n");
            imported_.reset();
            glDeleteTextures(1, &texture_);
            texture_ = 0;
            width_ = 0;
            height_ = 0;
            return false;
        }
        width_ = width;
        height_ = height;
        revision_ = 0;
        return true;
    }

    GLuint texture_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::shared_ptr<const eui::GpuImage> imported_;
    std::uint64_t revision_ = 0;
};

}  // namespace viewer
