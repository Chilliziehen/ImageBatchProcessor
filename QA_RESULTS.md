# 验证记录

核对日期：2026-10-25（Asia/Shanghai）。以下是本地测试结果，不表示所有操作系统、GPU 厂商或相机格式均已验证。

## 软件环境与覆盖

| 检查 | 结果 |
| --- | --- |
| Windows x64 / MSVC 2022 Release / CMake | 通过 |
| .NET 9 WinForms win-x64 self-contained publish | 通过 |
| Intel Arc 实际 Vulkan Compute | 通过；compute family 0，独立 transfer family 2 |
| `imgbp selftest` + Khronos validation layer | 通过，无 validation 错误输出 |
| 独立 Python/NumPy 与 GPU 数值对照 | 通过：均值、RMS、Hasler colourfulness、曝光、白平衡、色相、降噪平坦场不变性 |
| 径向、渐变、亮度蒙版 | 通过：独立中心/边缘/常量场数值断言 |
| Panini 非零映射 | 通过：有限值及非恒等变换；不是完整几何精度标定 |
| CPU/GPU 上传下载及奇数尺寸 | 通过 |
| IMGARG 往返、损坏 CRC、失效句柄、NaN 参数 | 通过 |
| Unicode 路径及 16 位 TIFF/PNG 往返 | 通过 |
| JPEG/TIFF 同时导出与 resume | 通过 |
| 独立 shaderc 工具 / spirv-val | 通过 |
| GUI 实际启动、RAW 前后预览、指标、高 DPI 面板 | 通过 |
| GUI 与 CLI 发行 DLL / SPV 的 SHA-256 一致性 | 通过 |

运行命令与依赖见 README；数值断言位于 `tests/integration.py`，GPU 基础检查位于 `imgbp/main.cpp` 的 selftest。测试采用合成输入以便公开复现。需要 Vulkan 设备的测试不可等同于普通编译检查。

## 实际批处理观察

- 本地 336 张 NEF 输出全分辨率、质量 95、带 sRGB ICC 的 JPEG，并保存配方及前后分析。
- 336 张输出全部可完整解码；尺寸、ICC、四通道直方图样本数、5408 字节 IMGARG 文件头/CRC 检查通过。
- 逐页检查所有输出缩略图，另检查若干原尺寸/预览；未宣称逐像素审查全部照片。
- 对 5 张近乎全黑输入进行 +6 EV 诊断后仍无有效场景细节。这些不可恢复情况保留为批处理的已知限制。
- 在同一版本、同一环境中，用一张图像的 `.imgarg` 和 `--fixed` 从原 RAW 重放，JPEG 的 SHA-256 与首次输出一致。
- 测试照片、相机元数据、个人文件路径及本地图库报告不包含在公开仓库或发行包中。

## 性能观察

GPU 下载缓冲优先 HOST_CACHED 后，同一约 24 MP 图像的 readback 从约 4.35 s 降至 0.25 s。数字来自单机观测，受内存、磁盘、进程和系统负载影响，不是可推广的端到端基准。所有例程同步返回，未实现多批次 CPU/GPU 异步重叠。

## 文档与发行

发布文档逐项对照 `analyze.comp`、`process.comp`、`core.cpp` 和 `arguments.cpp`，明确区分标准公式、工程近似与默认启发式。23 项分析量、采样步长、直方图量化、HDR 邻域差异和软目标规则均在 README 中说明。
