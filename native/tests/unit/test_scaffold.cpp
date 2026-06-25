// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav.h"

TEST_CASE("scaffold: abi version is 1" * doctest::test_suite("[scaffold]")) {
    CHECK(uav_abi_version() == UAV_ABI_VERSION);
    CHECK(uav_abi_version() == 1u);
    CHECK(uav_abi_version() == uav_abi_version());
}

TEST_CASE("scaffold: null-handle getters return documented sentinels"
          * doctest::test_suite("[scaffold]")) {
    CHECK(uav_get_state(nullptr) == UAV_STATE_ERROR);
    CHECK(uav_last_error(nullptr) == UAV_ERR_INVALID);
    uav_destroy(nullptr);
    uav_release_frame(nullptr);
}
