// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>

namespace {

int uav_force_sw_decode_baseline() {
#if defined(_WIN32)
    if (const char* existing = std::getenv("UAV_HWDECODE")) {
        if (existing[0] != '\0') return 0;
    }
    return ::_putenv_s("UAV_HWDECODE", "none");
#else
    return ::setenv("UAV_HWDECODE", "none", 0);
#endif
}

const int g_uav_sw_baseline = uav_force_sw_decode_baseline();

}

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "uav_doctest.h"
