// SPDX-License-Identifier: Apache-2.0

#include "oracle_metrics.hpp"

#include <cmath>

namespace uav_oracle {

namespace {
inline double luma601(const uint8_t* px) {
    return 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
}
} // namespace

double psnr_rgb(const uint8_t* a, const uint8_t* b, int w, int h, int stride) {
    if (!a || !b || w <= 0 || h <= 0 || stride < w * 4) return -1.0;
    double mse = 0.0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* ra = a + (size_t)y * stride;
        const uint8_t* rb = b + (size_t)y * stride;
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                double d = (double)ra[x * 4 + c] - (double)rb[x * 4 + c];
                mse += d * d;
            }
        }
    }
    const double n = (double)w * (double)h * 3.0;
    mse /= n;
    if (mse <= 0.0) return kPsnrInf;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

double mean_abs_diff_rgb(const uint8_t* a, const uint8_t* b, int w, int h, int stride) {
    if (!a || !b || w <= 0 || h <= 0 || stride < w * 4) return -1.0;
    double sum = 0.0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* ra = a + (size_t)y * stride;
        const uint8_t* rb = b + (size_t)y * stride;
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                sum += std::fabs((double)ra[x * 4 + c] - (double)rb[x * 4 + c]);
            }
        }
    }
    return sum / ((double)w * (double)h * 3.0);
}

double ssim_luma(const uint8_t* a, const uint8_t* b, int w, int h, int stride) {
    if (!a || !b || w < 8 || h < 8 || stride < w * 4) return -1.0;
    const double L  = 255.0;
    const double C1 = (0.01 * L) * (0.01 * L);
    const double C2 = (0.03 * L) * (0.03 * L);
    const int win = 8;
    const double N = win * win;

    double ssim_sum = 0.0;
    long   win_count = 0;

    for (int by = 0; by + win <= h; by += win) {
        for (int bx = 0; bx + win <= w; bx += win) {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            for (int dy = 0; dy < win; ++dy) {
                const uint8_t* ra = a + (size_t)(by + dy) * stride + (size_t)bx * 4;
                const uint8_t* rb = b + (size_t)(by + dy) * stride + (size_t)bx * 4;
                for (int dx = 0; dx < win; ++dx) {
                    double la = luma601(ra + dx * 4);
                    double lb = luma601(rb + dx * 4);
                    sa  += la;  sb  += lb;
                    saa += la * la; sbb += lb * lb; sab += la * lb;
                }
            }
            double mu_a = sa / N, mu_b = sb / N;
            double var_a = saa / N - mu_a * mu_a;
            double var_b = sbb / N - mu_b * mu_b;
            double cov   = sab / N - mu_a * mu_b;
            double num = (2.0 * mu_a * mu_b + C1) * (2.0 * cov + C2);
            double den = (mu_a * mu_a + mu_b * mu_b + C1) * (var_a + var_b + C2);
            double s = (den != 0.0) ? num / den : 1.0;
            ssim_sum += s;
            ++win_count;
        }
    }
    if (win_count == 0) return -1.0;
    return ssim_sum / (double)win_count;
}

double pearson_corr(const float* xs, const float* ys, size_t n) {
    if (!xs || !ys || n < 2) return 0.0;
    double mx = 0, my = 0;
    for (size_t i = 0; i < n; ++i) { mx += xs[i]; my += ys[i]; }
    mx /= (double)n; my /= (double)n;
    double sxx = 0, syy = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        double dx = xs[i] - mx, dy = ys[i] - my;
        sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
    }
    if (sxx <= 0.0 || syy <= 0.0) {
        return (sxx == 0.0 && syy == 0.0 && mx == my) ? 1.0 : 0.0;
    }
    return sxy / std::sqrt(sxx * syy);
}

double best_lag_corr(const float* xs, const float* ys, size_t n,
                     size_t max_lag, size_t* best_lag) {
    if (!xs || !ys || n < 2) { if (best_lag) *best_lag = 0; return 0.0; }
    double best = -2.0;
    size_t bl = 0;
    for (size_t lag = 0; lag <= max_lag && lag + 1 < n; ++lag) {
        size_t cmp = n - lag;
        double c = pearson_corr(xs, ys + lag, cmp);
        if (c > best) { best = c; bl = lag; }
    }
    if (best < -1.5) best = 0.0;
    if (best_lag) *best_lag = bl;
    return best;
}

double rms(const float* xs, size_t n) {
    if (!xs || n == 0) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += (double)xs[i] * (double)xs[i];
    return std::sqrt(s / (double)n);
}

} // namespace uav_oracle
