#pragma once

// 参数面板与中间结果查看的平台无关纯逻辑层（M5-04，ui_workspace_design.md
// §5.5/§5.6/§7）。职责边界（DEC-015 决策 2：ParamDescriptor 到控件状态的绑定
// 下沉纯逻辑，独立单测）：
// - 控件-参数绑定：严格文本解析、值对声明的校验（种类/范围闭区间/枚举选项/
//   RealArray 有限性）、滑条归一化映射、生效值解析（未赋值取声明默认）；
// - RealArray 矩阵网格：行主序数据的行列视图与重排尺寸语义（§5.5 矩阵编辑器
//   的数据模型）；
// - 节点中间产物有界缓存：每节点仅最新一幅，容量上限驱逐，停止/关闭排空
//   （§4"停止/关闭排空"）；
// - 缩略图降采样：快照 → 有界 RGBA（提交侧有界工作，RULE-05），Gray8/Rgba8
//   按格式渲染；
// - 节点执行失败标注：NodeFailed 事件的会话级标注（§4 destructive 徽标）。
//
// 本头文件不包含 EUI-NEO 类型（纯逻辑单测对象，tests/test_param_panel.cpp）；
// EUI 组装见 param_panel.hpp，画布会话状态见 node_canvas.hpp。

#include <rin/camera_types.hpp>
#include <rin/workflow_engine.hpp>
#include <rin/workflow_types.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

// --- 控件-参数绑定（§5.5 / §7） ---

/// 一次参数编辑的校验与提交载体：ok=false 时 error 携带就地报错文案（§5.5
/// "同步拒绝……就地报错"），assignment 仅在 ok=true 时有效。
struct ParamEditResult {
    bool ok = false;
    std::string error;
    rin::ParamAssignment assignment;
};

/// 实数 → 控件初值文本（统一 %.6g：整数不带小数尾、短小数完整可读）。
[[nodiscard]] inline std::string formatRealText(const double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

/// 严格十进制整数文本解析：拒绝空串、前后空白、非法字符与溢出。
[[nodiscard]] inline std::optional<std::int64_t> parseIntegerText(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto [end, ec] = std::from_chars(first, last, value, 10);
    if (ec != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

/// 严格实数文本解析（十进制/科学计数，接受一个前导 '+'）：拒绝空串、尾随字符
/// 与非有限值（契约要求有限值，"inf"/"nan" 文本一并拒绝）。
[[nodiscard]] inline std::optional<double> parseRealText(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::string_view view = text;
    if (view.front() == '+') {
        view.remove_prefix(1);
    }
    if (view.empty()) {
        return std::nullopt;
    }
    double value = 0.0;
    const auto [end, ec] = std::from_chars(view.data(), view.data() + view.size(), value);
    if (ec != std::errc{} || end != view.data() + view.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

/// 范围呈现文本（§5.5"hasRange 时夹取并显示区间"）：无范围返回空串。
[[nodiscard]] inline std::string paramRangeText(const rin::ParamDescriptor& descriptor) {
    if (!descriptor.hasRange) {
        return {};
    }
    return formatRealText(descriptor.minValue) + ".." + formatRealText(descriptor.maxValue);
}

/// 值对参数声明的校验（§5.5/§7）：种类匹配、hasRange 闭区间、枚举在选项内、
/// RealArray 全部有限。通过返回空串，否则返回就地报错文案。
[[nodiscard]] inline std::string paramValidationError(const rin::ParamDescriptor& descriptor,
                                                      const rin::ParamValue& value) {
    if (rin::paramKindOf(value) != descriptor.kind) {
        return "value type mismatches declaration";
    }
    switch (descriptor.kind) {
        case rin::ParamKind::Boolean:
            return {};
        case rin::ParamKind::Integer: {
            const std::int64_t number = std::get<std::int64_t>(value);
            if (descriptor.hasRange &&
                (static_cast<double>(number) < descriptor.minValue ||
                 static_cast<double>(number) > descriptor.maxValue)) {
                return "value out of range (" + paramRangeText(descriptor) + ")";
            }
            return {};
        }
        case rin::ParamKind::Real: {
            const double number = std::get<double>(value);
            if (!std::isfinite(number)) {
                return "value must be finite";
            }
            if (descriptor.hasRange &&
                (number < descriptor.minValue || number > descriptor.maxValue)) {
                return "value out of range (" + paramRangeText(descriptor) + ")";
            }
            return {};
        }
        case rin::ParamKind::Enumeration: {
            const std::string& option = std::get<std::string>(value);
            if (std::find(descriptor.enumOptions.begin(), descriptor.enumOptions.end(),
                          option) == descriptor.enumOptions.end()) {
                return "value is not a declared option";
            }
            return {};
        }
        case rin::ParamKind::RealArray: {
            const std::vector<double>& array = std::get<std::vector<double>>(value);
            for (const double element : array) {
                if (!std::isfinite(element)) {
                    return "values must be finite";
                }
            }
            return {};
        }
    }
    return "unknown parameter kind";
}

/// 校验并包装为可提交赋值（面板提交的公共出口）。
[[nodiscard]] inline ParamEditResult makeParamAssignment(const rin::ParamDescriptor& descriptor,
                                                         rin::ParamValue value) {
    ParamEditResult result;
    result.error = paramValidationError(descriptor, value);
    result.ok = result.error.empty();
    if (result.ok) {
        result.assignment = {descriptor.id, std::move(value)};
    }
    return result;
}

/// 文本 → Integer/Real/Enumeration 赋值（文本输入与下拉的公共入口）；解析失败
/// 或校验拒绝时 error 非空。Boolean/RealArray 不经文本标量入口。
[[nodiscard]] inline ParamEditResult paramAssignmentFromText(
    const rin::ParamDescriptor& descriptor, const std::string& text) {
    switch (descriptor.kind) {
        case rin::ParamKind::Integer: {
            const std::optional<std::int64_t> parsed = parseIntegerText(text);
            if (!parsed) {
                return {false, "not an integer", {}};
            }
            return makeParamAssignment(descriptor, *parsed);
        }
        case rin::ParamKind::Real: {
            const std::optional<double> parsed = parseRealText(text);
            if (!parsed) {
                return {false, "not a number", {}};
            }
            return makeParamAssignment(descriptor, *parsed);
        }
        case rin::ParamKind::Enumeration:
            return makeParamAssignment(descriptor, text);
        case rin::ParamKind::Boolean:
        case rin::ParamKind::RealArray:
            return {false, "parameter is not text-editable", {}};
    }
    return {false, "unknown parameter kind", {}};
}

/// 滑条归一化 [0,1] → 声明值域（§5.5 仅 hasRange 参数使用滑条）：线性映射后
/// Integer 就近取整；t 非有限或声明无范围返回 nullopt。
[[nodiscard]] inline std::optional<double> sliderToValue(const rin::ParamDescriptor& descriptor,
                                                         double t) {
    if (!descriptor.hasRange || !std::isfinite(t)) {
        return std::nullopt;
    }
    t = std::clamp(t, 0.0, 1.0);
    double value = descriptor.minValue + t * (descriptor.maxValue - descriptor.minValue);
    if (descriptor.kind == rin::ParamKind::Integer) {
        value = std::round(value);
    }
    return value;
}

/// 声明值 → 滑条归一化（Integer/Real 且 hasRange；否则 nullopt）：闭区间外值
/// 夹取到 [0,1]（§5.5"夹取"）。
[[nodiscard]] inline std::optional<double> valueToSlider(
    const rin::ParamDescriptor& descriptor, const rin::ParamValue& value) {
    if (!descriptor.hasRange) {
        return std::nullopt;
    }
    const double* number = nullptr;
    double converted = 0.0;
    if (const auto* integer = std::get_if<std::int64_t>(&value);
        integer != nullptr && descriptor.kind == rin::ParamKind::Integer) {
        converted = static_cast<double>(*integer);
        number = &converted;
    } else if (const auto* real = std::get_if<double>(&value);
               real != nullptr && descriptor.kind == rin::ParamKind::Real) {
        number = real;
    }
    if (number == nullptr) {
        return std::nullopt;
    }
    const double span = descriptor.maxValue - descriptor.minValue;
    if (span <= 0.0) {
        return 0.0;
    }
    return std::clamp((*number - descriptor.minValue) / span, 0.0, 1.0);
}

/// 参数生效值（§5.5"未赋值参数显示声明默认值"）：赋值存在时指向赋值，否则指向
/// 声明默认值；paramId 不在声明内返回 nullptr。指针寿命由持有方模型保证。
[[nodiscard]] inline const rin::ParamValue* effectiveParamValue(
    const rin::NodeDescriptor& descriptor, const std::vector<rin::ParamAssignment>& params,
    const std::string& paramId) {
    for (const rin::ParamAssignment& assignment : params) {
        if (assignment.paramId == paramId) {
            return &assignment.value;
        }
    }
    for (const rin::ParamDescriptor& candidate : descriptor.params) {
        if (candidate.id == paramId) {
            return &candidate.defaultValue;
        }
    }
    return nullptr;
}

/// 生效值 → 控件初值文本（面板重建绑定时用）：Boolean "on"/"off"，Enumeration
/// 原文，Integer/Real 统一 %.6g，RealArray 不适用（矩阵单元单独取值）。
[[nodiscard]] inline std::string paramValueText(const rin::ParamValue& value) {
    switch (rin::paramKindOf(value)) {
        case rin::ParamKind::Boolean:
            return std::get<bool>(value) ? "on" : "off";
        case rin::ParamKind::Integer:
            return std::to_string(std::get<std::int64_t>(value));
        case rin::ParamKind::Real:
            return formatRealText(std::get<double>(value));
        case rin::ParamKind::Enumeration:
            return std::get<std::string>(value);
        case rin::ParamKind::RealArray:
            return {};
    }
    return {};
}

// --- RealArray 矩阵网格（§5.5 自研矩阵网格编辑器的数据模型） ---

/// 行主序实数数组的行列视图：重排尺寸保留原数据（行主序截断/零扩展）。
/// 行列数由 UI 侧步进控制（"行列数可配"），形状不入契约。
struct RealArrayGrid {
    std::size_t rows = 1;
    std::size_t cols = 1;
    std::vector<double> values{0.0};

    /// 扁平数据 → 网格：完全平方数取 KxK（卷积核主用例），否则 1xN；空数据
    /// 取 1x1 零值。
    [[nodiscard]] static RealArrayGrid fromFlat(std::vector<double> flat) {
        RealArrayGrid grid;
        if (flat.empty()) {
            return grid;
        }
        const std::size_t n = flat.size();
        const std::size_t side = static_cast<std::size_t>(std::llround(std::sqrt(
            static_cast<long double>(n))));
        if (side * side == n && side > 0) {
            grid.rows = side;
            grid.cols = side;
        } else {
            grid.rows = 1;
            grid.cols = n;
        }
        grid.values = std::move(flat);
        return grid;
    }

    /// 重排尺寸：保留行主序原数据，截断多余尾值 / 以 0 扩展；rows/cols 下限 1。
    void reshape(const std::size_t newRows, const std::size_t newCols) {
        rows = std::max<std::size_t>(1, newRows);
        cols = std::max<std::size_t>(1, newCols);
        values.resize(rows * cols, 0.0);
    }

    [[nodiscard]] double at(const std::size_t row, const std::size_t col) const {
        return values[row * cols + col];
    }
};

// --- 节点中间产物有界缓存（§5.6 / §4 停止排空） ---

/// 缓存容量上限（有界预算：最多保留 4 个节点的最新一幅，总幅面有界）。
inline constexpr std::size_t kMaxCachedNodeOutputs = 4;

/// 节点中间产物有界缓存（§5.6"每节点仅最新一幅，UI 侧不做有界缓存之外的
/// 保留"）：容量超限驱逐最久未访问条目；停止/关闭路径 clear() 排空（§4
/// "快照缩略图清空"，stale 快照不得恢复活动状态）。
class NodeOutputCache {
public:
    /// 从引擎拉取 node 的最新产物（sequence > 内部已见序号）；有新产物时刷新
    /// 条目并返回 true。无新产物（未运行/未产出/节点不在生效图）时保持现状。
    bool pull(rin::IWorkflowEngine& engine, const rin::NodeId node) {
        std::uint64_t seen = 0;
        if (const auto it = entries_.find(node); it != entries_.end()) {
            seen = it->second.lastSeenSequence;
        }
        rin::NodeOutputSnapshot snapshot;
        if (!engine.tryLoadNodeOutput(node, seen, snapshot)) {
            return false;
        }
        Entry entry;
        entry.snapshot = std::move(snapshot);
        entry.lastSeenSequence = seen;
        entry.lastUse = ++clock_;
        entries_[node] = std::move(entry);
        evictIfNeeded();
        return true;
    }

    /// 访问节点缓存（查看即使用，影响 LRU 时钟）；无缓存返回 nullptr。
    [[nodiscard]] const rin::NodeOutputSnapshot* find(const rin::NodeId node) const {
        const auto it = entries_.find(node);
        if (it == entries_.end()) {
            return nullptr;
        }
        it->second.lastUse = ++clock_;
        return &it->second.snapshot;
    }

    /// 停止/关闭排空（§4）：清空全部缓存。
    void clear() { entries_.clear(); }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        rin::NodeOutputSnapshot snapshot;
        std::uint64_t lastSeenSequence = 0;
        std::uint64_t lastUse = 0;
    };

    void evictIfNeeded() {
        while (entries_.size() > kMaxCachedNodeOutputs) {
            auto oldest = entries_.begin();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->second.lastUse < oldest->second.lastUse) {
                    oldest = it;
                }
            }
            entries_.erase(oldest);
        }
    }

    mutable std::uint64_t clock_ = 0;
    mutable std::map<rin::NodeId, Entry> entries_;  // find() 触碰 LRU（逻辑常量性）。
};

// --- 缩略图降采样（§5.6，提交侧有界工作 RULE-05） ---

/// 缩略图最长边上限（有界：上传幅面 ≤ 256x256）。
inline constexpr std::uint32_t kThumbnailMaxDim = 256;

/// 快照 → RGBA 缩略图（§5.6"缩略图按格式渲染（Gray8/Rgba8）"）：最近邻降采样、
/// 保持宽高比、只缩不放、最长边 ≤ maxDim。成功时 out 为 stride=width*4 的
/// RGBA8 帧（sequence 透传 sourceSequence 供 UI 显示源帧序号）；快照无效或
/// 像素缺失返回 false。
[[nodiscard]] inline bool thumbnailRgbaFromSnapshot(const rin::NodeOutputSnapshot& snapshot,
                                                    const std::uint32_t maxDim,
                                                    rin::Frame& out) {
    if (!snapshot.valid() || maxDim == 0) {
        return false;
    }
    std::uint32_t thumbW = snapshot.width;
    std::uint32_t thumbH = snapshot.height;
    if (thumbW > maxDim || thumbH > maxDim) {
        const double scale =
            static_cast<double>(maxDim) / static_cast<double>(std::max(thumbW, thumbH));
        thumbW = std::max(1.0, std::floor(static_cast<double>(thumbW) * scale));
        thumbH = std::max(1.0, std::floor(static_cast<double>(thumbH) * scale));
    }
    const std::size_t rowBytes = static_cast<std::size_t>(thumbW) * 4u;
    std::vector<std::uint8_t> rgba(rowBytes * thumbH);
    const std::vector<std::uint8_t>& source = *snapshot.pixels;
    for (std::uint32_t y = 0; y < thumbH; ++y) {
        const std::size_t sourceY = static_cast<std::size_t>(
            std::min<std::uint64_t>(snapshot.height - 1,
                                    static_cast<std::uint64_t>(y) * snapshot.height / thumbH));
        const std::uint8_t* sourceRow = source.data() + static_cast<std::size_t>(sourceY) *
                                                                snapshot.stride;
        std::uint8_t* targetRow = rgba.data() + static_cast<std::size_t>(y) * rowBytes;
        for (std::uint32_t x = 0; x < thumbW; ++x) {
            const std::size_t sourceX = static_cast<std::size_t>(
                std::min<std::uint64_t>(snapshot.width - 1,
                                        static_cast<std::uint64_t>(x) * snapshot.width /
                                            thumbW));
            std::uint8_t* target = targetRow + static_cast<std::size_t>(x) * 4u;
            if (snapshot.format == rin::PortType::Gray8) {
                const std::uint8_t gray = sourceRow[sourceX];
                target[0] = gray;
                target[1] = gray;
                target[2] = gray;
                target[3] = 255;
            } else {
                const std::uint8_t* pixel = sourceRow + sourceX * 4u;
                target[0] = pixel[0];
                target[1] = pixel[1];
                target[2] = pixel[2];
                target[3] = pixel[3];
            }
        }
    }
    out.kind = rin::FrameKind::Rgb;
    out.width = thumbW;
    out.height = thumbH;
    out.stride = static_cast<std::uint32_t>(rowBytes);
    out.sequence = snapshot.sourceSequence;
    out.pixels = std::make_shared<const std::vector<std::uint8_t>>(std::move(rgba));
    return true;
}

// --- 节点执行失败标注（§4 NodeFailed → destructive 徽标） ---

/// 节点执行失败标注：NodeFailed 事件 → 节点 → 最近一次失败消息。会话级：
/// Started（新会话）/Stopped（会话结束）事件清空；引擎 Failed 终态保留标注供
/// 定位（恢复路径 = 重新启动，§4）。事件通道为最新态语义（tryLoadEvent 无序号，
/// 密集事件下可能漏读中间事件——契约允许，漏读只影响标注时效，不影响引擎行为）。
class NodeFailureMarks {
public:
    void applyEvent(const rin::WorkflowEvent& event) {
        switch (event.kind) {
            case rin::WorkflowEventKind::NodeFailed:
                marks_[event.node] = event.message;
                break;
            case rin::WorkflowEventKind::Started:
            case rin::WorkflowEventKind::Stopped:
                marks_.clear();
                break;
            case rin::WorkflowEventKind::GraphApplied:
            case rin::WorkflowEventKind::ParamUpdated:
            case rin::WorkflowEventKind::Info:
            case rin::WorkflowEventKind::Failed:
                break;
        }
    }

    /// 节点失败消息；无标注返回 nullptr。
    [[nodiscard]] const std::string* failureOf(const rin::NodeId node) const {
        const auto it = marks_.find(node);
        return it != marks_.end() ? &it->second : nullptr;
    }

    void clear() { marks_.clear(); }

    [[nodiscard]] std::size_t size() const noexcept { return marks_.size(); }

private:
    std::map<rin::NodeId, std::string> marks_;
};

}  // namespace viewer
