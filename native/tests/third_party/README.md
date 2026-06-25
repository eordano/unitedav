# Vendored test-only third-party headers (fallback)

The native test suites prefer **doctest from the nix devShell** (`nixpkgs#doctest`,
on the compiler include path as `<doctest/doctest.h>`). The CMake scaffold in
`native/CMakeLists.txt` searches for it with `find_path(... doctest/doctest.h)`.

If you are building OUTSIDE the dev shell (no nixpkgs doctest available), vendor
the single header here:

    native/tests/third_party/doctest.h

When that file is present and nixpkgs doctest is not found, the scaffold puts this
dir on the include path and defines `UAV_DOCTEST_FLAT=1`, so the
`uav_doctest.h` shim includes `<doctest.h>` instead of `<doctest/doctest.h>`.

Only header-only **MIT/BSD/LGPL** headers may be vendored here — never GPL.
doctest is MIT. Get it from: https://github.com/doctest/doctest (`doctest/doctest.h`).
