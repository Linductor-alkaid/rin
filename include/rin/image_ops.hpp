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
/// - "gaussian_blur"：高斯模糊（M4-04）。Gray8；radius（Integer，[1,10]）与
///   sigma（Real，[0,10]）构造期定型（越界构造抛 std::invalid_argument）；
///   一维核 K = 2·radius+1 归一化，可分离两趟 + 固定 clamp 边界，sigma=0 为
///   δ 核恒等输出；数值语义见设计文档 §7（M4-04 冻结公式）。
/// - "conv_kernel"：自定义卷积（M4-04）。Gray8；size（Enumeration "1"|"3"|"5"）、
///   kernel（RealArray 行主序 K² 个系数，长度不符构造抛 std::invalid_argument）、
///   border（Enumeration clamp|reflect|zero）构造期定型；相关语义（核不翻转），
///   double 累加 round-half-up 饱和量化；数值语义见设计文档 §7。
/// - "grayify"：灰度化（M4-05）。Rgba8 → Gray8；BT.601 定点亮度（RGBA 亮度域
///   唯一冻结公式）Y = (77·R + 150·G + 29·B + 128) >> 8，alpha 不参与；数值语义
///   见设计文档 §7。
/// - "hist_eq"：直方图均衡（M4-05）。Gray8；cdf_min 映射 + 整数 round-half-up，
///   常值图恒等输出；数值语义见设计文档 §7。
/// - "fft_lowpass"/"fft_highpass"/"fft_bandpass"：FFT 滤波族（M4-06）。Gray8；
///   cutoff / lowCut / highCut（Real，归一化频率 ∈ [0,1]，违者构造抛
///   std::invalid_argument；带通 lowCut > highCut 同样构造拒绝）构造期定型；
///   内部零填充到 2 幂（宽高各自、下界 2），理想锐截止掩膜按归一化频率在填充
///   分辨率上构造（高通 = 低通补、带通 = 低通掩膜差），IFFT 显式 1/(padW·padH)
///   归一化后裁回原尺寸、round-half-up 饱和量化；数值语义见设计文档 §7
///   （DEC-012：FFT 后端为 pinned kissfft float，实现私有链接）。
/// - "source"：返回 nullptr（注入型源节点，执行时由引擎注入相机帧）。
/// - "viewer"：监看器恒等透传（M11/DEC-021）。Any→Any 单输入单输出，apply
///   返回输入本身（共享像素零拷贝）；引擎按节点照常发布产物快照，UI 在节点
///   内嵌预览窗消费；无参数。
/// - 其他 typeId：抛 std::invalid_argument（M4 目录类型已全部实现，此为未知
///   类型的显式暴露路径，不静默）。
///
/// 参数读取经 effectiveParamValue/param* 助手（未赋值取声明默认值）；运行期
/// 直接构造的实例若参数缺失或种类与声明错位，构造抛 std::invalid_argument
/// （buildNodeGraph 转为显式校验问题）。节点输出一律为提交后不可变的紧凑
/// 缓冲（stride = 宽×elementSize）。
std::unique_ptr<IImageNode> makeDefaultImageNode(const NodeDescriptor& descriptor,
                                                 const NodeInstance& instance);

}  // namespace rin
