// M5-04 参数面板与中间结果查看纯逻辑测试 —— 独立验证（Independent Verification
// Agent）。按被测契约（apps/viewer/param_model.hpp、canvas_model.hpp 参数面、
// ui_workspace_design.md §5.5/§5.6/§7）独立设计与执行，不依赖开发者自述。
//
// 被测面：
//   1. 严格文本解析：parseIntegerText / parseRealText / formatRealText（%.6g）；
//   2. 值对声明校验：paramValidationError 五 ParamKind 正反例（种类匹配、
//      hasRange 闭区间端点、枚举选项、RealArray 有限性）；
//   3. 赋值包装：makeParamAssignment / paramAssignmentFromText（含
//      Boolean/RealArray 的 not text-editable 拒绝）；
//   4. 滑条归一化映射：sliderToValue / valueToSlider（端点/夹取/取整/往返/
//      span<=0/非有限/无范围/种类不符）；
//   5. 生效值解析与控件初值文本：effectiveParamValue / paramValueText /
//      paramRangeText；
//   6. RealArrayGrid：fromFlat 形状推断、reshape 行主序截断/零扩展、at；
//   7. NodeOutputCache（对 M4-07 真引擎 + 合成帧源，M5-06 假换真）：Idle 无产物、
//      Running 拉取与最新幅、每节点仅最新一幅、容量驱逐（find 触碰 LRU）、停止后
//      无新产物、clear 排空；
//   8. thumbnailRgbaFromSnapshot：无效快照/maxDim=0 拒绝、Gray8 灰度复制、
//      Rgba8 stride 处理、最近邻降采样、只缩不放、floor 缩放、sequence 透传；
//   9. NodeFailureMarks：NodeFailed 置位/覆盖、其他事件保留、Started/Stopped
//      清空、clear；
//  10. canvas_model 参数面（默认目录描述符）：setParam 追加/替换/未知节点、
//      toGraph 携带、越界赋值经校验标注（BadParam）、合法赋值保持校验通过、
//      connect 预检携带参数（BadParam 非阻塞）、删除节点参数随节点消失。
//  11. 裁切 ROI 联动约束（M6-06，DEC-017 控件层防呆）：RoiConstraint::isRoiParam
//      （x/y/width/height 且带范围 Integer）、effectiveRange 互约束（x∈[0,W−w]、
//      width∈[1,W−x]、y/h 同理）∩ 声明 range、退化输入/退化声明夹成单点、
//      无输入尺寸与非 ROI 参数返回 nullopt、兄弟参数取生效值（赋值优先于默认）、
//      clampValue 闭区间夹取与 llround 取整（非 ROI/非 Integer/无尺寸原样返回）、
//      valueToSliderInRange/sliderToValueInRange（端点/夹取/取整/零跨度）、
//      paramScalarAsDouble（标量取值/非标量 0 防御）。pumpNodeOutput 的输入驱动
//      节点拉取语义（本约束的尺寸源）在 tests/test_run_control.cpp §7 覆盖
//      （需 eui_neo 链接，headless 纪律同文件头说明）。
//
// 测试壳为 tests/test_util.hpp 的 RIN_CHECK*（无第三方框架），main 返回
// rin_test::exitStatus()。NodeOutputCache 走真引擎契约面（owner 线程直接调用，
// 产物到达用秒级死限的有界轮询等待；帧输入为 32x24 Rgba8 合成图案的饱和帧源，
// crop 节点显式携带合法 ROI 参数——默认 64x64 ROI 越出 32x24 帧会被真实 crop
// 算子运行期拒绝）；其余分区单线程纯逻辑。
// param_panel.hpp 的 EUI 组装与 pumpNodeOutput（GpuFrameView GL 上传）headless
// 不可测，不在本文件范围（与 test_navigation/test_node_canvas 同纪律）。

#include "param_model.hpp"

#include "canvas_model.hpp"

#include "test_util.hpp"

#include <kairo/executor.hpp>

#include <rin/camera_types.hpp>
#include <rin/workflow_engine.hpp>
#include <rin/workflow_types.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "default_catalog.hpp"
#include "engine.hpp"

namespace {

// --- 断言辅助 ---

void runSection(const char* name, void (*fn)()) {
    std::printf("== %s\n", name);
    fn();
}

[[nodiscard]] bool nearD(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

// 有界轮询（秒级死限，防悬挂；pred() 为真即返回）。
template <typename Pred>
bool pollUntil(Pred&& pred, std::chrono::milliseconds timeout =
                                std::chrono::milliseconds{5000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

// 构造 NodeOutputSnapshot（node 固定非零以满足 valid()）。
[[nodiscard]] rin::NodeOutputSnapshot makeSnapshot(const rin::PortType format,
                                                   const std::uint32_t width,
                                                   const std::uint32_t height,
                                                   const std::uint32_t stride,
                                                   const std::uint64_t sequence,
                                                   std::vector<std::uint8_t> pixels) {
    rin::NodeOutputSnapshot snapshot;
    snapshot.node = 1;
    snapshot.format = format;
    snapshot.width = width;
    snapshot.height = height;
    snapshot.stride = stride;
    snapshot.sourceSequence = sequence;
    snapshot.pixels =
        std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
    return snapshot;
}

// 读缩略图 Frame 的一个 RGBA 像素分量。
[[nodiscard]] std::uint8_t frameAt(const rin::Frame& frame, const std::uint32_t y,
                                   const std::uint32_t x, const int channel) {
    return (*frame.pixels)[static_cast<std::size_t>(y) * frame.stride +
                           static_cast<std::size_t>(x) * 4u +
                           static_cast<std::size_t>(channel)];
}

// 校验问题检索：kind 与 node 均匹配。
[[nodiscard]] bool hasIssueOn(const viewer::CanvasGraphModel& model,
                              const rin::ValidationIssueKind kind,
                              const rin::NodeId node) {
    for (const rin::ValidationIssue& issue : model.validation.issues) {
        if (issue.kind == kind && issue.node == node) {
            return true;
        }
    }
    return false;
}

// --- 参数声明夹具（自建 + 默认假目录两路） ---

// 本地自建声明：覆盖五个 ParamKind 与无范围/零跨度形态（不走目录，隔离目录
// schema 变化对纯函数边界的干扰）。
struct LocalParams {
    rin::ParamDescriptor flag;      // Boolean，无范围
    rin::ParamDescriptor count;     // Integer -2..8
    rin::ParamDescriptor ratio;     // Real 0..1，无范围变体 plain
    rin::ParamDescriptor plainReal; // Real 无范围
    rin::ParamDescriptor mode;      // Enumeration {off, low, high}
    rin::ParamDescriptor kernel;    // RealArray

    LocalParams() {
        flag.id = "flag";
        flag.label = "Flag";
        flag.kind = rin::ParamKind::Boolean;
        flag.defaultValue = false;

        count.id = "count";
        count.label = "Count";
        count.kind = rin::ParamKind::Integer;
        count.defaultValue = static_cast<std::int64_t>(3);
        count.hasRange = true;
        count.minValue = -2.0;
        count.maxValue = 8.0;

        ratio.id = "ratio";
        ratio.label = "Ratio";
        ratio.kind = rin::ParamKind::Real;
        ratio.defaultValue = 0.5;
        ratio.hasRange = true;
        ratio.minValue = 0.0;
        ratio.maxValue = 1.0;

        plainReal.id = "plain";
        plainReal.label = "Plain";
        plainReal.kind = rin::ParamKind::Real;
        plainReal.defaultValue = 0.25;

        mode.id = "mode";
        mode.label = "Mode";
        mode.kind = rin::ParamKind::Enumeration;
        mode.defaultValue = std::string("off");
        mode.enumOptions = {"off", "low", "high"};

        kernel.id = "kernel";
        kernel.label = "Kernel";
        kernel.kind = rin::ParamKind::RealArray;
        kernel.defaultValue = rin::ParamValue{std::vector<double>{0, 0, 0, 0, 1, 0, 0, 0, 0}};
    }
};

// 断言目录描述符存在；失败时返回 nullptr（后续检查失败但不崩溃）。
[[nodiscard]] const rin::ParamDescriptor* catalogParam(const rin::NodeCatalog& catalog,
                                                      const char* typeId,
                                                      const char* paramId) {
    const rin::NodeDescriptor* node = rin::findNodeDescriptor(catalog, typeId);
    if (node == nullptr) {
        return nullptr;
    }
    for (const rin::ParamDescriptor& param : node->params) {
        if (param.id == paramId) {
            return &param;
        }
    }
    return nullptr;
}

// --- 1. 严格文本解析 ---

void testStrictTextParsing() {
    const LocalParams p;

    // parseIntegerText 正例：严格十进制（from_chars 有符号支持 '-'，不支持 '+'）。
    RIN_CHECK_EQ(viewer::parseIntegerText("0").value_or(-1), 0);
    RIN_CHECK_EQ(viewer::parseIntegerText("-3").value_or(-1), -3);
    RIN_CHECK_EQ(viewer::parseIntegerText("7").value_or(-1), 7);
    RIN_CHECK_EQ(viewer::parseIntegerText("9223372036854775807").value_or(-1),
                 std::numeric_limits<std::int64_t>::max());

    // parseIntegerText 负例：空串/空白/非法字符/溢出/前导 '+'。
    RIN_CHECK_MSG(!viewer::parseIntegerText("").has_value(), "integer: empty rejected");
    RIN_CHECK_MSG(!viewer::parseIntegerText(" 1").has_value(),
                  "integer: leading space rejected");
    RIN_CHECK_MSG(!viewer::parseIntegerText("1 ").has_value(),
                  "integer: trailing space rejected");
    RIN_CHECK_MSG(!viewer::parseIntegerText("1.5").has_value(),
                  "integer: decimal point rejected");
    RIN_CHECK_MSG(!viewer::parseIntegerText("abc").has_value(),
                  "integer: letters rejected");
    RIN_CHECK_MSG(!viewer::parseIntegerText("+7").has_value(),
                  "integer: leading '+' rejected (only '-' is signed)");
    RIN_CHECK_MSG(!viewer::parseIntegerText("-").has_value(),
                  "integer: bare sign rejected");
    RIN_CHECK_MSG(!viewer::parseIntegerText("99999999999999999999").has_value(),
                  "integer: overflow rejected");
    RIN_CHECK_MSG(!viewer::parseIntegerText("9223372036854775808").has_value(),
                  "integer: INT64_MAX+1 rejected");

    // parseRealText 正例：十进制/科学计数/一个前导 '+'。
    RIN_CHECK_MSG(viewer::parseRealText("0.5").value_or(-1.0) == 0.5, "real: 0.5");
    RIN_CHECK_MSG(viewer::parseRealText("+0.5").value_or(-1.0) == 0.5, "real: +0.5");
    RIN_CHECK_MSG(viewer::parseRealText("1e-2").value_or(-1.0) == 0.01, "real: 1e-2");
    RIN_CHECK_MSG(viewer::parseRealText("-3").value_or(0.0) == -3.0, "real: -3");
    RIN_CHECK_MSG(viewer::parseRealText("2").value_or(0.0) == 2.0, "real: bare 2");
    RIN_CHECK_MSG(viewer::parseRealText("1e308").value_or(0.0) == 1e308,
                  "real: large finite kept");

    // parseRealText 负例：空串/尾随字符/非有限文本/缺数字/上溢。
    RIN_CHECK_MSG(!viewer::parseRealText("").has_value(), "real: empty rejected");
    RIN_CHECK_MSG(!viewer::parseRealText("0.5x").has_value(),
                  "real: trailing garbage rejected");
    RIN_CHECK_MSG(!viewer::parseRealText("nan").has_value(), "real: 'nan' rejected");
    RIN_CHECK_MSG(!viewer::parseRealText("inf").has_value(), "real: 'inf' rejected");
    RIN_CHECK_MSG(!viewer::parseRealText("-inf").has_value(), "real: '-inf' rejected");
    RIN_CHECK_MSG(!viewer::parseRealText(".").has_value(), "real: bare dot rejected");
    RIN_CHECK_MSG(!viewer::parseRealText("1e").has_value(),
                  "real: dangling exponent rejected");
    RIN_CHECK_MSG(!viewer::parseRealText("1e309").has_value(),
                  "real: overflow to inf rejected");
    RIN_CHECK_MSG(!viewer::parseRealText("+").has_value(),
                  "real: lone '+' rejected");

    // formatRealText：统一 %.6g。
    RIN_CHECK_EQ(viewer::formatRealText(0.5), std::string("0.5"));
    RIN_CHECK_EQ(viewer::formatRealText(10.0), std::string("10"));
    RIN_CHECK_EQ(viewer::formatRealText(1000000.0), std::string("1e+06"));
    RIN_CHECK_EQ(viewer::formatRealText(1.5), std::string("1.5"));
    RIN_CHECK_EQ(viewer::formatRealText(-2.5), std::string("-2.5"));
    RIN_CHECK_EQ(viewer::formatRealText(0.0), std::string("0"));
}

// --- 2. 值对声明校验 ---

void testParamValidation() {
    const LocalParams p;

    // Boolean：种类匹配通过；不匹配拒绝。
    RIN_CHECK_EQ(viewer::paramValidationError(p.flag, rin::ParamValue{true}),
                 std::string());
    RIN_CHECK_MSG(viewer::paramValidationError(p.flag, rin::ParamValue{static_cast<std::int64_t>(1)})
                      .find("mismatch") != std::string::npos,
                  "Boolean: int64 value mismatches declaration");

    // Integer：闭区间端点恰好合法，越界拒绝。
    RIN_CHECK_EQ(viewer::paramValidationError(p.count, rin::ParamValue{static_cast<std::int64_t>(-2)}),
                 std::string());
    RIN_CHECK_EQ(viewer::paramValidationError(p.count, rin::ParamValue{static_cast<std::int64_t>(8)}),
                 std::string());
    RIN_CHECK_EQ(viewer::paramValidationError(p.count, rin::ParamValue{static_cast<std::int64_t>(3)}),
                 std::string());
    const std::string below = viewer::paramValidationError(p.count, rin::ParamValue{static_cast<std::int64_t>(-3)});
    RIN_CHECK_MSG(below.find("out of range") != std::string::npos,
                  "Integer: below min rejected");
    RIN_CHECK_MSG(below.find("-2..8") != std::string::npos,
                  "Integer: rejection names the declared range");
    RIN_CHECK_MSG(viewer::paramValidationError(p.count, rin::ParamValue{static_cast<std::int64_t>(9)})
                      .find("out of range") != std::string::npos,
                  "Integer: above max rejected");
    RIN_CHECK_MSG(viewer::paramValidationError(p.count, rin::ParamValue{3.0})
                      .find("mismatch") != std::string::npos,
                  "Integer: double value mismatches declaration");

    // Real：端点/有限性；无范围仍拒绝非有限。
    RIN_CHECK_EQ(viewer::paramValidationError(p.ratio, rin::ParamValue{0.0}),
                 std::string());
    RIN_CHECK_EQ(viewer::paramValidationError(p.ratio, rin::ParamValue{1.0}),
                 std::string());
    RIN_CHECK_MSG(viewer::paramValidationError(p.ratio, rin::ParamValue{-0.001})
                      .find("out of range") != std::string::npos,
                  "Real: below min rejected");
    RIN_CHECK_MSG(viewer::paramValidationError(p.ratio, rin::ParamValue{1.0001})
                      .find("out of range") != std::string::npos,
                  "Real: above max rejected");
    RIN_CHECK_MSG(viewer::paramValidationError(p.ratio, rin::ParamValue{std::numeric_limits<double>::quiet_NaN()})
                      .find("finite") != std::string::npos,
                  "Real: NaN rejected as non-finite");
    RIN_CHECK_MSG(viewer::paramValidationError(p.ratio, rin::ParamValue{std::numeric_limits<double>::infinity()})
                      .find("finite") != std::string::npos,
                  "Real: +Inf rejected as non-finite");
    RIN_CHECK_MSG(viewer::paramValidationError(p.plainReal, rin::ParamValue{std::numeric_limits<double>::signaling_NaN()})
                      .find("finite") != std::string::npos,
                  "Real without range: NaN still rejected");
    RIN_CHECK_EQ(viewer::paramValidationError(p.plainReal, rin::ParamValue{123.0}),
                 std::string());
    RIN_CHECK_MSG(viewer::paramValidationError(p.ratio, rin::ParamValue{std::string("0.5")})
                      .find("mismatch") != std::string::npos,
                  "Real: string value mismatches declaration");

    // Enumeration：在选项内通过，未知选项与种类不符拒绝。
    RIN_CHECK_EQ(viewer::paramValidationError(p.mode, rin::ParamValue{std::string("low")}),
                 std::string());
    RIN_CHECK_MSG(viewer::paramValidationError(p.mode, rin::ParamValue{std::string("auto")})
                      .find("not a declared option") != std::string::npos,
                  "Enumeration: undeclared option rejected");
    RIN_CHECK_MSG(viewer::paramValidationError(p.mode, rin::ParamValue{true})
                      .find("mismatch") != std::string::npos,
                  "Enumeration: bool value mismatches declaration");

    // RealArray：全有限通过；含 NaN/±Inf 拒绝；种类不符拒绝。
    const std::vector<double> identityKernel{0, 0, 0, 0, 1, 0, 0, 0, 0};
    RIN_CHECK_EQ(viewer::paramValidationError(p.kernel, rin::ParamValue{identityKernel}),
                 std::string());
    const std::vector<double> withNaN{0.0, std::numeric_limits<double>::quiet_NaN(), 0.0};
    RIN_CHECK_MSG(viewer::paramValidationError(p.kernel, rin::ParamValue{withNaN})
                      .find("finite") != std::string::npos,
                  "RealArray: NaN element rejected");
    const std::vector<double> withInf{std::numeric_limits<double>::infinity()};
    RIN_CHECK_MSG(viewer::paramValidationError(p.kernel, rin::ParamValue{withInf})
                      .find("finite") != std::string::npos,
                  "RealArray: +Inf element rejected");
    const std::vector<double> withNegInf{0.0, -std::numeric_limits<double>::infinity()};
    RIN_CHECK_MSG(viewer::paramValidationError(p.kernel, rin::ParamValue{withNegInf})
                      .find("finite") != std::string::npos,
                  "RealArray: -Inf element rejected");
    RIN_CHECK_MSG(viewer::paramValidationError(p.kernel, rin::ParamValue{std::string("kernel")})
                      .find("mismatch") != std::string::npos,
                  "RealArray: string value mismatches declaration");
}

// --- 3. 赋值包装 ---

void testAssignmentWrapping() {
    const LocalParams p;

    // makeParamAssignment：通过时携带 {paramId, value}；拒绝时 error 非空且
    // assignment 保持默认。
    {
        const viewer::ParamEditResult ok =
            viewer::makeParamAssignment(p.count, rin::ParamValue{static_cast<std::int64_t>(5)});
        RIN_CHECK_MSG(ok.ok, "make: in-range integer accepted");
        RIN_CHECK_EQ(ok.assignment.paramId, std::string("count"));
        RIN_CHECK_EQ(std::get<std::int64_t>(ok.assignment.value), 5);
        RIN_CHECK_EQ(ok.error, std::string());
    }
    {
        const viewer::ParamEditResult bad =
            viewer::makeParamAssignment(p.count, rin::ParamValue{static_cast<std::int64_t>(99)});
        RIN_CHECK_MSG(!bad.ok, "make: out-of-range integer rejected");
        RIN_CHECK_MSG(!bad.error.empty(), "make: rejection carries an error");
        RIN_CHECK_EQ(bad.assignment.paramId, std::string());
    }
    RIN_CHECK_MSG(viewer::makeParamAssignment(p.mode, rin::ParamValue{std::string("high")}).ok,
                  "make: declared enum option accepted");

    // paramAssignmentFromText：Integer。
    {
        const viewer::ParamEditResult ok = viewer::paramAssignmentFromText(p.count, "4");
        RIN_CHECK_MSG(ok.ok, "text: integer digits accepted");
        RIN_CHECK_EQ(std::get<std::int64_t>(ok.assignment.value), 4);
        RIN_CHECK_EQ(ok.assignment.paramId, std::string("count"));
    }
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.count, "3.5").error == "not an integer",
                  "text: non-integer text -> 'not an integer'");
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.count, "").error == "not an integer",
                  "text: empty -> 'not an integer'");
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.count, "abc").error == "not an integer",
                  "text: letters -> 'not an integer'");
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.count, "99").error.find("out of range") !=
                      std::string::npos,
                  "text: parsed integer still range-checked");

    // paramAssignmentFromText：Real。
    {
        const viewer::ParamEditResult ok = viewer::paramAssignmentFromText(p.ratio, "0.25");
        RIN_CHECK_MSG(ok.ok, "text: real digits accepted");
        RIN_CHECK_EQ(std::get<double>(ok.assignment.value), 0.25);
    }
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.ratio, "+0.5").ok,
                  "text: leading '+' accepted for Real");
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.ratio, "nan").error == "not a number",
                  "text: 'nan' -> 'not a number'");
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.ratio, "inf").error == "not a number",
                  "text: 'inf' -> 'not a number'");
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.ratio, "4").error.find("out of range") !=
                      std::string::npos,
                  "text: parsed real still range-checked");

    // paramAssignmentFromText：Enumeration（原文作为值，经声明校验）。
    {
        const viewer::ParamEditResult ok =
            viewer::paramAssignmentFromText(p.mode, "high");
        RIN_CHECK_MSG(ok.ok, "text: declared enum option accepted");
        RIN_CHECK_EQ(std::get<std::string>(ok.assignment.value), std::string("high"));
    }
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.mode, "auto").error.find(
                      "not a declared option") != std::string::npos,
                  "text: undeclared enum option rejected");

    // Boolean/RealArray 不经文本标量入口。
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.flag, "true").error ==
                      "parameter is not text-editable",
                  "text: Boolean is not text-editable");
    RIN_CHECK_MSG(viewer::paramAssignmentFromText(p.kernel, "1").error ==
                      "parameter is not text-editable",
                  "text: RealArray is not text-editable");
}

// --- 4. 滑条归一化映射 ---

void testSliderMapping() {
    const LocalParams p;

    // sliderToValue：端点、线性映射、Integer 就近取整、越界夹取。
    RIN_CHECK_MSG(viewer::sliderToValue(p.count, 0.0).value_or(-99.0) == -2.0,
                  "slider: t=0 maps to min");
    RIN_CHECK_MSG(viewer::sliderToValue(p.count, 1.0).value_or(-99.0) == 8.0,
                  "slider: t=1 maps to max");
    // t=0.5 -> -2 + 0.5*10 = 3（整数域精确中点）。
    RIN_CHECK_MSG(viewer::sliderToValue(p.count, 0.5).value_or(-99.0) == 3.0,
                  "slider: integer midpoint exact");
    // t=0.55 -> -2 + 5.5 = 3.5 -> std::round(3.5) = 4（half away from zero）。
    RIN_CHECK_MSG(viewer::sliderToValue(p.count, 0.55).value_or(-99.0) == 4.0,
                  "slider: integer rounds half away from zero (3.5 -> 4)");
    RIN_CHECK_MSG(viewer::sliderToValue(p.ratio, 0.25).value_or(-99.0) == 0.25,
                  "slider: real linear map");
    RIN_CHECK_MSG(viewer::sliderToValue(p.ratio, 1.5).value_or(-99.0) == 1.0,
                  "slider: t above 1 clamps to max");
    RIN_CHECK_MSG(viewer::sliderToValue(p.ratio, -0.5).value_or(-99.0) == 0.0,
                  "slider: negative t clamps to min");
    RIN_CHECK_MSG(!viewer::sliderToValue(p.ratio, std::numeric_limits<double>::quiet_NaN())
                       .has_value(),
                  "slider: NaN t rejected");
    RIN_CHECK_MSG(!viewer::sliderToValue(p.ratio, std::numeric_limits<double>::infinity())
                       .has_value(),
                  "slider: +Inf t rejected");
    RIN_CHECK_MSG(!viewer::sliderToValue(p.plainReal, 0.5).has_value(),
                  "slider: no-range declaration rejected");

    // valueToSlider：端点、中点、越界夹取。
    RIN_CHECK_MSG(viewer::valueToSlider(p.count, rin::ParamValue{static_cast<std::int64_t>(-2)})
                      .value_or(-99.0) == 0.0,
                  "slider inverse: min -> 0");
    RIN_CHECK_MSG(viewer::valueToSlider(p.count, rin::ParamValue{static_cast<std::int64_t>(8)})
                      .value_or(-99.0) == 1.0,
                  "slider inverse: max -> 1");
    RIN_CHECK_MSG(nearD(viewer::valueToSlider(p.count, rin::ParamValue{static_cast<std::int64_t>(3)})
                            .value_or(-99.0),
                        0.5),
                  "slider inverse: midpoint -> 0.5");
    RIN_CHECK_MSG(viewer::valueToSlider(p.count, rin::ParamValue{static_cast<std::int64_t>(99)})
                      .value_or(-99.0) == 1.0,
                  "slider inverse: above max clamps to 1");
    RIN_CHECK_MSG(viewer::valueToSlider(p.count, rin::ParamValue{static_cast<std::int64_t>(-99)})
                      .value_or(-99.0) == 0.0,
                  "slider inverse: below min clamps to 0");
    RIN_CHECK_MSG(nearD(viewer::valueToSlider(p.ratio, rin::ParamValue{0.3}).value_or(-99.0),
                        0.3),
                  "slider inverse: real 0.3 in 0..1");

    // 种类不符 / 无范围 / 零跨度。
    RIN_CHECK_MSG(!viewer::valueToSlider(p.count, rin::ParamValue{3.0}).has_value(),
                  "slider inverse: Real value for Integer descriptor rejected");
    RIN_CHECK_MSG(!viewer::valueToSlider(p.ratio, rin::ParamValue{static_cast<std::int64_t>(0)})
                      .has_value(),
                  "slider inverse: Integer value for Real descriptor rejected");
    RIN_CHECK_MSG(!viewer::valueToSlider(p.flag, rin::ParamValue{true}).has_value(),
                  "slider inverse: Boolean value rejected");
    RIN_CHECK_MSG(!viewer::valueToSlider(p.mode, rin::ParamValue{std::string("off")}).has_value(),
                  "slider inverse: no-range enumeration rejected");
    {
        rin::ParamDescriptor flat = p.count;
        flat.minValue = 5.0;
        flat.maxValue = 5.0;  // span <= 0。
        RIN_CHECK_MSG(viewer::valueToSlider(flat, rin::ParamValue{static_cast<std::int64_t>(5)})
                          .value_or(-1.0) == 0.0,
                      "slider inverse: zero span -> 0.0");
        RIN_CHECK_MSG(viewer::valueToSlider(flat, rin::ParamValue{static_cast<std::int64_t>(7)})
                          .value_or(-1.0) == 0.0,
                      "slider inverse: zero span clamps any value to 0.0");
    }

    // 往返：Real 误差 < 1e-9；Integer 往返落在取整格点。
    for (const double value : {0.0, 0.1, 0.3, 0.5, 0.77, 1.0}) {
        const std::optional<double> t = viewer::valueToSlider(p.ratio, rin::ParamValue{value});
        RIN_CHECK(t.has_value());
        if (!t.has_value()) {
            continue;
        }
        const std::optional<double> back = viewer::sliderToValue(p.ratio, *t);
        RIN_CHECK_MSG(back.has_value() && nearD(*back, value),
                      "slider round trip: Real value survives within 1e-9");
    }
    {
        const std::optional<double> t =
            viewer::valueToSlider(p.count, rin::ParamValue{static_cast<std::int64_t>(6)});
        RIN_CHECK(t.has_value());
        const std::optional<double> back =
            t.has_value() ? viewer::sliderToValue(p.count, *t) : std::nullopt;
        RIN_CHECK_MSG(back.has_value() && *back == 6.0,
                      "slider round trip: Integer 6 returns to the same rung");
    }
}

// --- 5. 生效值与控件文本 ---

void testEffectiveValueAndText() {
    const LocalParams p;

    // 节点声明：承载多个参数描述（生效值按 paramId 在声明内回退默认值）。
    rin::NodeDescriptor node;
    node.typeId = "probe";
    node.displayName = "Probe";
    node.params = {p.count, p.ratio, p.mode};

    // 缺省回退：未赋值指向声明默认值。
    {
        const std::vector<rin::ParamAssignment> none;
        const rin::ParamValue* effective =
            viewer::effectiveParamValue(node, none, "count");
        RIN_CHECK(effective != nullptr);
        if (effective != nullptr) {
            RIN_CHECK_EQ(std::get<std::int64_t>(*effective), 3);
        }
    }
    // 赋值优先：存在赋值时指向赋值而非默认。
    {
        const std::vector<rin::ParamAssignment> assigned = {
            {"count", rin::ParamValue{static_cast<std::int64_t>(7)}}};
        const rin::ParamValue* effective =
            viewer::effectiveParamValue(node, assigned, "count");
        RIN_CHECK(effective != nullptr);
        if (effective != nullptr) {
            RIN_CHECK_EQ(std::get<std::int64_t>(*effective), 7);
        }
        // 同一赋值列表内未覆盖的参数仍取默认。
        const rin::ParamValue* other = viewer::effectiveParamValue(node, assigned, "ratio");
        RIN_CHECK(other != nullptr && std::get<double>(*other) == 0.5);
    }
    // 未知 paramId：nullptr（赋值与声明都没有）。
    {
        const std::vector<rin::ParamAssignment> assigned = {
            {"count", rin::ParamValue{static_cast<std::int64_t>(7)}}};
        RIN_CHECK(viewer::effectiveParamValue(node, assigned, "nope") == nullptr);
        // paramId 不在本节点声明内：空赋值列表下无回退来源。
        rin::NodeDescriptor otherNode;
        otherNode.typeId = "other";
        otherNode.displayName = "Other";
        otherNode.params = {p.flag};
        const std::vector<rin::ParamAssignment> none;
        RIN_CHECK(viewer::effectiveParamValue(otherNode, none, "count") == nullptr);
        // 赋值优先于声明：即使值不在声明内，已有赋值也生效（查到即返回）。
        RIN_CHECK(viewer::effectiveParamValue(otherNode, assigned, "count") != nullptr);
    }

    // paramValueText。
    RIN_CHECK_EQ(viewer::paramValueText(rin::ParamValue{true}), std::string("on"));
    RIN_CHECK_EQ(viewer::paramValueText(rin::ParamValue{false}), std::string("off"));
    RIN_CHECK_EQ(viewer::paramValueText(rin::ParamValue{static_cast<std::int64_t>(42)}),
                 std::string("42"));
    RIN_CHECK_EQ(viewer::paramValueText(rin::ParamValue{static_cast<std::int64_t>(-7)}),
                 std::string("-7"));
    RIN_CHECK_EQ(viewer::paramValueText(rin::ParamValue{0.5}), std::string("0.5"));
    RIN_CHECK_EQ(viewer::paramValueText(rin::ParamValue{1000000.0}), std::string("1e+06"));
    RIN_CHECK_EQ(viewer::paramValueText(rin::ParamValue{std::string("nearest")}),
                 std::string("nearest"));
    const std::vector<double> someArray{1.0, 2.0};
    RIN_CHECK_EQ(viewer::paramValueText(rin::ParamValue{someArray}), std::string());

    // paramRangeText：hasRange 才呈现，格式统一 %.6g。
    RIN_CHECK_EQ(viewer::paramRangeText(p.count), std::string("-2..8"));
    RIN_CHECK_EQ(viewer::paramRangeText(p.ratio), std::string("0..1"));
    RIN_CHECK_EQ(viewer::paramRangeText(p.flag), std::string());
    RIN_CHECK_EQ(viewer::paramRangeText(p.mode), std::string());
}

// --- 6. RealArray 矩阵网格 ---

void testRealArrayGrid() {
    // 完全平方数 -> KxK。
    {
        const viewer::RealArrayGrid grid =
            viewer::RealArrayGrid::fromFlat({1, 2, 3, 4, 5, 6, 7, 8, 9});
        RIN_CHECK_MSG((grid.rows == 3 && grid.cols == 3), "grid: 9 elements infer 3x3");
        RIN_CHECK_MSG(grid.values.size() == 9, "grid: data preserved");
        RIN_CHECK_MSG(grid.at(0, 0) == 1 && grid.at(1, 1) == 5 && grid.at(2, 2) == 9,
                      "grid: row-major at()");
        RIN_CHECK_MSG(grid.at(0, 2) == 3 && grid.at(2, 0) == 7,
                      "grid: row-major ordering distinguishes corners");
    }
    {
        const viewer::RealArrayGrid grid = viewer::RealArrayGrid::fromFlat({1, 2, 3, 4});
        RIN_CHECK_MSG((grid.rows == 2 && grid.cols == 2), "grid: 4 elements infer 2x2");
    }
    {
        const viewer::RealArrayGrid grid =
            viewer::RealArrayGrid::fromFlat({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
        RIN_CHECK_MSG((grid.rows == 4 && grid.cols == 4), "grid: 16 elements infer 4x4");
    }
    // 非完全平方数 -> 1xN。
    {
        const viewer::RealArrayGrid grid = viewer::RealArrayGrid::fromFlat({9, 8, 7, 6, 5});
        RIN_CHECK_MSG((grid.rows == 1 && grid.cols == 5), "grid: 5 elements infer 1x5");
        RIN_CHECK_MSG(grid.at(0, 3) == 6, "grid: 1xN row-major at()");
    }
    // 空 -> 1x1 零值。
    {
        const viewer::RealArrayGrid grid = viewer::RealArrayGrid::fromFlat({});
        RIN_CHECK_MSG((grid.rows == 1 && grid.cols == 1 && grid.values.size() == 1 &&
                       grid.values[0] == 0.0),
                      "grid: empty data infers 1x1 zero");
    }
    // reshape：截断保留行主序前缀。
    {
        viewer::RealArrayGrid grid = viewer::RealArrayGrid::fromFlat({1, 2, 3, 4, 5, 6, 7, 8, 9});
        grid.reshape(2, 2);
        RIN_CHECK_MSG((grid.rows == 2 && grid.cols == 2 && grid.values.size() == 4),
                      "grid: reshape truncates to new area");
        RIN_CHECK_MSG((grid.at(0, 0) == 1 && grid.at(0, 1) == 2 && grid.at(1, 0) == 3 &&
                       grid.at(1, 1) == 4),
                      "grid: truncation keeps the row-major prefix");
    }
    // reshape：非平方面积的部分截断。
    {
        viewer::RealArrayGrid grid = viewer::RealArrayGrid::fromFlat({1, 2, 3, 4, 5, 6, 7, 8, 9});
        grid.reshape(2, 3);
        RIN_CHECK_MSG((grid.rows == 2 && grid.cols == 3 && grid.values.size() == 6),
                      "grid: reshape to 2x3 keeps six values");
        RIN_CHECK_MSG((grid.at(1, 2) == 6), "grid: prefix order preserved across reshape");
    }
    // reshape：零扩展（行主序语义：扁平序列保留前缀，(r,c) 寻址按新行列数重映射）。
    {
        viewer::RealArrayGrid grid = viewer::RealArrayGrid::fromFlat({1, 2, 3, 4, 5, 6, 7, 8, 9});
        grid.reshape(4, 4);
        RIN_CHECK_MSG((grid.rows == 4 && grid.cols == 4 && grid.values.size() == 16),
                      "grid: reshape grows the storage");
        RIN_CHECK_MSG((grid.at(0, 0) == 1 && grid.at(2, 0) == 9),
                      "grid: growth keeps the original flat prefix (9 = flat index 8 -> (2,0))");
        RIN_CHECK_MSG((grid.at(3, 3) == 0.0 && grid.at(3, 0) == 0.0 && grid.at(2, 3) == 0.0),
                      "grid: growth zero-fills the tail");
    }
    // reshape：行列下限 1。
    {
        viewer::RealArrayGrid grid = viewer::RealArrayGrid::fromFlat({1, 2, 3, 4});
        grid.reshape(0, 5);
        RIN_CHECK_MSG((grid.rows == 1 && grid.cols == 5 && grid.values.size() == 5),
                      "grid: reshape clamps rows to >= 1");
        grid.reshape(3, 0);
        RIN_CHECK_MSG((grid.rows == 3 && grid.cols == 1 && grid.values.size() == 3),
                      "grid: reshape clamps cols to >= 1");
        grid.reshape(0, 0);
        RIN_CHECK_MSG((grid.rows == 1 && grid.cols == 1 && grid.at(0, 0) == 1.0),
                      "grid: reshape(0,0) degenerates to 1x1 keeping the first value");
    }
}

// --- 7. 节点中间产物有界缓存（对 M4-07 真引擎 + 合成帧源） ---

void testNodeOutputCache() {
    kairo::Executor executor;
    kairo::ExecutorConfig executorConfig;
    const bool initialized = static_cast<bool>(executor.initialize(executorConfig));
    RIN_CHECK(initialized);
    if (!initialized) {
        return;
    }

    // 真引擎 fixture（M5-06 假换真）：32x24 Rgba8 确定性图案 + 饱和帧源（每次
    // 探测交付新帧，workflow_bench saturatingSource 同款），2ms 泵保证有界轮询
    // 快。帧源无状态（水位由引擎按节点传入的 lastSeen 驱动）。
    constexpr std::uint32_t kFrameWidth = 32;
    constexpr std::uint32_t kFrameHeight = 24;
    static const std::shared_ptr<const std::vector<std::uint8_t>> kFixturePixels = [] {
        auto buffer = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(kFrameWidth) * kFrameHeight * 4);
        for (std::uint32_t y = 0; y < kFrameHeight; ++y) {
            for (std::uint32_t x = 0; x < kFrameWidth; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kFrameWidth + x) * 4;
                (*buffer)[offset + 0] = static_cast<std::uint8_t>((x * 3 + y) & 0xFF);
                (*buffer)[offset + 1] = static_cast<std::uint8_t>((x + y * 2) & 0xFF);
                (*buffer)[offset + 2] = static_cast<std::uint8_t>((x * 7 + y * 5) & 0xFF);
                (*buffer)[offset + 3] = 0xFF;
            }
        }
        return buffer;
    }();
    rin::WorkflowEngineConfig config;
    config.pumpInterval = std::chrono::milliseconds{2};
    config.frameSource = [](rin::NodeId, std::uint64_t& lastSeen,
                            rin::WorkflowFrameInput& out) {
        out.sourceSequence = lastSeen + 1;
        out.image = rin::ImageU8::wrap(rin::PortType::Rgba8, kFrameWidth, kFrameHeight,
                                       kFrameWidth * 4, kFixturePixels);
        lastSeen = out.sourceSequence;
        return true;
    };
    std::shared_ptr<rin::IWorkflowEngine> engine =
        rin::createWorkflowEngine(executor, std::move(config));

    // 五节点链：source(Rgba8) -> crop -> downscale -> grayify(->Gray8) ->
    // gaussian_blur；节点 1..5 每帧各发布一幅产物。crop 显式携带合法 ROI
    // （16x12 @ (0,0)）：真实 crop 算子对越界 ROI 运行期拒绝，默认 64x64 ROI
    // 越出 32x24 帧（假引擎语义不含此校验，真引擎下必须显式给参）。
    rin::WorkflowGraph graph;
    const char* typeIds[5] = {"source", "crop", "downscale", "grayify", "gaussian_blur"};
    for (std::uint64_t i = 0; i < 5; ++i) {
        rin::NodeInstance instance;
        instance.id = i + 1;
        instance.typeId = typeIds[i];
        if (instance.typeId == "crop") {
            instance.params = {
                {"x", rin::ParamValue{static_cast<std::int64_t>(0)}},
                {"y", rin::ParamValue{static_cast<std::int64_t>(0)}},
                {"width", rin::ParamValue{static_cast<std::int64_t>(16)}},
                {"height", rin::ParamValue{static_cast<std::int64_t>(12)}},
            };
        }
        graph.nodes.push_back(instance);
    }
    for (std::uint64_t i = 0; i < 4; ++i) {
        rin::Connection connection;
        connection.from = rin::PortRef{i + 1, rin::PortDirection::Output, 0};
        connection.to = rin::PortRef{i + 2, rin::PortDirection::Input, 0};
        graph.connections.push_back(connection);
    }
    const bool graphOk = engine->applyGraph(graph).ok;
    RIN_CHECK(graphOk);
    if (!graphOk) {
        engine->stop();
        return;
    }

    viewer::NodeOutputCache cache;
    RIN_CHECK_EQ(cache.size(), std::size_t{0});
    RIN_CHECK(cache.find(1) == nullptr);

    // Idle（未 start）：无产物可拉。
    RIN_CHECK_MSG(!cache.pull(*engine, 1), "cache: Idle engine yields no output");
    RIN_CHECK_MSG(!cache.pull(*engine, 99), "cache: node outside the graph yields nothing");
    RIN_CHECK_EQ(cache.size(), std::size_t{0});

    const rin::AdmissionResult admission = engine->start();
    RIN_CHECK(admission.admitted);
    if (!admission.admitted) {
        engine->stop();
        return;
    }

    // Running：单选节点拉取到首幅产物。
    RIN_CHECK_MSG(pollUntil([&] { return cache.pull(*engine, 1); }),
                  "cache: source node publishes while running");
    const rin::NodeOutputSnapshot* first = cache.find(1);
    RIN_CHECK(first != nullptr);
    if (first != nullptr) {
        RIN_CHECK_MSG(first->valid(), "cache: cached snapshot is valid");
        RIN_CHECK_EQ(first->node, 1u);
        RIN_CHECK_EQ(first->format, rin::PortType::Rgba8);
    }
    const std::uint64_t s1 = first != nullptr ? first->sourceSequence : 0;

    // 连续两帧：拉到更新的第二幅，find 返回较大 sourceSequence（每节点仅最新）。
    RIN_CHECK_MSG(pollUntil([&] { return cache.pull(*engine, 1); }),
                  "cache: second frame yields a newer output");
    const rin::NodeOutputSnapshot* second = cache.find(1);
    RIN_CHECK(second != nullptr);
    if (second != nullptr) {
        RIN_CHECK_MSG(second->sourceSequence > s1,
                      "cache: find returns the newer frame after a second pull");
    }
    RIN_CHECK_MSG(pollUntil([&] { return cache.pull(*engine, 4); }),
                  "cache: downstream grayify node publishes too");
    RIN_CHECK_MSG(pollUntil([&] { return cache.pull(*engine, 5); }),
                  "cache: terminal blur node publishes too");
    RIN_CHECK(cache.find(4) != nullptr);
    RIN_CHECK(cache.find(5) != nullptr);
    if (cache.find(4) != nullptr) {
        RIN_CHECK_EQ(cache.find(4)->format, rin::PortType::Gray8);
    }

    // 容量驱逐：先独立排水（保证 1..5 邮箱均非空），再做一轮有序拉取，
    // 使 lastUse 顺序确定为 pull 调用序（find 触碰 LRU）。
    viewer::NodeOutputCache lru;
    {
        std::uint64_t seen[5] = {0, 0, 0, 0, 0};
        rin::NodeOutputSnapshot drain;
        RIN_CHECK_MSG(pollUntil([&] {
            for (std::uint64_t node = 1; node <= 5; ++node) {
                if (!engine->tryLoadNodeOutput(node, seen[node - 1], drain)) {
                    return false;
                }
            }
            return true;
        }), "cache: all five nodes have published at least once");
    }
    RIN_CHECK(lru.pull(*engine, 1));
    RIN_CHECK(lru.pull(*engine, 2));
    RIN_CHECK(lru.pull(*engine, 3));
    RIN_CHECK(lru.pull(*engine, 4));
    RIN_CHECK_EQ(lru.size(), std::size_t{4});
    RIN_CHECK_MSG(lru.find(1) != nullptr,
                  "cache: touched entry stays (find refreshes LRU)");
    RIN_CHECK(lru.pull(*engine, 5));  // 第 5 个入队，驱逐最久未使用的节点 2。
    RIN_CHECK_MSG(lru.size() == std::size_t{4},
                  "cache: capacity stays at the bound after eviction");
    RIN_CHECK_MSG(lru.find(2) == nullptr,
                  "cache: least-recently-used node 2 is the evicted victim");
    RIN_CHECK_MSG(lru.find(1) != nullptr,
                  "cache: refreshed entry survives eviction");
    RIN_CHECK(lru.find(3) != nullptr);
    RIN_CHECK(lru.find(4) != nullptr);
    RIN_CHECK(lru.find(5) != nullptr);

    // 停止：先追平停止前可能残留的未读产物（停止前从未拉取的节点可能落后
    // 多帧）。每个节点的 catch-up 循环必须以 pull()==false 终止——停止后发布
    // 停止，收敛是确定性的（guard 只防悬挂）。
    // 注意：不在此后再补一次"必须为 false"的拉取断言——catch-up 期间新建的
    // 条目可能触发容量驱逐，被驱逐节点的再拉取按契约视为无先见状态（seen=0）
    // 合法重读引擎保留的 stale 快照；"停止路径排空"的缓存纪律由下方 clear()
    // 断言承载（UI 关闭路径 clear 后不再 pull）。
    engine->stop();
    {
        int guard = 0;
        for (std::uint64_t node = 1; node <= 5; ++node) {
            while (cache.pull(*engine, node) && ++guard < 100000) {
            }
        }
        RIN_CHECK_MSG(guard < 100000,
                      "cache: post-stop pulls converge (publishes ceased)");
    }

    // 排空：clear() 后缓存为空，stale 快照不在缓存内复活。
    cache.clear();
    RIN_CHECK_EQ(cache.size(), std::size_t{0});
    RIN_CHECK_MSG(cache.find(1) == nullptr, "cache: clear drains node 1");
    RIN_CHECK_MSG(cache.find(5) == nullptr, "cache: clear drains node 5");

    engine.reset();
    RIN_CHECK(executor.shutdown(true) == kairo::ShutdownResult::Completed);
}

// --- 8. 缩略图降采样 ---

void testThumbnailRgba() {
    // 无效快照：width=0 / pixels 缺失 / stride 不足。
    {
        rin::NodeOutputSnapshot zeroWidth =
            makeSnapshot(rin::PortType::Gray8, 0, 4, 0, 1, std::vector<std::uint8_t>(4));
        rin::Frame out;
        RIN_CHECK_MSG(!viewer::thumbnailRgbaFromSnapshot(zeroWidth, 256, out),
                      "thumbnail: zero-width snapshot rejected");
    }
    {
        rin::NodeOutputSnapshot noPixels;
        noPixels.node = 1;
        noPixels.width = 4;
        noPixels.height = 4;
        rin::Frame out;
        RIN_CHECK_MSG(!viewer::thumbnailRgbaFromSnapshot(noPixels, 256, out),
                      "thumbnail: pixel-less snapshot rejected");
    }
    {
        rin::NodeOutputSnapshot thinStride =
            makeSnapshot(rin::PortType::Gray8, 4, 2, 2, 1, std::vector<std::uint8_t>(4));
        rin::Frame out;
        RIN_CHECK_MSG(!viewer::thumbnailRgbaFromSnapshot(thinStride, 256, out),
                      "thumbnail: undersized stride rejected");
    }
    {
        const rin::NodeOutputSnapshot good =
            makeSnapshot(rin::PortType::Gray8, 4, 4, 4, 1, std::vector<std::uint8_t>(16));
        rin::Frame out;
        RIN_CHECK_MSG(!viewer::thumbnailRgbaFromSnapshot(good, 0, out),
                      "thumbnail: maxDim=0 rejected");
    }

    // Gray8 4x4 无缩放：灰度复制三通道 + alpha=255，stride=width*4。
    {
        std::vector<std::uint8_t> gray(16);
        for (std::size_t i = 0; i < gray.size(); ++i) {
            gray[i] = static_cast<std::uint8_t>(i);
        }
        const rin::NodeOutputSnapshot snapshot =
            makeSnapshot(rin::PortType::Gray8, 4, 4, 4, 77, std::move(gray));
        rin::Frame out;
        RIN_CHECK_MSG(viewer::thumbnailRgbaFromSnapshot(snapshot, 256, out),
                      "thumbnail: gray 4x4 accepted");
        RIN_CHECK(out.valid());
        RIN_CHECK_EQ(out.width, 4u);
        RIN_CHECK_EQ(out.height, 4u);
        RIN_CHECK_EQ(out.stride, 16u);
        RIN_CHECK_EQ(out.sequence, std::uint64_t{77});
        RIN_CHECK_EQ(out.pixels->size(), std::size_t{64});
        bool allMatch = true;
        for (std::uint32_t y = 0; y < 4 && allMatch; ++y) {
            for (std::uint32_t x = 0; x < 4 && allMatch; ++x) {
                const std::uint8_t expected = static_cast<std::uint8_t>(y * 4 + x);
                allMatch = frameAt(out, y, x, 0) == expected &&
                           frameAt(out, y, x, 1) == expected &&
                           frameAt(out, y, x, 2) == expected &&
                           frameAt(out, y, x, 3) == 255;
            }
        }
        RIN_CHECK_MSG(allMatch, "thumbnail: gray replicated to RGBA with alpha 255");
    }

    // Rgba8 stride > width*4：行尾对齐填充必须被跳过（像素 x 在行内偏移 x*4，
    // 行 y 起点为 y*stride）。
    {
        const std::vector<std::uint8_t> padded = {
            10, 20, 30, 40, 50, 60, 70, 80, 222, 222, 222, 222,          // row 0: 2 px + pad
            90, 100, 110, 120, 130, 140, 150, 160, 222, 222, 222, 222};  // row 1: 2 px + pad
        const rin::NodeOutputSnapshot snapshot =
            makeSnapshot(rin::PortType::Rgba8, 2, 2, 12, 5, padded);
        rin::Frame out;
        RIN_CHECK_MSG(viewer::thumbnailRgbaFromSnapshot(snapshot, 256, out),
                      "thumbnail: padded rgba accepted");
        RIN_CHECK(out.valid());
        RIN_CHECK_EQ(out.width, 2u);
        RIN_CHECK_EQ(out.height, 2u);
        RIN_CHECK_EQ(out.stride, 8u);
        RIN_CHECK_EQ(out.pixels->size(), std::size_t{16});
        RIN_CHECK_MSG(frameAt(out, 0, 0, 0) == 10 && frameAt(out, 0, 0, 3) == 40 &&
                          frameAt(out, 0, 1, 0) == 50 && frameAt(out, 0, 1, 3) == 80 &&
                          frameAt(out, 1, 0, 0) == 90 && frameAt(out, 1, 1, 1) == 140 &&
                          frameAt(out, 1, 1, 3) == 160,
                      "thumbnail: rgba rows located by stride, pixels packed at x*4");
        bool noPaddingLeak = true;
        for (std::size_t i = 0; i < out.pixels->size() && noPaddingLeak; ++i) {
            noPaddingLeak = (*out.pixels)[i] != 222;
        }
        RIN_CHECK_MSG(noPaddingLeak, "thumbnail: no padding byte leaks into the output");
    }

    // 最近邻降采样：4x2 -> 2x1，采样 (0,0) 与 (2,0)。
    {
        const rin::NodeOutputSnapshot snapshot = makeSnapshot(
            rin::PortType::Gray8, 4, 2, 4, 9, std::vector<std::uint8_t>{100, 101, 102, 103,
                                                                        200, 201, 202, 203});
        rin::Frame out;
        RIN_CHECK_MSG(viewer::thumbnailRgbaFromSnapshot(snapshot, 2, out),
                      "thumbnail: 4x2 downscales");
        RIN_CHECK_EQ(out.width, 2u);
        RIN_CHECK_EQ(out.height, 1u);
        RIN_CHECK_MSG(frameAt(out, 0, 0, 0) == 100 && frameAt(out, 0, 0, 3) == 255 &&
                          frameAt(out, 0, 1, 0) == 102,
                      "thumbnail: nearest-neighbor taps source columns 0 and 2");
    }

    // 只缩不放：小于 maxDim 的源保持原尺寸。
    {
        const rin::NodeOutputSnapshot snapshot =
            makeSnapshot(rin::PortType::Gray8, 64, 48, 64, 11,
                         std::vector<std::uint8_t>(64 * 48, 7));
        rin::Frame out;
        RIN_CHECK(viewer::thumbnailRgbaFromSnapshot(snapshot, viewer::kThumbnailMaxDim, out));
        RIN_CHECK_MSG((out.width == 64u && out.height == 48u),
                      "thumbnail: 64x48 stays 64x48 under the 256 bound (never upscales)");
    }

    // floor 缩放：512x256 -> 256x128。
    {
        const rin::NodeOutputSnapshot snapshot =
            makeSnapshot(rin::PortType::Gray8, 512, 256, 512, 987654321,
                         std::vector<std::uint8_t>(512 * 256, 3));
        rin::Frame out;
        RIN_CHECK(viewer::thumbnailRgbaFromSnapshot(snapshot, 256, out));
        RIN_CHECK_MSG((out.width == 256u && out.height == 128u),
                      "thumbnail: 512x256 floors to 256x128");
        RIN_CHECK_EQ(out.stride, 1024u);
        RIN_CHECK_EQ(out.pixels->size(), static_cast<std::size_t>(1024) * 128);
        RIN_CHECK_EQ(out.sequence, std::uint64_t{987654321});
        RIN_CHECK(out.valid());
    }
}

// --- 8b. 枚举点击循环与监看器预览上限（M11/DEC-021 决策 5/6 纯逻辑） ---

void testCycleEnumOptionAndMonitorPreviewCap() {
    // cycleEnumOption：current 的下一选项、末项环绕回首项。
    {
        const std::vector<std::string> options{"nearest", "bilinear"};
        RIN_CHECK_MSG(viewer::cycleEnumOption(options, "nearest") ==
                          std::optional<std::string>{"bilinear"},
                      "cycle: first option advances to the second");
        RIN_CHECK_MSG(viewer::cycleEnumOption(options, "bilinear") ==
                          std::optional<std::string>{"nearest"},
                      "cycle: last option wraps to the first");
    }
    {
        const std::vector<std::string> options{"clamp", "reflect", "zero"};
        RIN_CHECK(viewer::cycleEnumOption(options, "clamp") == std::optional<std::string>{"reflect"});
        RIN_CHECK(viewer::cycleEnumOption(options, "reflect") == std::optional<std::string>{"zero"});
        RIN_CHECK_MSG(viewer::cycleEnumOption(options, "zero") ==
                          std::optional<std::string>{"clamp"},
                      "cycle: three options wrap end to start");
    }
    // 单选项：环绕到自身。
    {
        const std::vector<std::string> options{"only"};
        RIN_CHECK_MSG(viewer::cycleEnumOption(options, "only") ==
                          std::optional<std::string>{"only"},
                      "cycle: single option cycles to itself");
    }
    // current 不在选项内 / 空选项 → nullopt（调用方保持现值不变）。
    {
        const std::vector<std::string> options{"a", "b"};
        RIN_CHECK_MSG(!viewer::cycleEnumOption(options, "c").has_value(),
                      "cycle: unknown current yields nullopt");
        RIN_CHECK_MSG(!viewer::cycleEnumOption(options, "").has_value(),
                      "cycle: empty current yields nullopt");
        const std::vector<std::string> empty;
        RIN_CHECK_MSG(!viewer::cycleEnumOption(empty, "a").has_value(),
                      "cycle: empty options yield nullopt");
    }
    // 完整遍历回到起点（点击循环经全选项）。
    {
        const std::vector<std::string> options{"1", "3", "5"};
        std::string current = "1";
        for (std::size_t i = 0; i < options.size(); ++i) {
            const std::optional<std::string> next = viewer::cycleEnumOption(options, current);
            RIN_CHECK(next.has_value());
            if (next) {
                current = *next;
            }
        }
        RIN_CHECK_MSG(current == "1", "cycle: full traversal returns to the start");
    }

    // 监看器预览最长边上限（M11/DEC-021：有界；> 缩略图 256，放大档清晰。
    // 2026-10-07 视窗宽高双向可调（28685c3）后窗口可达 800x640 画布单位，
    // 上限 512→1024 保证宽窗常见放大档清晰）。
    RIN_CHECK_EQ(viewer::kMonitorPreviewMaxDim, std::uint32_t{1024});
    RIN_CHECK_MSG(viewer::kMonitorPreviewMaxDim > viewer::kThumbnailMaxDim,
                  "monitor: preview cap exceeds the thumbnail cap");
    // maxDim=1024 下降采样边界：1200x600 快照 → 1024x512（保持宽高比、只缩
    // 不放）；maxDim=512 旧边界行为回归（600x300 → 512x256）。
    {
        rin::NodeOutputSnapshot snapshot;
        snapshot.node = 1;
        snapshot.format = rin::PortType::Gray8;
        snapshot.width = 1200;
        snapshot.height = 600;
        snapshot.stride = 1200;
        snapshot.sourceSequence = 11;
        snapshot.pixels = std::make_shared<const std::vector<std::uint8_t>>(
            static_cast<std::size_t>(1200) * 600, 0x55);
        rin::Frame out;
        RIN_CHECK(viewer::thumbnailRgbaFromSnapshot(snapshot, viewer::kMonitorPreviewMaxDim, out));
        RIN_CHECK_EQ(out.width, 1024u);
        RIN_CHECK_EQ(out.height, 512u);
        // 显式 maxDim=512 旧边界行为回归：600x300 → 512x256（函数语义与常量
        // 上限解耦，上限抬升不改变下降采样数学）。
        rin::NodeOutputSnapshot legacy = snapshot;
        legacy.sourceSequence = 12;
        rin::Frame legacyOut;
        RIN_CHECK(viewer::thumbnailRgbaFromSnapshot(legacy, 512u, legacyOut));
        RIN_CHECK_EQ(legacyOut.width, 512u);
        RIN_CHECK_EQ(legacyOut.height, 256u);
    }
}

// --- 9. 节点执行失败标注 ---

void testNodeFailureMarks() {
    viewer::NodeFailureMarks marks;
    RIN_CHECK_EQ(marks.size(), std::size_t{0});
    RIN_CHECK(marks.failureOf(1) == nullptr);

    // NodeFailed 置位。
    marks.applyEvent({rin::WorkflowEventKind::NodeFailed, 5, "boom", 0.0});
    RIN_CHECK_EQ(marks.size(), std::size_t{1});
    const std::string* message = marks.failureOf(5);
    RIN_CHECK(message != nullptr);
    RIN_CHECK_MSG(message != nullptr && *message == "boom", "marks: failure message stored");

    // 再次 NodeFailed：同节点覆盖，不累积。
    marks.applyEvent({rin::WorkflowEventKind::NodeFailed, 5, "boom2", 1.0});
    RIN_CHECK_EQ(marks.size(), std::size_t{1});
    message = marks.failureOf(5);
    RIN_CHECK_MSG(message != nullptr && *message == "boom2", "marks: latest failure wins");

    // 其他节点置位互不干扰。
    marks.applyEvent({rin::WorkflowEventKind::NodeFailed, 7, "crash", 2.0});
    RIN_CHECK_EQ(marks.size(), std::size_t{2});
    RIN_CHECK(marks.failureOf(7) != nullptr);

    // 非清空事件保留标注（含引擎 Failed 终态——恢复路径 = 重新启动）。
    marks.applyEvent({rin::WorkflowEventKind::GraphApplied, rin::kInvalidNode, "", 3.0});
    marks.applyEvent({rin::WorkflowEventKind::ParamUpdated, 5, "", 4.0});
    marks.applyEvent({rin::WorkflowEventKind::Info, rin::kInvalidNode, "", 5.0});
    marks.applyEvent({rin::WorkflowEventKind::Failed, rin::kInvalidNode, "engine failed", 6.0});
    RIN_CHECK_MSG(marks.size() == std::size_t{2},
                  "marks: GraphApplied/ParamUpdated/Info/Failed keep the marks");
    RIN_CHECK(marks.failureOf(5) != nullptr && marks.failureOf(7) != nullptr);

    // Started（新会话）清空。
    marks.applyEvent({rin::WorkflowEventKind::Started, rin::kInvalidNode, "", 7.0});
    RIN_CHECK_EQ(marks.size(), std::size_t{0});
    RIN_CHECK(marks.failureOf(5) == nullptr);
    RIN_CHECK(marks.failureOf(7) == nullptr);

    // Stopped（会话结束）清空。
    marks.applyEvent({rin::WorkflowEventKind::NodeFailed, 3, "late", 8.0});
    RIN_CHECK_EQ(marks.size(), std::size_t{1});
    marks.applyEvent({rin::WorkflowEventKind::Stopped, rin::kInvalidNode, "", 9.0});
    RIN_CHECK_EQ(marks.size(), std::size_t{0});
    RIN_CHECK(marks.failureOf(3) == nullptr);

    // 直接 clear()。
    marks.applyEvent({rin::WorkflowEventKind::NodeFailed, 4, "x", 10.0});
    marks.clear();
    RIN_CHECK_EQ(marks.size(), std::size_t{0});
    RIN_CHECK(marks.failureOf(4) == nullptr);
}

// --- 10. canvas_model 参数面 ---

// 图模型夹具：默认目录（与引擎/调色板同一 schema 源，M5-06 起为
// makeDefaultImageNodeCatalog 单一事实源）。
struct ParamGraphFixture {
    rin::NodeCatalog catalog;
    viewer::CanvasGraphModel model;

    ParamGraphFixture()
        : catalog{rin::workflow_catalog::makeDefaultImageNodeCatalog()},
          model{&catalog} {}
};

void testCanvasParamSurface() {
    // 基础链：source(1) -> grayify(2) -> gaussian_blur(3)，结构校验通过。
    ParamGraphFixture fx;
    RIN_CHECK(fx.model.createNode("source", {0.0f, 0.0f}).ok);
    RIN_CHECK(fx.model.createNode("grayify", {300.0f, 0.0f}).ok);
    RIN_CHECK(fx.model.createNode("gaussian_blur", {600.0f, 0.0f}).ok);
    RIN_CHECK(fx.model.connect({1, rin::PortDirection::Output, 0},
                               {2, rin::PortDirection::Input, 0})
                  .ok);
    RIN_CHECK(fx.model.connect({2, rin::PortDirection::Output, 0},
                               {3, rin::PortDirection::Input, 0})
                  .ok);
    RIN_CHECK_MSG(fx.model.validation.ok, "fixture: connected chain validates clean");

    // setParam 追加 + toGraph 携带。
    {
        const viewer::CanvasOpResult r =
            fx.model.setParam(3, {"radius", rin::ParamValue{static_cast<std::int64_t>(5)}});
        RIN_CHECK_MSG(r.ok, "setParam: legal assignment accepted");
        const viewer::CanvasNode* node = fx.model.findNode(3);
        RIN_CHECK(node != nullptr);
        RIN_CHECK_MSG(node != nullptr && node->params.size() == 1,
                      "setParam: first assignment appended");
        const rin::WorkflowGraph graph = fx.model.toGraph();
        RIN_CHECK_MSG(graph.nodes.size() == 3 && graph.nodes[2].params.size() == 1,
                      "toGraph: carries node params");
        const rin::ParamAssignment expected{"radius",
                                            rin::ParamValue{static_cast<std::int64_t>(5)}};
        RIN_CHECK_MSG(graph.nodes[2].params.size() == 1 && graph.nodes[2].params[0] == expected,
                      "toGraph: assignment value round-trips");
    }

    // 同 paramId 替换（不重复）。
    {
        const viewer::CanvasOpResult r =
            fx.model.setParam(3, {"radius", rin::ParamValue{static_cast<std::int64_t>(7)}});
        RIN_CHECK_MSG(r.ok, "setParam: replacement accepted");
        const viewer::CanvasNode* node = fx.model.findNode(3);
        RIN_CHECK_MSG(node != nullptr && node->params.size() == 1,
                      "setParam: same paramId replaced, not duplicated");
        RIN_CHECK_MSG(node != nullptr &&
                          std::get<std::int64_t>(node->params[0].value) == 7,
                      "setParam: replacement stores the new value");
    }

    // 第二个参数追加。
    {
        const viewer::CanvasOpResult r = fx.model.setParam(3, {"sigma", rin::ParamValue{2.5}});
        RIN_CHECK(r.ok);
        const viewer::CanvasNode* node = fx.model.findNode(3);
        RIN_CHECK_MSG(node != nullptr && node->params.size() == 2,
                      "setParam: distinct paramId appends");
    }

    // 未知节点拒绝。
    {
        const viewer::CanvasOpResult r =
            fx.model.setParam(999, {"radius", rin::ParamValue{static_cast<std::int64_t>(1)}});
        RIN_CHECK_MSG(!r.ok, "setParam: unknown node rejected");
        RIN_CHECK_MSG(r.error == "unknown node", "setParam: rejection names the cause");
    }

    // 越界赋值：存储层接受（面板负责提交前校验），但图校验标注 BadParam。
    {
        const viewer::CanvasOpResult r =
            fx.model.setParam(3, {"radius", rin::ParamValue{static_cast<std::int64_t>(99)}});
        RIN_CHECK_MSG(r.ok, "setParam: storage accepts what the panel submits");
        RIN_CHECK_MSG(!fx.model.validation.ok,
                      "validation: out-of-range assignment marks the graph invalid");
        RIN_CHECK_MSG(hasIssueOn(fx.model, rin::ValidationIssueKind::BadParam, 3),
                      "validation: BadParam attached to the assigned node");
        RIN_CHECK_MSG(fx.model.validation.issues.size() == 1,
                      "validation: no unrelated issues alongside BadParam");
        RIN_CHECK(fx.model.nodeHasIssue(3));
        RIN_CHECK_MSG(!fx.model.nodeHasIssue(1) && !fx.model.nodeHasIssue(2),
                      "validation: upstream nodes stay clean");
    }

    // 恢复合法赋值：校验重新通过。
    const viewer::CanvasOpResult restored =
        fx.model.setParam(3, {"radius", rin::ParamValue{static_cast<std::int64_t>(5)}});
    RIN_CHECK(restored.ok);
    RIN_CHECK_MSG(fx.model.validation.ok && fx.model.validation.issues.empty(),
                  "validation: legal assignment restores a clean graph");

    // 未知 paramId 的赋值同样经校验标注（模型只存储与标注）。
    {
        ParamGraphFixture fx2;
        RIN_CHECK(fx2.model.createNode("gaussian_blur", {0.0f, 0.0f}).ok);
        const viewer::CanvasOpResult r =
            fx2.model.setParam(1, {"nope", rin::ParamValue{static_cast<std::int64_t>(1)}});
        RIN_CHECK_MSG(r.ok, "setParam: unknown paramId still stored (caller validates)");
        RIN_CHECK_MSG(hasIssueOn(fx2.model, rin::ValidationIssueKind::BadParam, 1),
                      "validation: unknown paramId flagged as BadParam");
    }
}

void testCanvasParamWithConnectAndDelete() {
    // connect 预检图构造携带 params：带越界参数时连线仍成功（BadParam
    // 非阻塞），且连线后参数不被图重建清掉。
    {
        ParamGraphFixture fx;
        RIN_CHECK(fx.model.createNode("source", {0.0f, 0.0f}).ok);
        RIN_CHECK(fx.model.createNode("grayify", {300.0f, 0.0f}).ok);
        RIN_CHECK(fx.model.createNode("gaussian_blur", {600.0f, 0.0f}).ok);
        RIN_CHECK(fx.model.setParam(3, {"radius", rin::ParamValue{static_cast<std::int64_t>(99)}})
                       .ok);
        RIN_CHECK_MSG(!fx.model.validation.ok, "fixture: bad param pre-annotated");
        const viewer::CanvasOpResult c1 = fx.model.connect(
            {1, rin::PortDirection::Output, 0}, {2, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(c1.ok, "connect: BadParam is non-blocking (precheck carries params)");
        const viewer::CanvasOpResult c2 = fx.model.connect(
            {2, rin::PortDirection::Output, 0}, {3, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(c2.ok, "connect: chain completes with the param annotated");
        RIN_CHECK_MSG(hasIssueOn(fx.model, rin::ValidationIssueKind::BadParam, 3),
                      "connect: BadParam annotation survives the connect revalidation");
        const rin::WorkflowGraph graph = fx.model.toGraph();
        bool paramsRetained = false;
        for (const rin::NodeInstance& node : graph.nodes) {
            if (node.id == 3 && node.params.size() == 1 &&
                node.params[0].paramId == "radius") {
                paramsRetained = true;
            }
        }
        RIN_CHECK_MSG(paramsRetained,
                      "connect: node params survive graph rebuild (toGraph still carries)");
    }

    // 删除节点：params 随节点消失。
    {
        ParamGraphFixture fx;
        RIN_CHECK(fx.model.createNode("source", {0.0f, 0.0f}).ok);
        RIN_CHECK(fx.model.createNode("grayify", {300.0f, 0.0f}).ok);
        RIN_CHECK(fx.model.createNode("gaussian_blur", {600.0f, 0.0f}).ok);
        RIN_CHECK(fx.model.connect({1, rin::PortDirection::Output, 0},
                                   {2, rin::PortDirection::Input, 0})
                       .ok);
        RIN_CHECK(fx.model.connect({2, rin::PortDirection::Output, 0},
                                   {3, rin::PortDirection::Input, 0})
                       .ok);
        RIN_CHECK(fx.model.setParam(3, {"radius", rin::ParamValue{static_cast<std::int64_t>(4)}})
                       .ok);
        const rin::WorkflowGraph before = fx.model.toGraph();
        bool present = false;
        for (const rin::NodeInstance& node : before.nodes) {
            present = present || (node.id == 3 && node.params.size() == 1);
        }
        RIN_CHECK_MSG(present, "delete: params present before deletion");
        const viewer::CanvasOpResult r = fx.model.deleteNodes({3});
        RIN_CHECK_MSG(r.ok, "delete: node removed");
        RIN_CHECK(fx.model.findNode(3) == nullptr);
        const rin::WorkflowGraph after = fx.model.toGraph();
        bool gone = true;
        for (const rin::NodeInstance& node : after.nodes) {
            gone = gone && node.id != 3;
        }
        RIN_CHECK_MSG(gone && after.nodes.size() == 2,
                      "delete: assigned params vanish with the node");
    }
}

}  // namespace

// --- 11. 裁切 ROI 联动约束（M6-06，DEC-017 控件层防呆；param_model.hpp 纯逻辑） ---

namespace {

void testRoiConstraint() {
    // 目录版 crop schema（与引擎/面板同源）：x/y [0,4096] 默认 0；
    // width/height [0,4096] 默认 64。
    const rin::NodeCatalog catalog = rin::workflow_catalog::makeDefaultImageNodeCatalog();
    const rin::NodeDescriptor* cropDesc = rin::findNodeDescriptor(catalog, "crop");
    RIN_CHECK(cropDesc != nullptr);
    if (cropDesc == nullptr) {
        return;
    }
    const rin::ParamDescriptor* x = catalogParam(catalog, "crop", "x");
    const rin::ParamDescriptor* y = catalogParam(catalog, "crop", "y");
    const rin::ParamDescriptor* width = catalogParam(catalog, "crop", "width");
    const rin::ParamDescriptor* height = catalogParam(catalog, "crop", "height");
    RIN_CHECK(x != nullptr && y != nullptr && width != nullptr && height != nullptr);
    if (x == nullptr || y == nullptr || width == nullptr || height == nullptr) {
        return;
    }
    const viewer::RoiConstraint roi = viewer::RoiConstraint::forInput(848, 480);
    const std::vector<rin::ParamAssignment> none;

    // isRoiParam：x/y/width/height 且带范围 Integer；去范围/换种类/换 id 均否。
    RIN_CHECK(viewer::RoiConstraint::isRoiParam(*x) &&
              viewer::RoiConstraint::isRoiParam(*y) &&
              viewer::RoiConstraint::isRoiParam(*width) &&
              viewer::RoiConstraint::isRoiParam(*height));
    {
        rin::ParamDescriptor noRange = *x;
        noRange.hasRange = false;
        RIN_CHECK_MSG(!viewer::RoiConstraint::isRoiParam(noRange),
                      "isRoiParam: Integer without range rejected");
        rin::ParamDescriptor realX = *x;
        realX.kind = rin::ParamKind::Real;
        RIN_CHECK_MSG(!viewer::RoiConstraint::isRoiParam(realX),
                      "isRoiParam: Real kind rejected");
        rin::ParamDescriptor other = *x;
        other.id = "u";
        RIN_CHECK_MSG(!viewer::RoiConstraint::isRoiParam(other),
                      "isRoiParam: unrelated id rejected");
    }

    // effectiveRange 互约束（848x480，兄弟取默认 64/0）：
    // x∈[0,784]、y∈[0,416]、width∈[1,848]、height∈[1,480]。
    {
        const auto xr = roi.effectiveRange(*cropDesc, none, *x);
        RIN_CHECK(xr.has_value() && nearD(xr->first, 0.0) && nearD(xr->second, 784.0));
        const auto yr = roi.effectiveRange(*cropDesc, none, *y);
        RIN_CHECK(yr.has_value() && nearD(yr->first, 0.0) && nearD(yr->second, 416.0));
        const auto wr = roi.effectiveRange(*cropDesc, none, *width);
        RIN_CHECK(wr.has_value() && nearD(wr->first, 1.0) && nearD(wr->second, 848.0));
        const auto hr = roi.effectiveRange(*cropDesc, none, *height);
        RIN_CHECK(hr.has_value() && nearD(hr->first, 1.0) && nearD(hr->second, 480.0));
    }

    // 兄弟参数取生效值：赋值优先于声明默认（width=64 显式与默认同域；x=100 →
    // width∈[1,748]；y=100 → height∈[1,380]；width=100 → x∈[0,748]）。
    {
        const std::vector<rin::ParamAssignment> widthDefault = {
            {"width", rin::ParamValue{static_cast<std::int64_t>(64)}}};
        const auto xr = roi.effectiveRange(*cropDesc, widthDefault, *x);
        RIN_CHECK(xr.has_value() && nearD(xr->first, 0.0) && nearD(xr->second, 784.0));

        const std::vector<rin::ParamAssignment> xAssigned = {
            {"x", rin::ParamValue{static_cast<std::int64_t>(100)}}};
        const auto wr = roi.effectiveRange(*cropDesc, xAssigned, *width);
        RIN_CHECK_MSG(wr.has_value() && nearD(wr->first, 1.0) && nearD(wr->second, 748.0),
                      "effectiveRange: x=100 narrows width to [1,748]");

        const std::vector<rin::ParamAssignment> yAssigned = {
            {"y", rin::ParamValue{static_cast<std::int64_t>(100)}}};
        const auto hr = roi.effectiveRange(*cropDesc, yAssigned, *height);
        RIN_CHECK(hr.has_value() && nearD(hr->first, 1.0) && nearD(hr->second, 380.0));

        const std::vector<rin::ParamAssignment> widthAssigned = {
            {"width", rin::ParamValue{static_cast<std::int64_t>(100)}}};
        const auto xr2 = roi.effectiveRange(*cropDesc, widthAssigned, *x);
        RIN_CHECK_MSG(xr2.has_value() && nearD(xr2->second, 748.0),
                      "effectiveRange: sibling assignment beats the declared default");
    }

    // 与声明 range 求交：x 声明 [700,4096] → 有效域 [700,784]。
    {
        rin::NodeDescriptor narrow = *cropDesc;
        narrow.params.clear();
        rin::ParamDescriptor nx = *x;
        nx.minValue = 700.0;
        nx.maxValue = 4096.0;
        narrow.params = {nx, *y, *width, *height};
        const auto r = roi.effectiveRange(narrow, none, narrow.params[0]);
        RIN_CHECK_MSG(r.has_value() && nearD(r->first, 700.0) && nearD(r->second, 784.0),
                      "effectiveRange: intersects the declared range");
    }

    // 退化声明夹成单点：x 声明 [800,900] 与互约束上界 784 相交为空 → 夹成
    // [784,784]（区间保持合法，M4-03 兜底不再可达）。
    {
        rin::NodeDescriptor degenerate = *cropDesc;
        degenerate.params.clear();
        rin::ParamDescriptor dx = *x;
        dx.minValue = 800.0;
        dx.maxValue = 900.0;
        degenerate.params = {dx, *y, *width, *height};
        const auto r = roi.effectiveRange(degenerate, none, degenerate.params[0]);
        RIN_CHECK_MSG(r.has_value() && nearD(r->first, 784.0) && nearD(r->second, 784.0),
                      "effectiveRange: empty intersection clamps to a single point");
    }

    // 退化输入夹单点：2x2 输入、默认 64x64 ROI → x/y∈[0,0]、width/height∈[1,2]。
    {
        const viewer::RoiConstraint tiny = viewer::RoiConstraint::forInput(2, 2);
        const auto xr = tiny.effectiveRange(*cropDesc, none, *x);
        RIN_CHECK(xr.has_value() && nearD(xr->first, 0.0) && nearD(xr->second, 0.0));
        const auto wr = tiny.effectiveRange(*cropDesc, none, *width);
        RIN_CHECK(wr.has_value() && nearD(wr->first, 1.0) && nearD(wr->second, 2.0));
    }

    // 非法形态回退：无输入尺寸（unconstrained）与非 ROI 参数 → nullopt。
    {
        const viewer::RoiConstraint free = viewer::RoiConstraint::unconstrained();
        RIN_CHECK(!free.effectiveRange(*cropDesc, none, *x).has_value());
        const rin::ParamDescriptor* radius = catalogParam(catalog, "gaussian_blur", "radius");
        RIN_CHECK(radius != nullptr);
        if (radius != nullptr) {
            RIN_CHECK_MSG(!roi.effectiveRange(*cropDesc, none, *radius).has_value(),
                          "effectiveRange: non-ROI param yields nullopt");
        }
        rin::ParamDescriptor realX = *x;
        realX.kind = rin::ParamKind::Real;
        RIN_CHECK(!roi.effectiveRange(*cropDesc, none, realX).has_value());
    }

    // clampValue：Integer 闭区间夹取；取整用 llround（声明端点可带小数）；
    // 非 ROI 参数/非 Integer 值/无输入尺寸原样返回。
    {
        rin::ParamValue v =
            roi.clampValue(*cropDesc, none, *x, rin::ParamValue{static_cast<std::int64_t>(1000)});
        RIN_CHECK_EQ(std::get<std::int64_t>(v), std::int64_t{784});
        v = roi.clampValue(*cropDesc, none, *x, rin::ParamValue{static_cast<std::int64_t>(-5)});
        RIN_CHECK_EQ(std::get<std::int64_t>(v), std::int64_t{0});
        v = roi.clampValue(*cropDesc, none, *x, rin::ParamValue{static_cast<std::int64_t>(100)});
        RIN_CHECK_EQ(std::get<std::int64_t>(v), std::int64_t{100});

        const std::vector<rin::ParamAssignment> xAssigned = {
            {"x", rin::ParamValue{static_cast<std::int64_t>(100)}}};
        v = roi.clampValue(*cropDesc, xAssigned, *width,
                           rin::ParamValue{static_cast<std::int64_t>(2000)});
        RIN_CHECK_EQ(std::get<std::int64_t>(v), std::int64_t{748});
        v = roi.clampValue(*cropDesc, xAssigned, *width,
                           rin::ParamValue{static_cast<std::int64_t>(0)});
        RIN_CHECK_EQ(std::get<std::int64_t>(v), std::int64_t{1});

        // 声明端点带小数：x ∈ [0.4, 783.6] → llround 端点 [0, 784]。
        rin::NodeDescriptor frac = *cropDesc;
        frac.params.clear();
        rin::ParamDescriptor fx = *x;
        fx.minValue = 0.4;
        fx.maxValue = 783.6;
        frac.params = {fx, *y, *width, *height};
        const auto fr = roi.effectiveRange(frac, none, frac.params[0]);
        RIN_CHECK(fr.has_value() && nearD(fr->first, 0.4) && nearD(fr->second, 783.6));
        v = roi.clampValue(frac, none, frac.params[0],
                           rin::ParamValue{static_cast<std::int64_t>(5000)});
        RIN_CHECK_EQ(std::get<std::int64_t>(v), std::int64_t{784});
        v = roi.clampValue(frac, none, frac.params[0],
                           rin::ParamValue{static_cast<std::int64_t>(-10)});
        RIN_CHECK_EQ(std::get<std::int64_t>(v), std::int64_t{0});

        // 非 Integer 值原样返回（种类收窄由校验层负责）。
        rin::ParamValue real = roi.clampValue(*cropDesc, none, *x, rin::ParamValue{3.5});
        RIN_CHECK(std::holds_alternative<double>(real) &&
                  std::get<double>(real) == 3.5);
        // 非 ROI 参数原样返回。
        const rin::ParamDescriptor* radius = catalogParam(catalog, "gaussian_blur", "radius");
        RIN_CHECK(radius != nullptr);
        if (radius != nullptr) {
            v = roi.clampValue(*cropDesc, none, *radius,
                               rin::ParamValue{static_cast<std::int64_t>(99)});
            RIN_CHECK_EQ(std::get<std::int64_t>(v), std::int64_t{99});
        }
        // 无输入尺寸原样返回。
        const viewer::RoiConstraint free = viewer::RoiConstraint::unconstrained();
        v = free.clampValue(*cropDesc, none, *x,
                            rin::ParamValue{static_cast<std::int64_t>(99999)});
        RIN_CHECK_EQ(std::get<std::int64_t>(v), std::int64_t{99999});
    }

    // 有效域滑条映射：端点/夹取/零跨度；Integer 取整（round half away from zero）。
    RIN_CHECK(nearD(viewer::valueToSliderInRange(0.0, 784.0, 392.0), 0.5));
    RIN_CHECK(nearD(viewer::valueToSliderInRange(0.0, 784.0, 784.0), 1.0));
    RIN_CHECK(nearD(viewer::valueToSliderInRange(0.0, 784.0, -10.0), 0.0));
    RIN_CHECK(nearD(viewer::valueToSliderInRange(0.0, 784.0, 1000.0), 1.0));
    RIN_CHECK(nearD(viewer::valueToSliderInRange(5.0, 5.0, 5.0), 0.0));
    RIN_CHECK(nearD(viewer::sliderToValueInRange(0.0, 784.0, 0.5, true), 392.0));
    RIN_CHECK(nearD(viewer::sliderToValueInRange(0.0, 784.0, 1.5, true), 784.0));
    RIN_CHECK(nearD(viewer::sliderToValueInRange(0.0, 784.0, -0.5, true), 0.0));
    RIN_CHECK(nearD(viewer::sliderToValueInRange(0.0, 5.0, 0.5, true), 3.0));
    RIN_CHECK(nearD(viewer::sliderToValueInRange(0.0, 5.0, 0.5, false), 2.5));

    // 标量取值防御：Integer/Real 直取；字符串/数组按 0。
    RIN_CHECK(nearD(viewer::paramScalarAsDouble(rin::ParamValue{static_cast<std::int64_t>(42)}),
                    42.0));
    RIN_CHECK(nearD(viewer::paramScalarAsDouble(rin::ParamValue{0.25}), 0.25));
    RIN_CHECK(nearD(viewer::paramScalarAsDouble(rin::ParamValue{std::string("x")}), 0.0));
    RIN_CHECK(nearD(viewer::paramScalarAsDouble(rin::ParamValue{std::vector<double>{1.0}}),
                    0.0));
}

}  // namespace

int main() {
    runSection("catalog_sanity", [] {
        const rin::NodeCatalog catalog =
            rin::workflow_catalog::makeDefaultImageNodeCatalog();
        RIN_CHECK(catalog.valid());
        // 本测试依赖的目录描述符存在且形态符合预期（radius 1..10、scale 0.1..1）。
        const rin::ParamDescriptor* radius =
            catalogParam(catalog, "gaussian_blur", "radius");
        RIN_CHECK(radius != nullptr);
        if (radius != nullptr) {
            RIN_CHECK(radius->kind == rin::ParamKind::Integer);
            RIN_CHECK(radius->hasRange && radius->minValue == 1.0 && radius->maxValue == 10.0);
        }
        const rin::ParamDescriptor* scale = catalogParam(catalog, "downscale", "scale");
        RIN_CHECK(scale != nullptr);
        if (scale != nullptr) {
            RIN_CHECK(scale->kind == rin::ParamKind::Real);
            RIN_CHECK(scale->hasRange && scale->minValue == 0.1 && scale->maxValue == 1.0);
        }
        const rin::ParamDescriptor* interpolation =
            catalogParam(catalog, "downscale", "interpolation");
        RIN_CHECK(interpolation != nullptr);
        if (interpolation != nullptr) {
            RIN_CHECK(interpolation->kind == rin::ParamKind::Enumeration);
            RIN_CHECK(interpolation->enumOptions.size() == 2);
        }
    });
    runSection("strict_text_parsing", testStrictTextParsing);
    runSection("param_validation", testParamValidation);
    runSection("assignment_wrapping", testAssignmentWrapping);
    runSection("slider_mapping", testSliderMapping);
    runSection("effective_value_and_text", testEffectiveValueAndText);
    runSection("real_array_grid", testRealArrayGrid);
    runSection("cycle_enum_and_monitor_cap", testCycleEnumOptionAndMonitorPreviewCap);
    runSection("node_output_cache", testNodeOutputCache);
    runSection("thumbnail_rgba", testThumbnailRgba);
    runSection("node_failure_marks", testNodeFailureMarks);
    runSection("canvas_param_surface", testCanvasParamSurface);
    runSection("canvas_param_connect_delete", testCanvasParamWithConnectAndDelete);
    runSection("roi_constraint", testRoiConstraint);
    return rin_test::exitStatus();
}
