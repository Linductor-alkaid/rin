// M5-06 工作流运行控制 + 假换真集成（ui_workspace_design.md §5.8 / DEC-016）
// 引擎-UI 集成测试 —— 独立验证（Independent Verification Agent）。
// M6 扩展（M6-03/04/05/06，DEC-017 工作流相机源四型 + ROI 控件防呆）。
//
// 被测面：
//   1. 相机帧源接缝（apps/viewer/workflow_frame_source.hpp viewer::
//      makeCameraFrameSource，对脚本化假 ICameraService）：最新帧语义
//      （"上次已见序号"过滤读取 + 探测即消费）、零拷贝（ImageU8::wrap 与发布
//      Frame/GrayFrame 共享同一像素缓冲，指针相等）、多源分发（图内每个 source
//      节点按调用方传入水位独立推进）、无效帧（valid()==false → false）与
//      service 空指针防御、sourceSequence 透传帧序号、wrap 后格式/尺寸/stride
//      一致；
//   2. 运行控制状态机（WorkflowCanvasState::afterGraphChange 纯逻辑 + 真引擎）：
//      Idle 下同步生效无待生效（graphPending==false）、Running 下校验通过入队
//      置 graphPending、app.cpp pump 契约行内联复刻（GraphApplied 事件清除 +
//      引擎离开 Running 清除）、校验拒绝路径（Running 下悬空输入 / 成环图
//      → validation.ok==false 且 graphPending 不置位）、stop 后 Idle + 待生效
//      标志清除、重启后 UI 侧标志衔接（perf live 翻转 + 待生效语义继续工作）；
//   3. 引擎-UI 端到端（假相机服务 → makeCameraFrameSource → M4-07 真引擎 →
//      UI 消费管道）：NodeOutputCache 对 source/crop/grayify 拉到 valid 快照
//      （Rgba8/Rgba8/Gray8、sourceSequence == 相机帧序号）、参数热更新"下一帧
//      生效"（requestParamUpdate crop.width → 输出尺寸变化）、stop 排空（§4：
//      pumpNodeOutput 非 Running 分支清空缩略图缓存、tryLoadStats 无新快照、
//      stale 读取保持旧值、状态 Idle、幂等双 stop）、onShutdown 关闭顺序回归
//      （M5-06 完成判据，app.cpp shutdown() 同构全序：服务 stop（停止后取帧
//      恒 false）→ 引擎 stop + 释放 → 面板排空（NodeOutputCache/WorkflowPerfState/
//      NodeFailureMarks clear）→ executor.shutdown(true) Completed；stop 返回后
//      有界稳定窗内无新发布）、Running 中直接 stop 数轮稳定收敛；
//   4. 关闭竞态防御（对齐 test_workflow_engine §11 纪律）：Running 中
//      executor.shutdown(false) 后 stop() 有界收敛 Idle 不悬挂（确认帧源接缝
//      存在时引擎自身纪律仍成立）；
//   5. 相机源 rendition 路由（M6-05，DEC-017）：renditionForSourceType 五映射
//      + 未知 typeId 回退 RGB；WorkflowSourceRouter 初态/未知节点回退、replace
//      整体换新对读方生效；makeCameraFrameSource 对路由四 rendition（RGB/深度
//      伪彩/深度灰度/深度自适应）按节点分发取帧（tryLoadFrame 三 kind +
//      tryLoadGrayFrame 两 kind）、格式/尺寸/序号正确、Rgba8/Gray8 均零拷贝
//      （pixels 指针相等）、无效帧 false 且水位不动、灰度通道与 RGB 隔离；
//   6. 灰度链真引擎端到端（M6-03，DEC-017）：合成 Gray8 帧源 →
//      source_depth_gray → crop_gray → gaussian_blur：各节点产物 valid、格式
//      Gray8、尺寸符合冻结公式（源 32x24 / 裁切 16x12 / 模糊 16x12）、
//      sourceSequence 透传、引擎保持 Running 无失败；
//   7. pumpNodeOutput 输入驱动节点拉取（M6-06 ROI 联动约束的尺寸源）：选中
//      crop 节点（入边来自 source）Running 后 panel.outputs 同时含选中节点与
//      驱动节点快照（尺寸可得）；引擎回 Idle 后整体排空（一次 true、幂等
//      二次 false）；
//   8. 工作台布局状态（M7-03/04）：WorkflowCanvasState 五区 docking 字段默认值
//      见证（= M5-02 冻结骨架几何 200/264/120/148、dockDrag==-1、paletteScroll
//      初值 0）与 compose 期 std::clamp 夹取边界常量语义（默认值在范围内恒等、
//      越界拖拽收敛边界、范围内值不被修改）；分隔条 mouseArea 回调时序与视觉
//      呈现 headless 不可测（归真机验收）。
//
// DOD-02 适用性说明（与 tests/test_shutdown_drain.cpp 同纪律，如实取舍）：
// 正常完成——§3 全链路覆盖（发布→捕获→执行→发布→UI 消费）；任务异常——节点
// 失败归因经 future 排空 → NodeFailed/Failed 可观察，细节由
// test_workflow_engine §10 与 test_perf_panel 失败冻结覆盖，本文件 §4 回归
// 关停竞态；提交拒绝与执行中取消——属 Executor 自身设施（引擎经
// submit_cancellable/TimerHandle 使用，Executor 自测覆盖；引擎层显式化结果
// droppedFrames/Info 事件由 test_workflow_engine §6 断言），本文件 stop 排空
// 路径覆盖协作取消消费；超时——全部等待为有界轮询（5s 死限）/静默窗
// （300ms），不悬挂；shutdown——§3 onShutdown 同构全序断言 shutdown(true)
// Completed、§4 覆盖 shutdown(false) 对抗路径。
//
// headless 不可测（归 M5-07 真机验收，M5-03/04/05 同纪律）：compose 绘制路径，
// 含 composeWorkflowToolbar 的启动/停止按钮可用条件推导（validation.ok/state
// 组合）与"pending - applies next frame"待生效标注的视觉呈现、pumpNodeOutput
// 的缩略图 GL 上传分支。本文件只覆盖其平台无关语义（缓存清空分支）。
//
// 测试壳为 tests/test_util.hpp 的 RIN_CHECK*；有界等待用本文件匿名命名空间的
// pollUntil/quietFor（与 workflow_engine_contract_suite.hpp 同款，不引入该头以
// 避免契约套件符号混入）。假相机服务为脚本化最小实现：ICameraService 全部纯虚
// 函数（含 M6-04 的 tryLoadGrayFrame 灰度 rendition 通道），帧由测试线程同步
// 发布（按 FrameKind / GrayFrameKind 分槽的最新帧槽，"上次已见序号"语义），
// 不创建 std::thread；Executor 线程（引擎帧泵）与测试线程的全部共享经该 mutex。
// DOD-02 适用性说明（文件头"取舍说明"段）：§5/§6/§7 为 M6 新增——§5 路由分发
// 与 §6 灰度链覆盖正常完成路径；§7 覆盖非 Running 排空消费；失败/拒绝/超时/
// shutdown 取舍与 §1-§4 既有说明一致（失败归因归 test_workflow_engine §10，
// stop 排空与关停竞态归 §3/§4，全部等待有界不悬挂）。

#include "param_panel.hpp"  // WorkflowPanelState / pumpNodeOutput（含 node_canvas.hpp）

#include "workflow_frame_source.hpp"

#include "test_util.hpp"

#include <executor/executor.hpp>

#include <rin/camera_service.hpp>
#include <rin/camera_types.hpp>
#include <rin/workflow_engine.hpp>
#include <rin/workflow_types.hpp>

#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using viewer::WorkflowCanvasState;
using viewer::WorkflowPanelState;

// --- 断言与有界等待辅助（与 test_param_panel.cpp 同纪律） ---

void runSection(const char* name, void (*fn)()) {
    std::printf("== %s\n", name);
    fn();
}

// 有界轮询（5s 死限，防悬挂；pred() 为真即返回；超时后最后一次 pred() 定结果）。
template <typename Pred>
bool pollUntil(Pred&& pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

// 静默检查：window 内 hasNew() 一旦为真即返回 false（出现新发布）；全窗安静
// 返回 true。
template <typename HasNew>
bool quietFor(HasNew&& hasNew, std::chrono::milliseconds window) {
    const auto deadline = std::chrono::steady_clock::now() + window;
    while (std::chrono::steady_clock::now() < deadline) {
        if (hasNew()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

constexpr std::chrono::milliseconds kQuietWindow{300};

// --- 脚本化假相机服务（测试脚本面 + ICameraService 全纯虚实现） ---

/// 测试线程同步发布最新帧（RGBA8 rendition 按 FrameKind 分槽；M6-04 起灰度
/// rendition 按 GrayFrameKind 分槽）；stop() 后 tryLoadFrame/tryLoadGrayFrame
/// 恒为 false（ICameraService::stop 排空契约的最小等价：返回后全部数据通道
/// 不再有新发布）。帧读取与引擎帧泵线程之间的共享经 mutex（帧源契约"非阻塞"
/// 指不等待帧到达，mutex 短临界区满足）。
class FakeCameraService final : public rin::ICameraService {
public:
    // --- 测试脚本面（测试主线程同步调用） ---
    void publish(rin::Frame frame) {
        const std::scoped_lock lock(mutex_);
        latestRgba_[frame.kind] = std::move(frame);
    }

    void publishGray(rin::GrayFrameKind kind, rin::GrayFrame frame) {
        const std::scoped_lock lock(mutex_);
        latestGray_[kind] = std::move(frame);
    }

    // --- ICameraService（相机源接缝只经 tryLoadFrame/tryLoadGrayFrame；其余为
    //     契约桩） ---
    rin::StartOutcome start(const rin::StreamRequest&) override {
        const std::scoped_lock lock(mutex_);
        state_ = rin::CameraServiceState::Streaming;
        rin::StartOutcome outcome;
        outcome.admitted = true;
        return outcome;
    }

    bool requestResolution(const rin::StreamRequest&, std::string*) override {
        return true;
    }

    bool requestDevice(const std::string&, std::string*) override { return true; }

    bool requestDepthColorScheme(rin::DepthColorScheme, std::string*) override {
        return true;
    }

    void stop() override {
        const std::scoped_lock lock(mutex_);
        stopped_ = true;
        state_ = rin::CameraServiceState::Idle;
    }

    [[nodiscard]] rin::CameraServiceState state() const override {
        const std::scoped_lock lock(mutex_);
        return state_;
    }

    [[nodiscard]] std::string lastError() const override { return {}; }

    [[nodiscard]] bool tryLoadFrame(rin::FrameKind kind,
                                    std::uint64_t& lastSeenSequence,
                                    rin::Frame& out) override {
        const std::scoped_lock lock(mutex_);
        if (stopped_) {
            return false;  // 排空契约：stop 返回后不再有新发布。
        }
        const auto it = latestRgba_.find(kind);
        if (it == latestRgba_.end() || !it->second.valid() ||
            it->second.sequence <= lastSeenSequence) {
            return false;
        }
        out = it->second;
        lastSeenSequence = it->second.sequence;
        return true;
    }

    [[nodiscard]] bool tryLoadGrayFrame(rin::GrayFrameKind kind,
                                        std::uint64_t& lastSeenSequence,
                                        rin::GrayFrame& out) override {
        const std::scoped_lock lock(mutex_);
        if (stopped_) {
            return false;  // 排空契约：stop 返回后不再有新发布。
        }
        const auto it = latestGray_.find(kind);
        if (it == latestGray_.end() || !it->second.valid() ||
            it->second.sequence <= lastSeenSequence) {
            return false;
        }
        out = it->second;
        lastSeenSequence = it->second.sequence;
        return true;
    }

    [[nodiscard]] bool tryLoadIntrinsics(std::uint64_t&,
                                         rin::IntrinsicsSnapshot&) override {
        return false;
    }

    [[nodiscard]] bool tryLoadMotion(std::uint64_t&, rin::MotionSample&) override {
        return false;
    }

    [[nodiscard]] bool tryLoadPose(std::uint64_t&, rin::ImuSnapshot&) override {
        return false;
    }

    [[nodiscard]] bool tryLoadCatalog(std::uint64_t&, rin::DeviceCatalog&) override {
        return false;
    }

    [[nodiscard]] bool tryLoadEvent(rin::ServiceEvent&) override { return false; }

private:
    mutable std::mutex mutex_;
    std::map<rin::FrameKind, rin::Frame> latestRgba_;
    std::map<rin::GrayFrameKind, rin::GrayFrame> latestGray_;
    bool stopped_ = false;
    rin::CameraServiceState state_ = rin::CameraServiceState::Idle;
};

// --- 帧夹具 ---

/// 确定性 Rgba8 帧（每序号独立缓冲：零拷贝指针相等检查需要可区分的缓冲对象）。
[[nodiscard]] rin::Frame makeFrame(std::uint64_t sequence, std::uint32_t width,
                                   std::uint32_t height) {
    rin::Frame frame;
    frame.kind = rin::FrameKind::Rgb;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4u;
    frame.sequence = sequence;
    frame.pixels = std::make_shared<const std::vector<std::uint8_t>>(
        static_cast<std::size_t>(frame.stride) * height,
        static_cast<std::uint8_t>(sequence & 0xFF));
    return frame;
}

/// 确定性 Gray8 帧（紧凑行距 stride=width，M6-04 契约；递增图案逐像素可核对）。
[[nodiscard]] rin::GrayFrame makeGrayFrame(std::uint64_t sequence, std::uint32_t width,
                                           std::uint32_t height) {
    rin::GrayFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width;
    frame.sequence = sequence;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint8_t>((x + y * width) & 0xFF);
        }
    }
    frame.pixels =
        std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
    return frame;
}

/// 将 node → rendition 路由表装入 router（映射表外节点防御性回退 RGB）。
/// WorkflowSourceRouter 持有 atomic<shared_ptr>（不可拷贝/移动），只能在所属
/// 作用域就地构造后填充。
void fillRouter(
    viewer::WorkflowSourceRouter& router,
    std::initializer_list<
        std::pair<const rin::NodeId, viewer::WorkflowSourceRendition>> routes) {
    auto map = std::make_shared<viewer::WorkflowSourceRouter::RouteMap>();
    for (const auto& entry : routes) {
        map->insert_or_assign(entry.first, entry.second);
    }
    router.replace(std::move(map));
}

/// source(id) -> crop -> grayify 标准三节点链（crop 默认 ROI (0,0,64,64) 适配
/// 128x96 帧；末端 grayify 产出 Gray8）。
[[nodiscard]] rin::WorkflowGraph makeTripleChainGraph() {
    rin::WorkflowGraph graph;
    const char* typeIds[3] = {"source", "crop", "grayify"};
    for (std::uint64_t i = 0; i < 3; ++i) {
        rin::NodeInstance instance;
        instance.id = i + 1;
        instance.typeId = typeIds[i];
        graph.nodes.push_back(instance);
    }
    for (std::uint64_t i = 0; i < 2; ++i) {
        rin::Connection connection;
        connection.from = rin::PortRef{i + 1, rin::PortDirection::Output, 0};
        connection.to = rin::PortRef{i + 2, rin::PortDirection::Input, 0};
        graph.connections.push_back(connection);
    }
    return graph;
}

// --- 1. 相机帧源接缝（对脚本化假 ICameraService） ---

void testCameraFrameSourceSeam() {
    // 最新帧语义 + 零拷贝 + sourceSequence 透传 + 格式/尺寸/stride 一致。
    {
        auto service = std::make_shared<FakeCameraService>();
        viewer::WorkflowSourceRouter router;
        fillRouter(router, {{1, viewer::WorkflowSourceRendition::RgbColor}});
        const rin::WorkflowFrameSource source =
            viewer::makeCameraFrameSource(service, router);

        // 发布 seq 1..3（保留各缓冲 shared_ptr 供零拷贝核对）。
        rin::Frame frame1 = makeFrame(1, 32, 24);
        rin::Frame frame2 = makeFrame(2, 32, 24);
        rin::Frame frame3 = makeFrame(3, 32, 24);
        service->publish(frame1);
        service->publish(frame2);
        service->publish(frame3);

        const std::vector<std::uint8_t>* buffer3 = frame3.pixels.get();

        // 探测一次：得最新帧 seq3（lastSeen 由实现推进），零拷贝共享缓冲。
        std::uint64_t lastSeen = 0;
        rin::WorkflowFrameInput out;
        RIN_CHECK_MSG(source(1, lastSeen, out),
                      "seam: probe after publish delivers the latest frame");
        RIN_CHECK_EQ(out.sourceSequence, std::uint64_t{3});
        RIN_CHECK_EQ(lastSeen, std::uint64_t{3});
        RIN_CHECK_MSG(out.image.pixels().get() == buffer3,
                      "seam: wrapped image shares the published frame buffer "
                      "(zero copy)");
        RIN_CHECK(out.image.valid());
        RIN_CHECK(out.image.format() == rin::PortType::Rgba8);
        RIN_CHECK_EQ(out.image.width(), frame3.width);
        RIN_CHECK_EQ(out.image.height(), frame3.height);
        RIN_CHECK_EQ(out.image.stride(), frame3.stride);
        RIN_CHECK_EQ(out.image.byteSize(),
                     static_cast<std::uint64_t>(frame3.stride) * frame3.height);

        // 再探测：无比 lastSeen 更新的帧 → false（探测即消费，最新态过滤）。
        rin::WorkflowFrameInput discarded;
        std::uint64_t consumed = lastSeen;
        RIN_CHECK_MSG(!source(1, consumed, discarded),
                      "seam: re-probe without a newer frame returns false");
        RIN_CHECK_EQ(consumed, std::uint64_t{3});  // 无新帧时水位不动。

        // 再发布 seq4 → 得 seq4。
        rin::Frame frame4 = makeFrame(4, 32, 24);
        service->publish(frame4);
        rin::WorkflowFrameInput out4;
        RIN_CHECK(source(1, lastSeen, out4));
        RIN_CHECK_EQ(out4.sourceSequence, std::uint64_t{4});
        RIN_CHECK_EQ(lastSeen, std::uint64_t{4});
        RIN_CHECK(out4.image.pixels().get() == frame4.pixels.get());
    }

    // 多源分发：两个 source 节点各自独立水位（节点 A 消费不推进节点 B 水位）。
    // 节点 10/20 不在路由表内 → 防御性回退 RGB（路由覆盖与水位语义正交）。
    {
        auto service = std::make_shared<FakeCameraService>();
        viewer::WorkflowSourceRouter router;
        fillRouter(router, {});
        const rin::WorkflowFrameSource source =
            viewer::makeCameraFrameSource(service, router);
        service->publish(makeFrame(1, 16, 16));
        service->publish(makeFrame(2, 16, 16));
        service->publish(makeFrame(3, 16, 16));

        std::uint64_t lastSeenA = 0;
        std::uint64_t lastSeenB = 0;
        rin::WorkflowFrameInput outA;
        rin::WorkflowFrameInput outB;
        RIN_CHECK(source(10, lastSeenA, outA));  // 节点 A 首探测 → seq3。
        RIN_CHECK_EQ(outA.sourceSequence, std::uint64_t{3});
        RIN_CHECK_EQ(lastSeenA, std::uint64_t{3});
        RIN_CHECK_MSG(source(20, lastSeenB, outB),
                      "seam: node B keeps its own watermark");
        RIN_CHECK_EQ(outB.sourceSequence, std::uint64_t{3});
        RIN_CHECK_EQ(lastSeenB, std::uint64_t{3});

        service->publish(makeFrame(4, 16, 16));
        RIN_CHECK(source(10, lastSeenA, outA));  // A 消费 seq4。
        RIN_CHECK_EQ(lastSeenA, std::uint64_t{4});
        RIN_CHECK_MSG(source(20, lastSeenB, outB),
                      "seam: node A consumption must not advance node B");
        RIN_CHECK_EQ(lastSeenB, std::uint64_t{4});
        std::uint64_t probe = lastSeenA;
        rin::WorkflowFrameInput discarded;
        RIN_CHECK(!source(10, probe, discarded));  // A 再次探测无新帧。
    }

    // 无效帧防御：width=0（Frame::valid()==false）与空像素缓冲 → false。
    {
        auto service = std::make_shared<FakeCameraService>();
        viewer::WorkflowSourceRouter router;
        fillRouter(router, {{1, viewer::WorkflowSourceRendition::RgbColor}});
        const rin::WorkflowFrameSource source =
            viewer::makeCameraFrameSource(service, router);
        rin::Frame zeroWidth = makeFrame(1, 32, 24);
        zeroWidth.width = 0;
        service->publish(zeroWidth);
        std::uint64_t lastSeen = 0;
        rin::WorkflowFrameInput out;
        RIN_CHECK_MSG(!source(1, lastSeen, out),
                      "seam: invalid frame (zero width) is treated as no input");
        RIN_CHECK_EQ(lastSeen, std::uint64_t{0});

        rin::Frame noPixels = makeFrame(2, 32, 24);
        noPixels.pixels = nullptr;
        service->publish(noPixels);
        RIN_CHECK_MSG(!source(1, lastSeen, out),
                      "seam: pixel-less frame is treated as no input");

        // 恢复有效帧后正常交付。
        service->publish(makeFrame(3, 32, 24));
        RIN_CHECK(source(1, lastSeen, out));
        RIN_CHECK_EQ(out.sourceSequence, std::uint64_t{3});
    }

    // service 空指针 → false（引擎停止拉帧前的关闭窗口内安全）。
    {
        viewer::WorkflowSourceRouter router;
        fillRouter(router, {{1, viewer::WorkflowSourceRendition::RgbColor}});
        const rin::WorkflowFrameSource nullSource =
            viewer::makeCameraFrameSource(nullptr, router);
        std::uint64_t lastSeen = 0;
        rin::WorkflowFrameInput out;
        RIN_CHECK_MSG(!nullSource(1, lastSeen, out),
                      "seam: null service returns false (no crash)");
    }

    // 灰度 rendition（M6-04）：Gray8 帧经接缝零拷贝包装、sourceSequence 透传。
    {
        auto service = std::make_shared<FakeCameraService>();
        viewer::WorkflowSourceRouter router;
        fillRouter(router, {{1, viewer::WorkflowSourceRendition::DepthGray}});
        const rin::WorkflowFrameSource source =
            viewer::makeCameraFrameSource(service, router);
        rin::GrayFrame gray = makeGrayFrame(4, 24, 18);
        service->publishGray(rin::GrayFrameKind::Depth, gray);

        std::uint64_t lastSeen = 0;
        rin::WorkflowFrameInput out;
        RIN_CHECK_MSG(source(1, lastSeen, out),
                      "seam: gray rendition delivers the latest gray frame");
        RIN_CHECK_EQ(out.sourceSequence, std::uint64_t{4});
        RIN_CHECK_EQ(lastSeen, std::uint64_t{4});
        RIN_CHECK_MSG(out.image.pixels().get() == gray.pixels.get(),
                      "seam: wrapped gray image shares the published buffer (zero copy)");
        RIN_CHECK(out.image.format() == rin::PortType::Gray8);
        RIN_CHECK_EQ(out.image.width(), 24u);
        RIN_CHECK_EQ(out.image.height(), 18u);
        RIN_CHECK_EQ(out.image.stride(), 24u);  // 紧凑行距（Gray8 stride >= width）。

        // 再探测：无更新灰度帧 → false。
        rin::WorkflowFrameInput discarded;
        RIN_CHECK(!source(1, lastSeen, discarded));
    }
}

// --- 2. 运行控制状态机（WorkflowCanvasState 纯逻辑 + 真引擎） ---

/// app.cpp pump 的工作流事件消费契约行内联复刻（app.cpp:378-399 的平台无关
/// 语义：事件行 + 失败标注 + GraphApplied 清待生效 + 离开 Running 清待生效）。
/// 返回本 tick 是否清除了待生效标志。
bool pumpWorkflowEvents(WorkflowCanvasState& canvas, rin::IWorkflowEngine& engine) {
    bool cleared = false;
    rin::WorkflowEvent event;
    if (engine.tryLoadEvent(event)) {
        canvas.failures.applyEvent(event);
        if (event.kind == rin::WorkflowEventKind::GraphApplied && canvas.graphPending) {
            canvas.graphPending = false;
            cleared = true;
        }
    }
    if (canvas.graphPending && engine.state() != rin::WorkflowEngineState::Running) {
        canvas.graphPending = false;
        cleared = true;
    }
    return cleared;
}

void testRunControlStateMachine() {
    executor::Executor executor;
    executor::ExecutorConfig executorConfig;
    const bool initialized = executor.initialize(executorConfig);
    RIN_CHECK(initialized);
    if (!initialized) {
        return;
    }
    auto service = std::make_shared<FakeCameraService>();
    viewer::WorkflowSourceRouter router;
    fillRouter(router, {{1, viewer::WorkflowSourceRendition::RgbColor}});
    rin::WorkflowEngineConfig config;
    config.pumpInterval = std::chrono::milliseconds{2};
    config.frameSource = viewer::makeCameraFrameSource(service, router);
    std::shared_ptr<rin::IWorkflowEngine> engine =
        rin::createWorkflowEngine(executor, std::move(config));
    RIN_CHECK(static_cast<bool>(engine));
    if (!engine) {
        return;
    }

    WorkflowCanvasState canvas;
    canvas.model.catalog = &engine->catalog();

    // ---- Idle：单 source 节点 afterGraphChange → 校验通过、无待生效 ----
    RIN_CHECK(canvas.model.createNode("source", {0.0f, 0.0f}).ok);
    canvas.afterGraphChange(engine.get());
    RIN_CHECK_MSG(canvas.model.validation.ok,
                  "runctl: single source graph validates in Idle");
    RIN_CHECK_MSG(!canvas.graphPending,
                  "runctl: Idle applies synchronously (no pending)");

    // ---- start 准入 → Running：改图（追加 grayify）→ graphPending 置位 ----
    const rin::AdmissionResult admission = engine->start();
    RIN_CHECK_MSG(admission.admitted, "runctl: start admitted with valid graph");
    RIN_CHECK(engine->state() == rin::WorkflowEngineState::Running);

    RIN_CHECK(canvas.model.createNode("grayify", {300.0f, 0.0f}).ok);
    RIN_CHECK(canvas.model
                  .connect({1, rin::PortDirection::Output, 0},
                           {2, rin::PortDirection::Input, 0})
                  .ok);
    canvas.afterGraphChange(engine.get());
    RIN_CHECK_MSG(canvas.model.validation.ok,
                  "runctl: appended chain still validates while running");
    RIN_CHECK_MSG(canvas.graphPending,
                  "runctl: graph change while Running enqueues (pending set)");

    // ---- pump 同构消费：GraphApplied 在有界轮询内清除待生效 ----
    RIN_CHECK_MSG(pollUntil([&] { return pumpWorkflowEvents(canvas, *engine); }),
                  "runctl: GraphApplied clears the pending flag at frame boundary");
    RIN_CHECK(!canvas.graphPending);

    // ---- 校验拒绝路径（Running 下验证）：悬空输入 ----
    {
        WorkflowCanvasState danglingCanvas;
        danglingCanvas.model.catalog = &engine->catalog();
        RIN_CHECK(danglingCanvas.model.createNode("grayify", {0.0f, 0.0f}).ok);
        danglingCanvas.afterGraphChange(engine.get());
        RIN_CHECK_MSG(!danglingCanvas.model.validation.ok,
                      "runctl: dangling input is rejected while Running");
        RIN_CHECK_MSG(!danglingCanvas.graphPending,
                      "runctl: rejected change must not set pending");
    }

    // ---- 校验拒绝路径（Running 下验证）：成环图 ----
    {
        // crop(Rgba8→Rgba8) 与 downscale(Rgba8→Rgba8) 互连成环（端口类型合法、
        // 无多驱动）；直接写入 connections 绕过 connect() 预检，模拟到达引擎
        // 准入面的程序化构造路径——applyGraph 同步拒绝且不置待生效。
        WorkflowCanvasState cycleCanvas;
        cycleCanvas.model.catalog = &engine->catalog();
        RIN_CHECK(cycleCanvas.model.createNode("crop", {0.0f, 0.0f}).ok);
        RIN_CHECK(cycleCanvas.model.createNode("downscale", {300.0f, 0.0f}).ok);
        cycleCanvas.model.connections.push_back(
            rin::Connection{{1, rin::PortDirection::Output, 0},
                            {2, rin::PortDirection::Input, 0}});
        cycleCanvas.model.connections.push_back(
            rin::Connection{{2, rin::PortDirection::Output, 0},
                            {1, rin::PortDirection::Input, 0}});
        cycleCanvas.afterGraphChange(engine.get());
        bool hasCycleIssue = false;
        for (const rin::ValidationIssue& issue : cycleCanvas.model.validation.issues) {
            hasCycleIssue = hasCycleIssue || issue.kind == rin::ValidationIssueKind::Cycle;
        }
        RIN_CHECK_MSG(!cycleCanvas.model.validation.ok && hasCycleIssue,
                      "runctl: cyclic graph is rejected with a Cycle issue");
        RIN_CHECK_MSG(!cycleCanvas.graphPending,
                      "runctl: rejected cycle must not set pending");
        RIN_CHECK_MSG(engine->state() == rin::WorkflowEngineState::Running,
                      "runctl: engine keeps running after a rejected graph");
    }

    // ---- stop：引擎回 Idle；pump 契约"state != Running 清 pending"生效 ----
    // 先制造一次待生效（Running 下改图），随即 stop（有界阻塞排空）：
    // GraphApplied 与 stop 竞态，两条清除路径任一生效都收敛为 false。
    RIN_CHECK(canvas.model.createNode("gaussian_blur", {600.0f, 0.0f}).ok);
    RIN_CHECK(canvas.model
                  .connect({2, rin::PortDirection::Output, 0},
                           {3, rin::PortDirection::Input, 0})
                  .ok);
    canvas.afterGraphChange(engine.get());
    RIN_CHECK(canvas.graphPending);  // grayify(Gray8) → gaussian_blur(Gray8) 合法。
    engine->stop();
    RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);
    RIN_CHECK_MSG(pollUntil([&] { return pumpWorkflowEvents(canvas, *engine); }),
                  "runctl: leaving Running clears the pending flag");
    RIN_CHECK(!canvas.graphPending);
    engine->stop();  // 幂等双 stop。
    RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);

    // ---- 重启衔接（UI 侧标志）：live 翻转恢复 + 待生效语义继续工作 ----
    const rin::AdmissionResult restart = engine->start();
    RIN_CHECK_MSG(restart.admitted, "runctl: restart after stop is admitted");
    RIN_CHECK_MSG(canvas.perf.consume(*engine) && canvas.perf.live(),
                  "runctl: UI perf pipeline reconnects on the new session");
    canvas.afterGraphChange(engine.get());
    RIN_CHECK_MSG(canvas.model.validation.ok && canvas.graphPending,
                  "runctl: pending semantics keep working in the new session");
    RIN_CHECK_MSG(pollUntil([&] { return pumpWorkflowEvents(canvas, *engine); }),
                  "runctl: new-session GraphApplied clears pending");
    RIN_CHECK(!canvas.graphPending);

    // compose 按钮可用条件推导（validation.ok/state 组合）与待生效标注视觉
    // 呈现属 compose 层，headless 不可测——归 M5-07 真机验收（文件头说明）。

    engine->stop();
    engine.reset();
    RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
}

// --- 3. 引擎-UI 端到端（相机帧源 → 真引擎 → UI 消费管道） ---

void testEngineUiEndToEnd() {
    executor::Executor executor;
    executor::ExecutorConfig executorConfig;
    const bool initialized = executor.initialize(executorConfig);
    RIN_CHECK(initialized);
    if (!initialized) {
        return;
    }
    auto service = std::make_shared<FakeCameraService>();
    viewer::WorkflowSourceRouter router;
    fillRouter(router, {{1, viewer::WorkflowSourceRendition::RgbColor}});
    rin::WorkflowEngineConfig config;
    config.pumpInterval = std::chrono::milliseconds{2};
    config.frameSource = viewer::makeCameraFrameSource(service, router);
    std::shared_ptr<rin::IWorkflowEngine> engine =
        rin::createWorkflowEngine(executor, std::move(config));
    RIN_CHECK(static_cast<bool>(engine));
    if (!engine) {
        return;
    }

    RIN_CHECK(engine->applyGraph(makeTripleChainGraph()).ok);
    const rin::AdmissionResult admission = engine->start();
    RIN_CHECK(admission.admitted);
    if (!admission.admitted) {
        engine->stop();
        engine.reset();
        (void)executor.shutdown(true);
        return;
    }

    // 发布 seq 1..3 后停止发布（最新帧语义：引擎最终收敛到 seq3）。
    service->publish(makeFrame(1, 128, 96));
    service->publish(makeFrame(2, 128, 96));
    service->publish(makeFrame(3, 128, 96));

    // NodeOutputCache 对三节点拉到 valid 快照：格式/尺寸正确、sourceSequence
    // 为相机帧序号（最新帧 seq3 经接缝透传至源及其下游）。
    viewer::NodeOutputCache cache;
    {
        std::uint64_t seen1 = 0;
        rin::NodeOutputSnapshot out1;
        RIN_CHECK_MSG(pollUntil([&] {
                          return engine->tryLoadNodeOutput(1, seen1, out1) &&
                                 out1.valid() && out1.sourceSequence == 3;
                      }),
                      "e2e: source node carries the latest camera frame sequence");
        RIN_CHECK(out1.format == rin::PortType::Rgba8);
        RIN_CHECK_EQ(out1.width, 128u);
        RIN_CHECK_EQ(out1.height, 96u);

        std::uint64_t seen2 = 0;
        rin::NodeOutputSnapshot out2;
        RIN_CHECK(pollUntil([&] {
            return engine->tryLoadNodeOutput(2, seen2, out2) && out2.valid() &&
                   out2.sourceSequence == 3;
        }));
        RIN_CHECK(out2.format == rin::PortType::Rgba8);
        RIN_CHECK_EQ(out2.width, 64u);  // crop 默认 ROI (0,0,64,64)。
        RIN_CHECK_EQ(out2.height, 64u);

        std::uint64_t seen3 = 0;
        rin::NodeOutputSnapshot out3;
        RIN_CHECK(pollUntil([&] {
            return engine->tryLoadNodeOutput(3, seen3, out3) && out3.valid() &&
                   out3.sourceSequence == 3;
        }));
        RIN_CHECK(out3.format == rin::PortType::Gray8);
        RIN_CHECK_EQ(out3.width, 64u);
        RIN_CHECK_EQ(out3.height, 64u);
    }

    // 参数热更新"下一帧生效"（经 UI 侧 requestParamUpdate 通道）：crop.width
    // 64→32 后输出尺寸随帧边界变化。
    {
        std::string error;
        RIN_CHECK_MSG(engine->requestParamUpdate(2, "width",
                                                 rin::ParamValue{static_cast<std::int64_t>(32)},
                                                 &error),
                      "e2e: param hot update accepted while running");
        RIN_CHECK(error.empty());

        // 继续供帧（帧泵只在有新帧时提交；发布由测试线程同步驱动）。
        std::uint64_t publishSeq = 3;
        std::uint64_t seen2 = 0;
        rin::NodeOutputSnapshot out2;
        RIN_CHECK_MSG(pollUntil([&] {
                          service->publish(makeFrame(++publishSeq, 128, 96));
                          const bool got = engine->tryLoadNodeOutput(2, seen2, out2);
                          return got && out2.valid() && out2.width == 32;
                      }),
                      "e2e: param applies at the next frame boundary (64->32)");
        RIN_CHECK_EQ(out2.height, 64u);
        RIN_CHECK_MSG(out2.sourceSequence > 3,
                      "e2e: resized output comes from a post-update frame");

        // ParamUpdated 事件经事件通道可读（app.cpp pump 事件行语义）。
        rin::WorkflowEvent event;
        RIN_CHECK(pollUntil([&] {
            return engine->tryLoadEvent(event) &&
                   event.kind == rin::WorkflowEventKind::ParamUpdated;
        }));
    }

    // stop 排空（§4）：stop 返回后状态 Idle、缓存清空语义、无新发布、stale
    // 保持旧值、幂等双 stop。
    viewer::WorkflowCanvasState canvas;
    viewer::WorkflowPanelState panel;
    // 预置面板缓存（缩略图缓存承载"非 Running 清空"的排空对象；GL 上传分支
    // headless 不可测，本处只驱动 pumpNodeOutput 的平台无关清空分支）。
    RIN_CHECK(panel.outputs.pull(*engine, 1));
    RIN_CHECK(panel.outputs.size() > 0);

    // stop 前排空统计水位（等在飞帧收敛），记录停止前末次序号。
    std::uint64_t statsSeen = 0;
    rin::WorkflowStats last;
    RIN_CHECK(pollUntil([&] {
        std::uint64_t probe = 0;
        rin::WorkflowStats settle;
        if (!engine->tryLoadStats(probe, settle)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{8});
        std::uint64_t probe2 = 0;
        rin::WorkflowStats settle2;
        return !engine->tryLoadStats(probe2, settle2) ||
               settle2.sequence == settle.sequence;
    }));
    RIN_CHECK(engine->tryLoadStats(statsSeen, last));
    const std::uint64_t sequenceAtStop = last.sequence;

    // 节点产物水位同法收敛记录（供 stop 后"无新发布"静默窗使用）。
    std::uint64_t outSeenAtStop = 0;
    RIN_CHECK(pollUntil([&] {
        std::uint64_t probe = 0;
        rin::NodeOutputSnapshot settle;
        if (!engine->tryLoadNodeOutput(1, probe, settle)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{8});
        std::uint64_t probe2 = 0;
        rin::NodeOutputSnapshot settle2;
        return !engine->tryLoadNodeOutput(1, probe2, settle2) ||
               settle2.sourceSequence == settle.sourceSequence;
    }));
    {
        std::uint64_t seen = 0;
        rin::NodeOutputSnapshot finalOut;
        RIN_CHECK(engine->tryLoadNodeOutput(1, seen, finalOut));
        outSeenAtStop = seen;
    }

    engine->stop();
    RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);

    // pumpNodeOutput 非 Running 分支：缩略图缓存整体清空（返回 true = 有可见
    // 变更）；再次调用无变更（幂等）。
    RIN_CHECK_MSG(viewer::pumpNodeOutput(canvas, panel, *engine),
                  "e2e: pumpNodeOutput drains the thumbnail cache once stopped");
    RIN_CHECK_EQ(panel.outputs.size(), std::size_t{0});
    RIN_CHECK(!viewer::pumpNodeOutput(canvas, panel, *engine));

    // stop 返回后有界稳定窗：无新统计、无新节点产物（发布停止语义）。
    RIN_CHECK_MSG(quietFor(
                      [&] {
                          std::uint64_t probe = sequenceAtStop;
                          rin::WorkflowStats discarded;
                          return engine->tryLoadStats(probe, discarded);
                      },
                      kQuietWindow),
                  "e2e: no new stats after stop returns");
    RIN_CHECK_MSG(quietFor(
                      [&] {
                          std::uint64_t probe = outSeenAtStop;
                          rin::NodeOutputSnapshot discarded;
                          return engine->tryLoadNodeOutput(1, probe, discarded);
                      },
                      kQuietWindow),
                  "e2e: no new node outputs after stop returns");
    // stale 读取保持旧值（序号不回退；通道保留末次值）。
    {
        std::uint64_t fromZero = 0;
        rin::WorkflowStats stale;
        RIN_CHECK(engine->tryLoadStats(fromZero, stale));
        RIN_CHECK_EQ(stale.sequence, sequenceAtStop);
    }

    engine->stop();  // 幂等双 stop。
    RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);

    // ---- Running 中直接 stop 数轮：drain 竞态稳定收敛 ----
    for (int round = 0; round < 3; ++round) {
        const rin::AdmissionResult reAdmission = engine->start();
        RIN_CHECK(reAdmission.admitted);
        // 立即 stop（帧尚未流动的竞态窗）与前几帧流动后的 stop 交替。
        if (round % 2 == 1) {
            std::uint64_t publishSeq = 100 * static_cast<std::uint64_t>(round + 1);
            RIN_CHECK(pollUntil([&] {
                service->publish(makeFrame(++publishSeq, 128, 96));
                std::uint64_t probe = 0;
                rin::WorkflowStats stats;
                return engine->tryLoadStats(probe, stats) &&
                       stats.processedFrames >= 2;
            }));
        }
        engine->stop();
        RIN_CHECK_MSG(engine->state() == rin::WorkflowEngineState::Idle,
                      "e2e: repeated stop-while-running converges to Idle");
    }

    // ---- onShutdown 关闭顺序回归（M5-06 完成判据，app.cpp shutdown() 同构
    // 全序；位姿/GPU 通道不在本测试范围，见文件头取舍说明）----
    {
        // 新会话流动，保证关闭路径覆盖活动态（而非 Idle 快路径）。
        const rin::AdmissionResult beforeShutdown = engine->start();
        RIN_CHECK(beforeShutdown.admitted);
        std::uint64_t publishSeq = 1000;
        RIN_CHECK(pollUntil([&] {
            service->publish(makeFrame(++publishSeq, 128, 96));
            std::uint64_t probe = 0;
            rin::WorkflowStats stats;
            return engine->tryLoadStats(probe, stats) &&
                   stats.processedFrames >= 2;
        }));

        // 1) 停止任务生产者：服务 stop（之后经接缝取帧恒为"无新帧"）。
        service->stop();
        std::uint64_t seamProbe = 0;
        rin::WorkflowFrameInput seamOut;
        RIN_CHECK_MSG(!viewer::makeCameraFrameSource(service, router)(1, seamProbe, seamOut),
                      "e2e: stopped service yields no frames through the seam");
        service.reset();  // app.cpp：服务 reset（帧源闭包以 shared_ptr 持有，不悬垂）。
        // 2) 工作流引擎 stop + 释放。
        engine->stop();
        RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);
        engine.reset();
        canvas.graphPending = false;
        // 3) 工作流面板 UI 侧排空（app.cpp shutdown 同序）。
        panel.outputs.clear();
        panel.thumbnail.release();
        panel.thumbMeta.clear();
        panel.resetBindings();
        canvas.failures.clear();
        canvas.perf.clear();
        RIN_CHECK_EQ(panel.outputs.size(), std::size_t{0});
        RIN_CHECK(!canvas.perf.live() && canvas.perf.stats() == nullptr);
        RIN_CHECK_EQ(canvas.failures.size(), std::size_t{0});
        // 4) executor.shutdown(true)（非 worker 线程 = 测试主线程）。
        RIN_CHECK_MSG(executor.shutdown(true) == executor::ShutdownResult::Completed,
                      "e2e: app.cpp shutdown order converges cleanly");
        // reset 后无崩溃 + 引用释放后 shutdown 收敛（到达此处即回归通过）。
        return;  // 本节持有 executor 生命周期，收尾即返回。
    }
}

// --- 4. 关闭竞态防御（Running 中 executor.shutdown(false) 后 stop 有界收敛） ---

void testShutdownRaceDefense() {
    executor::Executor executor;
    executor::ExecutorConfig executorConfig;
    const bool initialized = executor.initialize(executorConfig);
    RIN_CHECK(initialized);
    if (!initialized) {
        return;
    }
    auto service = std::make_shared<FakeCameraService>();
    viewer::WorkflowSourceRouter router;
    fillRouter(router, {{1, viewer::WorkflowSourceRendition::RgbColor}});
    rin::WorkflowEngineConfig config;
    config.pumpInterval = std::chrono::milliseconds{2};
    config.frameSource = viewer::makeCameraFrameSource(service, router);
    std::shared_ptr<rin::IWorkflowEngine> engine =
        rin::createWorkflowEngine(executor, std::move(config));
    RIN_CHECK(static_cast<bool>(engine));
    if (!engine) {
        return;
    }
    RIN_CHECK(engine->applyGraph(makeTripleChainGraph()).ok);
    RIN_CHECK(engine->start().admitted);

    // 帧流动确认接缝处于活动路径。
    std::uint64_t publishSeq = 0;
    RIN_CHECK(pollUntil([&] {
        service->publish(makeFrame(++publishSeq, 128, 96));
        std::uint64_t probe = 0;
        rin::WorkflowStats stats;
        return engine->tryLoadStats(probe, stats) && stats.processedFrames >= 2;
    }));

    executor.shutdown(false);  // 对抗路径：引擎仍 Running（帧源接缝存在）。
    engine->stop();            // 必须有界返回，不悬挂。
    RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);
    engine.reset();  // 析构幂等 stop（executor 已 shutdown，不再调 shutdown）。
}

// --- 5. 相机源 rendition 路由（M6-05，DEC-017） ---

void testCameraSourceRenditionRouting() {
    using viewer::WorkflowSourceRendition;

    // renditionForSourceType：四个 source 型映射 + 未知 typeId 回退 RGB。
    RIN_CHECK(viewer::renditionForSourceType("source") ==
              WorkflowSourceRendition::RgbColor);
    RIN_CHECK(viewer::renditionForSourceType("source_depth_jet") ==
              WorkflowSourceRendition::DepthJet);
    RIN_CHECK(viewer::renditionForSourceType("source_depth_gray") ==
              WorkflowSourceRendition::DepthGray);
    RIN_CHECK(viewer::renditionForSourceType("source_depth_adaptive") ==
              WorkflowSourceRendition::DepthAdaptiveGray);
    RIN_CHECK_MSG(viewer::renditionForSourceType("") ==
                      WorkflowSourceRendition::RgbColor,
                  "routing: empty typeId falls back to RGB");
    RIN_CHECK_MSG(viewer::renditionForSourceType("source_depth") ==
                      WorkflowSourceRendition::RgbColor,
                  "routing: unknown depth typeId falls back to RGB");
    RIN_CHECK_MSG(viewer::renditionForSourceType("crop") ==
                      WorkflowSourceRendition::RgbColor,
                  "routing: non-source typeId falls back to RGB");

    // WorkflowSourceRouter：初态回退、路由命中、未知节点回退、replace 整体换新。
    {
        viewer::WorkflowSourceRouter router;  // 未 replace（空快照）：回退 RGB。
        RIN_CHECK(router.renditionFor(1) == WorkflowSourceRendition::RgbColor);
        auto routes = std::make_shared<viewer::WorkflowSourceRouter::RouteMap>();
        (*routes)[7] = WorkflowSourceRendition::DepthGray;
        router.replace(std::move(routes));
        RIN_CHECK(router.renditionFor(7) == WorkflowSourceRendition::DepthGray);
        RIN_CHECK_MSG(router.renditionFor(8) == WorkflowSourceRendition::RgbColor,
                      "router: unknown node falls back to RGB");
        auto replaced = std::make_shared<viewer::WorkflowSourceRouter::RouteMap>();
        (*replaced)[7] = WorkflowSourceRendition::DepthJet;
        router.replace(std::move(replaced));
        RIN_CHECK_MSG(router.renditionFor(7) == WorkflowSourceRendition::DepthJet,
                      "router: replace swaps the whole snapshot for readers");
    }

    // makeCameraFrameSource 按路由分发四 rendition：各自取对应通道、格式/尺寸/
    // 序号正确、Rgba8/Gray8 均零拷贝、水位按节点独立。
    {
        auto service = std::make_shared<FakeCameraService>();
        viewer::WorkflowSourceRouter router;
        fillRouter(router,
                   {{1, WorkflowSourceRendition::RgbColor},
                    {2, WorkflowSourceRendition::DepthJet},
                    {3, WorkflowSourceRendition::DepthGray},
                    {4, WorkflowSourceRendition::DepthAdaptiveGray}});
        const rin::WorkflowFrameSource source =
            viewer::makeCameraFrameSource(service, router);

        rin::Frame rgb = makeFrame(5, 32, 24);
        rin::Frame jet = makeFrame(6, 32, 24);
        jet.kind = rin::FrameKind::DepthJet;
        rin::GrayFrame gray = makeGrayFrame(7, 32, 24);
        rin::GrayFrame adaptive = makeGrayFrame(8, 32, 24);
        service->publish(rgb);
        service->publish(jet);
        service->publishGray(rin::GrayFrameKind::Depth, gray);
        service->publishGray(rin::GrayFrameKind::DepthAdaptive, adaptive);

        std::uint64_t seen1 = 0;
        rin::WorkflowFrameInput out1;
        RIN_CHECK_MSG(source(1, seen1, out1), "routing: RGB node gets the RGB rendition");
        RIN_CHECK_EQ(out1.sourceSequence, std::uint64_t{5});
        RIN_CHECK_EQ(seen1, std::uint64_t{5});
        RIN_CHECK(out1.image.format() == rin::PortType::Rgba8);
        RIN_CHECK(out1.image.pixels().get() == rgb.pixels.get());
        RIN_CHECK_EQ(out1.image.width(), 32u);
        RIN_CHECK_EQ(out1.image.stride(), 128u);

        std::uint64_t seen2 = 0;
        rin::WorkflowFrameInput out2;
        RIN_CHECK_MSG(source(2, seen2, out2),
                      "routing: DepthJet node gets the fixed jet rendition");
        RIN_CHECK_EQ(out2.sourceSequence, std::uint64_t{6});
        RIN_CHECK(out2.image.format() == rin::PortType::Rgba8);
        RIN_CHECK(out2.image.pixels().get() == jet.pixels.get());

        std::uint64_t seen3 = 0;
        rin::WorkflowFrameInput out3;
        RIN_CHECK_MSG(source(3, seen3, out3),
                      "routing: DepthGray node gets the gray rendition");
        RIN_CHECK_EQ(out3.sourceSequence, std::uint64_t{7});
        RIN_CHECK(out3.image.format() == rin::PortType::Gray8);
        RIN_CHECK_EQ(out3.image.stride(), 32u);
        RIN_CHECK(out3.image.pixels().get() == gray.pixels.get());

        std::uint64_t seen4 = 0;
        rin::WorkflowFrameInput out4;
        RIN_CHECK_MSG(source(4, seen4, out4),
                      "routing: DepthAdaptive node gets the adaptive rendition");
        RIN_CHECK_EQ(out4.sourceSequence, std::uint64_t{8});
        RIN_CHECK(out4.image.format() == rin::PortType::Gray8);
        RIN_CHECK(out4.image.pixels().get() == adaptive.pixels.get());

        // 探测即消费：各节点再探测无新帧（水位按 rendition 通道独立）。
        rin::WorkflowFrameInput discarded;
        RIN_CHECK(!source(1, seen1, discarded));
        RIN_CHECK(!source(2, seen2, discarded));
        RIN_CHECK(!source(3, seen3, discarded));
        RIN_CHECK(!source(4, seen4, discarded));

        // 未知节点回退 RGB：自带水位独立取到当前 RGB 最新帧。
        std::uint64_t seen9 = 0;
        rin::WorkflowFrameInput out9;
        RIN_CHECK_MSG(source(9, seen9, out9),
                      "routing: unlisted node falls back to the RGB channel");
        RIN_CHECK_EQ(out9.sourceSequence, std::uint64_t{5});
        RIN_CHECK(out9.image.pixels().get() == rgb.pixels.get());

        // 无效帧防御（RGBA8 与 Gray8 两路）：false 且水位不动、不吞后续有效帧。
        rin::Frame badJet = makeFrame(9, 32, 24);
        badJet.kind = rin::FrameKind::DepthJet;
        badJet.width = 0;
        service->publish(badJet);
        std::uint64_t probe2 = seen2;
        RIN_CHECK(!source(2, probe2, discarded));
        RIN_CHECK_EQ(probe2, std::uint64_t{6});

        rin::GrayFrame badGray = makeGrayFrame(9, 32, 24);
        badGray.stride = 16;  // < width → GrayFrame::valid() == false。
        service->publishGray(rin::GrayFrameKind::Depth, badGray);
        std::uint64_t probe3 = seen3;
        RIN_CHECK(!source(3, probe3, discarded));
        RIN_CHECK_EQ(probe3, std::uint64_t{7});

        service->publishGray(rin::GrayFrameKind::Depth, makeGrayFrame(10, 32, 24));
        std::uint64_t seen3b = seen3;
        rin::WorkflowFrameInput out3b;
        RIN_CHECK(source(3, seen3b, out3b));
        RIN_CHECK_EQ(out3b.sourceSequence, std::uint64_t{10});
    }

    // 通道隔离：灰度节点在仅有 RGB 发布时取不到帧（rendition 固定语义）。
    {
        auto service = std::make_shared<FakeCameraService>();
        viewer::WorkflowSourceRouter router;
        fillRouter(router, {{3, WorkflowSourceRendition::DepthGray}});
        const rin::WorkflowFrameSource source =
            viewer::makeCameraFrameSource(service, router);
        service->publish(makeFrame(1, 16, 16));  // 仅 RGB 通道有帧。
        std::uint64_t seen = 0;
        rin::WorkflowFrameInput out;
        RIN_CHECK_MSG(!source(3, seen, out),
                      "routing: gray rendition is bound to its gray channel");
        RIN_CHECK_EQ(seen, std::uint64_t{0});
    }

    // 空指针服务 + 路由 → false（关闭窗口内安全）。
    {
        viewer::WorkflowSourceRouter router;
        fillRouter(router, {{1, WorkflowSourceRendition::DepthJet}});
        const rin::WorkflowFrameSource nullSource =
            viewer::makeCameraFrameSource(nullptr, router);
        std::uint64_t seen = 0;
        rin::WorkflowFrameInput out;
        RIN_CHECK(!nullSource(1, seen, out));
    }
}

// --- 6. 灰度链真引擎端到端（M6-03，DEC-017） ---

void testGrayChainEndToEnd() {
    executor::Executor executor;
    executor::ExecutorConfig executorConfig;
    const bool initialized = executor.initialize(executorConfig);
    RIN_CHECK(initialized);
    if (!initialized) {
        return;
    }
    auto service = std::make_shared<FakeCameraService>();
    viewer::WorkflowSourceRouter router;
    fillRouter(router, {{1, viewer::WorkflowSourceRendition::DepthGray}});
    rin::WorkflowEngineConfig config;
    config.pumpInterval = std::chrono::milliseconds{2};
    config.frameSource = viewer::makeCameraFrameSource(service, router);
    std::shared_ptr<rin::IWorkflowEngine> engine =
        rin::createWorkflowEngine(executor, std::move(config));
    RIN_CHECK(static_cast<bool>(engine));
    if (!engine) {
        return;
    }

    // source_depth_gray -> crop_gray(16x12 @ (0,0)) -> gaussian_blur（默认
    // radius=3/sigma=1.5；Gray8 保持尺寸）。
    rin::WorkflowGraph graph;
    {
        rin::NodeInstance source;
        source.id = 1;
        source.typeId = "source_depth_gray";
        rin::NodeInstance crop;
        crop.id = 2;
        crop.typeId = "crop_gray";
        crop.params = {
            {"x", rin::ParamValue{static_cast<std::int64_t>(0)}},
            {"y", rin::ParamValue{static_cast<std::int64_t>(0)}},
            {"width", rin::ParamValue{static_cast<std::int64_t>(16)}},
            {"height", rin::ParamValue{static_cast<std::int64_t>(12)}},
        };
        rin::NodeInstance blur;
        blur.id = 3;
        blur.typeId = "gaussian_blur";
        graph.nodes = {source, crop, blur};
        graph.connections = {
            rin::Connection{{1, rin::PortDirection::Output, 0},
                            {2, rin::PortDirection::Input, 0}},
            rin::Connection{{2, rin::PortDirection::Output, 0},
                            {3, rin::PortDirection::Input, 0}},
        };
    }
    RIN_CHECK_MSG(engine->applyGraph(graph).ok, "gray chain: graph is admitted");
    const rin::AdmissionResult admission = engine->start();
    RIN_CHECK_MSG(admission.admitted, "gray chain: start admitted");
    if (!admission.admitted) {
        engine->stop();
        engine.reset();
        (void)executor.shutdown(true);
        return;
    }

    // 合成 Gray8 帧源（32x24 递增图案，发布 3 帧后停止 → 最新帧语义收敛 seq3）。
    service->publishGray(rin::GrayFrameKind::Depth, makeGrayFrame(1, 32, 24));
    service->publishGray(rin::GrayFrameKind::Depth, makeGrayFrame(2, 32, 24));
    service->publishGray(rin::GrayFrameKind::Depth, makeGrayFrame(3, 32, 24));

    // 各节点产物：valid、格式 Gray8、尺寸符合冻结公式、sourceSequence 透传。
    {
        std::uint64_t seen1 = 0;
        rin::NodeOutputSnapshot out1;
        RIN_CHECK_MSG(pollUntil([&] {
                          return engine->tryLoadNodeOutput(1, seen1, out1) &&
                                 out1.valid() && out1.sourceSequence == 3;
                      }),
                      "gray chain: source_depth_gray carries the camera frame sequence");
        RIN_CHECK(out1.format == rin::PortType::Gray8);
        RIN_CHECK_EQ(out1.width, 32u);
        RIN_CHECK_EQ(out1.height, 24u);

        std::uint64_t seen2 = 0;
        rin::NodeOutputSnapshot out2;
        RIN_CHECK_MSG(pollUntil([&] {
                          return engine->tryLoadNodeOutput(2, seen2, out2) &&
                                 out2.valid() && out2.sourceSequence == 3;
                      }),
                      "gray chain: crop_gray executes on the gray rendition");
        RIN_CHECK(out2.format == rin::PortType::Gray8);
        RIN_CHECK_EQ(out2.width, 16u);  // 冻结 ROI 公式：(x+w, y+h) = (16, 12)。
        RIN_CHECK_EQ(out2.height, 12u);

        std::uint64_t seen3 = 0;
        rin::NodeOutputSnapshot out3;
        RIN_CHECK_MSG(pollUntil([&] {
                          return engine->tryLoadNodeOutput(3, seen3, out3) &&
                                 out3.valid() && out3.sourceSequence == 3;
                      }),
                      "gray chain: gaussian_blur consumes crop_gray output");
        RIN_CHECK(out3.format == rin::PortType::Gray8);
        RIN_CHECK_EQ(out3.width, 16u);  // 高斯模糊保持尺寸。
        RIN_CHECK_EQ(out3.height, 12u);
    }

    // 灰度链合法执行：引擎保持 Running、无 Failed。
    RIN_CHECK_MSG(engine->state() == rin::WorkflowEngineState::Running,
                  "gray chain: engine stays Running after executing the gray chain");

    engine->stop();
    RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);
    engine.reset();
    RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
}

// --- 7. pumpNodeOutput 输入驱动节点拉取（M6-06 ROI 联动约束的尺寸源） ---

void testPumpNodeOutputDriverPull() {
    executor::Executor executor;
    executor::ExecutorConfig executorConfig;
    const bool initialized = executor.initialize(executorConfig);
    RIN_CHECK(initialized);
    if (!initialized) {
        return;
    }
    auto service = std::make_shared<FakeCameraService>();
    viewer::WorkflowSourceRouter router;
    fillRouter(router, {{1, viewer::WorkflowSourceRendition::RgbColor}});
    rin::WorkflowEngineConfig config;
    config.pumpInterval = std::chrono::milliseconds{2};
    config.frameSource = viewer::makeCameraFrameSource(service, router);
    std::shared_ptr<rin::IWorkflowEngine> engine =
        rin::createWorkflowEngine(executor, std::move(config));
    RIN_CHECK(static_cast<bool>(engine));
    if (!engine) {
        return;
    }

    // 画布：source(1) -> crop(2)，选中 crop（入边来自 source）。
    WorkflowCanvasState canvas;
    canvas.model.catalog = &engine->catalog();
    RIN_CHECK(canvas.model.createNode("source", {0.0f, 0.0f}).ok);
    RIN_CHECK(canvas.model.createNode("crop", {300.0f, 0.0f}).ok);
    // 32x24 帧下默认 64x64 ROI 会被真实 crop 算子运行期拒绝：显式给合法 ROI。
    RIN_CHECK(canvas.model.setParam(2, {"x", rin::ParamValue{static_cast<std::int64_t>(0)}}).ok);
    RIN_CHECK(canvas.model.setParam(2, {"y", rin::ParamValue{static_cast<std::int64_t>(0)}}).ok);
    RIN_CHECK(
        canvas.model.setParam(2, {"width", rin::ParamValue{static_cast<std::int64_t>(16)}})
            .ok);
    RIN_CHECK(
        canvas.model.setParam(2, {"height", rin::ParamValue{static_cast<std::int64_t>(12)}})
            .ok);
    RIN_CHECK(canvas.model
                  .connect({1, rin::PortDirection::Output, 0},
                           {2, rin::PortDirection::Input, 0})
                  .ok);
    canvas.afterGraphChange(engine.get());
    RIN_CHECK(canvas.model.validation.ok);
    canvas.model.selection.assign(1, 2);

    const rin::AdmissionResult admission = engine->start();
    RIN_CHECK(admission.admitted);
    if (!admission.admitted) {
        engine->stop();
        engine.reset();
        (void)executor.shutdown(true);
        return;
    }

    // Running 后 pump：panel.outputs 同时含选中节点（crop 16x12）与输入驱动
    // 节点（source 32x24）快照——驱动拉取即 ROI 联动约束的尺寸源。
    WorkflowPanelState panel;
    std::uint64_t publishSeq = 0;
    RIN_CHECK_MSG(pollUntil([&] {
                      service->publish(makeFrame(++publishSeq, 32, 24));
                      (void)viewer::pumpNodeOutput(canvas, panel, *engine);
                      const rin::NodeOutputSnapshot* driver = panel.outputs.find(1);
                      return driver != nullptr && driver->valid() &&
                             driver->width == 32 && driver->height == 24;
                  }),
                  "pump: driver node output (input size source) is cached");
    const rin::NodeOutputSnapshot* selected = panel.outputs.find(2);
    RIN_CHECK_MSG(selected != nullptr && selected->valid(),
                  "pump: selected node output cached alongside the driver");
    if (selected != nullptr) {
        RIN_CHECK_EQ(selected->width, 16u);
        RIN_CHECK_EQ(selected->height, 12u);
    }
    RIN_CHECK(panel.outputs.size() >= 1);

    // 引擎回 Idle：pumpNodeOutput 整体排空（一次 true、幂等二次 false）。
    engine->stop();
    RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);
    RIN_CHECK_MSG(viewer::pumpNodeOutput(canvas, panel, *engine),
                  "pump: leaving Running drains the panel cache once");
    RIN_CHECK_EQ(panel.outputs.size(), std::size_t{0});
    RIN_CHECK(!viewer::pumpNodeOutput(canvas, panel, *engine));

    engine.reset();
    RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
}

// --- 8. 工作台布局状态（M7-03/04）：WorkflowCanvasState docking 字段默认值
//        见证 + compose 期夹取边界常量语义 ---
//
// 被测契约（apps/viewer/node_canvas.hpp WorkflowCanvasState + param_panel.hpp
// composeWorkflowPage/composeWorkflowContext 的 std::clamp 夹取）：
//   - 五区 docking 布局字段默认值 = M5-02 冻结骨架几何（调色板 200 / 上下文
//     264 / 底部 120 / Context 输出块 148），无活动拖拽（dockDrag == -1）、
//     拖拽起始状态归零；M7-03 调色板滚动偏移信号默认 0（scrollView bind 初值）；
//   - 夹取边界常量语义：分隔条 onDrag 把未夹取值写入状态字段，compose 期以
//     [150,400]/[220,460]/[72,300]/[96,340] 夹取。headless 以与 compose 同式的
//     std::clamp 见证三点不变量：默认值在范围内（夹取恒等——出厂几何即骨架）、
//     越界拖拽写入收敛到边界、范围内任意拖拽值不被修改。
// 分隔条/输出块分隔条的 mouseArea 回调时序（onDragStart/onDrag/onDragEnd）与
// 视觉呈现需活动 EUI 运行时，headless 不可测——归真机验收（M7-05 冒烟）。

void testWorkflowCanvasLayoutState() {
    viewer::WorkflowCanvasState canvas;

    // 默认值见证（DEC-014"画布布局为 UI 私有状态"：值语义构造，无隐式全局）。
    RIN_CHECK_MSG(canvas.paletteWidth == 200.0f && canvas.contextWidth == 264.0f &&
                      canvas.bottomHeight == 120.0f &&
                      canvas.outputBlockHeight == 148.0f,
                  "layout: defaults match the frozen M5-02 five-zone skeleton");
    RIN_CHECK_MSG(canvas.dockDrag == -1 && canvas.dockDragStartPointer == 0.0f &&
                      canvas.dockDragStartValue == 0.0f,
                  "layout: no active dock drag after construction");
    RIN_CHECK_MSG(canvas.paletteScroll.get() == 0.0f,
                  "layout: palette scroll offset starts at 0 (scrollView bind)");

    // compose 期夹取（param_panel.hpp composeWorkflowPage:1092-1094 与
    // composeWorkflowContext:447 的 std::clamp 同式）：拖拽写入 → 夹取读取。
    struct DockField {
        float value;
        float lo;
        float hi;
        const char* name;
    };
    const DockField fields[] = {
        {canvas.paletteWidth, 150.0f, 400.0f, "paletteWidth"},
        {canvas.contextWidth, 220.0f, 460.0f, "contextWidth"},
        {canvas.bottomHeight, 72.0f, 300.0f, "bottomHeight"},
        {canvas.outputBlockHeight, 96.0f, 340.0f, "outputBlockHeight"},
    };
    for (const DockField& field : fields) {
        RIN_CHECK_MSG(field.lo < field.value && field.value < field.hi,
                      (std::string("layout: default ") + field.name +
                       " sits inside its clamp range")
                          .c_str());
        // 范围内合成拖拽值不被夹取修改（合法性边界内的自由调节）。
        const float mid = field.lo + (field.hi - field.lo) * 0.5f;
        RIN_CHECK_MSG(std::clamp(field.value, field.lo, field.hi) == field.value &&
                          std::clamp(mid, field.lo, field.hi) == mid,
                      (std::string("layout: in-range values pass the ") + field.name +
                       " clamp unchanged")
                          .c_str());
        // 越界拖拽写入收敛到闭区间边界（两端点可达）。
        RIN_CHECK_MSG(std::clamp(field.lo - 50.0f, field.lo, field.hi) == field.lo &&
                          std::clamp(field.hi + 50.0f, field.lo, field.hi) == field.hi,
                      (std::string("layout: out-of-range drags clamp to the ") +
                       field.name + " bounds")
                          .c_str());
    }
}

}  // namespace

int main() {
    runSection("camera_frame_source_seam", testCameraFrameSourceSeam);
    runSection("run_control_state_machine", testRunControlStateMachine);
    runSection("engine_ui_end_to_end", testEngineUiEndToEnd);
    runSection("shutdown_race_defense", testShutdownRaceDefense);
    runSection("camera_source_rendition_routing", testCameraSourceRenditionRouting);
    runSection("gray_chain_end_to_end", testGrayChainEndToEnd);
    runSection("pump_node_output_driver_pull", testPumpNodeOutputDriverPull);
    runSection("workflow_canvas_layout_state", testWorkflowCanvasLayoutState);
    return rin_test::exitStatus();
}
