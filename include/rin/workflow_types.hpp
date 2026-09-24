#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace rin {

/// 工作流视图契约类型（M4-09，DEC-016）：UI 与引擎之间的唯一数据面。
///
/// 公开头零第三方类型（RULE-01）；通道承载（executor::comm 选型）属实现细节，
/// 契约只冻结语义。画布布局（节点坐标）是 UI 私有状态，不入本契约；
/// 图持久化（含布局）见总计划 POST-04。

/// 节点实例标识；由 UI/图构造方分配，图内唯一（重复 id 校验拒绝）。
/// 0 保留为无效值。
using NodeId = std::uint64_t;

inline constexpr NodeId kInvalidNode = 0;

/// 端口方向。
enum class PortDirection {
    Input,
    Output,
};

/// 端口图像类型（M4 节点集；新格式按决策扩展，UI 据此渲染端口与类型兼容性）。
enum class PortType {
    Gray8,
    Rgba8,
};

[[nodiscard]] const char* toString(PortType type) noexcept;

/// 连线端点：节点 + 方向 + 端口在节点签名内的序号。
struct PortRef {
    NodeId node = kInvalidNode;
    PortDirection direction = PortDirection::Input;
    std::uint32_t index = 0;

    [[nodiscard]] friend bool operator==(const PortRef&, const PortRef&) = default;
};

/// 一条连线：输出端口 -> 输入端口。每个输入端口至多一条入边（多驱动拒绝）；
/// 输出端口允许多条出边（扇出）；输出端口允许悬空（末端结果可直接查看）。
struct Connection {
    PortRef from;
    PortRef to;

    [[nodiscard]] friend bool operator==(const Connection&, const Connection&) = default;
};

/// 参数值类型（M4 节点集参数面；与 ParamKind 一一对应）。
using ParamValue = std::variant<bool, std::int64_t, double, std::string, std::vector<double>>;

/// 参数种类（参数面板的声明类型）。
enum class ParamKind {
    Boolean,
    Integer,
    Real,
    Enumeration,
    /// 实数数组（行主序；如自定义卷积核系数）。
    RealArray,
};

[[nodiscard]] const char* toString(ParamKind kind) noexcept;

/// 取参数值当前承载的种类；与 ParamKind 比对判定类型匹配。
[[nodiscard]] ParamKind paramKindOf(const ParamValue& value) noexcept;

/// 参数声明（节点目录项；UI 据此生成类型化参数面板）。
struct ParamDescriptor {
    std::string id;        /// 节点类型内唯一，如 "sigma"。
    std::string label;     /// UI 展示名。
    ParamKind kind = ParamKind::Boolean;
    ParamValue defaultValue = false;
    /// Integer/Real 的取值范围；hasRange=false 表示不限。
    bool hasRange = false;
    double minValue = 0.0;
    double maxValue = 0.0;
    /// Enumeration 的合法选项 id 列表（非空）。
    std::vector<std::string> enumOptions;

    /// 有效性：id 非空、defaultValue 与 kind 匹配、枚举选项非空且默认值在选项内、
    /// hasRange 仅用于 Integer/Real 且 min <= max。
    [[nodiscard]] bool valid() const noexcept;
};

/// 一次参数赋值（节点实例内 paramId 唯一）。
struct ParamAssignment {
    std::string paramId;
    ParamValue value;

    [[nodiscard]] friend bool operator==(const ParamAssignment&,
                                         const ParamAssignment&) = default;
};

/// 节点类型描述（调色板/端口签名/参数 schema 的唯一来源）。
struct NodeDescriptor {
    std::string typeId;  /// 目录内唯一，如 "gaussian_blur"。
    std::string displayName;
    std::vector<PortType> inputs;
    std::vector<PortType> outputs;
    std::vector<ParamDescriptor> params;

    [[nodiscard]] bool valid() const noexcept;
};

/// 节点目录：引擎支持的节点类型全集（构建期确定，运行期不变）。
struct NodeCatalog {
    std::vector<NodeDescriptor> nodes;

    [[nodiscard]] bool valid() const noexcept;
};

/// 按 typeId 查目录项；不存在返回 nullptr。
[[nodiscard]] const NodeDescriptor* findNodeDescriptor(const NodeCatalog& catalog,
                                                       const std::string& typeId) noexcept;

/// 节点实例：类型 + 已赋值参数（未赋值的参数取声明默认值）。
struct NodeInstance {
    NodeId id = kInvalidNode;
    std::string typeId;
    std::vector<ParamAssignment> params;

    [[nodiscard]] friend bool operator==(const NodeInstance&, const NodeInstance&) = default;
};

/// 工作流图（DAG）：节点实例 + 连线。合法性由 validateWorkflowGraph 判定。
struct WorkflowGraph {
    std::vector<NodeInstance> nodes;
    std::vector<Connection> connections;

    [[nodiscard]] friend bool operator==(const WorkflowGraph&, const WorkflowGraph&) = default;
};

/// 校验问题类别（issue.message 携带人可读描述；node 为相关节点，无节点上下文时
/// 为 kInvalidNode）。
enum class ValidationIssueKind {
    InvalidNodeId,
    DuplicateNodeId,
    UnknownNodeType,
    PortOutOfRange,
    DirectionMismatch,
    UnknownConnectionNode,
    TypeMismatch,
    MultipleDrivers,
    SelfLoop,
    Cycle,
    DanglingInput,
    BadParam,
};

/// 一次图校验的问题清单条目。
struct ValidationIssue {
    ValidationIssueKind kind = ValidationIssueKind::InvalidNodeId;
    NodeId node = kInvalidNode;
    std::string message;
};

/// 图校验结果：ok 当且仅当 issues 为空。
struct WorkflowValidation {
    bool ok = false;
    std::vector<ValidationIssue> issues;
};

/// 工作流图结构校验（Core 纯逻辑，UI 预检与引擎准入共用的唯一判据）：
/// - 节点：id 非 kInvalidNode、图内唯一、typeId 存在于目录；
/// - 连线：from 为 Output、to 为 Input、两端节点在图内、端口序号在签名内、
///   两端 PortType 一致、无完全重复连线、每个输入至多一条入边、无自环、无环；
/// - 悬空输入拒绝（每个输入端口必须恰有一条入边）；悬空输出允许；
/// - 参数：paramId 在类型声明内、图内不重复赋值、值种类与声明匹配、
///   hasRange 时数值在闭区间内、枚举值在选项内、RealArray 全部有限。
/// 未赋值参数允许（引擎取声明默认值）。
[[nodiscard]] WorkflowValidation validateWorkflowGraph(const WorkflowGraph& graph,
                                                       const NodeCatalog& catalog);

/// 单节点执行统计（滚动窗口均值由实现定义窗口长度，契约只冻结语义）。
struct NodeStats {
    NodeId node = kInvalidNode;
    double lastCostMs = 0.0;          /// 最近一次执行耗时。
    double avgCostMs = 0.0;           /// 滚动窗口平均耗时。
    std::uint64_t executedFrames = 0; /// 会话累计执行次数。

    [[nodiscard]] friend bool operator==(const NodeStats&, const NodeStats&) = default;
};

/// 工作流统计快照（统计通道最新态语义；sequence 通道内单调递增）。
struct WorkflowStats {
    std::uint64_t sequence = 0;
    double endToEndFps = 0.0;           /// 实测端到端帧率（滚动窗口）。
    std::uint64_t processedFrames = 0;  /// 会话累计完成帧。
    std::uint64_t droppedFrames = 0;    /// 过载显式丢弃累计（EXEC-07，禁止静默排队）。
    std::uint32_t inFlight = 0;         /// 当前在飞任务数（有界准入的可观察面）。
    std::vector<NodeStats> nodes;

    [[nodiscard]] friend bool operator==(const WorkflowStats&, const WorkflowStats&) = default;
};

/// 节点中间产物快照（有界保留语义：每节点仅保留最新一幅；像素提交后不可变，
/// 消费方共享所有权）。stride 为字节；Gray8 行按实现对齐，Rgba8 stride >= width*4。
struct NodeOutputSnapshot {
    NodeId node = kInvalidNode;
    PortType format = PortType::Gray8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    /// 该输出对应的源帧序号（与相机帧 sequence 同源，供 UI 对齐显示）。
    std::uint64_t sourceSequence = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;

    [[nodiscard]] bool valid() const noexcept;
};

/// 引擎显式状态集（AGENTS.md "Runtime 与状态模型"；Idle 为已排空的稳定态）。
enum class WorkflowEngineState {
    Idle,
    Running,
    Stopping,
    Failed,
};

[[nodiscard]] const char* toString(WorkflowEngineState state) noexcept;

enum class WorkflowEventKind {
    Started,
    GraphApplied,
    ParamUpdated,
    Info,
    NodeFailed,
    Stopped,
    Failed,
};

/// 引擎事件（事件通道最新态语义；node 仅 NodeFailed 时有效）。
struct WorkflowEvent {
    WorkflowEventKind kind = WorkflowEventKind::Info;
    NodeId node = kInvalidNode;
    std::string message;
    double timestampMs = 0.0;
};

/// start() 的准入结果；admitted 仅表示进入 Running 准入，后续失败经事件通道报告。
struct AdmissionResult {
    bool admitted = false;
    std::string error;
};

}  // namespace rin
