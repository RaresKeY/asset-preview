# Images, textures and baked materials

Register the authoritative saved image with a stable ID. PNG, JPEG, WebP, BMP,
TIFF, SVG, EXR and HDR route to image preview automatically; animated GIF uses
video playback. Image zoom/pan, pixel filtering and checker/dark/light backgrounds
are inspection choices, not modifications to the source. Source saves refresh
an active view without repeated registration. Hidden views retain configuration
only. Use the owning project's finite capture/export wrapper for rendered proof.

For a shaded material sample, use `material ALBEDO --normal NORMAL --orm ORM`
with sphere/cube/plane as needed. Albedo is sRGB; tangent normal and packed ORM
are linear. ORM stores occlusion, roughness and metallic in R/G/B. The sample
uses F3D lighting and does not execute procedural material graphs or engine shaders.
For exact shading, connect an owning-engine PNG/video capture alongside baked maps.

For a flush strip, panel, opening or modular seam, connect the actual host GLB
and owner proof images. A generic material sphere cannot establish material-region
ownership or fit. Read [3D assets](3d-assets.md) for that workflow. The viewer
does not replace saved-geometry, UV, tangent, bake or dimensional verification.
