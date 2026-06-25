# UnitedAV native tests (Tier 1/2)

ctest-driven unit + oracle + fuzz suites that exercise the **frozen** public C
ABI (`native/include/unitedav.h`, `unitedav_send.h`) of the shipped
`libUnitedAV`. No production code is under test from the inside — the suites link
the real `.so` and call exactly what the C# P/Invoke layer calls.

## Layout & how files are picked up (GLOB — no CMakeLists edits)

The wiring lives in `native/CMakeLists.txt` under `option(UAV_BUILD_TESTS ON)`.
Add a test by **dropping a file in the right dir, then re-running cmake** (the
GLOB is `CONFIGURE_DEPENDS`, so a touch/reconfigure is enough; a bare
`cmake --build` will not see a brand-new file):

| Dir                 | Glob            | Produces                                  | Link / build                                  |
|---------------------|-----------------|-------------------------------------------|-----------------------------------------------|
| `tests/unit/*.cpp`  | one executable  | `uav_tests` (doctest)                     | `UnitedAV` + `Threads`                        |
| `tests/oracle/*.cpp`| one tool        | `uav_oracle` (needs FFmpeg)               | `UnitedAV` + `PkgConfig::FFMPEG` + `Threads`  |
| `tests/fuzz/*.cpp`  | one per file    | `<name>` libFuzzer target (`-DUAV_FUZZ=ON`)| `UnitedAV` + `clang -fsanitize=fuzzer,address`|

## Rules for unit files (`tests/unit/`)

- **doctest TEST_CASEs only.** Do NOT define `main()` or
  `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` — `_uav_tests_main.cpp` owns the single
  main TU. A second main TU breaks the link.
- Include the framework via the shim: `#include "uav_doctest.h"` (works for both
  nixpkgs `<doctest/doctest.h>` and the vendored flat `<doctest.h>`).
- Drive ONLY the public C ABI (`#include "unitedav.h"` / `"unitedav_send.h"`).
  Never include private headers (`decoder.hpp`, `sender.hpp`).
- Tag every case with a component suite, e.g.
  `TEST_CASE("..." * doctest::test_suite("[ring]"))`, so
  `uav_tests --test-case='*ring*'` / `ctest -R` can select it. `test_scaffold.cpp`
  is the copy-me template.
- **Media-backed cases must self-skip**, never hard-fail, when the clip or an
  encoder is absent: emit a `MESSAGE(...)` and `return;`. Resolve media from
  `UAV_TEST_MEDIA_DIR` with a repo-relative `tests/media/out` fallback. On a
  no-FFmpeg plugin build `uav_open` returns `UAV_ERR_UNSUPPORTED`; guard those
  cases behind `#if defined(UAV_HAVE_FFMPEG)` (the scaffold propagates that
  define onto `uav_tests`).

## doctest dependency (LGPL/BSD hygiene)

doctest is **header-only, MIT** — provided by the nix devShell (`nixpkgs#doctest`)
or vendored as the single header at `tests/third_party/doctest.h`. No GoogleTest,
no Catch2, no GPL, and the plugin never enables FFmpeg `--enable-gpl`.

## Running

```sh
nix --extra-experimental-features 'nix-command flakes' develop -c bash -c '
  cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo &&
  cmake --build native/build &&
  ctest --test-dir native/build --output-on-failure'
# select a tier/suite:
ctest --test-dir native/build -L tier1
ctest --test-dir native/build -R uav_tests
ctest --test-dir native/build -N            # list discovered tests
```

The same files build under the sanitizer dirs (`native/build-asan`,
`native/build-tsan`) with zero extra wiring — they inherit `UAV_SANITIZE` flags.
But the GLOB is `CONFIGURE_DEPENDS`, evaluated **at configure time**: a sanitizer
dir configured *before* a test `.cpp` existed has NO `uav_tests`/`uav_oracle`
target and NO `CTestTestfile.cmake`, so `ctest` there discovers nothing and a
"green" run proves nothing. Running the suites in `native/build` with
`UAV_SANITIZE=none` is **not** a memory-safety pass. To actually get the
ASan/UBSan/LSan (or TSan) pass you must (re)configure the sanitizer dir against
the *present* test files, then build + ctest. The committed, self-checking gate
that does this — and **fails loudly** if the suites aren't instrumented — is
`tests/run-sanitizers.sh`:

```sh
nix --extra-experimental-features 'nix-command flakes' develop -c \
  bash tests/run-sanitizers.sh             # default: ASan+UBSan+LSan AND TSan
UAV_SAN=address bash tests/run-sanitizers.sh  # just ASan/UBSan/LSan over the suites
UAV_SAN=thread  bash tests/run-sanitizers.sh  # just TSan (uses native/tools/tsan.supp)
```

Fuzz targets are opt-in (`-DUAV_FUZZ=ON`, clang) and run as a short bounded
`-max_total_time=10` ctest smoke under the `tier2;fuzz` label.
