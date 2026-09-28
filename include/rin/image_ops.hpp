#pragma once

#include <memory>

#include "rin/image_node.hpp"

namespace rin {

/// M4 算子节点默认工厂（Core 实现，零第三方公开依赖，RULE-01）：
///
/// - "crop"：裁切（M4-03）。ROI（x/y/width/height，构造期定型）对输入图像
///   取子矩形；退化区域（width/height ≤ 0）与越界（x+width > 输入宽等，
///   uint64 运算）在 apply 期抛 std::invalid_argument；输出为紧凑新缓冲。
/// - "downscale"：降分辨率（M4-03）。scale ∈ (0,1]（构造期定型，违者构造抛
///   std::invalid_argument）；输出尺寸 floor(in×scale)，< 1 时 apply 抛
///   std::invalid_argument；nearest / bilinear 数值语义见
///   docs/design/image_workflow_design.md §7（M4-03 冻结公式）。
/// - "source"：返回 nullptr（注入型源节点，执行时由引擎注入相机帧）。
/// - 其他 typeId：抛 std::invalid_argument（M4-04..06 的卷积/直方图/FFT 族
///   随对应工作项扩展；目录声明与工厂能力的偏差显式暴露，不静默）。
///
/// 参数读取经 effectiveParamValue/param* 助手（未赋值取声明默认值）；运行期
/// 直接构造的实例若参数缺失或种类与声明错位，构造抛 std::invalid_argument
/// （buildNodeGraph 转为显式校验问题）。节点输出一律为提交后不可变的紧凑
/// 缓冲（stride = 宽×elementSize）。
std::unique_ptr<IImageNode> makeDefaultImageNode(const NodeDescriptor& descriptor,
                                                 const NodeInstance& instance);

}  // namespace rin
