# UnitedAV Showcase — video gallery + orbiting camera

A visual showcase: a ring of screens, each playing a **different** video stream
through its own `MediaPlayer` (looping), with a camera flying around the ring so
you see many simultaneous UnitedAV decodes rendering onto world geometry at once.

Everything is built procedurally by `ShowcaseController` (one component on one
GameObject), so the scene is trivial and robust.

## Run it (editor)
Open a scene with a single GameObject carrying `ShowcaseController`, set
`UAV_TEST_MEDIA_DIR` to a folder of clips (or let it find `tests/media/out`), and
press Play. Each screen opens a distinct clip; the `OrbitCamera` circles the ring.

## Record a fly-through (standalone, headless-friendly)
Build the `Showcase` scene to a standalone Player and run it with capture on:

```sh
UAV_HWDECODE=auto \
UAV_TEST_MEDIA_DIR=/path/to/tests/media/out \
UAV_SHOWCASE_CAPTURE=1 UAV_SHOWCASE_SECONDS=12 UAV_SHOWCASE_FPS=12 \
UAV_SHOWCASE_DIR=/tmp/uav-showcase \
  ./Showcase            # (Linux: launch via steam-run; macOS: the .app binary)

# assemble the frames into a video:
ffmpeg -framerate 12 -i /tmp/uav-showcase/frame-%05d.png \
  -c:v libvpx-vp9 -b:v 4M /tmp/uav-showcase.webm
```

`ShowcaseController` mutes every player's audio (N overlapping tracks would just be
noise) and applies the `RequiresVerticalFlip()` contract per screen. HW decode
engages per platform on the H.264 screens (`vaapi` / `videotoolbox` / `cuda`);
VP8/VP9/AV1 decode in software, which is fine for the visual.

Knobs: `screenCount`, `ringRadius`, `screenHeight`, `screenElevation` on the
controller; capture via the `UAV_SHOWCASE_*` env vars (see `ShowcaseCapture`).
