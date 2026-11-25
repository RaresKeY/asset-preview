# Files, Markdown and diffs

Source ownership: `src/file_document.*`, `src/file_view.*`, `src/markdown_compat.*`, file registration and
watching in `src/service.cpp`, `bin/asset-preview` and shell completions.

`file` is a read-only native Qt preview. Markdown, source code, Godot scripts,
scenes/resources, shaders, JSON/JSONL, YAML, TOML, INI, CSV/TSV, XML/HTML, logs,
shell scripts, patches and common extensionless configuration/build files
auto-detect. Existing image, media and material-graph detection takes precedence;
SVG remains an image unless explicitly registered with `--kind file`.
Unknown text extensions can also use `--kind file`. HTML/XML display as source.

Markdown defaults to Rendered with a Source toggle. It supports Qt's GitHub
Markdown dialect, including tables, lists and fenced code. A native compatibility
bridge also accepts `<br>` (including case, whitespace, attributes and self-closing
forms) and `<img>` with src/alt/title and numeric width/height. MD4C's source events
identify real HTML without transforming escaped tags, entities, code, comments,
Markdown image labels or link destinations. Ordinary Markdown without candidate
tags keeps the original single-parser path. Recognized tags become native line
separators and image objects after Qt parsing; arbitrary HTML remains inert
literal text. Qt's HTML importer stays disabled because mixed inline HTML dropped
following text in the installed runtime. This is a presentation subset, not full
HTML/browser support. The bridge is shared with content thumbnails.
Opaque HTML blocks are lexed across callback lines: comments, declarations,
raw script/style/pre/textarea sections, quoted attribute examples and custom
tag names never become native images or breaks. Tags split across opaque block
callbacks remain literal. Inserted images retain enclosing Markdown links and
emphasis. Marker positions are collected once per text block and replaced in
reverse order, avoiding repeated whole-paragraph searches for large inputs.
Rendered presentation follows VS Code/VSCodium's default Markdown preview:
14 px system body text with a 22.4 px line height, 52 px side insets
(the combined html/body padding in Code), bold native heading faces with
rules under H1/H2, and explicit paragraph/list spacing. The document retains
Code-style space beyond the final line. Fenced and indented code use padded,
theme-aware native frames with
Code-style basic syntax colors for recognized fence languages, including italic
green comments; unlabelled, text and unknown-language fences retain literal
coloring. Inline code
has a subtle background. Tables have bold headers, padded cells and horizontal
separators. Quotes use continuous shaded frames and nested grey left bars; lists
use 40 px indentation with hollow bullets at the second level.
Long code wraps within narrow cards. This remains a native QTextDocument view,
without browser extensions, HTML execution or browser-specific Markdown features.
Rendered Markdown supports Ctrl+scroll and Ctrl++ / Ctrl+- (including Ctrl+=)
to zoom in 10 percentage-point steps from 50% to 300%. Ctrl+0 retains its
application-wide All Open Previews action.
Partial wheel ticks accumulate and pixel-only scrolling is supported. Ordinary
scrolling keeps its normal behavior. Zoom scales body/heading typography, line
heights, document spacing, code/quote/table insets and embedded-image rectangles;
images still fit their containers. The existing document and decoded image cache
are reused, with reading anchor and text selection retained through reflow.
Zoom survives mode/theme changes and reloads while that view is loaded; it is
local to the rendered view and resets after unloading/restart. `file_zoom` in
metrics reports its percentage. Default typography above is specified at 100%.
Links are inert. Image resources resolve only inside the document's canonical
directory, never through remote URLs or symlinks leading outside that directory.
Image sources must be regular files, are bounded to 8 MiB and 16 megapixels,
and decode at most 1600 px. Their displayed rectangles fit the available page,
quote, list or table-cell width without enlargement and retain their aspect
ratio. Explicit HTML dimensions retain their authored size (with each dimension
capped at 1600); a single dimension derives the other from the source aspect
ratio. An authored pair may specify its own aspect ratio. All images still shrink
to fit their container; authored HTML sizes can enlarge a small source. Resize
rounds available image widths down to a logical pixel so fractional table columns
do not enlarge their image minimums. A one-pixel document layout allowance also
prevents Qt's fitted-table rounding from adding a horizontal scrollbar at odd
viewport widths; genuinely oversized content retains ordinary scrolling. It
uses indexed authored sizes and retained intrinsic dimensions;
it does not request offscreen pixels after their eviction. Dimension metadata
is bounded by admitted image characters and refreshes when rendered mode prepares
a successful reload, including unchanged text. Decoded pixels remain independently bounded.

Right-clicking an embedded image offers **Copy Image**. Clicking an image selects
it for Ctrl+C; a selection containing exactly one image copies actual image data
and a PNG payload, preserving alpha and decoded dimensions (up to the existing
1600 px bound), independent of its displayed size. Ordinary and mixed text
selections keep standard text/HTML copy behavior. Copying uses the same restricted
resource cache and never publishes a denied/missing-image placeholder as image
pixels. Inline and table images share these actions. The rendered document owns
its content context menu inside the main window; card toolbar options retain
the ordinary preview menu.

Pipes and devices are rejected before opening. Missing and denied images return
a small drawn placeholder so Qt cannot retry them through its filename loader.
Embedded SVG is limited to self-contained vectors: a bounded immutable snapshot
is parsed before decoder sizing or rendering. Nested raster/feImage elements,
external hrefs, processing instructions and custom entity declarations are denied;
internal CSS, vector definitions and fragment references work. SVG nesting is
limited to 128 and elements to 100,000. Compressed SVG is denied because it
cannot be inspected within this loader's encoded-input bound. Detection uses
content, including SVG disguised with a raster filename. These restrictions
prevent decoder subresources from bypassing the directory and pixel limits.
The document owns an LRU resource cache with a 64 MiB cost limit, including
decoded pixels and a per-request overhead allowance. Successful and denied
requests are reused during layout, scrolling, resizing and theme changes;
evicted resources load on demand through the same restricted loader. Every
successful input reload clears this cache, even when Markdown text is unchanged,
and invalidates image layout so replaced bytes and dimensions refresh. Rejected
text saves retain the last good document and its resources. Cache lifetime ends
with the FileView; there is no process-wide image retention or polling.
`file_image_decodes` counts QImageReader raster-read calls for the view's lifetime, and
`file_image_cache_cost` reports its current bounded resource cost.
`file_scroll_y` and `file_scroll_max` report the displayed pane's vertical
position/range (the after pane for a side-by-side comparison).

Source has line numbers, fixed-width text, basic syntax colors and selectable text.
Ordinary files keep their filename beside the mode picker at every card width, including
Gallery and the folder explorer. Its tooltip retains the full path. Narrow
cards move Find onto a second row; only diff summaries retain the existing
1000 px visibility threshold because their filename/counts also appear in the
changed-file picker.
Soft wrapping is on by default, with Ctrl+F / F3 / Shift+F3 search. Alt+Z toggles
Soft wrap in source and both diff modes. It never edits or executes
the file. Search wraps within the selected file and displayed mode.
Mouse movement within or across previews preserves the Find text box's focus
and keyboard input. Explicit clicks, scrolling and preview selection retain their
ordinary targeting behavior. Transient `file_search` metrics expose its focus
and text for native interaction verification; neither is saved as a preference.

## Comparing versions

```sh
asset-preview diff /absolute/before.cpp /absolute/after.cpp --id changes --mode split
asset-preview add /absolute/changes.patch --id patch
asset-preview add /absolute/after.md --before /absolute/before.md
```

`diff` and `add` register in the background and preserve the current window,
selection and layout. The native File > Compare Files action selects two files
and opens their comparison. Open Assets, drag/drop and `open` accept documents.

Ordinary unified/Git text patches support multiple files and hunk line offsets.
Git rename, mode and binary change metadata is shown without decoding binary
patch payloads. Combined merge diffs are available as Source only, including
mixed ordinary/combined patches so unsupported merge edits are never omitted.
Comparing two
files computes a bounded Myers line diff; edits beyond 1200 insertions/deletions
use an exact-content coarse replacement between shared prefix/suffix lines.
CRLF is normalized for comparison; final-newline changes are shown explicitly.

Inline is the default comparison mode. Inline and Side by side show original
line numbers, addition/deletion counts, semantic row colors and changed-substring
emphasis. Addition counts are green and deletion counts red in the header,
changed-file list and narrow selector, including its popup. Signs remain visible;
selected rows and light/dark palettes retain contrasting colors. Paired panes align
replacement rows and synchronize both scroll axes. Wrapped rows reserve matching
heights through document layout without changing selectable text. Turning Soft
wrap off enables horizontal scrolling. Resizing realigns wrapped rows.
Changed-file navigation uses a left list, replaced with a selector below 760 px.
Previous/Next change in the preview options menu wraps between change groups,
including deletion-only groups. These actions do not occupy toolbar buttons.
Unchanged runs longer than ten lines collapse to three lines at each end.
Full context or Ctrl+Shift+Space reveals all context present in the input; patch
files cannot supply omitted source lines. Source shows the patch or the after file.

`file_mode` (`source`, `rendered`, `inline`, `split`), `file_wrap` and `file_context`
persist per registration and in the `file` defaults group. Modes unavailable for
the current input fall back to the appropriate document view. Reading position
survives setting changes and clamps to the new document on live reload.
Rendered Markdown restores its numeric cursor/selection positions and scroll
offsets after a same-path text reparse. Opening a different document starts at
the top; the parser's temporary end cursor must not become the reading cursor.

Only the selected mode prepares document text and highlighting. A page is reused
until the input load, selected file or displayed context changes. Every successful
reload invalidates all pages, including same-path/same-size replacements. Soft
wrap changes layout without repopulating the mode selector or rebuilding text
and syntax in visible or hidden panes. Language classification is prepared once
per extension; new text uses Qt's automatic highlighting. Paired alignment is
reused only for the same content, wrap setting, viewport widths and fonts.
Font changes and splitter/window resizing schedule a finite realignment.
Each pane recolors after receiving its own palette change, so switching themes
updates row backgrounds and syntax in both visible and cached modes.
Rendered Markdown recolors cached links for the browser's current palette while
preserving their targets, other formatting, document text and reading selection.
Code panels, inline code and table separators also follow palette changes in
place. Code and quote frames are prepared once after parsing; resizing or theme changes
never insert additional frames or reparse Markdown. Document layout is suspended
across nested parsing and presentation batches, then restored to its
previous state. Font, frame and image-rectangle updates therefore share one
layout pass per preparation batch. The reading space is an empty trailing block;
resizing updates that block's fixed height rather than changing the root frame,
which would invalidate the entire document. Ordinary resize uses Qt's incremental
layout and the indexed image positions, with no queued resize callback.
Image indexing includes every object even when adjacent identical images share
a text fragment.

The attached toolbar starts at the top of the card with mode, Soft wrap,
Full context for diffs, a filename/count summary when space permits,
find and card actions. Below 900 px, search and context move to a second compact
row. There is no reserved overlay gap, duplicate title,
standalone metadata row or Fit action. Healthy refresh details live in the card
actions' tooltip; loading/building/waiting/errors appear below the toolbar.
The [per-type presentation inventory](presentation.md) owns the shared policy.

## Loading and bounds

Overview and Gallery generate first-page thumbnails without opening an ordinary
file preview. Markdown uses the same native rendered styling (or Source when
selected); other text uses the existing syntax colors and line numbers. A finite
background job reads within the ordinary input limits, prepares at most 16 Ki
characters / 96 lines, then stores a 160×100 derivative of a 640×400 crop. Embedded
image references use placeholders in this text-only thumbnail; it never fetches
subresources. Thumbnails use a consistent dark document palette and are independent
of reading position. Source/settings changes invalidate them through the shared
[thumbnail queue and cache](cache-overview.md). Invalid/oversized inputs retain
their type placeholder. `tools/test.sh content-thumbnails-gpu` covers native
Overview/Gallery, source replacement, disk reuse and selected-item regeneration.

Each input must be a regular UTF-8 text file, at most 2 MiB, 20,000 newline
characters and 16,384 characters per line. Invalid UTF-8, NUL/binary controls,
missing files and oversized saves report Waiting while retaining last-good
content. Incomplete final UTF-8 sequences also reject the save rather than
silently dropping its trailing bytes. Both comparison paths participate in ordinary file/directory watching,
including atomic replacement and missing-file creation. Optional registration
`before` is an absolute local file path and requires kind `file`.

Only visible cards load documents. Hidden/off-page views unload through the
shared lifecycle. No perpetual render timer, media player, browser engine,
generator or dependency download is introduced. PDF and Office documents have
no document renderer in this implementation.

## Verification

`tools/test.sh files` checks deterministic diff reconstruction, patch parsing,
embedded-image clipboard MIME/PNG bytes, exact alpha and decoded dimensions,
inline/table image selection and context menus, ordinary/mixed text copying,
content/resource bounds, native controls/search, collapsed context, navigation, reading position,
lazy preparation, cache invalidation, font realignment, cached Markdown link
colors/selection, Markdown heading scale/spacing, code frames and table borders,
rendered dark/light desktop/narrow layouts, complete and truncated multibyte UTF-8 and theme changes,
image reuse across real native scroll/resize, non-regular resource rejection,
cache eviction, rejected-save retention, actual layout/paint of denied images
(including a named pipe and SVG referencing a pipe), self-contained vectors and
denied SVG raster/style/entity/reference/compression/nesting cases,
responsive page/quote/list/table and adjacent images,
restoration of layout after nested batches, and same-text image replacement with
changed rendered dimensions,
over-budget image guides without resize decode sweeps, literal fence colors,
quoted-code insets, collapsed panel/heading gaps and heading/table spacing,
both-input watching, last-good behavior and restart persistence.
It also covers HTML breaks without lost text inside paragraphs/tables, retained
Markdown emphasis, code/escape/entity/comment literals, inert unsupported HTML,
Unicode/entity attributes, authored image sizing, cache reuse and narrow reflow.
The service check also verifies HTML image reload, hidden unload and fresh reentry.
`tools/test.sh files-gpu` runs the native view in headless Gamescope and verifies
the actual OpenGL renderer. Its window check exercises both image context-menu
and Ctrl+C clipboard ownership across processes using the native test executable
as an image receiver, checking decoded PNG dimensions and pixel colors.
`ASSET_PREVIEW_EVIDENCE_DIR` retains dark/light,
desktop/narrow screenshots and `renderer.json` outside committed specs.
Opt-in latency probes are `tools/profile_file_view.cpp` for native controls and
`tools/profile_file_settings.py` for the full service settings path. Reproduction,
measurement scope and accepted results live in [performance](../docs/performance.md).
`tools/profile_markdown.cpp` measures native scroll/resize and optional rendered
pixel parity, with `synthetic` text-heavy and `images` over-budget presets and
per-operation decode counts; `tools/profile_markdown_service.py` exercises the same document
through real wheel/resize input, hidden unload and reentry in a private service.
The service probe waits for each wheel movement in its requested direction and
for each requested window size; an X server acknowledgement alone is insufficient.
The native probe rejects non-rendered/non-scrollable inputs, software renderers
and failed or overwritten evidence captures.
The service probe's `--allow-image-eviction` admits visible paint misses for
over-budget screenshot guides while requiring each captured resize to decode
at most three newly visible screenshots. Ordinary guides require zero repeats.

## Markdown release qualification

Human input, summarized: research and optimize rendered Markdown, review and
patch it to release quality comparable to apps such as Code. Scrolling and
resizing are the reported slow interactions.

AI-selected acceptance criteria within that authorized scope: qualify the
existing read-only native renderer against actual Code's default presentation,
preserving the resource and lifecycle boundaries above. Cover headings,
emphasis, code, nested quotes/lists, tables, task markers, mixed Unicode and
large screenshots in both themes at usable widths 985/625 px. Require readable
content without unintended horizontal overflow, preserved selection/search,
fresh image bytes/dimensions on reload and bounded cache ownership. Code's
CommonMark/browser extensions and Qt's GitHub dialect are separate contracts.

For the frozen seven-image guide, 160-section text fixture and sixteen-image
guide above the pixel budget, target native scroll/resize p95 at respectively
16.7/33, 16.7/100 and 50/33 ms on the recorded workstation. The image-heavy
scroll probe jumps between distant positions, so that budget includes visible
cache misses. First native readiness targets are respectively 500/1000/1000 ms.
These are explicit fixture-specific qualification budgets, excluding scanout,
not universal timing guarantees for every input admitted by the text limits.
Require final-server input, reload and hidden reentry checks, plus focused X11
and Wayland gates. Record shared-host variance and unverified limits with results.

Qt's native text document preserves selection and search while keeping the
existing widget and input lifecycle. Intrinsic image dimensions stay with each
document revision so pixel eviction does not force offscreen decoding during
layout. Results are in [performance](../docs/performance.md) and
[verification](../evidence/verification.md).

At matched content widths of 985 and 625 px, dark and light captures fit the
1600×1000 reference image to 881 and 521 px with zero horizontal overflow.
The native Qt preview differs from Code's browser-based preview in font and
image rasterization, syntax colors, inline padding and marker spacing.
