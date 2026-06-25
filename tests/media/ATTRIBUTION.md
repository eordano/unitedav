# Test media attribution & licensing

Hard rule for this repo: tests/CI use **only public-domain or Creative-Commons**
media. No copyrighted clips.

## Tier 1 — Synthetic (the source of truth) — PUBLIC DOMAIN, no attribution

Everything in `tests/media/out/` is generated locally by `tests/media/gen.sh`
from FFmpeg `lavfi` sources only:

- Video: `testsrc2` (synthetic test pattern)
- Audio: `sine` (440 Hz tone)

These are **synthetic output generated locally**: not derived from any third-party work,
**public domain**, and require **no attribution**. They are deterministic, tiny,
need no network, and are regenerated on demand (the `out/` dir is gitignored).
The format/codec/transport decode matrix runs entirely on these.

## Tier 2 — Real Creative-Commons content (OPTIONAL, best-effort, off by default)

If — and only if — network is available, `fetch-cc.sh` may download a Blender
Foundation open-movie clip as an *additional* real-content sample. These are
**CC-BY 3.0**: redistribution/derivatives are fine **with credit**. Attribution
required by the license:

| Work | Author / © | License | Source |
|------|------------|---------|--------|
| Big Buck Bunny | © Blender Foundation | CC-BY 3.0 | https://peach.blender.org |
| Sintel | © Blender Foundation | CC-BY 3.0 | https://durian.blender.org |
| Tears of Steel | © Blender Foundation (Mango) | CC-BY 3.0 | https://mango.blender.org |

CC-BY 3.0 legal code: https://creativecommons.org/licenses/by/3.0/

The fetch is **best-effort and skipped when offline**; the matrix does NOT depend
on it. Any fetched file is verified against a pinned SHA-256 checksum and is NOT
committed (it lives under the gitignored `tests/media/out/`). Checksums are
recorded here when a clip is actually pinned:

| File | SHA-256 |
|------|---------|
| (none pinned — synthetic is the source of truth) | — |

## License hygiene note

No GPL/patent **encoders** are used to produce any of this media. H.264/HEVC test
inputs are produced via **VAAPI hardware encoders** (`h264_vaapi` /
`hevc_vaapi`) purely to exercise the **decode** path; everything else uses
LGPL/BSD software encoders (libvpx, SVT-AV1, libopus, libvorbis, libmp3lame,
native AAC, FLAC). This keeps the whole test pipeline LGPL/BSD-clean.
