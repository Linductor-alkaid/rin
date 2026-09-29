#pragma once

#include "rin/workflow_types.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rin::workflow_catalog {

/// M4 节点目录构建（两引擎共享的单一事实源，DEC-013）：source / crop /
/// downscale / grayify / gaussian_blur / conv_kernel / hist_eq / fft_lowpass /
/// fft_highpass / fft_bandpass。签名与参数 schema 按 M5-08 假目录冻结基线
/// （image_workflow_design.md §6，schema 演进随算子工作项双向落盘）；假引擎
/// （makeDefaultFakeCatalog）与真引擎默认目录均委托本函数，算子数值语义由
/// M4 真实现提供（假引擎只产出合成图案）。
[[nodiscard]] inline NodeCatalog makeDefaultImageNodeCatalog() {
    NodeCatalog catalog;

    NodeDescriptor source;
    source.typeId = "source";
    source.displayName = "相机源";
    source.outputs = {PortType::Rgba8};
    catalog.nodes.push_back(std::move(source));

    NodeDescriptor crop;
    crop.typeId = "crop";
    crop.displayName = "裁切";
    crop.inputs = {PortType::Rgba8};
    crop.outputs = {PortType::Rgba8};
    ParamDescriptor cropX;
    cropX.id = "x";
    cropX.label = "X";
    cropX.kind = ParamKind::Integer;
    cropX.defaultValue = static_cast<std::int64_t>(0);
    cropX.hasRange = true;
    cropX.minValue = 0.0;
    cropX.maxValue = 4096.0;
    ParamDescriptor cropY = cropX;
    cropY.id = "y";
    cropY.label = "Y";
    ParamDescriptor cropW = cropX;
    cropW.id = "width";
    cropW.label = "宽";
    cropW.defaultValue = static_cast<std::int64_t>(64);
    ParamDescriptor cropH = cropW;
    cropH.id = "height";
    cropH.label = "高";
    crop.params = {cropX, cropY, cropW, cropH};
    catalog.nodes.push_back(std::move(crop));

    NodeDescriptor downscale;
    downscale.typeId = "downscale";
    downscale.displayName = "降分辨率";
    downscale.inputs = {PortType::Rgba8};
    downscale.outputs = {PortType::Rgba8};
    ParamDescriptor interpolation;
    interpolation.id = "interpolation";
    interpolation.label = "插值";
    interpolation.kind = ParamKind::Enumeration;
    interpolation.defaultValue = std::string("nearest");
    interpolation.enumOptions = {"nearest", "bilinear"};
    ParamDescriptor scale;
    scale.id = "scale";
    scale.label = "缩放";
    scale.kind = ParamKind::Real;
    scale.defaultValue = 0.5;
    scale.hasRange = true;
    scale.minValue = 0.1;
    scale.maxValue = 1.0;
    downscale.params = {interpolation, scale};
    catalog.nodes.push_back(std::move(downscale));

    NodeDescriptor grayify;
    grayify.typeId = "grayify";
    grayify.displayName = "灰度化";
    grayify.inputs = {PortType::Rgba8};
    grayify.outputs = {PortType::Gray8};
    catalog.nodes.push_back(std::move(grayify));

    NodeDescriptor blur;
    blur.typeId = "gaussian_blur";
    blur.displayName = "高斯模糊";
    blur.inputs = {PortType::Gray8};
    blur.outputs = {PortType::Gray8};
    ParamDescriptor radius;
    radius.id = "radius";
    radius.label = "半径";
    radius.kind = ParamKind::Integer;
    radius.defaultValue = static_cast<std::int64_t>(3);
    radius.hasRange = true;
    radius.minValue = 1.0;
    radius.maxValue = 10.0;
    ParamDescriptor sigma;
    sigma.id = "sigma";
    sigma.label = "sigma";
    sigma.kind = ParamKind::Real;
    sigma.defaultValue = 1.5;
    sigma.hasRange = true;
    sigma.minValue = 0.0;
    sigma.maxValue = 10.0;
    blur.params = {radius, sigma};
    catalog.nodes.push_back(std::move(blur));

    NodeDescriptor conv;
    conv.typeId = "conv_kernel";
    conv.displayName = "自定义卷积";
    conv.inputs = {PortType::Gray8};
    conv.outputs = {PortType::Gray8};
    ParamDescriptor kernelSize;
    kernelSize.id = "size";
    kernelSize.label = "核尺寸";
    kernelSize.kind = ParamKind::Enumeration;
    kernelSize.defaultValue = std::string("3");
    kernelSize.enumOptions = {"1", "3", "5"};
    ParamDescriptor kernel;
    kernel.id = "kernel";
    kernel.label = "卷积核";
    kernel.kind = ParamKind::RealArray;
    // 3x3 单位核（行主序），中心为 1。
    kernel.defaultValue = std::vector<double>{0, 0, 0, 0, 1, 0, 0, 0, 0};
    // M4-04 冻结的边界填充策略（image_workflow_design.md §6/§7）。
    ParamDescriptor border;
    border.id = "border";
    border.label = "边界";
    border.kind = ParamKind::Enumeration;
    border.defaultValue = std::string("clamp");
    border.enumOptions = {"clamp", "reflect", "zero"};
    conv.params = {kernelSize, kernel, border};
    catalog.nodes.push_back(std::move(conv));

    NodeDescriptor histEq;
    histEq.typeId = "hist_eq";
    histEq.displayName = "直方图均衡";
    histEq.inputs = {PortType::Gray8};
    histEq.outputs = {PortType::Gray8};
    catalog.nodes.push_back(std::move(histEq));

    NodeDescriptor fftLow;
    fftLow.typeId = "fft_lowpass";
    fftLow.displayName = "FFT 低通";
    fftLow.inputs = {PortType::Gray8};
    fftLow.outputs = {PortType::Gray8};
    ParamDescriptor lowCutoff;
    lowCutoff.id = "cutoff";
    lowCutoff.label = "截止";
    lowCutoff.kind = ParamKind::Real;
    lowCutoff.defaultValue = 0.2;
    lowCutoff.hasRange = true;
    lowCutoff.minValue = 0.0;
    lowCutoff.maxValue = 1.0;
    fftLow.params = {lowCutoff};
    catalog.nodes.push_back(std::move(fftLow));

    NodeDescriptor fftHigh;
    fftHigh.typeId = "fft_highpass";
    fftHigh.displayName = "FFT 高通";
    fftHigh.inputs = {PortType::Gray8};
    fftHigh.outputs = {PortType::Gray8};
    fftHigh.params = {lowCutoff};
    catalog.nodes.push_back(std::move(fftHigh));

    NodeDescriptor fftBand;
    fftBand.typeId = "fft_bandpass";
    fftBand.displayName = "FFT 带通";
    fftBand.inputs = {PortType::Gray8};
    fftBand.outputs = {PortType::Gray8};
    ParamDescriptor bandLow;
    bandLow.id = "lowCut";
    bandLow.label = "下限截止";
    bandLow.kind = ParamKind::Real;
    bandLow.defaultValue = 0.2;
    bandLow.hasRange = true;
    bandLow.minValue = 0.0;
    bandLow.maxValue = 1.0;
    ParamDescriptor bandHigh = bandLow;
    bandHigh.id = "highCut";
    bandHigh.label = "上限截止";
    bandHigh.defaultValue = 0.6;
    fftBand.params = {bandLow, bandHigh};
    catalog.nodes.push_back(std::move(fftBand));

    return catalog;
}

}  // namespace rin::workflow_catalog
