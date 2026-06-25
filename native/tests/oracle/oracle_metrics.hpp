// SPDX-License-Identifier: Apache-2.0
#ifndef UAV_ORACLE_METRICS_HPP
#define UAV_ORACLE_METRICS_HPP

#include <cstdint>
#include <cstddef>

namespace uav_oracle {

// psnr_rgb returns this sentinel when the two buffers are bit-identical (MSE==0).
constexpr double kPsnrInf = 1e9;

double psnr_rgb(const uint8_t* a, const uint8_t* b, int w, int h, int stride);
double mean_abs_diff_rgb(const uint8_t* a, const uint8_t* b, int w, int h, int stride);
double ssim_luma(const uint8_t* a, const uint8_t* b, int w, int h, int stride);
double pearson_corr(const float* xs, const float* ys, size_t n);
double best_lag_corr(const float* xs, const float* ys, size_t n,
                     size_t max_lag, size_t* best_lag);
double rms(const float* xs, size_t n);

} // namespace uav_oracle

#endif // UAV_ORACLE_METRICS_HPP
