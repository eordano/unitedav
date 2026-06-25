# Licensing

## This project's code
- The C# package (`unity/`) and native plugin (`native/src`, `native/include`) are
  licensed under **Apache-2.0** (see `LICENSE` and `NOTICE`). Source files carry an
  SPDX `Apache-2.0` identifier.
- FFmpeg/libav is the only third-party dependency.

## FFmpeg / libav (dynamically linked, LGPL)
- The plugin links **only LGPL-licensed** FFmpeg components and ships FFmpeg as
  **separate, replaceable shared libraries** (`.dll`/`.dylib`/`.so`) loaded
  dynamically.
- **Never** enable `--enable-gpl` / `--enable-nonfree`. No GPL-only filters, no
  nonfree codecs — this keeps the plugin's own license unconstrained by GPL.
- The default nixpkgs `ffmpeg` is built `--enable-gpl`; `flake.nix` overrides it
  (`withGPL = false; withGPLv3 = false; withUnfree = false;`) → an LGPLv3 build
  (`--enable-version3` remains), which is fine for dynamic linking. We only decode;
  H.264/H.265 decoders are FFmpeg-native LGPL, AV1 via dav1d (BSD), VP8/9 via
  libvpx (BSD).

## LGPL obligations for binary redistribution
- Ship FFmpeg as replaceable shared libs (dynamic linking).
- Provide the corresponding FFmpeg source (or a written offer/link) for the exact
  build shipped, plus the `configure` flags used.
- Include FFmpeg's license texts and attribution notices.

## Patent / codec note
H.264/H.265/AAC carry patent-pool considerations independent of software license.
Prefer royalty-free codecs (VP8/VP9/AV1/Opus) where content allows; patented
codecs are supported via decode only. Flag for legal review before release.
