# Third-party notices

The MIT license at the repository root applies to original ImageBatchProcessor code and documentation. It does not replace the licenses of dependencies or of bundled runtime components.

## LibRaw 0.21.5

- Upstream: https://github.com/LibRaw/LibRaw
- Exact source revision: `e8b172e2197c3d45b35ffa992f6cfd9c49fdc100`.
- Source: the `external/LibRaw` submodule; the `ImageBatchProcessor-1.0.0-source-with-dependencies.zip` release asset contains a complete snapshot without requiring Git.
- Upstream offers LGPL 2.1 or CDDL 1.0. **This distribution elects CDDL 1.0.**
- LibRaw is statically linked into `ImageBatchProcessorCore.dll`; the upstream source is unmodified.
- Original notices are preserved in the source tree and the binary package under `licenses/LibRaw/`: `COPYRIGHT`, `LICENSE.CDDL` and `LICENSE.LGPL`.
- The included upstream sources contain additional notices for dcraw, DCB/FBDD, X3F and Adobe DNG SDK portions. Those notices remain applicable; see the upstream `COPYRIGHT` and affected source files.

Source is available alongside each published binary Release at https://github.com/Chilliziehen/ImageBatchProcessor/releases and through the pinned submodule. CDDL-covered files remain under CDDL; original project files are MIT. No warranty, support or indemnity is offered on behalf of upstream contributors.

## .NET runtime / Windows Desktop runtime

The self-contained Windows GUI includes Microsoft .NET runtime components. The packaging script copies license and third-party notice files from the exact restored NuGet runtime packages into `licenses/dotnet/`.

- Runtime source and notices: https://github.com/dotnet/runtime
- Windows Desktop / WinForms: https://github.com/dotnet/winforms and https://github.com/dotnet/wpf
- Package metadata records the runtime dependency versions in `gui/ImageBatchProcessorGui.deps.json` and `gui/ImageBatchProcessorGui.runtimeconfig.json`.

These components retain their own licenses and third-party terms. Consult the bundled notice files before redistributing them.

## Microsoft Visual C++ / OpenMP runtime

The Windows distribution includes redistributable Visual C++ / OpenMP runtime DLLs selected from the installed Visual Studio toolchain by CMake's `InstallRequiredSystemLibraries` module. These Microsoft files are not relicensed under MIT.

- [Visual Studio 2022 redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)
- [Distributing Visual C++ files](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)
- [Microsoft license terms](https://visualstudio.microsoft.com/license-terms/)

Windows itself, Windows Imaging Component and the GPU driver's Vulkan loader are system prerequisites, not bundled project code.

## Vulkan / shader compiler

The project uses Vulkan headers and import libraries from the developer's Vulkan SDK. Vulkan calls are implemented at runtime by the installed graphics driver.

`imgbp-shaderc.exe` is this project's MIT-licensed launcher. It invokes a separately installed `glslc`; neither `glslc` nor the Vulkan SDK is redistributed in the application package. Offline SPIR-V files are compiled from this project's shader sources.

## Papers and documentation

The README links to research papers and technical specifications as algorithmic references. Their PDFs and product implementations are not included or relicensed.
