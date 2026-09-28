#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "rin/image_node.hpp"

namespace rin {

/// 单图节点准入上限默认值（与 M5-08 假引擎 applyGraph 准入同默认；构建时可配）。
inline constexpr std::size_t kDefaultMaxGraphNodes = 64;

struct NodeGraphBuild;

/// 已编译可执行工作流图（M4-02 Core）：WorkflowGraph 经目录与工厂编译后的
/// 结构——稳定拓扑序节点序列 + 已解析入边（生产者执行序下标 + 输出端口）。
///
/// 生命周期：Node 内 descriptor 指向构建所用 NodeCatalog 中的声明；目录按契约
/// 构建期确定、运行期不变，NodeGraph 不得比目录长寿。图值不可拷贝（impl 持有
/// 多态节点实例），可移动。
class NodeGraph {
public:
    /// 一条已解析的入边（与 descriptor->inputs 对齐存放）。
    struct InputEdge {
        std::size_t producerIndex = 0;  /// 生产者在 nodes()（执行序）中的下标。
        std::uint32_t outputPort = 0;   /// 生产者输出端口序号。

        [[nodiscard]] friend bool operator==(const InputEdge&, const InputEdge&) = default;
    };

    struct Node {
        NodeId id = kInvalidNode;
        const NodeDescriptor* descriptor = nullptr;  /// 构建目录内的类型声明。
        std::unique_ptr<IImageNode> impl;            /// 注入型源节点为 nullptr。
        std::vector<InputEdge> inputs;               /// 与 descriptor->inputs 对齐。
    };

    NodeGraph() = default;
    NodeGraph(NodeGraph&&) = default;
    NodeGraph& operator=(NodeGraph&&) = default;
    NodeGraph(const NodeGraph&) = delete;
    NodeGraph& operator=(const NodeGraph&) = delete;

    /// 节点序列（稳定拓扑序：所有生产者先于消费者；同轮就绪节点按图内声明序）。
    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return nodes_; }

    /// 图内节点数。
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    /// 按 id 查节点；不存在返回 nullptr。
    [[nodiscard]] const Node* find(NodeId id) const noexcept;

private:
    /// 编译入口唯一（图只能经 buildNodeGraph 构造，保证校验/准入/拓扑序不变量）。
    friend NodeGraphBuild buildNodeGraph(const WorkflowGraph& graph,
                                         const NodeCatalog& catalog,
                                         const ImageNodeFactory& factory,
                                         std::size_t maxNodes);

    std::vector<Node> nodes_;
};

/// NodeGraph 构建结果：graph 非 null 当且仅当 validation.ok（失败时 issues 携带
/// 全部问题，与视图契约 WorkflowValidation 同型）。
struct NodeGraphBuild {
    WorkflowValidation validation;
    std::unique_ptr<NodeGraph> graph;
};

/// 编译工作流图（M4-02 引擎侧唯一构建入口）：
/// 1. 先执行 validateWorkflowGraph（与 UI 预检共用唯一判据，M4-09 契约）；
/// 2. 节点数准入：超过 maxNodes 显式拒绝（问题类别 BadParam）；
/// 3. 逐节点调用工厂：返回 nullptr 即注入型源节点；工厂抛异常或返回实例的
///    typeId 与目录声明不一致转为显式校验问题（BadParam）；
/// 4. 稳定拓扑排序（校验通过后环不存在，防御性兜底拒绝并报 Cycle）。
[[nodiscard]] NodeGraphBuild buildNodeGraph(const WorkflowGraph& graph,
                                            const NodeCatalog& catalog,
                                            const ImageNodeFactory& factory,
                                            std::size_t maxNodes = kDefaultMaxGraphNodes);

/// 注入器：为注入型源节点（impl == nullptr）提供本帧图像。返回图像必须有效且
/// 格式与节点声明输出类型一致。引擎注入相机帧（M4-07 源节点语义）；测试注入
/// 合成帧。
using SourceInjector = std::function<ImageU8(const NodeGraph::Node&)>;

/// 单帧同步求值（M4-02 契约语义）：按拓扑序执行整图，返回与 nodes() 执行序
/// 对齐的逐节点输出（引擎据此做有界产物保留与快照发布，DEC-013 冻结保留策略；
/// M4-07 引擎在 Executor 有限任务内调用本函数并承载统计/取消/有界在飞）。
///
/// - 注入型节点：声明输出数为 0 时不注入、输出为空；输出数非 1，或注入器返回
///   图像无效/格式与声明不一致，抛 std::runtime_error（显式失败，禁止静默）；
/// - 实现节点：按入边收集各生产者输出（像素共享，零拷贝）后调用
///   IImageNode::apply，并对返回值做防御性契约核对（数量 = 声明输出数、逐幅
///   valid 且格式 = 声明格式），违反抛 std::runtime_error（算子实现缺陷显式
///   暴露，不静默）；
/// - 节点内部异常原样传播（引擎捕获 → NodeFailed → Failed，EXEC-07）。
[[nodiscard]] std::vector<std::vector<ImageU8>> runNodeGraph(const NodeGraph& graph,
                                                             const SourceInjector& injectFrame);

}  // namespace rin
