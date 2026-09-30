#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rin/image_types.hpp"

namespace rin {

/// 工作流图像节点契约（M4-02 Core）：M4 算子节点（裁切/降分辨率/卷积/高斯模糊/
/// 直方图均衡/FFT 滤波族）的统一执行接缝。
///
/// 节点是同步 CPU 工作单元（RULE-07），由 M4-07 引擎在 Executor 有限任务内调用
/// （EXEC-07）；公开头零第三方类型（RULE-01）。节点实例在构造期接收已解析参数
/// 并定型：运行期参数热更新（"下一帧生效"，DEC-014）由引擎在帧边界以新参数重建
/// 节点实例实现，apply 路径不感知参数变更。
class IImageNode {
public:
    virtual ~IImageNode() = default;

    /// 节点类型签名（端口面 + 参数 schema）。实现必须返回与其在目录中的声明
    /// 一致的描述（buildNodeGraph 构造时防御性核对 typeId 一致）。
    [[nodiscard]] virtual const NodeDescriptor& descriptor() const noexcept = 0;

    /// 单帧同步执行。inputs 数量与 descriptor().inputs 一致，顺序与入边
    /// （连线语义）一致，图像类型由图连线的端口类型系统保证与声明一致；返回
    /// 数量与 descriptor().outputs 一致、逐幅类型与声明一致（runNodeGraph 做
    /// 防御性核对）。实现必须为逻辑只读（const；可由引擎并发调用或在帧边界
    /// 重建），不得保留 inputs 的引用越过本次调用（需要延续时显式共享像素
    /// 所有权拷贝）。失败以异常报告（引擎捕获 → NodeFailed 事件），不得返回
    /// 无效图像静默失败。
    [[nodiscard]] virtual std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const = 0;
};

/// 有状态节点标记接口（M10，DEC-020 加性修订 DEC-013）：跨帧保持内部状态
/// 的节点（如时序历史堆叠）额外实现本接口。引擎契约：
/// - 编译期探测（buildGeneration），含状态节点的生成代以**串行在飞**执行——
///   上一帧任务未完成时跳过本帧提交（staged 最新帧语义，下一 tick 重试，
///   不计过载丢弃），保证 apply 按帧序调用；
/// - 状态生命周期 = 节点实例生命周期：图替换/参数热更新重建生成代即复位；
/// - 实现仍须满足 IImageNode::apply 的 const/可并发调用签名纪律——串行序
///   由引擎保证，实现内部以 mutable 状态承载（不得另开线程/队列）。
class IStatefulImageNode {
public:
    virtual ~IStatefulImageNode() = default;
};

/// 节点工厂接缝：按目录声明与实例参数构造可执行节点。参数解析经
/// effectiveParamValue/param* 助手（未赋值参数取声明默认值；赋值合法性已由
/// validateWorkflowGraph 在图准入时判定，工厂内只做读取）。
///
/// 返回 nullptr 表示注入型源节点（typeId 无节点实现；M4-07 源节点语义——执行时
/// 由引擎经注入器提供相机帧，见 node_graph.hpp 的 SourceInjector）。构造失败
/// （参数缺陷、内部不变量破坏等）抛异常，由 buildNodeGraph 转为显式校验问题。
using ImageNodeFactory =
    std::function<std::unique_ptr<IImageNode>(const NodeDescriptor&, const NodeInstance&)>;

/// 生效参数值（类型化参数模型的读取基元）：实例已赋值取该值（同 paramId 多条
/// 赋值时取最后一条——图校验拒绝重复赋值，此为防御语义），未赋值取声明默认值；
/// paramId 不在声明内返回 nullptr。线性查找（节点参数量小，非热路径）。
[[nodiscard]] const ParamValue* effectiveParamValue(const NodeDescriptor& descriptor,
                                                    const NodeInstance& instance,
                                                    const std::string& paramId);

/// 类型化参数读取：按声明 kind 匹配后取值拷贝；paramId 未声明或值种类与声明
/// kind 不符返回空（图校验已保证种类一致，此处防御运行期直接构造的实例）。
[[nodiscard]] std::optional<bool> paramBoolean(const NodeDescriptor& descriptor,
                                               const NodeInstance& instance,
                                               const std::string& paramId);
[[nodiscard]] std::optional<std::int64_t> paramInteger(const NodeDescriptor& descriptor,
                                                       const NodeInstance& instance,
                                                       const std::string& paramId);
[[nodiscard]] std::optional<double> paramReal(const NodeDescriptor& descriptor,
                                              const NodeInstance& instance,
                                              const std::string& paramId);
/// Enumeration 选项 id（值承载为 string）。
[[nodiscard]] std::optional<std::string> paramEnumeration(const NodeDescriptor& descriptor,
                                                          const NodeInstance& instance,
                                                          const std::string& paramId);
/// 实数数组（行主序；如卷积核系数）。
[[nodiscard]] std::optional<std::vector<double>> paramRealArray(const NodeDescriptor& descriptor,
                                                                const NodeInstance& instance,
                                                                const std::string& paramId);

}  // namespace rin
