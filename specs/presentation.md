# Preview presentation by type

Source ownership: `src/preview_presentation.h`, card/menu routes in
`src/service.cpp` and each native view. Add a row when introducing a kind or a
distinct content mode. This is the inventory of visible information and controls.

The window owns the selected-preview picker, navigation, layout, Reference,
Codex, Open and Close actions. Card actions offer options, remove and selection.
Its leftmost toolbar button toggles the fully collapsible category/file explorer;
folder activation opens Gallery and file selection opens Single. See [Projects](projects.md).
Fit belongs to content with spatial framing; Reset camera belongs to 3D. These
capabilities share one policy in `preview_presentation.h` across card buttons,
the View menu and context menus.

| Type / mode | Content and information shown | Controls and conditional details |
|---|---|---|
| Selection (`selection`) | Sectioned selectable Markdown proposals and stable decision counts | Batch / One at a time, search, answer filter, large square green check/red X, Clear choice; saved separate results with changed-content invalidation. No Fit or duplicate title overlay. [Selection](selection.md). |
| Image (`image`) | Image, title, live/loading/error status | Fit, options, remove, select; pan/zoom. Anti-aliasing (checked = smooth), Automatic anti-aliasing and background in options. Automatic keeps raster sources up to 256 × 256 crisp. View > Compare With opens a searchable thumbnail/name grid; optional Sync pan / zoom for image pairs. [Images](images.md). |
| Model (`model`) | Exported geometry, title, status, enabled triangle count | Fit, Reset, options, remove, select; orbit/pan/zoom. Camera, projection, materials/textures, edges, grid/axes, lighting and texture detail in menus. [Models](models.md). |
| Scene / modular assembly (`model`) | Complete authoritative export and model information above | Model controls; companion registrations can show owner-generated part/fit proof views. These are content uses of `model`. [Scenes](scenes.md), [modular assets](modular.md). |
| Animation / rig (`model`) | Model information, available clips, timeline and position | Model controls plus clip, play/pause, loop, speed, seek and stepped playback. Transport appears only when clips exist. [Animations](animations.md). |
| Material sample (`material`) | Baked maps on a sample or supplied host, title/status/count | Fit, Reset, options, remove, select; camera controls. Sample shape, fallback roughness/metallic and display settings in menus. [Materials](materials.md). |
| Material graph (`graph`) | Recipe graph, title/status, node inspector and saved-map material preview | Fit, options, remove, select; graph zoom, node expansion, channel/target selectors. Selected-node parameters in inspector. [Graphs](material-graphs.md). |
| Audio / music (`audio`) | Filename, status, energy-history bars, clip waveform and position/duration | Play/pause, seek, restart, mute, loop; options, remove, select. No Fit or Reset. Controls condense in small cards. [Audio](audio.md). |
| Video (`video`) | Frame, title/status and position/duration | Play/pause, seek, restart, mute, loop, fullscreen; options, remove, select. No Fit or Reset. Controls and healthy status hide after inactivity; busy/errors stay visible. [Video](video.md). |
| Markdown (`file`, Rendered) | Rendered document and bounded local images | Mode, find, options, remove, select. Ctrl+scroll or Ctrl++ / Ctrl+- zooms. No Fit, duplicate title badge or separate metadata row. Source mode enables Soft wrap. [Files](files.md). |
| Code / configuration / logs (`file`, Source) | Selectable source, line numbers and syntax colors | Mode, Soft wrap, find, options, remove, select. Wrap defaults on; Alt+Z toggles it. No Fit. [Files](files.md). |
| Comparison / patch (`file`, Inline / Side by side) | Changed-file navigation, original line numbers, green addition/red deletion counts, colored rows and changed substrings | Inline by default. Mode, Soft wrap, Full context, find and card actions. Previous/next change is available in the preview options menu. Wrap defaults on in both modes; Alt+Z toggles it without rebuilding document text. Paired rows align when wrapped. File list becomes a selector below 760 px; counts keep their colors in both. [Files](files.md). |
| 4 View (3D) (`model` / `material`) | Four joined panes: orbit, front, right and top, with camera labels | Preview Options > 4 View, Ctrl+Alt+Q; wheel zoom per pane, drag axis panes to pan. Shares one scene and the existing transport. [Models](models.md#4-view). |
| Mesh topology (`model`) | Source quad counts when available, alongside tessellated triangle counts | Edge topology: Quads / source faces or Triangulated faces; selecting enables edges. OBJ/PLY retain source polygons; GLB/glTF stays triangulated. [Models](models.md#source-quads-and-edge-topology). |
| Parts / Cuts workspace (`model`, inspection) | Geometry, camera, solid amber selection, optional per-part colors/explosion and filled sections | Preview/Parts/Cuts selector, Color each part, Explode/Reassemble, cut planes with Fill cut faces, visibility/selection and review controls. Parts prioritizes component tools; Cuts focuses on sections. [Inspection](inspection.md). |

File previews use one attached compact toolbar. Below 900 px, search and context
move to a second compact row so document and action controls stay reachable.
The selected filename/counts appear there when space permits. Path, encoding,
line count and read-only details
stay in tooltips. Loading/building/waiting/errors appear in an attached status
row; healthy refresh status stays in the action area's tooltip and status data.
Other kinds retain their existing overlays, including title/path tooltips.

Narrow cards condense controls before reducing content width. Hidden/off-page
previews unload; presentation does not create players, render loops or generators.
See [preview lifecycle](preview.md) for shared ownership and interaction rules.

The Gallery window layout adds a compact left thumbnail grid beside the selected
ordinary preview, retaining the controls above. Project filtering/grouping and
the resizable divider belong to [Projects](projects.md#gallery).
