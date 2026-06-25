// SPDX-License-Identifier: Apache-2.0

#include "oracle_metrics.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <vector>

namespace uav_oracle {

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "  [metrics-unit] FAIL: %s\n", what);
        ++g_failures;
    }
}

void check_close(double got, double want, double tol, const char* what) {
    if (std::fabs(got - want) > tol) {
        std::fprintf(stderr, "  [metrics-unit] FAIL: %s (got %.6f, want %.6f, tol %.6f)\n",
                     what, got, want, tol);
        ++g_failures;
    }
}

struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    uint32_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(s >> 33); }
    float unit() { return (float)(next() / 4294967296.0) * 2.0f - 1.0f; }
};

} // namespace

int oracle_metrics_selftest() {
    g_failures = 0;

    const int w = 32, h = 24, stride = w * 4;
    std::vector<uint8_t> a((size_t)stride * h);
    {
        Lcg rng(0xABCDEF01);
        for (size_t i = 0; i < a.size(); ++i) a[i] = (uint8_t)(rng.next() & 0xFF);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                a[(size_t)y * stride + x * 4 + 3] = 255;
    }

    {
        double psnr = psnr_rgb(a.data(), a.data(), w, h, stride);
        check(psnr >= kPsnrInf - 1.0, "identical PSNR is the +inf sentinel");
        double ssim = ssim_luma(a.data(), a.data(), w, h, stride);
        check_close(ssim, 1.0, 1e-6, "identical SSIM == 1.0");
        double md = mean_abs_diff_rgb(a.data(), a.data(), w, h, stride);
        check_close(md, 0.0, 1e-9, "identical mean|d| == 0");
    }

    {
        std::vector<uint8_t> base((size_t)stride * h, 0);
        std::vector<uint8_t> plus8((size_t)stride * h, 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 3; ++c) {
                    uint8_t v = (uint8_t)(50 + ((x + y + c) % 20));
                    base[(size_t)y * stride + x * 4 + c] = v;
                    plus8[(size_t)y * stride + x * 4 + c] = (uint8_t)(v + 8);
                }
                base[(size_t)y * stride + x * 4 + 3] = 255;
                plus8[(size_t)y * stride + x * 4 + 3] = 255;
            }
        }
        double psnr = psnr_rgb(base.data(), plus8.data(), w, h, stride);
        double want = 20.0 * std::log10(255.0 / 8.0);
        check_close(psnr, want, 0.01, "off-by-8 PSNR == 20*log10(255/8)");
        double md = mean_abs_diff_rgb(base.data(), plus8.data(), w, h, stride);
        check_close(md, 8.0, 1e-9, "off-by-8 mean|d| == 8");
    }

    {
        const size_t n = 4096;
        std::vector<float> x(n), negx(n), noise(n);
        Lcg rng(0x1357BDF9);
        for (size_t i = 0; i < n; ++i) {
            x[i]    = std::sin(2.0 * M_PI * 7.0 * (double)i / (double)n);
            negx[i] = -x[i];
        }
        Lcg rng2(0x99AA55CC);
        for (size_t i = 0; i < n; ++i) noise[i] = rng2.unit();
        (void)rng;

        check_close(pearson_corr(x.data(), x.data(), n), 1.0, 1e-9, "corr(x,x) == +1");
        check_close(pearson_corr(x.data(), negx.data(), n), -1.0, 1e-9, "corr(x,-x) == -1");
        double cn = pearson_corr(x.data(), noise.data(), n);
        check(std::fabs(cn) < 0.05, "corr(signal, indep noise) ~ 0 (|c|<0.05)");

        std::vector<float> y(n, 0.0f);
        const size_t lag = 5;
        for (size_t i = lag; i < n; ++i) y[i] = x[i - lag];
        size_t found = 999;
        double c = best_lag_corr(x.data(), y.data(), n, 16, &found);
        check(found == lag, "best_lag_corr recovers the injected lag");
        check(c > 0.99, "best_lag_corr at the right lag is ~1");

        check_close(rms(x.data(), n), 1.0 / std::sqrt(2.0), 0.02, "rms(sine) ~ 0.707");
    }

    if (g_failures == 0)
        std::printf("  [metrics-unit] OK (all metric checks passed)\n");
    return g_failures == 0 ? 0 : 1;
}

} // namespace uav_oracle
