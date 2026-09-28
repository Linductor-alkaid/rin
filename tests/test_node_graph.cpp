// M4-02 Core 图编译与单帧求值契约测试（独立验证）：include/rin/node_graph.hpp、
// src/core/node_graph.cpp（依赖 M4-09 视图契约与 M4-02 ImageU8/参数模型）。
//
// 被测面与范围（对应 docs/design/image_workflow_design.md §8 测试矩阵后两行）：
// 1) buildNodeGraph：
//    - 校验不通过原样透传 validateWorkflowGraph 问题清单且 graph 为 null（工厂
//      不被调用）；
//    - 节点数准入：超 maxNodes 拒绝（BadParam、node=kInvalidNode、message 含
//      上限值、graph 为 null、工厂不被调用）；恰等 maxNodes 成功；默认参数
//      kDefaultMaxGraphNodes=64 的 64/65 边界；
//    - 工厂抛异常：BadParam、node=实例 id、message 含该节点 typeId；工厂返回
//      实例 typeId 与目录不符：BadParam 显式化；返回 nullptr 即注入型源节点；
//    - 稳定拓扑序：链式（声明序与依赖序相反）、分叉扇出、菱形多轮就绪（确定性
//      顺序锁定）、工厂调用按图内声明序；
//    - 入边解析：InputEdge.producerIndex 为执行序下标、outputPort 为生产者输出
//      端口（含 2 输出生产者 split → mix 的端口分辨）；
//    - 空图（0 节点）构建成功且 size()==0；find 命中/未命中；NodeGraph 不可拷
//      贝、可移动（类型面 static_assert + 运行期移动）。
// 2) runNodeGraph：
//    - 按拓扑序求值（注入/apply 事件交错顺序锁定），输出与 nodes() 执行序对齐；
//    - 注入语义：注入帧像素同一性、声明 0 输出不调注入器且输出空、声明 != 1 抛、
//      注入器返回无效图/格式不符抛、未设置注入器抛、注入器按节点分发（多源）、
//      impl 非空（typeId 为源类型）不注入（注入型按 impl 判定而非 typeId）、
//      注入器存在但图内无注入点时不调用；
//    - 实现节点防御核对：apply 返回数量 != 声明、含无效图、格式与声明不符均抛
//      std::runtime_error（不良实现桩）；
//    - 节点异常原样传播：std::runtime_error 派生异常按精确类型捕获（不被包装）、
//      非 std 异常同样传播；
//    - 像素共享零拷贝：生产者输出缓冲对象与传给消费者的输入缓冲对象同一
//      （source→grayify→blur 链与分叉扇出全程同一缓冲）；
//    - 空图求值返回空。
//
// 节点桩直接内联实现 IImageNode（自建小目录，含 0 输出/双输出源与多输入汇聚，
// 比 makeDefaultFakeCatalog 更便于构造防御路径）；目录构造手法沿用
// test_workflow_contracts.cpp。
//
// DOD-02 适用性说明：buildNodeGraph/runNodeGraph 为单帧同步纯函数，本测试全部
// 单线程顺序调用，无跨上下文共享状态/关闭路径（DOD-02 并发矩阵归 M4-07 引擎
// 与 M5-08 契约套件，见设计 §8）。
#include "test_util.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <rin/node_graph.hpp>

namespace {

using rin::Connection;
using rin::IImageNode;
using rin::ImageNodeFactory;
using rin::ImageU8;
using rin::kInvalidNode;
using rin::NodeCatalog;
using rin::NodeDescriptor;
using rin::NodeId;
using rin::NodeInstance;
using rin::NodeGraph;
using rin::NodeGraphBuild;
using rin::PortDirection;
using rin::PortRef;
using rin::PortType;
using rin::SourceInjector;
using rin::ValidationIssueKind;
using rin::WorkflowGraph;
using rin::WorkflowValidation;
using rin::buildNodeGraph;
using rin::findNodeDescriptor;
using rin::runNodeGraph;

static_assert(!std::is_copy_constructible_v<NodeGraph> &&
                  !std::is_copy_assignable_v<NodeGraph>,
              "NodeGraph 不可拷贝（impl 持有多态实例）");
static_assert(std::is_move_constructible_v<NodeGraph> && std::is_move_assignable_v<NodeGraph>,
              "NodeGraph 可移动");
static_assert(std::is_default_constructible_v<NodeGraph>, "NodeGraph 可默认构造（占位）");

// --- 节点桩与事件日志 -------------------------------------------------------

struct GraphEvent {
    std::string action;  // "inject" / "apply"
    NodeId node = kInvalidNode;
    std::vector<const std::vector<std::uint8_t>*> inputBuffers;  // apply：逐输入像素缓冲
};

struct PlainError {};  // 非 std::exception 派生的自定义异常（原样传播探针）

class StubBoomError final : public std::runtime_error {
public:
    StubBoomError() : std::runtime_error("stub-boom-42") {}
};

class StubNode final : public IImageNode {
public:
    enum class Mode {
        Normal,       // 按声明逐输出生成新的有效小图（2x1）
        PassThrough,  // 以首输入的像素缓冲包出声明类型输出（零拷贝直通）
        WrongCount,   // 返回 0 幅（数量 != 声明输出数）
        InvalidImage, // 数量正确但全部为无效图
        WrongFormat,  // 数量正确但逐幅格式与声明不符
        ThrowStd,     // 抛 std::runtime_error 派生异常
        ThrowCustom,  // 抛非 std 异常
    };

    StubNode(NodeDescriptor descriptor, Mode mode, NodeId id,
             std::shared_ptr<std::vector<GraphEvent>> log)
        : descriptor_(std::move(descriptor)), mode_(mode), id_(id), log_(std::move(log)) {}

    const NodeDescriptor& descriptor() const noexcept override { return descriptor_; }

    std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const override {
        GraphEvent event;
        event.action = "apply";
        event.node = id_;
        for (const ImageU8& input : inputs) {
            event.inputBuffers.push_back(input.pixels().get());
        }
        log_->push_back(std::move(event));
        switch (mode_) {
            case Mode::PassThrough: {
                std::vector<ImageU8> out;
                for (const PortType type : descriptor_.outputs) {
                    if (!inputs.empty()) {
                        out.push_back(ImageU8::wrap(type, inputs.front().width(),
                                                    inputs.front().height(),
                                                    inputs.front().stride(),
                                                    inputs.front().pixels()));
                    } else {
                        out.push_back(ImageU8::make(type, 2, 1));
                    }
                }
                return out;
            }
            case Mode::WrongCount:
                return {};
            case Mode::InvalidImage:
                return std::vector<ImageU8>(descriptor_.outputs.size());
            case Mode::WrongFormat: {
                std::vector<ImageU8> out;
                for (const PortType type : descriptor_.outputs) {
                    const PortType wrong =
                        type == PortType::Gray8 ? PortType::Rgba8 : PortType::Gray8;
                    out.push_back(ImageU8::make(wrong, 2, 1));
                }
                return out;
            }
            case Mode::ThrowStd:
                throw StubBoomError();
            case Mode::ThrowCustom:
                throw PlainError{};
            case Mode::Normal:
            default: {
                std::vector<ImageU8> out;
                for (const PortType type : descriptor_.outputs) {
                    out.push_back(ImageU8::make(type, 2, 1));
                }
                return out;
            }
        }
    }

private:
    NodeDescriptor descriptor_;
    Mode mode_;
    NodeId id_;
    std::shared_ptr<std::vector<GraphEvent>> log_;
};

// --- 目录（小而全：注入型 0/1/2 输出源 + 灰度桥 + 多输入汇聚 + 双输出分叉）---

NodeDescriptor makeDescriptor(const std::string& typeId, std::vector<PortType> inputs,
                              std::vector<PortType> outputs) {
    NodeDescriptor d;
    d.typeId = typeId;
    d.displayName = typeId;
    d.inputs = std::move(inputs);
    d.outputs = std::move(outputs);
    return d;
}

NodeCatalog makeCatalog() {
    NodeCatalog catalog;
    catalog.nodes = {
        makeDescriptor("source", {}, {PortType::Rgba8}),
        makeDescriptor("noports", {}, {}),
        makeDescriptor("twin_source", {}, {PortType::Rgba8, PortType::Rgba8}),
        makeDescriptor("grayify", {PortType::Rgba8}, {PortType::Gray8}),
        makeDescriptor("blur", {PortType::Gray8}, {PortType::Gray8}),
        makeDescriptor("split", {PortType::Gray8}, {PortType::Gray8, PortType::Gray8}),
        makeDescriptor("mix", {PortType::Gray8, PortType::Gray8}, {PortType::Gray8}),
    };
    return catalog;
}

// --- 工厂与注入器 -----------------------------------------------------------

struct FactoryConfig {
    std::shared_ptr<std::vector<GraphEvent>> log = std::make_shared<std::vector<GraphEvent>>();
    std::vector<NodeId>* factoryOrder = nullptr;  // 记录工厂调用序（实例 id，声明序探针）
    std::string throwForType;                     // 对该 typeId 工厂抛异常
    bool mismatchGrayify = false;                 // grayify 返回 typeId 不符的实现
    std::vector<std::string> forceNullTypes;      // 强制按注入型处理（返回 nullptr）
    std::vector<std::pair<NodeId, StubNode::Mode>> modes;  // 按实例 id 指定桩行为
};

ImageNodeFactory makeFactory(FactoryConfig config) {
    return [config](const NodeDescriptor& descriptor,
                    const NodeInstance& instance) -> std::unique_ptr<IImageNode> {
        if (config.factoryOrder != nullptr) {
            config.factoryOrder->push_back(instance.id);
        }
        if (!config.throwForType.empty() && instance.typeId == config.throwForType) {
            throw std::runtime_error("stub factory failure: " + instance.typeId);
        }
        if (config.mismatchGrayify && instance.typeId == "grayify") {
            NodeDescriptor impostor = descriptor;
            impostor.typeId = "impostor";
            return std::make_unique<StubNode>(std::move(impostor), StubNode::Mode::Normal,
                                              instance.id, config.log);
        }
        for (const std::string& type : config.forceNullTypes) {
            if (instance.typeId == type) {
                return nullptr;
            }
        }
        // 注入型源类型默认无实现；显式 modes 条目可强制提供实现（探针：注入型
        // 按 impl 是否为空判定，而非 typeId）。
        const bool injectedType = instance.typeId == "source" ||
                                  instance.typeId == "noports" ||
                                  instance.typeId == "twin_source";
        StubNode::Mode mode = StubNode::Mode::Normal;
        bool hasMode = false;
        for (const auto& [id, assigned] : config.modes) {
            if (id == instance.id) {
                mode = assigned;
                hasMode = true;
                break;
            }
        }
        if (injectedType && !hasMode) {
            return nullptr;
        }
        return std::make_unique<StubNode>(descriptor, mode, instance.id, config.log);
    };
}

SourceInjector loggingInjector(const std::shared_ptr<std::vector<GraphEvent>>& log,
                               ImageU8 frame) {
    return [log, frame](const NodeGraph::Node& node) {
        log->push_back({"inject", node.id, {}});
        return frame;  // 拷贝共享像素（注入面同样零拷贝）
    };
}

SourceInjector dispatchInjector(const std::shared_ptr<std::vector<GraphEvent>>& log,
                                std::map<NodeId, ImageU8> frames) {
    return [log, frames](const NodeGraph::Node& node) {
        log->push_back({"inject", node.id, {}});
        return frames.at(node.id);
    };
}

// --- 图与断言助手 -----------------------------------------------------------

NodeInstance makeNode(NodeId id, const std::string& typeId) {
    NodeInstance instance;
    instance.id = id;
    instance.typeId = typeId;
    return instance;
}

Connection conn(NodeId from, std::uint32_t fromPort, NodeId to, std::uint32_t toPort) {
    Connection connection;
    connection.from = PortRef{from, PortDirection::Output, fromPort};
    connection.to = PortRef{to, PortDirection::Input, toPort};
    return connection;
}

// 声明序：blur(3) ← grayify(2) ← source(1)（与依赖序相反，探稳定拓扑序）。
WorkflowGraph makeChain() {
    WorkflowGraph graph;
    graph.nodes = {makeNode(3, "blur"), makeNode(2, "grayify"), makeNode(1, "source")};
    graph.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0)};
    return graph;
}

// 声明序与依赖序一致：source(1) → grayify(2) → blur(3)（隔离拓扑序维度，验证
// 执行/注入/防御等其余契约面）。
WorkflowGraph makeChainInOrder() {
    WorkflowGraph graph;
    graph.nodes = {makeNode(1, "source"), makeNode(2, "grayify"), makeNode(3, "blur")};
    graph.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0)};
    return graph;
}

bool eventsMatch(const std::vector<GraphEvent>& events,
                 const std::initializer_list<std::pair<const char*, NodeId>>& expected) {
    if (events.size() != expected.size()) {
        return false;
    }
    std::size_t index = 0;
    for (const auto& [action, node] : expected) {
        if (events[index].action != action || events[index].node != node) {
            return false;
        }
        ++index;
    }
    return true;
}

bool sameIssues(const WorkflowValidation& a, const WorkflowValidation& b) {
    if (a.issues.size() != b.issues.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.issues.size(); ++i) {
        if (a.issues[i].kind != b.issues[i].kind || a.issues[i].node != b.issues[i].node ||
            a.issues[i].message != b.issues[i].message) {
            return false;
        }
    }
    return true;
}

template <typename Exception, typename Fn>
bool throwsAs(Fn&& fn) {
    try {
        fn();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

std::vector<NodeId> execOrder(const NodeGraph& graph) {
    std::vector<NodeId> order;
    order.reserve(graph.size());
    for (const NodeGraph::Node& node : graph.nodes()) {
        order.push_back(node.id);
    }
    return order;
}

}  // namespace

int main() {
    const NodeCatalog catalog = makeCatalog();

    // === A) buildNodeGraph ===

    // --- A1) 校验不通过：问题清单原样透传、graph 为 null、工厂不被调用 ---
    {
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "ghost_type")};
        graph.connections = {conn(9, 0, 1, 0)};  // from 节点不在图内：第二类问题。

        std::vector<NodeId> factoryOrder;
        FactoryConfig config;
        config.factoryOrder = &factoryOrder;
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));

        RIN_CHECK(!build.validation.ok);
        RIN_CHECK(build.graph == nullptr);
        const WorkflowValidation direct = rin::validateWorkflowGraph(graph, catalog);
        RIN_CHECK(!direct.ok);
        RIN_CHECK(sameIssues(build.validation, direct));
        RIN_CHECK(factoryOrder.empty());  // 校验优先，工厂未被调用。
    }

    // --- A2) 节点数准入：显式上限与默认上限 ---
    {
        WorkflowGraph graph;
        for (int i = 1; i <= 4; ++i) {
            graph.nodes.push_back(makeNode(static_cast<NodeId>(i), "source"));
        }
        std::vector<NodeId> factoryOrder;
        FactoryConfig config;
        config.factoryOrder = &factoryOrder;

        const NodeGraphBuild rejected = buildNodeGraph(graph, catalog, makeFactory(config), 3);
        RIN_CHECK(!rejected.validation.ok);
        RIN_CHECK(rejected.graph == nullptr);
        RIN_CHECK_EQ(rejected.validation.issues.size(), std::size_t{1});
        RIN_CHECK(rejected.validation.issues[0].kind == ValidationIssueKind::BadParam);
        RIN_CHECK_EQ(rejected.validation.issues[0].node, kInvalidNode);
        RIN_CHECK(rejected.validation.issues[0].message.find("3") != std::string::npos);
        RIN_CHECK(factoryOrder.empty());  // 准入优先于工厂。

        // 恰等 maxNodes 成功。
        WorkflowGraph atLimit;
        atLimit.nodes = {makeNode(1, "source"), makeNode(2, "source"), makeNode(3, "source")};
        const NodeGraphBuild accepted =
            buildNodeGraph(atLimit, catalog, makeFactory(FactoryConfig{}), 3);
        RIN_CHECK(accepted.validation.ok);
        RIN_CHECK(accepted.graph != nullptr);
        RIN_CHECK_EQ(accepted.graph->size(), std::size_t{3});
    }
    {
        // 默认参数 kDefaultMaxGraphNodes=64：64 成功、65 拒绝。
        WorkflowGraph atDefault;
        for (int i = 1; i <= 64; ++i) {
            atDefault.nodes.push_back(makeNode(static_cast<NodeId>(i), "source"));
        }
        const NodeGraphBuild accepted =
            buildNodeGraph(atDefault, catalog, makeFactory(FactoryConfig{}));
        RIN_CHECK(accepted.validation.ok);
        RIN_CHECK(accepted.graph != nullptr);
        RIN_CHECK_EQ(accepted.graph->size(), std::size_t{64});

        WorkflowGraph overDefault;
        for (int i = 1; i <= 65; ++i) {
            overDefault.nodes.push_back(makeNode(static_cast<NodeId>(i), "source"));
        }
        const NodeGraphBuild rejected =
            buildNodeGraph(overDefault, catalog, makeFactory(FactoryConfig{}));
        RIN_CHECK(!rejected.validation.ok);
        RIN_CHECK(rejected.graph == nullptr);
        RIN_CHECK_EQ(rejected.validation.issues.size(), std::size_t{1});
        RIN_CHECK(rejected.validation.issues[0].kind == ValidationIssueKind::BadParam);
    }

    // --- A3) 工厂抛异常：BadParam、node=实例 id、message 含 typeId ---
    {
        FactoryConfig config;
        config.throwForType = "grayify";
        std::vector<NodeId> factoryOrder;
        config.factoryOrder = &factoryOrder;

        const NodeGraphBuild build = buildNodeGraph(makeChain(), catalog, makeFactory(config));
        RIN_CHECK(!build.validation.ok);
        RIN_CHECK(build.graph == nullptr);
        RIN_CHECK_EQ(build.validation.issues.size(), std::size_t{1});
        RIN_CHECK(build.validation.issues[0].kind == ValidationIssueKind::BadParam);
        RIN_CHECK_EQ(build.validation.issues[0].node, NodeId{2});
        RIN_CHECK(build.validation.issues[0].message.find("grayify") != std::string::npos);
        // 工厂按图内声明序调用：blur(3) 先于 grayify(2)；source(1) 未到达。
        RIN_CHECK((factoryOrder == std::vector<NodeId>{3, 2}));
    }

    // --- A4) 工厂返回实例 typeId 与目录不符：BadParam 显式化 ---
    {
        FactoryConfig config;
        config.mismatchGrayify = true;
        const NodeGraphBuild build = buildNodeGraph(makeChain(), catalog, makeFactory(config));
        RIN_CHECK(!build.validation.ok);
        RIN_CHECK(build.graph == nullptr);
        RIN_CHECK_EQ(build.validation.issues.size(), std::size_t{1});
        RIN_CHECK(build.validation.issues[0].kind == ValidationIssueKind::BadParam);
        RIN_CHECK_EQ(build.validation.issues[0].node, NodeId{2});
        RIN_CHECK(build.validation.issues[0].message.find("grayify") != std::string::npos);
    }

    // --- A5) 工厂返回 nullptr：注入型源节点（impl 为空、descriptor 指向目录）---
    {
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "source")};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(FactoryConfig{}));
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        RIN_CHECK_EQ(build.graph->size(), std::size_t{1});
        const NodeGraph::Node& node = build.graph->nodes().front();
        RIN_CHECK_EQ(node.id, NodeId{1});
        RIN_CHECK(node.impl == nullptr);
        RIN_CHECK(node.descriptor == findNodeDescriptor(catalog, "source"));
        RIN_CHECK(node.inputs.empty());
    }

    // --- A6) 稳定拓扑序：链式（声明序与依赖序相反）+ 入边执行序下标 ---
    // （契约边界探针：实现若未按拓扑序重排节点身份将在此失败。）
    {
        const NodeGraphBuild build =
            buildNodeGraph(makeChain(), catalog, makeFactory(FactoryConfig{}));
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        if (build.graph != nullptr) {
            RIN_CHECK((execOrder(*build.graph) == std::vector<NodeId>{1, 2, 3}));

            const std::vector<NodeGraph::Node>& nodes = build.graph->nodes();
            RIN_CHECK_EQ(nodes[0].id, NodeId{1});
            RIN_CHECK(nodes[0].impl == nullptr);  // source 注入型
            RIN_CHECK(nodes[1].impl != nullptr);
            RIN_CHECK(nodes[2].impl != nullptr);
            // 入边：grayify（执行序 1）← source（执行序 0，输出端口 0）。
            RIN_CHECK_EQ(nodes[1].inputs.size(), std::size_t{1});
            RIN_CHECK((nodes[1].inputs[0] == NodeGraph::InputEdge{0, 0}));
            RIN_CHECK_EQ(nodes[2].inputs.size(), std::size_t{1});
            RIN_CHECK((nodes[2].inputs[0] == NodeGraph::InputEdge{1, 0}));
        }
    }

    // --- A7) 稳定拓扑序：分叉扇出（一个输出喂两个消费者）---
    {
        WorkflowGraph graph;
        graph.nodes = {makeNode(2, "grayify"), makeNode(3, "grayify"), makeNode(1, "source")};
        graph.connections = {conn(1, 0, 2, 0), conn(1, 0, 3, 0)};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(FactoryConfig{}));
        RIN_CHECK(build.validation.ok);
        if (build.graph != nullptr) {
            RIN_CHECK((execOrder(*build.graph) == std::vector<NodeId>{1, 2, 3}));
            // 两个消费者都指向执行序 0 的 source、端口 0。
            RIN_CHECK((build.graph->nodes()[1].inputs[0] == NodeGraph::InputEdge{0, 0}));
            RIN_CHECK((build.graph->nodes()[2].inputs[0] == NodeGraph::InputEdge{0, 0}));
        }
    }

    // --- A8) 稳定拓扑序：菱形多轮就绪，确定性顺序锁定 ---
    {
        // source(1) → grayify(2), grayify(3) → mix(4)（in0←2, in1←3）；
        // 声明序 [4, 3, 2, 1]。稳定序：第 1 轮仅 source 就绪；第 2 轮按声明序
        // 收录 grayify(3) 再 grayify(2)；第 3 轮 mix(4)。
        WorkflowGraph graph;
        graph.nodes = {makeNode(4, "mix"), makeNode(3, "grayify"), makeNode(2, "grayify"),
                       makeNode(1, "source")};
        graph.connections = {conn(1, 0, 2, 0), conn(1, 0, 3, 0), conn(2, 0, 4, 0),
                             conn(3, 0, 4, 1)};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(FactoryConfig{}));
        RIN_CHECK(build.validation.ok);
        if (build.graph != nullptr) {
            RIN_CHECK((execOrder(*build.graph) == std::vector<NodeId>{1, 3, 2, 4}));
        }
    }

    // --- A9) 入边解析：双输出生产者的端口分辨 ---
    {
        // source(1) → grayify(2) → split(3) ⇒ out0→mix.in0, out1→mix.in1；
        // 声明序 [mix(4), split(3), grayify(2), source(1)]。
        WorkflowGraph graph;
        graph.nodes = {makeNode(4, "mix"), makeNode(3, "split"), makeNode(2, "grayify"),
                       makeNode(1, "source")};
        graph.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0), conn(3, 0, 4, 0),
                             conn(3, 1, 4, 1)};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(FactoryConfig{}));
        RIN_CHECK(build.validation.ok);
        if (build.graph != nullptr) {
            RIN_CHECK((execOrder(*build.graph) == std::vector<NodeId>{1, 2, 3, 4}));
            const std::vector<NodeGraph::Node>& nodes = build.graph->nodes();
            RIN_CHECK_EQ(nodes[2].inputs.size(), std::size_t{1});
            RIN_CHECK((nodes[2].inputs[0] == NodeGraph::InputEdge{1, 0}));
            RIN_CHECK_EQ(nodes[3].inputs.size(), std::size_t{2});
            RIN_CHECK((nodes[3].inputs[0] == NodeGraph::InputEdge{2, 0}));  // split.out0
            RIN_CHECK((nodes[3].inputs[1] == NodeGraph::InputEdge{2, 1}));  // split.out1
        }
    }

    // --- A10) 空图：校验通过、产出空图 ---
    {
        const WorkflowGraph graph;
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(FactoryConfig{}));
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        RIN_CHECK_EQ(build.graph->size(), std::size_t{0});
        RIN_CHECK(build.graph->nodes().empty());
    }

    // --- A11) find：命中 / 未命中 ---
    {
        const NodeGraphBuild build =
            buildNodeGraph(makeChain(), catalog, makeFactory(FactoryConfig{}));
        const NodeGraph::Node* found = build.graph->find(2);
        RIN_CHECK(found != nullptr);
        RIN_CHECK(found != nullptr && found->id == NodeId{2});
        RIN_CHECK(build.graph->find(42) == nullptr);
        RIN_CHECK(build.graph->find(kInvalidNode) == nullptr);
    }

    // --- A12) NodeGraph 不可拷贝、可移动 ---
    {
        const NodeGraphBuild build =
            buildNodeGraph(makeChain(), catalog, makeFactory(FactoryConfig{}));
        RIN_CHECK(build.graph != nullptr);
        NodeGraph moved = std::move(*build.graph);
        RIN_CHECK_EQ(moved.size(), std::size_t{3});
        RIN_CHECK(moved.find(3) != nullptr);  // 移动后节点仍可寻址（序契约归 A6）。

        NodeGraph relayed = std::move(moved);
        RIN_CHECK_EQ(relayed.size(), std::size_t{3});
        RIN_CHECK(relayed.find(3) != nullptr);

        const NodeGraph empty;
        RIN_CHECK_EQ(empty.size(), std::size_t{0});
    }

    // === B) runNodeGraph ===

    // --- B1) 空图求值：返回空，注入器不被调用 ---
    {
        const NodeGraphBuild build =
            buildNodeGraph(WorkflowGraph{}, catalog, makeFactory(FactoryConfig{}));
        FactoryConfig config;
        int calls = 0;
        const auto outputs = runNodeGraph(*build.graph, [&calls](const NodeGraph::Node&) {
            ++calls;
            return ImageU8{};
        });
        RIN_CHECK(outputs.empty());
        RIN_CHECK_EQ(calls, 0);
    }

    // --- B2) 端到端链（source→grayify→blur）：拓扑序执行 + 像素共享零拷贝 ---
    // （声明序与依赖序一致，隔离 A6-A9 锁定的拓扑序维度。）
    {
        FactoryConfig config;
        config.modes = {{2, StubNode::Mode::PassThrough}, {3, StubNode::Mode::PassThrough}};
        const NodeGraphBuild build =
            buildNodeGraph(makeChainInOrder(), catalog, makeFactory(config));
        RIN_CHECK(build.validation.ok);

        std::vector<std::uint8_t> pattern;
        for (std::uint8_t byte = 0x11; byte < 0x11 + 16; ++byte) {
            pattern.push_back(byte);
        }
        auto sourcePixels =
            std::make_shared<const std::vector<std::uint8_t>>(std::move(pattern));
        const ImageU8 frame = ImageU8::wrap(PortType::Rgba8, 2, 2, 8, sourcePixels);

        const auto outputs = runNodeGraph(*build.graph, loggingInjector(config.log, frame));
        RIN_CHECK_EQ(outputs.size(), std::size_t{3});
        // 输出与执行序对齐：source(Rgba8) / grayify(Gray8) / blur(Gray8)。
        RIN_CHECK_EQ(outputs[0].size(), std::size_t{1});
        RIN_CHECK_EQ(outputs[1].size(), std::size_t{1});
        RIN_CHECK_EQ(outputs[2].size(), std::size_t{1});
        RIN_CHECK(outputs[0][0].valid() && outputs[0][0].format() == PortType::Rgba8);
        RIN_CHECK(outputs[1][0].valid() && outputs[1][0].format() == PortType::Gray8);
        RIN_CHECK(outputs[2][0].valid() && outputs[2][0].format() == PortType::Gray8);
        // 像素共享：三级持有同一缓冲对象（零拷贝），内容原样保留。
        RIN_CHECK_EQ(outputs[0][0].pixels().get(), sourcePixels.get());
        RIN_CHECK_EQ(outputs[1][0].pixels().get(), sourcePixels.get());
        RIN_CHECK_EQ(outputs[2][0].pixels().get(), sourcePixels.get());
        RIN_CHECK(outputs[0][0].pixels().use_count() >= 3);
        RIN_CHECK_EQ(outputs[0][0].pixels()->at(3), std::uint8_t{0x14});
        // 执行序：注入 source → apply grayify → apply blur。
        RIN_CHECK(eventsMatch(*config.log, {{"inject", 1}, {"apply", 2}, {"apply", 3}}));
        // 桩收到的输入缓冲与生产者输出缓冲同一。
        RIN_CHECK_EQ(config.log->at(1).inputBuffers.size(), std::size_t{1});
        RIN_CHECK_EQ(config.log->at(1).inputBuffers[0], sourcePixels.get());
        RIN_CHECK_EQ(config.log->at(2).inputBuffers[0], sourcePixels.get());
    }

    // --- B3) 分叉扇出：一个输出喂两个消费者，消费者收到同一缓冲 ---
    {
        // source(1) → grayify(2) → {blur(4), blur(3)}；声明序 [1, 2, 4, 3]，
        // 稳定执行序 [1, 2, 4, 3]。
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "source"), makeNode(2, "grayify"), makeNode(4, "blur"),
                       makeNode(3, "blur")};
        graph.connections = {conn(1, 0, 2, 0), conn(2, 0, 4, 0), conn(2, 0, 3, 0)};
        FactoryConfig config;
        config.modes = {{2, StubNode::Mode::PassThrough}, {3, StubNode::Mode::PassThrough},
                        {4, StubNode::Mode::PassThrough}};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        RIN_CHECK((execOrder(*build.graph) == std::vector<NodeId>{1, 2, 4, 3}));

        auto sourcePixels =
            std::make_shared<const std::vector<std::uint8_t>>(std::size_t{16}, std::uint8_t{7});
        const auto outputs = runNodeGraph(
            *build.graph,
            loggingInjector(config.log, ImageU8::wrap(PortType::Rgba8, 2, 2, 8, sourcePixels)));
        RIN_CHECK_EQ(outputs.size(), std::size_t{4});
        RIN_CHECK_EQ(outputs[2][0].pixels().get(), sourcePixels.get());  // blur(4) 输出
        RIN_CHECK_EQ(outputs[3][0].pixels().get(), sourcePixels.get());  // blur(3) 输出
        RIN_CHECK(eventsMatch(*config.log,
                              {{"inject", 1}, {"apply", 2}, {"apply", 4}, {"apply", 3}}));
        // 两个消费者桩收到的输入缓冲彼此相同、且与 grayify 输出相同。
        RIN_CHECK_EQ(config.log->at(2).inputBuffers[0], sourcePixels.get());
        RIN_CHECK_EQ(config.log->at(3).inputBuffers[0], sourcePixels.get());
    }

    // --- B4) 双输出生产者 → 汇聚：按 InputEdge.outputPort 精确取幅 ---
    {
        FactoryConfig config;
        config.modes = {{2, StubNode::Mode::PassThrough}};
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "source"), makeNode(2, "grayify"), makeNode(3, "split"),
                       makeNode(4, "mix")};
        graph.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0), conn(3, 0, 4, 0),
                             conn(3, 1, 4, 1)};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        RIN_CHECK((execOrder(*build.graph) == std::vector<NodeId>{1, 2, 3, 4}));

        auto sourcePixels =
            std::make_shared<const std::vector<std::uint8_t>>(std::size_t{16}, std::uint8_t{9});
        const auto outputs = runNodeGraph(
            *build.graph,
            loggingInjector(config.log, ImageU8::wrap(PortType::Rgba8, 2, 2, 8, sourcePixels)));
        RIN_CHECK_EQ(outputs.size(), std::size_t{4});
        RIN_CHECK_EQ(outputs[2].size(), std::size_t{2});  // split 双输出
        RIN_CHECK(outputs[2][0].valid() && outputs[2][1].valid());
        // mix（执行序 3）的两个输入分别取 split 的端口 0 / 端口 1 输出，且互不相同。
        RIN_CHECK_EQ(config.log->at(3).inputBuffers.size(), std::size_t{2});
        RIN_CHECK_EQ(config.log->at(3).inputBuffers[0], outputs[2][0].pixels().get());
        RIN_CHECK_EQ(config.log->at(3).inputBuffers[1], outputs[2][1].pixels().get());
        RIN_CHECK(config.log->at(3).inputBuffers[0] != config.log->at(3).inputBuffers[1]);
        // split 的输入来自 grayify（PassThrough → 源缓冲）。
        RIN_CHECK_EQ(config.log->at(2).inputBuffers[0], sourcePixels.get());
    }

    // --- B5) 注入语义 ---
    {
        // B5a) impl 非空（typeId 为源类型）：不注入，按实现节点求值（注入型按
        // impl 判定而非 typeId）。
        FactoryConfig config;
        config.modes = {{1, StubNode::Mode::Normal}};  // 强制 source 有实现。
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "source")};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        RIN_CHECK(build.graph != nullptr && build.graph->nodes().front().impl != nullptr);
        const auto outputs = runNodeGraph(
            *build.graph, loggingInjector(config.log, ImageU8::make(PortType::Rgba8, 2, 1)));
        RIN_CHECK_EQ(outputs.size(), std::size_t{1});
        RIN_CHECK(outputs[0][0].valid() && outputs[0][0].format() == PortType::Rgba8);
        RIN_CHECK(eventsMatch(*config.log, {{"apply", 1}}));  // 无 inject 事件。
        RIN_CHECK(config.log->at(0).inputBuffers.empty());   // 零输入调用。
    }
    {
        // B5b) 注入型声明 0 输出：不调用注入器、输出为空。
        FactoryConfig config;
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "noports")};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        const auto outputs =
            runNodeGraph(*build.graph, loggingInjector(config.log, ImageU8::make(PortType::Gray8, 2, 1)));
        RIN_CHECK_EQ(outputs.size(), std::size_t{1});
        RIN_CHECK(outputs[0].empty());
        RIN_CHECK(config.log->empty());  // 注入器未被调用。
    }
    {
        // B5c) 注入型声明 2 个输出：抛 std::runtime_error。
        FactoryConfig config;
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "twin_source")};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        RIN_CHECK(throwsAs<std::runtime_error>([&] {
            (void)runNodeGraph(*build.graph, loggingInjector(config.log, ImageU8::make(PortType::Rgba8, 2, 1)));
        }));
    }
    {
        // B5d) 注入器返回无效图：抛。
        FactoryConfig config;
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "source")};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        RIN_CHECK(throwsAs<std::runtime_error>(
            [&] { (void)runNodeGraph(*build.graph, loggingInjector(config.log, ImageU8{})); }));
    }
    {
        // B5e) 注入器返回格式与声明输出不符（声明 Rgba8、返回 Gray8）：抛。
        FactoryConfig config;
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "source")};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        RIN_CHECK(throwsAs<std::runtime_error>([&] {
            (void)runNodeGraph(*build.graph,
                         loggingInjector(config.log, ImageU8::make(PortType::Gray8, 2, 1)));
        }));
    }
    {
        // B5f) 未设置注入器（空 std::function）且有注入节点：抛。
        FactoryConfig config;
        const NodeGraphBuild build =
            buildNodeGraph(makeChain(), catalog, makeFactory(config));
        const SourceInjector noInjector;
        RIN_CHECK(throwsAs<std::runtime_error>(
            [&] { (void)runNodeGraph(*build.graph, noInjector); }));
    }
    {
        // B5g) 注入器存在但图内无注入点（全部 impl）：不调用。
        FactoryConfig config;
        config.modes = {{1, StubNode::Mode::Normal}};
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "source")};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        int calls = 0;
        const auto outputs = runNodeGraph(*build.graph, [&calls](const NodeGraph::Node&) {
            ++calls;
            return ImageU8{};
        });
        RIN_CHECK_EQ(outputs.size(), std::size_t{1});
        RIN_CHECK_EQ(calls, 0);
    }
    {
        // B5h) 非源类型被工厂置为注入型（返回 nullptr）：按声明输出类型注入。
        // 注入型判定按 impl 是否为空而非 typeId——source(1) 与被强制注入型的
        // grayify(7) 都走注入路径（grayify 的入边对注入节点无语义，不读取）。
        FactoryConfig config;
        config.forceNullTypes = {"grayify"};
        config.modes = {};  // grayify 强制注入型经 forceNullTypes 生效。
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "source"), makeNode(7, "grayify")};
        graph.connections = {conn(1, 0, 7, 0)};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        if (build.graph != nullptr) {
            RIN_CHECK(build.graph->nodes()[0].impl == nullptr);
            RIN_CHECK(build.graph->nodes()[1].impl == nullptr);
            auto rgbaPixels =
                std::make_shared<const std::vector<std::uint8_t>>(std::size_t{16},
                                                                  std::uint8_t{1});
            auto grayPixels =
                std::make_shared<const std::vector<std::uint8_t>>(std::size_t{4},
                                                                  std::uint8_t{3});
            const auto outputs = runNodeGraph(
                *build.graph,
                dispatchInjector(config.log,
                                 {{1, ImageU8::wrap(PortType::Rgba8, 2, 2, 8, rgbaPixels)},
                                  {7, ImageU8::wrap(PortType::Gray8, 2, 2, 2, grayPixels)}}));
            RIN_CHECK_EQ(outputs[0][0].pixels().get(), rgbaPixels.get());
            RIN_CHECK_EQ(outputs[1][0].pixels().get(), grayPixels.get());
            RIN_CHECK(eventsMatch(*config.log, {{"inject", 1}, {"inject", 7}}));
        }
    }
    {
        // B5i) 多源图：注入器按节点分发（不同源不同帧），执行序仍确定。
        // source(1), source(2) → grayify(3), grayify(4) → mix(5)；声明序与依赖序
        // 一致，稳定执行序 [1, 2, 3, 4, 5]。
        WorkflowGraph graph;
        graph.nodes = {makeNode(1, "source"), makeNode(2, "source"), makeNode(3, "grayify"),
                       makeNode(4, "grayify"), makeNode(5, "mix")};
        graph.connections = {conn(1, 0, 3, 0), conn(2, 0, 4, 0), conn(3, 0, 5, 0),
                             conn(4, 0, 5, 1)};
        FactoryConfig config;
        config.modes = {{3, StubNode::Mode::PassThrough}, {4, StubNode::Mode::PassThrough}};
        const NodeGraphBuild build = buildNodeGraph(graph, catalog, makeFactory(config));
        RIN_CHECK((execOrder(*build.graph) == std::vector<NodeId>{1, 2, 3, 4, 5}));

        auto frameA =
            std::make_shared<const std::vector<std::uint8_t>>(std::size_t{16}, std::uint8_t{0xA0});
        auto frameB =
            std::make_shared<const std::vector<std::uint8_t>>(std::size_t{16}, std::uint8_t{0xB0});
        const auto outputs = runNodeGraph(
            *build.graph,
            dispatchInjector(config.log, {{2, ImageU8::wrap(PortType::Rgba8, 2, 2, 8, frameA)},
                                          {1, ImageU8::wrap(PortType::Rgba8, 2, 2, 8, frameB)}}));
        RIN_CHECK_EQ(outputs.size(), std::size_t{5});
        // grayify(3)（执行序 2）直通 source(1) 的 frameB；grayify(4)（执行序 3）
        // 直通 source(2) 的 frameA。
        RIN_CHECK_EQ(outputs[2][0].pixels().get(), frameB.get());
        RIN_CHECK_EQ(outputs[3][0].pixels().get(), frameA.get());
        RIN_CHECK(eventsMatch(*config.log, {{"inject", 1},
                                            {"inject", 2},
                                            {"apply", 3},
                                            {"apply", 4},
                                            {"apply", 5}}));
    }

    // --- B6) 实现节点防御性输出核对（不良实现桩）---
    {
        // B6a) 数量 != 声明输出数：抛。
        FactoryConfig config;
        config.modes = {{2, StubNode::Mode::PassThrough}, {3, StubNode::Mode::WrongCount}};
        const NodeGraphBuild build = buildNodeGraph(makeChainInOrder(), catalog, makeFactory(config));
        auto sourcePixels =
            std::make_shared<const std::vector<std::uint8_t>>(std::size_t{16}, std::uint8_t{1});
        RIN_CHECK(throwsAs<std::runtime_error>([&] {
            (void)runNodeGraph(*build.graph,
                         loggingInjector(config.log, ImageU8::wrap(PortType::Rgba8, 2, 2, 8, sourcePixels)));
        }));
        RIN_CHECK(eventsMatch(*config.log, {{"inject", 1}, {"apply", 2}, {"apply", 3}}));
    }
    {
        // B6b) 含无效图：抛。
        FactoryConfig config;
        config.modes = {{2, StubNode::Mode::InvalidImage}};
        const NodeGraphBuild build = buildNodeGraph(makeChainInOrder(), catalog, makeFactory(config));
        RIN_CHECK(throwsAs<std::runtime_error>([&] {
            (void)runNodeGraph(*build.graph,
                         loggingInjector(config.log, ImageU8::make(PortType::Rgba8, 2, 1)));
        }));
    }
    {
        // B6c) 格式与声明不符（grayify 声明 Gray8、返回 Rgba8）：抛。
        FactoryConfig config;
        config.modes = {{2, StubNode::Mode::WrongFormat}};
        const NodeGraphBuild build = buildNodeGraph(makeChainInOrder(), catalog, makeFactory(config));
        RIN_CHECK(throwsAs<std::runtime_error>([&] {
            (void)runNodeGraph(*build.graph,
                         loggingInjector(config.log, ImageU8::make(PortType::Rgba8, 2, 1)));
        }));
    }

    // --- B7) 节点异常原样传播（不包装、不吞）---
    {
        // std::runtime_error 派生异常：按精确类型传播，what() 原样保留。
        FactoryConfig config;
        config.modes = {{2, StubNode::Mode::PassThrough}, {3, StubNode::Mode::ThrowStd}};
        const NodeGraphBuild build = buildNodeGraph(makeChainInOrder(), catalog, makeFactory(config));
        bool caughtExactType = false;
        try {
            (void)runNodeGraph(*build.graph, loggingInjector(config.log, ImageU8::make(PortType::Rgba8, 2, 1)));
        } catch (const StubBoomError& error) {
            caughtExactType = true;
            RIN_CHECK(std::string(error.what()) == "stub-boom-42");
        } catch (...) {
            caughtExactType = false;
        }
        RIN_CHECK(caughtExactType);
    }
    {
        // 非 std::exception 异常同样原样传播。
        FactoryConfig config;
        config.modes = {{2, StubNode::Mode::ThrowCustom}};
        const NodeGraphBuild build = buildNodeGraph(makeChainInOrder(), catalog, makeFactory(config));
        RIN_CHECK(throwsAs<PlainError>(
            [&] { (void)runNodeGraph(*build.graph, loggingInjector(config.log, ImageU8::make(PortType::Rgba8, 2, 1))); }));
    }

    return rin_test::exitStatus();
}
