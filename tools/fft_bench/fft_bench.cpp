// M4-01（DEC-012）FFT 基准：三种实现策略在 512x512 与 848x480 灰度图上的
// 单线程 float32 正/逆 2D FFT 实测，并做正确性交叉校验（任何校验失败拒绝出数）。
//
// 对照方：
//   opencv  —— 系统 OpenCV 4.6.0 `cv::dft`（实输入 CCS 打包最短路径）
//   kissfft —— pinned kissfft（KISSFFT_DATATYPE=float，kiss_fftndr 实数 N 维 API）
//   self    —— 全自研迭代 radix-2 复数 FFT（行变换+列变换；非 2 幂尺寸零填充到
//              1024x512，填充后频域截止语义变化，见 DEC-012）
//
// 计时口径（各方从"实图缓冲/频谱缓冲"到其原生频谱/实图的最短路径，不含掩膜乘）：
//   forward  = 实图 -> 频谱。self 含实->复嵌入与填充置零（其算法固有部分）；
//              OpenCV/kissfft 直读实图缓冲，无额外拷贝。
//   inverse  = 频谱 -> 实图，含归一化：OpenCV DFT_SCALE 在内；kissfft 未归一化，
//              显式 1/(H*W) 缩放遍历计入；self 同。
//   输入缓冲在计时外预填；频谱掩膜乘（M4-06 滤波节点）不计入，与选型正交。
//
// 正确性校验：
//   - 三方各自 forward->inverse 往返误差（float 容差 1e-3）；
//   - kissfft 半谱、self（512x512 无填充时）与 OpenCV DFT_COMPLEX_OUTPUT 全复数
//     谱的幅值相对偏差（容差 5e-3）；848x480 下 self 因填充语义不同不参与谱对照。
//
// 用法：fft_bench <iters_per_op>；输出 CSV（RESULT/CHECK/SINK 行），CHECK 失败
// 退出码非 0。方法与构建命令见同目录 README.md。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <kiss_fft.h>
#include <kiss_fftndr.h>

#include <opencv2/core.hpp>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Size2D {
    int w;
    int h;
    const char* label;
};

const Size2D kSizes[] = {
    {512, 512, "512x512"},
    {848, 480, "848x480"},
    // 848x480 零填充的 2 幂目标：量化"库只跑 2 幂尺寸 + 节点填充"策略的成本
    //（self@848x480 即本尺寸工作量，此行用于直接对比 kissfft 填充策略）。
    {1024, 512, "1024x512"},
};

// 确定性伪随机灰度图，值域 [0,1)，行主序。
std::vector<float> MakeImage(int w, int h, std::uint64_t seed) {
    std::vector<float> img(static_cast<std::size_t>(w) * h);
    std::uint64_t state = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (float& v : img) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        v = static_cast<float>((state >> 11) & 0x1FFFFF) / 2097152.0f;
    }
    return img;
}

int NextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

// ---------------------------------------------------------------------------
// 全自研 radix-2（对照实现）
// ---------------------------------------------------------------------------
namespace selffft {

using Cpx = std::complex<float>;

struct Fft1d {
    int n = 0;
    std::vector<int> rev;
    std::vector<Cpx> w;  // exp(±2πi k / n)，k < n/2；inverse 取共轭方向

    void Init(int n_, bool inverse) {
        n = n_;
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        rev.assign(static_cast<std::size_t>(n), 0);
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[static_cast<std::size_t>(i)] = r;
        }
        w.resize(static_cast<std::size_t>(n / 2));
        const double sign = inverse ? 1.0 : -1.0;
        for (int k = 0; k < n / 2; ++k) {
            const double ang = sign * 2.0 * kPi * k / n;
            w[static_cast<std::size_t>(k)] = Cpx(static_cast<float>(std::cos(ang)),
                                                 static_cast<float>(std::sin(ang)));
        }
    }

    void Run(Cpx* a) const {
        for (int i = 0; i < n; ++i)
            if (i < rev[static_cast<std::size_t>(i)])
                std::swap(a[i], a[rev[static_cast<std::size_t>(i)]]);
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1;
            const int step = n / len;
            for (int base = 0; base < n; base += len) {
                for (int j = 0; j < half; ++j) {
                    const Cpx u = a[base + j];
                    const Cpx v = a[base + j + half] *
                                  w[static_cast<std::size_t>(j * step)];
                    a[base + j] = u + v;
                    a[base + j + half] = u - v;
                }
            }
        }
    }
};

// 2D = 行变换 + 列变换。padW/padH 为 2 的幂（非 2 幂输入零填充）。
// 工作缓冲在计划期预分配（与 kissfft cfg / OpenCV Mat 同口径，计时区内无分配）。
struct Fft2d {
    int w = 0, h = 0;        // 原始尺寸
    int padW = 0, padH = 0;  // 2 的幂工作尺寸
    Fft1d row;               // 长度 padW
    Fft1d col;               // 长度 padH
    mutable std::vector<Cpx> rows;    // padH×padW 工作缓冲
    mutable std::vector<Cpx> column;  // padH 列缓冲

    void Init(int w_, int h_, bool inverse) {
        w = w_;
        h = h_;
        padW = NextPow2(w);
        padH = NextPow2(h);
        row.Init(padW, inverse);
        col.Init(padH, inverse);
        rows.assign(static_cast<std::size_t>(padH) * padW, Cpx{});
        column.assign(static_cast<std::size_t>(padH), Cpx{});
    }

    // forward：实图（行主序 w×h）→ 频谱 padH×padW 复数（未归一化）。
    void Forward(const float* img, Cpx* spec) const {
        for (int y = 0; y < padH; ++y) {
            Cpx* line = rows.data() + static_cast<std::size_t>(y) * padW;
            const bool validY = y < h;
            for (int x = 0; x < padW; ++x)
                line[x] = (validY && x < w)
                              ? Cpx(img[static_cast<std::size_t>(y) * w + x], 0.0f)
                              : Cpx(0.0f, 0.0f);
            row.Run(line);
        }
        for (int x = 0; x < padW; ++x) {
            for (int y = 0; y < padH; ++y)
                column[static_cast<std::size_t>(y)] =
                    rows[static_cast<std::size_t>(y) * padW + x];
            col.Run(column.data());
            for (int y = 0; y < padH; ++y)
                spec[static_cast<std::size_t>(y) * padW + x] =
                    column[static_cast<std::size_t>(y)];
        }
    }

    // inverse：频谱 padH×padW 复数（未归一化）→ 实图（w×h，含 1/(padH*padW)）。
    void Inverse(const Cpx* spec, float* img) const {
        for (int y = 0; y < padH; ++y) {
            Cpx* line = rows.data() + static_cast<std::size_t>(y) * padW;
            std::memcpy(line, spec + static_cast<std::size_t>(y) * padW,
                        sizeof(Cpx) * static_cast<std::size_t>(padW));
            row.Run(line);
        }
        const float scale = 1.0f / (static_cast<float>(padH) * padW);
        for (int x = 0; x < padW; ++x) {
            for (int y = 0; y < padH; ++y)
                column[static_cast<std::size_t>(y)] =
                    rows[static_cast<std::size_t>(y) * padW + x];
            col.Run(column.data());
            if (x < w) {
                for (int y = 0; y < h; ++y)
                    img[static_cast<std::size_t>(y) * w + x] =
                        column[static_cast<std::size_t>(y)].real() * scale;
            }
        }
    }
};

}  // namespace selffft

// ---------------------------------------------------------------------------
// kissfft（pinned，float 实数 N 维 API）
// ---------------------------------------------------------------------------
struct KissBackend {
    int w = 0, h = 0;
    int specW = 0;  // w/2 + 1
    kiss_fftndr_cfg fwd = nullptr;
    kiss_fftndr_cfg inv = nullptr;
    std::vector<char> invMem;

    void Init(int w_, int h_) {
        w = w_;
        h = h_;
        // dims[ndims-1] 为做实数半谱的维（须偶数），且为内存最快维：
        // 传 {h, w} => 输入 h×w 行主序实图，输出 h×(w/2+1) 复数半谱。
        int dims[2] = {h, w};
        specW = w / 2 + 1;
        fwd = kiss_fftndr_alloc(dims, 2, 0, nullptr, nullptr);
        size_t len = 0;
        kiss_fftndr_alloc(dims, 2, 1, nullptr, &len);
        invMem.resize(len);
        inv = kiss_fftndr_alloc(dims, 2, 1, invMem.data(), &len);
    }

    void Forward(const float* img, kiss_fft_cpx* spec) const {
        kiss_fftndr(fwd, img, spec);
    }

    // kissfft 未归一化，逆变换后显式 1/(H*W) 缩放遍历（计入计时）。
    void Inverse(const kiss_fft_cpx* spec, float* img) const {
        kiss_fftndri(inv, spec, img);
        const float scale = 1.0f / (static_cast<float>(w) * h);
        const std::size_t n = static_cast<std::size_t>(w) * h;
        for (std::size_t i = 0; i < n; ++i) img[i] *= scale;
    }
};

// ---------------------------------------------------------------------------
// OpenCV（CCS 打包实数路径）
// ---------------------------------------------------------------------------
struct CvBackend {
    int w = 0, h = 0;
    cv::Mat img;   // CV_32FC1 h×w（计时外预填）
    cv::Mat spec;  // CV_32FC1 h×(w/2+1) CCS 打包

    void Init(int w_, int h_, const float* data) {
        w = w_;
        h = h_;
        img = cv::Mat(h, w, CV_32FC1);
        spec = cv::Mat(h, w / 2 + 1, CV_32FC1);
        std::memcpy(img.ptr<float>(), data,
                    sizeof(float) * static_cast<std::size_t>(w) * h);
    }

    void Forward(cv::Mat* outSpec) const { cv::dft(img, *outSpec); }

    void Inverse(const cv::Mat& inSpec, cv::Mat* outImg) const {
        cv::dft(inSpec, *outImg,
                cv::DFT_INVERSE | cv::DFT_SCALE | cv::DFT_REAL_OUTPUT);
    }
};

// ---------------------------------------------------------------------------
// 计时、统计与校验
// ---------------------------------------------------------------------------
struct Stats {
    double minMs = 0, p50Ms = 0, meanMs = 0, p99Ms = 0;
};

template <typename Op>
double TimeOnce(Op&& op) {
    const auto t0 = std::chrono::steady_clock::now();
    op();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

Stats Summarize(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    Stats s;
    s.minMs = samples.front();
    s.p50Ms = samples[samples.size() / 2];
    double sum = 0;
    for (double v : samples) sum += v;
    s.meanMs = sum / static_cast<double>(samples.size());
    s.p99Ms = samples[static_cast<std::size_t>(
        std::min<std::size_t>(samples.size() - 1,
                              static_cast<std::size_t>(samples.size() * 99 / 100)))];
    return s;
}

void PrintResult(const char* backend, const char* size, const char* op,
                 const Stats& s) {
    std::printf("RESULT,%s,%s,%s,%.4f,%.4f,%.4f,%.4f\n", backend, size, op,
                s.minMs, s.p50Ms, s.meanMs, s.p99Ms);
}

int gCheckFailures = 0;

void PrintCheck(const char* name, double worstErr, bool ok) {
    std::printf("CHECK,%s,%.3e,%s\n", name, worstErr, ok ? "PASS" : "FAIL");
    if (!ok) ++gCheckFailures;
}

// 复数谱 -> 幅值图（校验专用，非计时路径）。
std::vector<float> MagnitudeOf(const std::vector<std::complex<float>>& spec) {
    std::vector<float> mag(spec.size());
    for (std::size_t i = 0; i < spec.size(); ++i)
        mag[i] = std::abs(spec[i]);
    return mag;
}

// kissfft 半谱 -> 全复数谱（共轭对称补全，校验专用）。
std::vector<std::complex<float>> HalfToComplex(
    const std::vector<kiss_fft_cpx>& half, int w, int h) {
    std::vector<std::complex<float>> full(static_cast<std::size_t>(w) * h,
                                          {0.0f, 0.0f});
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x <= w / 2; ++x) {
            const auto& c = half[static_cast<std::size_t>(y) * (w / 2 + 1) + x];
            full[static_cast<std::size_t>(y) * w + x] = {c.r, c.i};
        }
        for (int x = w / 2 + 1; x < w; ++x)
            full[static_cast<std::size_t>(y) * w + x] =
                std::conj(full[static_cast<std::size_t>(y) * w + (w - x)]);
    }
    return full;
}

// 幅值谱相对偏差：max|a-b| / max|a|。
double MagnitudeRelErr(const std::vector<float>& a, const std::vector<float>& b) {
    float maxA = 0;
    double worst = 0;
    for (float v : a) maxA = std::max(maxA, v);
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst,
                         std::abs(static_cast<double>(a[i]) - b[i]) / maxA);
    return worst;
}

double MaxAbsDiff(const std::vector<float>& a, const float* b) {
    double worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(a[i]) - b[i]));
    return worst;
}

}  // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 200;
    const int warmups = 10;

    std::printf("# fft_bench iters=%d warmup=%d\n", iters, warmups);
    std::printf("# header: RESULT,backend,size,op,min_ms,p50_ms,mean_ms,p99_ms\n");

    for (const Size2D& sz : kSizes) {
        const int w = sz.w, h = sz.h;
        const std::size_t n = static_cast<std::size_t>(w) * h;
        const std::vector<float> img =
            MakeImage(w, h, 0x5eedULL + static_cast<std::uint64_t>(w) * h);

        // 参考全复数谱（校验专用，OpenCV DFT_COMPLEX_OUTPUT）。
        cv::Mat imgMat(h, w, CV_32FC1, const_cast<float*>(img.data()));
        cv::Mat fullSpec;
        cv::dft(imgMat, fullSpec, cv::DFT_COMPLEX_OUTPUT);
        const std::vector<float> magFull = MagnitudeOf(
            std::vector<std::complex<float>>(fullSpec.ptr<std::complex<float>>(),
                                             fullSpec.ptr<std::complex<float>>() + n));
        double sink = 0;

        // --- OpenCV ---
        {
            CvBackend cvb;
            cvb.Init(w, h, img.data());
            cv::Mat outSpec = cvb.spec.clone();
            cv::Mat outImg(h, w, CV_32FC1);

            cvb.Forward(&outSpec);
            cvb.Inverse(outSpec, &outImg);
            const double rt = MaxAbsDiff(img, outImg.ptr<float>());
            PrintCheck("opencv_roundtrip", rt, rt < 1e-3);

            std::vector<double> f, iv;
            for (int i = 0; i < warmups; ++i) cvb.Forward(&outSpec);
            for (int i = 0; i < iters; ++i)
                f.push_back(TimeOnce([&] { cvb.Forward(&outSpec); }));
            for (int i = 0; i < warmups; ++i) cvb.Inverse(outSpec, &outImg);
            for (int i = 0; i < iters; ++i)
                iv.push_back(TimeOnce([&] { cvb.Inverse(outSpec, &outImg); }));
            sink += outSpec.ptr<float>()[0] + outImg.ptr<float>()[0];
            PrintResult("opencv", sz.label, "forward", Summarize(std::move(f)));
            PrintResult("opencv", sz.label, "inverse", Summarize(std::move(iv)));
        }

        // --- kissfft ---
        {
            KissBackend kiss;
            kiss.Init(w, h);
            std::vector<kiss_fft_cpx> kspec(
                static_cast<std::size_t>(h) * kiss.specW);
            std::vector<float> kout(n);

            kiss.Forward(img.data(), kspec.data());
            kiss.Inverse(kspec.data(), kout.data());
            const double rt = MaxAbsDiff(img, kout.data());
            PrintCheck("kissfft_roundtrip", rt, rt < 1e-3);

            const double specErr = MagnitudeRelErr(
                magFull,
                MagnitudeOf(HalfToComplex(kspec, w, h)));
            PrintCheck("kissfft_vs_opencv_spectrum", specErr, specErr < 5e-3);

            std::vector<double> f, iv;
            for (int i = 0; i < warmups; ++i)
                kiss.Forward(img.data(), kspec.data());
            for (int i = 0; i < iters; ++i)
                f.push_back(TimeOnce(
                    [&] { kiss.Forward(img.data(), kspec.data()); }));
            for (int i = 0; i < warmups; ++i)
                kiss.Inverse(kspec.data(), kout.data());
            for (int i = 0; i < iters; ++i)
                iv.push_back(TimeOnce(
                    [&] { kiss.Inverse(kspec.data(), kout.data()); }));
            sink += kspec[0].r + kout[0];
            PrintResult("kissfft", sz.label, "forward", Summarize(std::move(f)));
            PrintResult("kissfft", sz.label, "inverse", Summarize(std::move(iv)));
        }

        // --- self radix-2 ---
        {
            selffft::Fft2d self;
            self.Init(w, h, false);
            selffft::Fft2d selfInv;
            selfInv.Init(w, h, true);
            std::vector<selffft::Cpx> sspec(
                static_cast<std::size_t>(self.padH) * self.padW);
            std::vector<float> sout(n);

            self.Forward(img.data(), sspec.data());
            selfInv.Inverse(sspec.data(), sout.data());
            const double rt = MaxAbsDiff(img, sout.data());
            PrintCheck("self_roundtrip", rt, rt < 1e-3);

            if (self.padW == w && self.padH == h) {  // 无填充才可谱对照
                std::vector<std::complex<float>> selfFull(sspec.begin(),
                                                          sspec.end());
                const double err =
                    MagnitudeRelErr(magFull, MagnitudeOf(selfFull));
                PrintCheck("self_vs_opencv_spectrum", err, err < 5e-3);
            }

            std::vector<double> f, iv;
            for (int i = 0; i < warmups; ++i)
                self.Forward(img.data(), sspec.data());
            for (int i = 0; i < iters; ++i)
                f.push_back(TimeOnce(
                    [&] { self.Forward(img.data(), sspec.data()); }));
            for (int i = 0; i < warmups; ++i)
                selfInv.Inverse(sspec.data(), sout.data());
            for (int i = 0; i < iters; ++i)
                iv.push_back(TimeOnce(
                    [&] { selfInv.Inverse(sspec.data(), sout.data()); }));
            sink += sspec[0].real() + sout[0];
            PrintResult("self_radix2", sz.label, "forward",
                        Summarize(std::move(f)));
            PrintResult("self_radix2", sz.label, "inverse",
                        Summarize(std::move(iv)));
        }

        std::printf("SINK,%s,%.6f\n", sz.label, sink);
    }

    if (gCheckFailures != 0) {
        std::printf("# %d correctness check(s) FAILED\n", gCheckFailures);
        return 1;
    }
    std::printf("# all correctness checks passed\n");
    return 0;
}
