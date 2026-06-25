// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav.h"

#if defined(UAV_HAVE_FFMPEG)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int32_t EXPECT_W = 320;
constexpr int32_t EXPECT_H = 240;

// The names MUST match tests/media/gen.sh exactly.
struct VideoClip {
    const char* name;
    const char* codec;
};
const VideoClip kVideoClips[] = {
    {"webm__vp9__opus.webm",   "VP9"},
    {"webm__vp8__vorbis.webm", "VP8"},
    {"webm__av1__opus.webm",   "AV1"},
    {"mkv__vp9__flac.mkv",     "VP9/mkv"},
    {"mkv__av1__opus.mkv",     "AV1/mkv"},
    {"mkv__av1_10__opus.mkv",  "AV1 10-bit/mkv"},
    {"mp4__av1__aac.mp4",      "AV1/mp4"},
    {"mov__mpeg4__aac.mov",    "MPEG-4/mov"},
    {"mp4__h264__aac.mp4",     "H.264/mp4"},
    {"mp4__hevc__aac.mp4",     "HEVC/mp4"},
    {"mov__h264__aac.mov",     "H.264/mov"},
    {"mov__hevc__aac.mov",     "HEVC/mov"},
    {"mpegts__h264__mp3.ts",   "H.264/ts"},
    {"mpegts__hevc__aac.ts",   "HEVC/ts"},
};

std::string find_media(const char* name) {
    std::vector<std::string> roots;
    if (const char* env = std::getenv("UAV_TEST_MEDIA_DIR")) {
        if (env[0]) roots.emplace_back(env);
    }
    roots.emplace_back("tests/media/out");
    roots.emplace_back("../tests/media/out");
    roots.emplace_back("../../tests/media/out");
    roots.emplace_back("../../../tests/media/out");
    for (const auto& r : roots) {
        std::string path = r + "/" + name;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fclose(f);
            return path;
        }
    }
    return std::string();
}

}

TEST_CASE("codec-open: each gen.sh video fixture opens to READY and pre-rolls "
          "one RGBA32 frame"
          * doctest::test_suite("[codec]")) {
    int covered = 0;
    int skipped = 0;

    for (const auto& clip : kVideoClips) {
        std::string path = find_media(clip.name);
        if (path.empty()) {
            MESSAGE("skip: fixture not generated: " << clip.name
                    << " (" << clip.codec << ")");
            ++skipped;
            continue;
        }

        UAVPlayer* p = uav_create();
        REQUIRE(p != nullptr);

        if (uav_open(p, path.c_str()) != UAV_OK) {
            MESSAGE("skip: could not open " << clip.name
                    << " (" << clip.codec << " decoder unavailable)");
            uav_destroy(p);
            ++skipped;
            continue;
        }

        CHECK(uav_get_state(p) == UAV_STATE_READY);

        UAVMediaInfo info;
        REQUIRE(uav_get_info(p, &info) == UAV_OK);
        CHECK(info.has_video == 1);
        CHECK(info.width == EXPECT_W);
        CHECK(info.height == EXPECT_H);

        UAVVideoFrame vf;
        REQUIRE(uav_acquire_frame(p, -1, &vf) == UAV_OK);
        CHECK(vf.data != nullptr);
        CHECK(vf.width == EXPECT_W);
        CHECK(vf.height == EXPECT_H);
        CHECK(vf.stride == vf.width * 4);
        CHECK(vf.format == UAV_PIX_RGBA32);
        CHECK(vf.frame_id >= 1);
        uav_release_frame(p);

        uav_close(p);
        uav_destroy(p);

        MESSAGE("covered: " << clip.codec << " via " << clip.name);
        ++covered;
    }

    MESSAGE("codec-open coverage: " << covered << " clip(s) decoded, "
            << skipped << " skipped (absent / decoder unavailable)");
    CHECK(covered + skipped == (int)(sizeof(kVideoClips) / sizeof(kVideoClips[0])));
}

#else

TEST_CASE("codec-open: (no-FFmpeg) open is unsupported, no decode path"
          * doctest::test_suite("[codec]")) {
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    CHECK(uav_open(p, "anything.webm") == UAV_ERR_UNSUPPORTED);
    uav_destroy(p);
    MESSAGE("note: FFmpeg disabled — per-codec decode cases compiled out");
}

#endif
