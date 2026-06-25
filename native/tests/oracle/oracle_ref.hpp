// SPDX-License-Identifier: Apache-2.0
#ifndef UAV_ORACLE_REF_HPP
#define UAV_ORACLE_REF_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace uav_oracle {

struct RefFrame {
    std::vector<uint8_t> rgba;
    int    width  = 0;
    int    height = 0;
    int    stride = 0;
    double pts    = 0.0;
};

struct RefColorimetry {
    int colorspace = 2;   // AVCOL_SPC_UNSPECIFIED
    int range      = 0;   // AVCOL_RANGE_UNSPECIFIED
    int transfer   = 2;   // AVCOL_TRC_UNSPECIFIED
    int primaries  = 2;   // AVCOL_PRI_UNSPECIFIED
    int src_pix_fmt = -1;
    int bit_depth   = 8;
};

struct RefAudio {
    std::vector<std::vector<float>> channels;
    int    sample_rate = 0;
    int    channel_count = 0;
};

class Reference {
public:
    Reference() = default;
    ~Reference();
    Reference(const Reference&) = delete;
    Reference& operator=(const Reference&) = delete;

    bool open(const std::string& path);

    bool has_video() const { return video_stream_ >= 0; }
    bool has_audio() const { return audio_stream_ >= 0; }

    std::vector<RefFrame> decode_video(int max_frames);
    RefAudio decode_audio(int out_channels, int out_rate, double min_seconds);

    const RefColorimetry& colorimetry() const { return colorimetry_; }
    const char* last_error() const { return last_error_.c_str(); }

private:
    struct Impl;
    Impl* d_ = nullptr;
    int   video_stream_ = -1;
    int   audio_stream_ = -1;
    RefColorimetry colorimetry_;
    std::string last_error_;
};

const char* colorspace_name(int avcol_spc);
const char* colorrange_name(int avcol_range);
const char* colortrc_name(int avcol_trc);

} // namespace uav_oracle

#endif // UAV_ORACLE_REF_HPP
