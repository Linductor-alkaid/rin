#include "rin/node_graph.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace rin {

const NodeGraph::Node* NodeGraph::find(NodeId id) const noexcept {
    for (const Node& node : nodes_) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

NodeGraphBuild buildNodeGraph(const WorkflowGraph& graph, const NodeCatalog& catalog,
                              const ImageNodeFactory& factory, std::size_t maxNodes) {
    NodeGraphBuild build;
    build.validation = validateWorkflowGraph(graph, catalog);
    if (!build.validation.ok) {
        return build;
    }
    auto reject = [&build](ValidationIssueKind kind, NodeId node, std::string message) {
        build.validation.ok = false;
        build.validation.issues.push_back({kind, node, std::move(message)});
    };
    if (graph.nodes.size() > maxNodes) {
        reject(ValidationIssueKind::BadParam, kInvalidNode,
               "graph exceeds node admission limit (" + std::to_string(maxNodes) + ")");
        return build;
    }

    const std::size_t count = graph.nodes.size();
    std::unordered_map<NodeId, std::size_t> indexOf;
    indexOf.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        indexOf.emplace(graph.nodes[i].id, i);
    }

    // 逐节点调用工厂（图内声明序）：注入型源节点 impl 为空；失败即中止并返回
    // 问题清单（类型缺失/签名不一致/参数缺陷显式可见，不静默吞掉）。编译产物
    // 暂存声明序，拓扑排序后整体重排进执行序（身份与入边同移）。
    std::vector<const NodeDescriptor*> descriptors(count);
    std::vector<std::unique_ptr<IImageNode>> impls(count);
    std::vector<std::vector<std::pair<std::size_t, std::uint32_t>>> producers(count);
    for (std::size_t i = 0; i < count; ++i) {
        const NodeInstance& instance = graph.nodes[i];
        const NodeDescriptor* descriptor = findNodeDescriptor(catalog, instance.typeId);
        if (descriptor == nullptr) {  // 校验已保证非空；防御性兜底。
            reject(ValidationIssueKind::UnknownNodeType, instance.id,
                   "unknown node type: " + instance.typeId);
            return build;
        }
        std::unique_ptr<IImageNode> impl;
        try {
            impl = factory(*descriptor, instance);
        } catch (const std::exception& error) {
            reject(ValidationIssueKind::BadParam, instance.id,
                   "node factory failed for '" + instance.typeId + "': " + error.what());
            return build;
        }
        if (impl != nullptr && impl->descriptor().typeId != instance.typeId) {
            reject(ValidationIssueKind::BadParam, instance.id,
                   "node factory returned mismatched type '" + impl->descriptor().typeId +
                       "' for '" + instance.typeId + "'");
            return build;
        }
        descriptors[i] = descriptor;
        impls[i] = std::move(impl);
        producers[i].assign(descriptor->inputs.size(), {0, 0});
    }

    // 入边解析（校验保证每个输入槽恰一条入边、端口序号在签名内、无环）。
    for (const Connection& connection : graph.connections) {
        const std::size_t consumer = indexOf[connection.to.node];
        const std::size_t producer = indexOf[connection.from.node];
        producers[consumer][connection.to.index] = {producer, connection.from.index};
    }

    // 稳定拓扑排序：反复按声明序收录就绪节点（生产者已全部收录）；同轮内后继
    // 立即可见，确定性且与声明序一致。
    std::vector<std::size_t> order;
    order.reserve(count);
    std::vector<char> placed(count, 0);
    while (order.size() < count) {
        bool progress = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (placed[i] != 0) {
                continue;
            }
            const bool ready = std::all_of(producers[i].begin(), producers[i].end(),
                                           [&placed](const auto& edge) {
                                               return placed[edge.first] != 0;
                                           });
            if (ready) {
                placed[i] = 1;
                order.push_back(i);
                progress = true;
            }
        }
        if (!progress) {  // 校验通过后不可达；防御性兜底（契约双保险）。
            reject(ValidationIssueKind::Cycle, kInvalidNode,
                   "internal: cycle survived graph validation");
            return build;
        }
    }

    // 组装执行序图：按 order 把身份（id/descriptor/impl）与入边整体重排，
    // 执行序下标 = order 中的位置。
    auto compiled = std::make_unique<NodeGraph>();
    compiled->nodes_.resize(count);
    std::vector<std::size_t> rankOf(count);
    for (std::size_t rank = 0; rank < count; ++rank) {
        rankOf[order[rank]] = rank;
    }
    for (std::size_t rank = 0; rank < count; ++rank) {
        const std::size_t i = order[rank];
        NodeGraph::Node& node = compiled->nodes_[rank];
        node.id = graph.nodes[i].id;
        node.descriptor = descriptors[i];
        node.impl = std::move(impls[i]);
        node.inputs.resize(producers[i].size());
        for (std::size_t slot = 0; slot < producers[i].size(); ++slot) {
            node.inputs[slot].producerIndex = rankOf[producers[i][slot].first];
            node.inputs[slot].outputPort = producers[i][slot].second;
        }
    }
    build.graph = std::move(compiled);
    return build;
}

std::vector<std::vector<ImageU8>> runNodeGraph(const NodeGraph& graph,
                                               const SourceInjector& injectFrame,
                                               const NodeExecutionObserver& observeNode) {
    const std::vector<NodeGraph::Node>& nodes = graph.nodes();
    std::vector<std::vector<ImageU8>> outputs(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const NodeGraph::Node& node = nodes[i];
        const std::size_t declaredOutputs = node.descriptor->outputs.size();

        if (node.impl == nullptr) {
            // 注入型源节点：相机帧注入语义（M4-07 引擎提供注入器）。
            if (declaredOutputs == 0) {
                continue;  // 无输出注入点：无需注入。
            }
            if (declaredOutputs != 1) {
                throw std::runtime_error("injected node " + std::to_string(node.id) +
                                         " declares " + std::to_string(declaredOutputs) +
                                         " outputs; exactly one expected");
            }
            if (!injectFrame) {
                throw std::runtime_error("source injector is not set");
            }
            ImageU8 frame = injectFrame(node);
            if (!frame.valid()) {
                throw std::runtime_error("injected frame for node " + std::to_string(node.id) +
                                         " is invalid");
            }
            if (frame.format() != node.descriptor->outputs.front()) {
                throw std::runtime_error(
                    "injected frame format for node " + std::to_string(node.id) + " is " +
                    toString(frame.format()) + ", declared output is " +
                    toString(node.descriptor->outputs.front()));
            }
            outputs[i].push_back(std::move(frame));
            continue;
        }

        std::vector<ImageU8> inputs;
        inputs.reserve(node.inputs.size());
        for (const NodeGraph::InputEdge& edge : node.inputs) {
            inputs.push_back(outputs[edge.producerIndex][edge.outputPort]);
        }
        // 逐节点观测（DEC-013 统计接缝）：耗时只覆盖 apply 本体；失败先回调
        // 异常副本再原样重传（传播语义不变）。
        const auto applyStarted = std::chrono::steady_clock::now();
        std::vector<ImageU8> result;
        try {
            result = node.impl->apply(inputs);
        } catch (...) {
            if (observeNode) {
                const double costMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - applyStarted)
                        .count();
                observeNode(node, costMs, std::current_exception());
            }
            throw;
        }
        if (observeNode) {
            const double costMs = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - applyStarted)
                                      .count();
            observeNode(node, costMs, nullptr);
        }
        if (result.size() != declaredOutputs) {
            throw std::runtime_error("node " + std::to_string(node.id) + " ('" +
                                     node.descriptor->typeId + "') produced " +
                                     std::to_string(result.size()) + " outputs, expected " +
                                     std::to_string(declaredOutputs));
        }
        for (std::size_t port = 0; port < result.size(); ++port) {
            if (!result[port].valid()) {
                throw std::runtime_error("node " + std::to_string(node.id) + " ('" +
                                         node.descriptor->typeId +
                                         "') produced an invalid image on output port " +
                                         std::to_string(port));
            }
            // 声明 Any 的输出端口接受任意具体格式（M11/DEC-021：监看器透传
            // 节点的输出格式随输入类型动态，恒等返回即覆盖全部既有可能性）。
            if (result[port].format() != node.descriptor->outputs[port] &&
                node.descriptor->outputs[port] != PortType::Any) {
                throw std::runtime_error(
                    "node " + std::to_string(node.id) + " ('" + node.descriptor->typeId +
                    "') produced " + toString(result[port].format()) + " on output port " +
                    std::to_string(port) + ", declared " +
                    toString(node.descriptor->outputs[port]));
            }
        }
        outputs[i] = std::move(result);
    }
    return outputs;
}

}  // namespace rin
