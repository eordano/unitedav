# UnitedAV — committed Unity test-runner project

A **minimal, committed Unity project** whose only job is to let the Unity Test
Runner discover and execute the EditMode + PlayMode tests that ship **inside** the
`org.unitedav` UPM package (`unity/Tests/`). The repo previously shipped the UPM
package only, with no Unity project to run it through; this project closes that gap
so `Unity -runTests` works on any target (macOS, Linux under xvfb, Windows).

## How it works

- `Packages/manifest.json` pulls the package from the working tree via a local
  `file:` reference and marks it testable:

  ```json
  "org.unitedav": "file:../../../unity",
  "testables": [ "org.unitedav" ]
  ```

  `file:` paths in a UPM manifest are resolved **relative to the `Packages/`
  folder**, so `../../../unity` points at `unitedav/unity/` (the package root that
  holds `package.json` → name `org.unitedav`). `testables` is what makes the Test
  Runner include a package's `Tests/` assemblies; without it the package tests are
  invisible even though the package is installed.

- `com.unity.test-framework` (1.4.6, the line that ships with Unity 6000.4) brings
  in NUnit + the `UnityEngine.TestRunner` / `UnityEditor.TestRunner` assemblies the
  package's test asmdefs reference. `com.unity.ext.nunit` is pulled transitively.

- The project itself has **no** test code under `Assets/` — all tests live in the
  package (`unity/Tests/EditMode`, `unity/Tests/PlayMode`).

## Pinned editor

`ProjectSettings/ProjectVersion.txt` pins **6000.4.11f1**. Open with the exact
editor so the package's
`defineConstraints: ["UNITY_INCLUDE_TESTS"]` and the test framework resolve
identically across machines.

## Running the tests

From this folder (`tests/unity/`), with `<Unity>` = the 6000.4.11f1 editor binary:

```sh
# EditMode (no graphics device needed):
<Unity> -runTests -batchmode -projectPath . \
  -testPlatform EditMode -testResults ./results-editmode.xml \
  -logFile ./Logs/editmode.log

# PlayMode — run WITH a real GfxDevice (NOT -nographics) so the texture /
# pixel-readback tests exercise a real renderer:
<Unity> -runTests -batchmode -projectPath . \
  -testPlatform PlayMode -testResults ./results-playmode.xml \
  -logFile ./Logs/playmode.log
```

`-batchmode` without `-nographics` still needs a display on Linux — run it under
`xvfb-run`. The media-backed PlayMode case self-skips unless a clip is reachable;
point it at one with `UAV_TEST_MEDIA_DIR` (it also falls back to
`tests/media/out/webm__vp9__opus.webm`). To force the hardware decode path, set
`UAV_HWDECODE=auto` and provide an H.264 fixture (VP9/AV1 fall back to software
silently).

## What is and isn't committed

Committed: `Packages/manifest.json`, `ProjectSettings/` (version + minimal
settings), `Assets/.gitkeep`, this README. Everything Unity generates
(`Library/`, `Temp/`, `Logs/`, `obj/`, IDE `*.csproj`/`*.sln`,
`Packages/packages-lock.json`, test-result XML) is git-ignored — see `.gitignore`.
The lock file is intentionally not committed so each editor re-resolves
`org.unitedav` from the live working tree.
