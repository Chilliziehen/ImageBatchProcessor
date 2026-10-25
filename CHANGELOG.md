# Changelog

## 1.1.0 — 2026-10-25

- New native Studio editor with Dear ImGui docking, SDL3 and Vulkan; large image canvas, sliders with numeric entry, filmstrip, before/after wipe, per-photo undo/redo and persisted edits.
- Interactive brightness/R/G/B curve editor with up to 32 control points and 1024-entry LUTs using shape-preserving cubic Hermite interpolation.
- Shared Vulkan context, GPU image reset/copy, display encoding and sampled-image access for preview without image readback.
- Version 2 IMGARG retains editable control points and exact LUTs; CLI can replay Studio recipes. Existing v1 C ABI, recipes and processing remain available.
- Background RAW preview decoding and full-size export, selection sync, native file dialogs and Unicode command-line paths.
- Numerical contracts for immutable originals, no accumulation, LUT CPU/GPU agreement and v2 corruption checks; documentation includes interpolation equations and limitations.

Studio currently ships on Windows; WIC remains a platform dependency. Preview is limited to 2048 pixels on the long edge; graphical masks, crop handles, full-resolution region previews and monitor ICC conversion are not part of this release.

## 1.0.0 — 2026-10-25

Initial public release for Windows x64.

- C++20 Vulkan Compute core with resident image handles, batch processing and offline SPIR-V.
- RAW decoding through pinned LibRaw 0.21.5; WIC JPEG / 16-bit TIFF / PNG export.
- Exposure, tone, HSL, curves, masks, bilateral denoise, detail and Panini geometry controls.
- Four histograms and 23 documented image metrics; versioned CRC-protected IMGARG recipes.
- `imgbp` CLI, C ABI / C++ wrapper and self-contained C# WinForms GUI.
- Reproducible build/package instructions, algorithm equations, research references and validation notes.

Known limits include Windows-only I/O, sRGB working space, synchronous API calls,
heuristic target matching and validation on one Intel Arc system. See README.
