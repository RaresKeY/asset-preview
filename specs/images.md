# Images and textures

Source ownership: `ImageView` in `src/viewer.{h,cpp}`, image routing/validation in
`src/service.cpp`, `tests/integration.py`. Shared lifetime is in [Preview](preview.md).

`kind:image` uses Qt image readers. PNG, JPEG, WebP, BMP, TIFF, SVG, EXR and HDR
extensions route automatically. Animated GIF routes to [video](video.md).
The decoded image has a 4096-pixel edge target and a 64 MiB allocation limit;
image files retain the shared 256 MiB source cap. Unsupported saves retain the
last successfully displayed image. Visible cards own active decoded pixels;
eligible unloaded pixels may enter the bounded [preview cache](cache-overview.md).

Visible image decoding uses a shared queue with at most two foreground workers.
Queued replacements cancel before opening; in-progress decodes discard their
results when superseded, unloaded or changed on disk. Pixels publish on the UI
thread only after a second source-identity check. Failures keep the last good
image. Cache hits transfer immediately. `metrics.loading` and a compact status
spinner describe pending work; `metrics.load_timing` reports queue/decode ms and
cache hits. Saved framing restores before decoding starts, so Fit/zoom/pan and
display settings changed during decode remain current. `tools/test.sh async-loading`
checks input preservation during initial publication.

Wheel zoom anchors at the pointer, drag pans, and double click/Fit restores the
fit. Zoom and viewport-normalized pan persist per registration, separate from
remembered defaults. Pixel filtering and checker/dark/light backgrounds are
display choices. Static images have no animation or render polling timer.

Use images for standalone texture inspection, rendered engine captures and
owner-generated proof views. A texture image does not evaluate a material graph;
[Materials](materials.md) owns shaded baked-map samples. A screenshot confirms
appearance only, not source topology, dimensions or export correctness.

Verification: functional image refresh/last-good/decode/pan tests and the native
input/capture cases in `tools/test.sh gpu`.

Save Screenshot and Copy Image use the active decoded pixels, preserving alpha and decoded dimensions
regardless of viewport background/pan/zoom. View screenshots still capture that
framing; see [Visual references](references.md).
