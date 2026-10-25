# Changelog

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
