// M13-01/M13-02 分辨率档位枚举纯逻辑契约测试（独立验证；
// apps/viewer/resolution_model.hpp buildResolutionUiOptions，契约出处
// resolution_model.hpp 头注 + docs/plans/m13-fps-selection.md M13-01 判据）。
//
// 覆盖（逐条对应 M13-01 验收判据）：
// - a. 彩色∩深度交集配对：仅当 depthOptions 存在同 (宽, 高, fps) 深度档时
//   对应彩色档才成为选项；仅彩色（D435IF 真机 960x540 无深度档）与仅深度的
//   档位（848x480@90 彩色不存在）均不得出现；不同 (宽,高) 同帧率不算配对。
// - b. 每个输出的 request 六个视频字段按三元组成对相等（colorWidth/Height/
//   Fps == depthWidth/Height/Fps == 该 (宽, 高, fps)；M1"双流成对"契约），
//   enableMotion 等于注入值（会话粘性透传，true/false 两态）；全量 StreamRequest
//   相等经 rin::operator==（链接 rin::core 的实现）交叉核对（restream 去重
//   依赖该相等语义）。
// - c. 去重：输入重复出现的同一 (宽, 高, fps) 只产出一个选项。
// - d. 排序确定性：高度降序 → 宽度降序 → 帧率升序（同 (宽,高) 内 fps 自低
//   到高；分辨率大在上与既有菜单观感一致）。
// - e. 标签格式 "<宽>x<高> · <fps>fps"（· 为 U+00B7 = UTF-8 C2 B7，两侧各一个
//   空格）逐字节断言（含 "848x480 · 60fps"）。
// - f. 空目录 / 仅彩色 / 仅深度 / 默认 DeviceInfo 返回空列表；未配对档位
//   不崩溃。
// - 输入顺序无关性：同一目录内容的多种确定排列产出完全一致的选项序列
//   （排序唯一判据的对抗面）。
//
// 范围与限制（如实说明）：app.cpp 的 ViewerContext::rebuildResolutionOptions
// 接线（motionEnabled = kDefaultRequest.enableMotion && !motionDisabledByEnv）
// 与设备变化时默认回选判据（colorWidth==848 && colorHeight==480 &&
// colorFps==30 && depthFps==30 四字段整体匹配，同宽高多帧率下必须选中 30fps
// 档）绑定 EUI 运行时且位于匿名命名空间，headless 不可直接单测——由真机验收
// 覆盖（M13-03：D435IF 选 848x480@60 建流实测 ≈60fps、设备就绪默认回选
// 848x480@30），此处不为它写不可达的测试。node_canvas.hpp 源节点胶囊布局
// 常量（标签 0.40 / 胶囊 0.60、"Resolution"）属 compose 绘制路径，同归真机
// 视觉验收。设备能力事实：真机 D435IF（serial 261922074392，firmware
// 5.15.1.55）rs-enumerate-devices 2026-10-08（见 m13-fps-selection.md）。
//
// 单线程纯函数值语义：无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用）。
#include "test_util.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "resolution_model.hpp"

namespace {

using viewer::ResolutionUiOption;
using viewer::buildResolutionUiOptions;
using rin::DeviceInfo;
using rin::ResolutionOption;
using rin::StreamRequest;

/// 三元组测试夹具（手推期望表用，避免逐处写三行字段赋值）。
struct Triple {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fps;
};

ResolutionOption opt(Triple t) { return ResolutionOption{t.width, t.height, t.fps}; }

std::vector<ResolutionOption> opts(const std::vector<Triple>& triples) {
    std::vector<ResolutionOption> out;
    out.reserve(triples.size());
    for (const Triple t : triples) {
        out.push_back(opt(t));
    }
    return out;
}

DeviceInfo makeDevice(std::vector<ResolutionOption> colorOptions,
                      std::vector<ResolutionOption> depthOptions) {
    DeviceInfo device;
    device.name = "Intel RealSense D435IF";
    device.serial = "261922074392";
    device.colorOptions = std::move(colorOptions);
    device.depthOptions = std::move(depthOptions);
    return device;
}

/// 与实现无关的期望标签推导（契约 e；std::to_string 独立拼接）。
std::string expectedLabel(std::uint32_t width, std::uint32_t height, std::uint32_t fps) {
    return std::to_string(width) + "x" + std::to_string(height) + " \xC2\xB7 " +
           std::to_string(fps) + "fps";
}

/// 单选项逐字段断言（契约 b + e）：六个视频字段成对相等 + 标签精确一致。
void expectOption(const ResolutionUiOption& option, Triple t, const char* context) {
    RIN_CHECK_MSG(option.request.colorWidth == t.width, context);
    RIN_CHECK_MSG(option.request.colorHeight == t.height, context);
    RIN_CHECK_MSG(option.request.colorFps == t.fps, context);
    RIN_CHECK_MSG(option.request.depthWidth == t.width, context);
    RIN_CHECK_MSG(option.request.depthHeight == t.height, context);
    RIN_CHECK_MSG(option.request.depthFps == t.fps, context);
    RIN_CHECK_MSG(option.label == expectedLabel(t.width, t.height, t.fps), context);
}

/// 选项序列与手推期望表整体比对（顺序即契约 d 的断言面）。
void expectSequence(const std::vector<ResolutionUiOption>& options,
                    const std::vector<Triple>& expected, const char* context) {
    RIN_CHECK_MSG(options.size() == expected.size(), context);
    for (std::size_t i = 0; i < expected.size() && i < options.size(); ++i) {
        expectOption(options[i], expected[i], context);
    }
}

// --- 1) D435IF 真机形态档位表（设备能力事实见 m13-fps-selection.md）---

DeviceInfo makeD435IfDevice() {
    // 彩色：1280x720/{30,15,6}、960x540/{30,15,6}（无深度档）、
    // 848x480/{60,30,15,6}、640x480/{60,30,15,6}、640x360/{60,30,15,6}、
    // 424x240/{60,30,15,6}。
    std::vector<Triple> color = {
        {1280, 720, 30}, {1280, 720, 15}, {1280, 720, 6}, {960, 540, 30}, {960, 540, 15},
        {960, 540, 6},   {848, 480, 60},  {848, 480, 30}, {848, 480, 15}, {848, 480, 6},
        {640, 480, 60},  {640, 480, 30},  {640, 480, 15}, {640, 480, 6},  {640, 360, 60},
        {640, 360, 30},  {640, 360, 15},  {640, 360, 6},  {424, 240, 60}, {424, 240, 30},
        {424, 240, 15},  {424, 240, 6},
    };
    // 深度：1280x720/{30,15,6}、848x480/{90,60,30,15,6}（90 彩色不存在）、
    // 640x480/{60,30,15,6}、640x360/{60,30,15,6}、424x240/{60,30,15,6}。
    std::vector<Triple> depth = {
        {1280, 720, 30},  {1280, 720, 15}, {1280, 720, 6},  {848, 480, 90},  {848, 480, 60},
        {848, 480, 30},   {848, 480, 15},  {848, 480, 6},   {640, 480, 60},  {640, 480, 30},
        {640, 480, 15},   {640, 480, 6},   {640, 360, 60},  {640, 360, 30},  {640, 360, 15},
        {640, 360, 6},    {424, 240, 60},  {424, 240, 30},  {424, 240, 15},  {424, 240, 6},
    };
    return makeDevice(opts(color), opts(depth));
}

void testD435IfCapabilityTable() {
    const DeviceInfo device = makeD435IfDevice();
    const std::vector<ResolutionUiOption> options = buildResolutionUiOptions(device, true);

    // 期望全集（手推）：交集 = {1280x720}×{6,15,30} ∪ {848,640x480,640x360,
    // 424x240}×{6,15,30,60}；960x540 仅彩色不成对、848x480@90 仅深度不成对。
    // 排序：高度降序 → 宽度降序 → 帧率升序（契约 d）。
    const std::vector<Triple> expected = {
        {1280, 720, 6},  {1280, 720, 15}, {1280, 720, 30},                                  // 720p
        {848, 480, 6},   {848, 480, 15},  {848, 480, 30},  {848, 480, 60},                  // 848 宽
        {640, 480, 6},   {640, 480, 15},  {640, 480, 30},  {640, 480, 60},                  // 640 宽
        {640, 360, 6},   {640, 360, 15},  {640, 360, 30},  {640, 360, 60},
        {424, 240, 6},   {424, 240, 15},  {424, 240, 30},  {424, 240, 60},
    };
    expectSequence(options, expected, "d435if full table");

    // 不成对档位的排除面（契约 a）：逐标签扫描，不存在即通过。
    for (const ResolutionUiOption& option : options) {
        RIN_CHECK_MSG(option.label.find("960x540") == std::string::npos,
                      "color-only 960x540 must not appear");
        RIN_CHECK_MSG(option.label.find("90fps") == std::string::npos,
                      "depth-only 90fps must not appear");
    }

    // 标签字节级抽查（契约 e）：恰 19 项之外，逐项已按 expectedLabel 断言；
    // 此处再对 "848x480 · 60fps" 做 UTF-8 字节定位：' ' C2 B7 ' '。
    bool found848x480x60 = false;
    for (const ResolutionUiOption& option : options) {
        if (option.request.colorWidth == 848 && option.request.colorHeight == 480 &&
            option.request.colorFps == 60) {
            found848x480x60 = true;
            const std::string& label = option.label;
            RIN_CHECK_MSG(label == "848x480 \xC2\xB7 60fps", "byte-exact label");
            RIN_CHECK_MSG(label.size() == 16, "label byte length (7 + 1 + 2 + 1 + 5)");
            RIN_CHECK_MSG(label[7] == ' ', "space before middle dot");
            RIN_CHECK_MSG(static_cast<unsigned char>(label[8]) == 0xC2 &&
                              static_cast<unsigned char>(label[9]) == 0xB7,
                          "U+00B7 as UTF-8 C2 B7");
            RIN_CHECK_MSG(label[10] == ' ', "space after middle dot");
        }
    }
    RIN_CHECK_MSG(found848x480x60, "848x480@60 paired option present (M13 user scenario)");
}

// --- 2) 交集配对判据的最小形态（契约 a 的对抗面）---

void testPairingRequiresBothSides() {
    // 同 (宽,高) 不同帧率：彩色 {30,60}、深度 {30,90} → 仅 30 成对。
    {
        const DeviceInfo device = makeDevice(
            opts({{848, 480, 30}, {848, 480, 60}}), opts({{848, 480, 30}, {848, 480, 90}}));
        const std::vector<ResolutionUiOption> options = buildResolutionUiOptions(device, true);
        expectSequence(options, {{848, 480, 30}}, "fps mismatch both directions");
    }
    // 不同 (宽,高) 同帧率：不得配对（三元组整体匹配，非仅 fps）。
    {
        const DeviceInfo device =
            makeDevice(opts({{640, 360, 30}}), opts({{960, 540, 30}}));
        RIN_CHECK_MSG(buildResolutionUiOptions(device, true).empty(),
                      "same fps different (w,h) is not a pair");
    }
    // 仅彩色 / 仅深度的单侧目录：空列表、不崩溃（契约 f 的非空退化）。
    {
        const DeviceInfo colorOnly = makeDevice(opts({{848, 480, 30}}), {});
        RIN_CHECK_MSG(buildResolutionUiOptions(colorOnly, true).empty(), "depth side empty");
        const DeviceInfo depthOnly = makeDevice({}, opts({{848, 480, 30}}));
        RIN_CHECK_MSG(buildResolutionUiOptions(depthOnly, true).empty(), "color side empty");
    }
}

// --- 3) 去重（契约 c）---

void testDedup() {
    // 彩色重复 + 深度重复：每三元组仍只产出一个选项。
    const DeviceInfo device = makeDevice(
        opts({{848, 480, 30}, {848, 480, 30}, {640, 480, 30}, {640, 480, 30}, {640, 480, 30}}),
        opts({{848, 480, 30}, {848, 480, 30}, {848, 480, 30}, {640, 480, 30}}));
    const std::vector<ResolutionUiOption> options = buildResolutionUiOptions(device, false);
    // 同高度 480 内宽度降序：848 在 640 前（契约 d）。
    expectSequence(options, {{848, 480, 30}, {640, 480, 30}}, "dedup keeps one per triple");
}

// --- 4) 排序规则最小对抗集（契约 d）---

void testOrderingRules() {
    // 输入乱序：同 (宽,高) 帧率乱序 + 高度乱序 + 同高不同宽乱序。
    const DeviceInfo device = makeDevice(
        opts({{640, 480, 30},
              {848, 480, 60},
              {1280, 720, 30},
              {848, 480, 6},
              {640, 360, 60},
              {848, 480, 15}}),
        opts({{640, 480, 30},
              {848, 480, 60},
              {1280, 720, 30},
              {848, 480, 6},
              {640, 360, 60},
              {848, 480, 15}}));
    const std::vector<ResolutionUiOption> options = buildResolutionUiOptions(device, false);
    // 手推期望：720 高度居首；480 高度内 848 宽在 640 宽前；848x480 内帧率
    // 6 < 15 < 60；640x360 高度最低殿后。
    const std::vector<Triple> expected = {
        {1280, 720, 30},
        {848, 480, 6},  {848, 480, 15}, {848, 480, 60},
        {640, 480, 30},
        {640, 360, 60},
    };
    expectSequence(options, expected, "height desc, width desc, fps asc");
}

// --- 5) 标签逐字节格式（契约 e 的补充定位面）---

void testLabelByteFormat() {
    const DeviceInfo device =
        makeDevice(opts({{1280, 720, 15}, {848, 480, 60}}), opts({{1280, 720, 15}, {848, 480, 60}}));
    const std::vector<ResolutionUiOption> options = buildResolutionUiOptions(device, true);
    RIN_CHECK_EQ(options.size(), std::size_t{2});
    if (options.size() != 2) {
        return;  // 防御：仅在上面的规模断言失败时避免越界（失败已记录）。
    }

    // "1280x720 · 15fps"：8 + 1 + 2 + 1 + 5 = 17 字节。"1280x720" 占 0..7，
    // 空格 8、U+00B7 的 UTF-8 C2 B7 占 9..10、空格 11、"15fps" 占 12..16。
    const std::string& hi = options[0].label;
    RIN_CHECK(hi == "1280x720 \xC2\xB7 15fps");
    RIN_CHECK(hi.size() == 17);
    RIN_CHECK(static_cast<unsigned char>(hi[9]) == 0xC2);
    RIN_CHECK(static_cast<unsigned char>(hi[10]) == 0xB7);
    RIN_CHECK(hi[8] == ' ' && hi[11] == ' ');
    // 无多余空格（分隔恰好一处：首尾无空白）。
    RIN_CHECK(!hi.empty() && hi.front() != ' ' && hi.back() == 's');

    const std::string& lo = options[1].label;
    RIN_CHECK(lo == "848x480 \xC2\xB7 60fps");
    RIN_CHECK(lo.size() == 16);
}

// --- 6) enableMotion 透传与全量 StreamRequest 相等（契约 b）---

void testMotionInjectionAndPairedRequest() {
    const DeviceInfo device =
        makeDevice(opts({{848, 480, 60}, {424, 240, 30}}), opts({{848, 480, 60}, {424, 240, 30}}));

    const std::vector<ResolutionUiOption> withMotion = buildResolutionUiOptions(device, true);
    RIN_CHECK_EQ(withMotion.size(), std::size_t{2});
    if (withMotion.size() != 2) {
        return;  // 防御：规模不符时避免后续越界（失败已记录）。
    }
    for (const ResolutionUiOption& option : withMotion) {
        RIN_CHECK_MSG(option.request.enableMotion, "motion=true injected");
    }
    // 全量相等（rin::operator==，链接 rin::core 的实现；restream 去重依赖）：
    // 六视频字段成对 + enableMotion 逐字段核对。
    StreamRequest expected60{};
    expected60.colorWidth = 848;
    expected60.colorHeight = 480;
    expected60.colorFps = 60;
    expected60.depthWidth = 848;
    expected60.depthHeight = 480;
    expected60.depthFps = 60;
    expected60.enableMotion = true;
    RIN_CHECK_MSG(withMotion[0].request == expected60, "operator== full request 848x480@60");

    const std::vector<ResolutionUiOption> withoutMotion = buildResolutionUiOptions(device, false);
    RIN_CHECK_EQ(withoutMotion.size(), std::size_t{2});
    for (const ResolutionUiOption& option : withoutMotion) {
        RIN_CHECK_MSG(!option.request.enableMotion, "motion=false injected");
    }
    // 注入只影响 enableMotion：两态调用的视频字段逐一相同（规模一致才逐项比）。
    if (withoutMotion.size() == withMotion.size()) {
        for (std::size_t i = 0; i < withMotion.size(); ++i) {
            RIN_CHECK(withMotion[i].request.colorWidth == withoutMotion[i].request.colorWidth);
            RIN_CHECK(withMotion[i].request.colorHeight == withoutMotion[i].request.colorHeight);
            RIN_CHECK(withMotion[i].request.colorFps == withoutMotion[i].request.colorFps);
            RIN_CHECK(withMotion[i].request.depthWidth == withoutMotion[i].request.depthWidth);
            RIN_CHECK(withMotion[i].request.depthHeight == withoutMotion[i].request.depthHeight);
            RIN_CHECK(withMotion[i].request.depthFps == withoutMotion[i].request.depthFps);
            RIN_CHECK(withMotion[i].label == withoutMotion[i].label);
        }
    } else {
        RIN_CHECK_MSG(false, "motion two-state output size mismatch");
    }
}

// --- 7) 空目录退化（契约 f）---

void testDegenerateCatalogs() {
    // 双侧为空。
    RIN_CHECK(buildResolutionUiOptions(DeviceInfo{}, true).empty());
    RIN_CHECK(buildResolutionUiOptions(DeviceInfo{}, false).empty());
    // 命名空设备（非默认构造，排除"空名字段特判"误判面）。
    const DeviceInfo emptyNamed = makeDevice({}, {});
    RIN_CHECK(buildResolutionUiOptions(emptyNamed, true).empty());
}

// --- 8) 输入顺序无关性（排序唯一判据的对抗面）---

void testInputOrderIndependence() {
    const std::vector<Triple> colorBase = {
        {848, 480, 60}, {1280, 720, 30}, {848, 480, 30}, {424, 240, 15}, {640, 480, 30},
    };
    const std::vector<Triple> depthBase = {
        {424, 240, 15}, {848, 480, 30}, {1280, 720, 30}, {848, 480, 60}, {640, 480, 30},
    };
    // 三种确定排列：原序 / 整体逆序 / 轮转 2。
    auto rotated = [](const std::vector<Triple>& in) {
        std::vector<Triple> out(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            out[(i + 2) % in.size()] = in[i];
        }
        return out;
    };
    auto reversed = [](const std::vector<Triple>& in) {
        return std::vector<Triple>(in.rbegin(), in.rend());
    };

    const std::vector<ResolutionUiOption> base =
        buildResolutionUiOptions(makeDevice(opts(colorBase), opts(depthBase)), true);
    const std::vector<ResolutionUiOption> flipped = buildResolutionUiOptions(
        makeDevice(opts(reversed(colorBase)), opts(reversed(depthBase))), true);
    const std::vector<ResolutionUiOption> shifted = buildResolutionUiOptions(
        makeDevice(opts(rotated(colorBase)), opts(rotated(depthBase))), true);

    RIN_CHECK_EQ(base.size(), std::size_t{5});  // 全部三元组双侧齐备 → 5 档。
    RIN_CHECK_EQ(flipped.size(), base.size());
    RIN_CHECK_EQ(shifted.size(), base.size());
    if (base.size() != 5 || flipped.size() != base.size() || shifted.size() != base.size()) {
        return;  // 防御：规模不符时避免后续越界（失败已记录）。
    }
    for (std::size_t i = 0; i < base.size(); ++i) {
        RIN_CHECK_MSG(flipped[i].label == base[i].label, "reversed input same output order");
        RIN_CHECK_MSG(shifted[i].label == base[i].label, "rotated input same output order");
        RIN_CHECK_MSG(flipped[i].request == base[i].request, "reversed input same request");
        RIN_CHECK_MSG(shifted[i].request == base[i].request, "rotated input same request");
    }
    // 手推该目录的期望序列（720 > 480 > 240 高度降序；480 高度内 848 > 640
    // 宽度降序；848x480 内帧率升序 30 < 60）。
    expectSequence(base,
                   {{1280, 720, 30},
                    {848, 480, 30},
                    {848, 480, 60},
                    {640, 480, 30},
                    {424, 240, 15}},
                   "mixed catalog expected order");
}

}  // namespace

int main() {
    rin_test::runSection("d435if_capability_table", testD435IfCapabilityTable);
    rin_test::runSection("pairing_requires_both_sides", testPairingRequiresBothSides);
    rin_test::runSection("dedup", testDedup);
    rin_test::runSection("ordering_rules", testOrderingRules);
    rin_test::runSection("label_byte_format", testLabelByteFormat);
    rin_test::runSection("motion_injection_and_paired_request", testMotionInjectionAndPairedRequest);
    rin_test::runSection("degenerate_catalogs", testDegenerateCatalogs);
    rin_test::runSection("input_order_independence", testInputOrderIndependence);
    return rin_test::exitStatus();
}
