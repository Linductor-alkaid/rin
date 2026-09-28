// M5-02 工作台导航壳纯逻辑与主题令牌扩展测试（独立验证；apps/viewer/navigation.hpp
// NavigationState/页面元数据表、apps/viewer/viewer_theme.hpp 的 workflowStateColor
// 与 PortTypeTokens、DEC-014 工作台信息架构）。
//
// 覆盖（实现自述契约，navigation.hpp:30-84、viewer_theme.hpp:110-153）：
// - 元数据表完整性：kWorkbenchPageCount == 4；表序与枚举声明序一致
//   （kWorkbenchPages[i].page == (WorkbenchPage)i，锁定 workbenchPageIndex 的
//   "值即表序"前提）；四枚举各恰出现一次；id/label 非空且互不相同，id 稳定为
//   "preview"/"pose"/"workflow"/"settings"（navigation.hpp:46 自述由单测锁定）；
//   icon 非零且互不相同（同图标会破坏导航栏可辨识性）；
// - workbenchPageIndex：逐枚举 0..3；越界/负值归 0（首页，navigation.hpp:57）；
//   constexpr 可用（编译期 static_assert）；
// - workbenchPageId：与对应表项 id 一致；越界值归表 0 项（"preview"）；constexpr
//   可用（navigation.hpp:64，编译期指针一致性 static_assert）；
// - NavigationState：初值 Preview、selectedIndex()==0；4x4 起始页×目标页切换
//   矩阵（异页返回 true 且 current 变为目标、同页 no-op 返回 false 且状态不变，
//   含初态自选 Preview 的 no-op）；selectedIndex 与表双向一致
//   （kWorkbenchPages[selectedIndex()].page == current）；往返切换复位；
//   switchTo 范围外枚举值经 workbenchPageIndex 归一为首页（自首页请求时归一
//   结果==当前页，按同页 no-op 语义返回 false；自任意非首页请求返回 true，
//   current 落在合法 Preview 而非非法值——随后 switchTo(Preview) 返回 false，
//   选中项与 id 均指向表 0 项，navigation.hpp:68-82 修订契约）；
// - 值语义与 DEC-014 结构契约：可拷贝（拷贝后 current 保持、改拷贝不影响原
//   状态）；sizeof(NavigationState) == sizeof(WorkbenchPage)（锁定"只拥有当前
//   页"——导航不拥有任何页面内状态的结构保证，navigation.hpp:7-9）；外部页面
//   状态见证结构在完整切换矩阵下逐位不变（切页路径不复位页面状态）；
// - viewer_theme 令牌映射：workflowStateColor 逐状态（Running→success、
//   Stopping→warning、Failed→destructive、Idle→fgSubtle）与 dark() 对应令牌
//   r/g/b/a 逐字段一致；portTypes()/portTypeColor 的 Gray8/Rgba8 逐字段一致、
//   两色不透明（a==1）且互不相同；四状态映射结果互不相同（状态→颜色信息
//   不丢失，色彩编码才有意义）。
//
// 范围与限制（如实说明）：composeNavRail（navigation.hpp:95-176）与
// composeWorkflowShell（workflow_shell.hpp）等 EUI-NEO 组装路径需要活动 EUI
// 运行时与窗口，无法 headless 单测——布局、选中态视觉、点击回调派发与图标
// 字体渲染归真机回归（navigation.hpp:86 自述视觉验收归 M5-07）；本测试只覆盖
// 其依赖的纯逻辑（页面模型、元数据、选中序号）与令牌取值。
//
// DOD-02 适用性说明：NavigationState 是 UI 线程独占的普通值状态，主题映射为
// 纯函数（无 Executor 任务/队列/取消/超时/shutdown 语义），并发矩阵不适用；
// 不产生跨上下文状态，无关闭路径（切页不触碰相机服务生命周期，navigation.hpp:10-11）。
//
// 构建注记：本测试包含 apps/viewer/navigation.hpp（经 viewer_theme.hpp 引入
// EUI-NEO 公开头）与 viewer_theme.hpp；仅使用其状态类型与 rin 公开枚举，不链接
// EUI-NEO 库——未引用的 inline 组装函数不产生外部符号依赖。头文件包含路径经
// eui_neo 目标的用法属性注入（见 tests/CMakeLists.txt，同 test_pose_view_state
// 先例）。颜色按 r/g/b/a 浮点精确比较是安全的：两侧均为同一编译期常量表
// （dark()/portTypes() 的 static 常量）按值复制，逐位相同。
#include "test_util.hpp"

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "navigation.hpp"
#include "viewer_theme.hpp"

namespace {

using viewer::kWorkbenchPageCount;
using viewer::kWorkbenchPages;
using viewer::NavigationState;
using viewer::WorkbenchPage;
using viewer::workbenchPageId;
using viewer::workbenchPageIndex;
using rin::PortType;
using rin::WorkflowEngineState;
using viewer::theme::dark;
using viewer::theme::portTypeColor;
using viewer::theme::portTypes;
using viewer::theme::workflowStateColor;

constexpr WorkbenchPage kAllPages[] = {
    WorkbenchPage::Preview, WorkbenchPage::Pose, WorkbenchPage::Workflow,
    WorkbenchPage::Settings};

bool colorEquals(const eui::Color& lhs, const eui::Color& rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

/// 两色至少一个通道不同（互异的最弱充分条件）。
bool colorDistinct(const eui::Color& lhs, const eui::Color& rhs) {
    return lhs.r != rhs.r || lhs.g != rhs.g || lhs.b != rhs.b || lhs.a != rhs.a;
}

/// 表内某页的元数据序号（按 page 字段线性查找，独立于 workbenchPageIndex 的
/// "值即表序"实现，用于交叉验证）。
int tableIndexOf(WorkbenchPage page) {
    for (int i = 0; i < kWorkbenchPageCount; ++i) {
        if (kWorkbenchPages[i].page == page) {
            return i;
        }
    }
    return -1;
}

/// 页面内状态见证（DEC-014：各页 UI 状态由页面自己持有，导航不复位/不触碰）。
struct PageStateWitness {
    std::uint32_t scroll[4] = {7, 8, 9, 10};
    std::uint64_t counters[4] = {100, 200, 300, 400};
    bool dirty[4] = {true, false, true, false};
};

}  // namespace

// --- 编译期契约：页数与索引归一化在常量求值下成立 ---
static_assert(kWorkbenchPageCount == 4, "四页全集");
static_assert(viewer::workbenchPageIndex(WorkbenchPage::Preview) == 0);
static_assert(viewer::workbenchPageIndex(WorkbenchPage::Pose) == 1);
static_assert(viewer::workbenchPageIndex(WorkbenchPage::Workflow) == 2);
static_assert(viewer::workbenchPageIndex(WorkbenchPage::Settings) == 3);
static_assert(viewer::workbenchPageIndex(static_cast<WorkbenchPage>(-1)) == 0);
static_assert(viewer::workbenchPageIndex(static_cast<WorkbenchPage>(42)) == 0);
static_assert(viewer::workbenchPageId(WorkbenchPage::Pose) == viewer::kWorkbenchPages[1].id);
static_assert(viewer::workbenchPageId(static_cast<WorkbenchPage>(7)) ==
              viewer::kWorkbenchPages[0].id);

int main() {
    // --- 1) 元数据表完整性：页数、表序=声明序、四枚举双射、字段非空唯一 ---
    {
        RIN_CHECK_EQ(kWorkbenchPageCount, 4);

        // 表序即枚举声明序（workbenchPageIndex 的"值即表序"前提）。
        for (int i = 0; i < kWorkbenchPageCount; ++i) {
            RIN_CHECK_MSG(kWorkbenchPages[i].page == static_cast<WorkbenchPage>(i),
                          "kWorkbenchPages 表序与枚举声明序一致");
        }

        // 四枚举各恰出现一次；按 page 线性查找的表序 == workbenchPageIndex 结果。
        for (const WorkbenchPage page : kAllPages) {
            int occurrences = 0;
            for (int i = 0; i < kWorkbenchPageCount; ++i) {
                if (kWorkbenchPages[i].page == page) {
                    ++occurrences;
                }
            }
            RIN_CHECK_MSG(occurrences == 1, "每个页面在元数据表中恰出现一次");
            RIN_CHECK_EQ(tableIndexOf(page), workbenchPageIndex(page));
        }

        // id/label 非空；id 稳定（compose 元素 id / 未来状态键前缀）；两者唯一。
        const char* expectedIds[kWorkbenchPageCount] = {"preview", "pose", "workflow",
                                                        "settings"};
        for (int i = 0; i < kWorkbenchPageCount; ++i) {
            RIN_CHECK_MSG(kWorkbenchPages[i].id != nullptr, "id 非空指针");
            RIN_CHECK_MSG(kWorkbenchPages[i].id[0] != '\0', "id 非空串");
            RIN_CHECK_MSG(kWorkbenchPages[i].label != nullptr, "label 非空指针");
            RIN_CHECK_MSG(kWorkbenchPages[i].label[0] != '\0', "label 非空串");
            RIN_CHECK_MSG(std::strcmp(kWorkbenchPages[i].id, expectedIds[i]) == 0,
                          "id 稳定（preview/pose/workflow/settings）");
            RIN_CHECK_MSG(kWorkbenchPages[i].icon != 0, "icon 非零 codepoint");
            for (int j = i + 1; j < kWorkbenchPageCount; ++j) {
                RIN_CHECK_MSG(std::strcmp(kWorkbenchPages[i].id, kWorkbenchPages[j].id) != 0,
                              "id 互不相同");
                RIN_CHECK_MSG(std::strcmp(kWorkbenchPages[i].label,
                                          kWorkbenchPages[j].label) != 0,
                              "label 互不相同");
                RIN_CHECK_MSG(kWorkbenchPages[i].icon != kWorkbenchPages[j].icon,
                              "icon 互不相同");
            }
        }
    }

    // --- 2) workbenchPageIndex / workbenchPageId：逐枚举与越界归一 ---
    {
        for (const WorkbenchPage page : kAllPages) {
            const int index = workbenchPageIndex(page);
            RIN_CHECK(index == tableIndexOf(page));
            RIN_CHECK(std::strcmp(workbenchPageId(page), kWorkbenchPages[index].id) == 0);
        }
        // 越界/负值归 0（首页）；id 随之取表 0 项。编译期版本见文件尾 static_assert。
        RIN_CHECK(workbenchPageIndex(static_cast<WorkbenchPage>(kWorkbenchPageCount)) == 0);
        RIN_CHECK(workbenchPageIndex(static_cast<WorkbenchPage>(99)) == 0);
        RIN_CHECK(workbenchPageIndex(static_cast<WorkbenchPage>(-1)) == 0);
        RIN_CHECK(std::strcmp(workbenchPageId(static_cast<WorkbenchPage>(99)),
                              kWorkbenchPages[0].id) == 0);
    }

    // --- 3) NavigationState：初值、结构契约（只拥有当前页）、聚合初始化 ---
    {
        const NavigationState nav;
        RIN_CHECK(nav.current == WorkbenchPage::Preview);
        RIN_CHECK_EQ(nav.selectedIndex(), 0);
        RIN_CHECK(kWorkbenchPages[nav.selectedIndex()].page == nav.current);

        // DEC-014"导航只拥有当前页"的结构锁定：除 current 外无其他数据成员。
        static_assert(std::is_trivially_copyable_v<NavigationState>);
        static_assert(sizeof(NavigationState) == sizeof(WorkbenchPage));
        RIN_CHECK(std::is_trivially_copyable_v<NavigationState>);

        // current 是聚合首成员（含默认成员初始化器的聚合仍可按成员初始化）。
        constexpr NavigationState aggregate{WorkbenchPage::Settings};
        static_assert(aggregate.current == WorkbenchPage::Settings);
    }

    // --- 4) 切换矩阵：4 起始页 × 4 目标页（异页切换 / 同页 no-op）---
    {
        for (int s = 0; s < kWorkbenchPageCount; ++s) {
            for (int t = 0; t < kWorkbenchPageCount; ++t) {
                NavigationState nav;
                // 从初态 Preview 到起始页（s==0 时即无切换，起始即初态）。
                const bool reachedStart = nav.switchTo(kAllPages[s]);
                RIN_CHECK_MSG(reachedStart == (s != 0), "异页首切返回 true / 同页 false");
                RIN_CHECK(nav.current == kAllPages[s]);

                const bool switched = nav.switchTo(kAllPages[t]);
                RIN_CHECK_MSG(switched == (t != s), "切页返回是否发生切换（同页 no-op）");
                RIN_CHECK(nav.current == kAllPages[t]);
                RIN_CHECK_EQ(nav.selectedIndex(), t);
                RIN_CHECK(kWorkbenchPages[nav.selectedIndex()].page == nav.current);
                RIN_CHECK(std::strcmp(kWorkbenchPages[nav.selectedIndex()].id,
                                      workbenchPageId(nav.current)) == 0);
            }
        }
    }

    // --- 5) switchTo 范围外枚举值归一为首页（navigation.hpp:68-82 修订契约）---
    {
        // 覆盖刚越过末尾、远端正数、-1 与远端负值四类范围外代表值。
        const WorkbenchPage outOfRange[] = {
            static_cast<WorkbenchPage>(kWorkbenchPageCount),
            static_cast<WorkbenchPage>(99),
            static_cast<WorkbenchPage>(-1),
            static_cast<WorkbenchPage>(-42),
        };

        for (const WorkbenchPage bogus : outOfRange) {
            // 自初态（Preview）请求范围外页：归一结果 == 当前页 → 按同页 no-op
            // 语义返回 false，状态不变。
            NavigationState atHome;
            RIN_CHECK_MSG(!atHome.switchTo(bogus),
                          "范围外页自首页请求归一为 Preview，同页 no-op 返回 false");
            RIN_CHECK(atHome.current == WorkbenchPage::Preview);
            RIN_CHECK_EQ(atHome.selectedIndex(), 0);

            // 自任意非首页请求范围外页：归一为 Preview，返回 true；current 是
            // 合法的 Preview 而非非法值——随后 switchTo(Preview) 返回 false，
            // 选中项与 id 均指向表 0 项。
            for (int s = 1; s < kWorkbenchPageCount; ++s) {
                NavigationState nav;
                RIN_CHECK(nav.switchTo(kAllPages[s]));
                const bool switched = nav.switchTo(bogus);
                RIN_CHECK_MSG(switched, "范围外页自非首页请求归一为首页并发生切换");
                RIN_CHECK_MSG(nav.current == WorkbenchPage::Preview,
                              "current 归一为合法首页，永不携带非法值");
                RIN_CHECK_EQ(nav.selectedIndex(), 0);
                RIN_CHECK(std::strcmp(workbenchPageId(nav.current), "preview") == 0);
                RIN_CHECK(!nav.switchTo(WorkbenchPage::Preview));
                RIN_CHECK(nav.current == WorkbenchPage::Preview);
            }
        }
    }

    // --- 6) 往返切换、值语义拷贝、页面状态见证（DEC-014 切页不复位页面状态）---
    {
        // 往返：Preview→Pose→Preview 与 Settings→Workflow→Settings。
        NavigationState nav;
        RIN_CHECK(nav.switchTo(WorkbenchPage::Pose));
        RIN_CHECK(nav.current == WorkbenchPage::Pose);
        RIN_CHECK(nav.switchTo(WorkbenchPage::Preview));
        RIN_CHECK(nav.current == WorkbenchPage::Preview);
        RIN_CHECK_EQ(nav.selectedIndex(), 0);

        nav.switchTo(WorkbenchPage::Settings);
        RIN_CHECK(nav.switchTo(WorkbenchPage::Workflow));
        RIN_CHECK(nav.switchTo(WorkbenchPage::Settings));
        RIN_CHECK(nav.current == WorkbenchPage::Settings);

        // 拷贝语义：拷贝携带 current；改拷贝不影响原状态。
        const NavigationState original = nav;
        NavigationState copy = original;
        RIN_CHECK(copy.current == WorkbenchPage::Settings);
        RIN_CHECK(copy.switchTo(WorkbenchPage::Preview));
        RIN_CHECK(copy.current == WorkbenchPage::Preview);
        RIN_CHECK(original.current == WorkbenchPage::Settings);

        // 页面状态见证：完整切换矩阵前后逐位一致（导航不触碰页面内状态）。
        PageStateWitness before;
        for (int s = 0; s < kWorkbenchPageCount; ++s) {
            for (int t = 0; t < kWorkbenchPageCount; ++t) {
                NavigationState walker;
                walker.switchTo(kAllPages[s]);
                walker.switchTo(kAllPages[t]);
            }
        }
        PageStateWitness after;
        RIN_CHECK(std::memcmp(&before, &after, sizeof(PageStateWitness)) == 0);
    }

    // --- 7) workflowStateColor：逐状态与 dark() 令牌逐字段一致、四结果互异 ---
    {
        const auto& tokens = dark();
        RIN_CHECK(colorEquals(workflowStateColor(WorkflowEngineState::Running),
                              tokens.success));
        RIN_CHECK(colorEquals(workflowStateColor(WorkflowEngineState::Stopping),
                              tokens.warning));
        RIN_CHECK(colorEquals(workflowStateColor(WorkflowEngineState::Failed),
                              tokens.destructive));
        RIN_CHECK(colorEquals(workflowStateColor(WorkflowEngineState::Idle),
                              tokens.fgSubtle));

        // 状态→颜色信息不丢失：四个状态的颜色互不相同（色彩编码才有辨识度）。
        const eui::Color running = workflowStateColor(WorkflowEngineState::Running);
        const eui::Color stopping = workflowStateColor(WorkflowEngineState::Stopping);
        const eui::Color failed = workflowStateColor(WorkflowEngineState::Failed);
        const eui::Color idle = workflowStateColor(WorkflowEngineState::Idle);
        RIN_CHECK(colorDistinct(running, stopping));
        RIN_CHECK(colorDistinct(running, failed));
        RIN_CHECK(colorDistinct(running, idle));
        RIN_CHECK(colorDistinct(stopping, failed));
        RIN_CHECK(colorDistinct(stopping, idle));
        RIN_CHECK(colorDistinct(failed, idle));
    }

    // --- 8) 端口类型令牌：逐字段一致、不透明、Gray8/Rgba8 互异 ---
    {
        const auto& ports = portTypes();
        RIN_CHECK(colorEquals(portTypeColor(PortType::Gray8), ports.gray8));
        RIN_CHECK(colorEquals(portTypeColor(PortType::Rgba8), ports.rgba8));

        // 不透明（用于端口/连线实色反馈）。
        RIN_CHECK_EQ(ports.gray8.a, 1.0f);
        RIN_CHECK_EQ(ports.rgba8.a, 1.0f);

        // 两类型色互异（类型兼容性反馈的前提）。
        RIN_CHECK(colorDistinct(ports.gray8, ports.rgba8));
        RIN_CHECK(colorDistinct(portTypeColor(PortType::Gray8),
                                portTypeColor(PortType::Rgba8)));
    }

    return rin_test::exitStatus();
}
