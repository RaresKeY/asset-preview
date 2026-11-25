# Images and textures

Source ownership: `ImageView` in `src/viewer.{h,cpp}`, image routing/validation in
`src/service.cpp`, `tests/integration.py`. Shared lifetime is in [Preview](preview.md).

`kind:image` uses Qt image readers. PNG, JPEG, WebP, BMP, TIFF, SVG, EXR and HDR
extensions route automatically. Animated GIF routes to [video](video.md).
The decoded image has a 4096-pixel edge target and a 64 MiB allocation limit;
image files retain the shared 256 MiB source cap. Unsupported saves retain the
last successfully displayed image. Visible cards own active decoded pixels;
eligible unloaded pixels may enter the bounded [preview cache](cache-overview.md).

Visible image decoding and GPU pixel preparation use a shared queue with at most
two foreground workers.
Queued replacements cancel before opening; in-progress decodes discard their
results when superseded, unloaded or changed on disk. Pixels publish on the UI
thread only after a second source-identity check. Failures keep the last good
image. Raster cache hits and GPU cache hits needing at most 1 MiB of RGBA display
pixels transfer immediately; larger GPU hits prepare asynchronously without
decoding again. Thumbnail scaling
and the premultiplied display copy are prepared on these same workers, so image
arrival does not perform a full-size pixel conversion on the input thread.
`metrics.loading` and a compact status spinner describe pending work;
`metrics.load_timing` reports queue/decode ms, worker preparation ms and cache hits.
Saved framing restores before decoding starts, so Fit/zoom/pan and
display settings changed during decode remain current. `tools/test.sh async-loading`
checks input preservation during initial publication.
The GPU lane also checks large cached preparation, current input ownership,
supersession, source replacement and last-good pixels.
The display budget also covers packed/indexed sources that expand on conversion.


Two images can be registered with reciprocal `add --toggle-with IMAGE_ID` options. In Single view, T switches to the paired registration on each press. Closing either image disables the shortcut. Each image retains its own zoom and pan.
Checking shortcut availability leaves both registrations untouched, including unpaired previews of any kind.
Wheel zoom anchors at the pointer, drag pans, and double click/Fit restores the
fit. Zoom and viewport-normalized pan persist per registration, separate from
remembered defaults. Anti-aliasing and checker/dark/light backgrounds are
display choices. The Anti-aliasing checkbox is checked for smooth filtering and
unchecked for nearest-neighbor sampling. Automatic anti-aliasing is the default
when `nearest` is absent or `"auto"`: raster sources with both dimensions at most
256 pixels use no AA; larger raster sources and SVGs use AA. Classification uses
original source dimensions, including cache hits, and updates only on successful
publication. It does not depend on viewport size or zoom. Boolean `nearest`
settings still override automatic behavior (`true` means AA off); remembered
preferences and explicit per-image choices retain their existing precedence.
The Automatic anti-aliasing checkbox restores resolution-based selection after
a manual override. Metrics expose effective `nearest` and `filtering_auto`.
Static images have no animation or render polling timer.
On native Wayland/X11, a child `QOpenGLWidget` draws the image and background.
The child is constructed with the card but stays explicitly hidden until the
first successful pixel publication. Empty, pending and failed initial images
use the existing opaque raster background without initializing a GL context or
blank framebuffer. Publication shows the already constructed child after its
worker-prepared pixels and current framing are installed. Later reloads keep
the last good GPU image visible, including failed replacements. This does not
change worker admission, cache-hit preparation or cancellation.
Qt retains the uploaded image between frames, so dragging changes its transform
without repeatedly scaling the source on the CPU or repainting card overlays.
The child passes pointer events to the owning image widget. Other Qt platforms
keep the raster path. Both paths fill the entire background and declare opaque
painting to avoid obscured ancestor repaints. Neither schedules idle frames.
GPU interpolation uses a premultiplied display copy (at most one additional
decoded-size CPU image per active view); original decoded pixels, source exports
and service cache entries remain intact. View captures read the image framebuffer;
whole-window captures compose it with the existing card overlays.
`tools/test.sh async-loading` checks raster alpha/background/filtering parity and
parent repaint counts, automatic source-size classification, SVGs, explicit
overrides, return to automatic, last-good filtering and original-size cache hits.
It checks magnified no-AA black/white texels for unwanted gray interpolation.
The GPU component lane also checks that empty/failed images have no initialized
display, followed by successful native publication. The original component is
a negative control for that initialization check.
`tools/test.sh async-loading-gpu` repeats these checks and also verifies the actual
GPU renderer, both filtering modes against the independent raster path, alpha,
all backgrounds, zoomed framing, unchanged source pixels and no idle repainting.

View > Compare With opens a searchable grid of thumbnails and names for other
registered images across projects. The current source and incompatible types
are omitted. Clicking a tile or pressing Enter starts side-by-side comparison;
Escape/Cancel preserves the existing layout, selection and comparison. Browsing
does not activate candidates or run generators. The grid shares the existing
bounded thumbnail queue; see [Cache and overview](cache-overview.md).
Sync pan / zoom links zoom relative to Fit, viewport-normalized pan, and Fit in
either direction; disable it for independent framing. Different image dimensions
retain their own aspect ratio and fit scale. Filtering/background remain independent
per image. Comparison and framing survive hide/show, reload and restart; choosing
a normal layout, closing a member or changing it to an incompatible kind ends the
comparison. Image/3D mixed pairs are rejected. The existing `compare` protocol and
`camera_sync` preference also own image comparisons.

Use images for standalone texture inspection, rendered engine captures and
owner-generated proof views. A texture image does not evaluate a material graph;
[Materials](materials.md) owns shaded baked-map samples. A screenshot confirms
appearance only, not source topology, dimensions or export correctness.

Verification: functional image refresh/last-good/decode/pan tests and the native
input/capture cases in `tools/test.sh gpu`.

Save Screenshot and Copy Image use the active decoded pixels, preserving alpha and decoded dimensions
regardless of viewport background/pan/zoom. View screenshots still capture that
framing; see [Visual references](references.md).
