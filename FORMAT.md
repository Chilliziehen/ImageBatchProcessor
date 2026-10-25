# IMGARG v1 与参数索引

二进制文件固定 5408 字节，所有数值为 little endian；IEEE-754 float32/float64。不序列化 C++ padding 或指针。读取拒绝未知版本、长度/端序/工作空间错误、CRC 错误、非有限处理参数以及非法目标范围；参考统计字段不作完整语义校验。完整指标公式与采样约定见 [README](README.md#metrics)。

| 偏移 | 字节 | 内容 |
| --- | --- | --- |
| 0 | 8 | ASCII `IMGARG\r\n` |
| 8 | 4 | 版本 1 |
| 12 | 4 | 端序标记 0x01020304 |
| 16 | 4 | 头长度 32 |
| 20 | 4 | payload 长度 5376 |
| 24 | 4 | payload CRC32 / IEEE，poly 0xEDB88320 |
| 28 | 4 | 工作空间 ID=1，linear sRGB / D65 |
| 32 | 1024 | 256 个 float32 处理参数 |
| 1056 | 64 | 8 个 float64 目标边界 |
| 1120 | 184 | 23 个 float64 观感/统计值 |
| 1304 | 8 | uint64 样本数 |
| 1312 | 4096 | uint32 histogram[4][256]，Y/R/G/B 顺序 |

目标边界顺序：medianLow、medianHigh、p99Low、p99High、maxWhiteClip、maxBlackClip、colorfulnessLow、colorfulnessHigh。上下界需有序，值在 0–1 内。

23 项指标顺序：mean、median、p01、p05、p95、p99、rmsContrast、entropy、logAverage、dynamicRangeEV、blackClip、whiteClip、meanSaturation、colorfulness、meanR、meanG、meanB、labL、labA、labB、sharpness、edgeEnergy、noiseEstimate。

- Y=0.2126 R+0.7152 G+0.0722 B，R/G/B 为 sRGB 编码值。直方图索引 floor(clamp(channel,0,1)×255)，分位数返回对应 bin 的 `(index+0.5)/256` 近似，约 1/256 的量化精度。
- rmsContrast = stddev(Y)，使用总体方差且不除以均值；entropy = −Σ p log2(p)。logAverage = exp(mean(log(max(Y_linear,1e-7))))。
- dynamicRangeEV = log2(decode(p99)/decode(p01))，为鲁棒分位数动态范围，并非相机传感器原生动态范围。
- blackClip = fraction(Y<0.005)；whiteClip = fraction(max(R,G,B)>0.995)，任意通道的接近剪切率。
- colourfulness = sqrt(var(rg)+var(yb))+0.3×sqrt(mean(rg)²+mean(yb)²)，rg=R−G，yb=(R+G)/2−B。
- labL/A/B 为从线性 sRGB → XYZ D65 → CIELAB 的采样均值。
- sharpness 为四邻域拉普拉斯平方均值（不减拉普拉斯均值平方），edgeEnergy 为水平/垂直一阶差绝对值均值；noiseEstimate 为梯度<0.02 区域的 mean(abs(laplacian))/sqrt(20)，无合格样本时返回 0，是保守噪声代理，非标定传感器标准差。中心 RGB 裁剪到 0–1，邻点编码只截负值；HDR 外部输入的差异见 README。

## 处理参数 float32 索引

| 索引 | 参数 | 恒等/默认值 |
| --- | --- | --- |
| 0–5 | exposure EV, contrast, shadows EV, highlights EV, black, white | 0,1,0,0,0,1 |
| 6–11 | saturation, vibrance, hue degrees, temperature, tint, gamma | 1,0,0,0,0,1 |
| 12–16 | sharpen, denoise, clarity, dehaze, vignette | 0 |
| 17–25 | Panini d, horizontal FOV degrees, rotation degrees, k1,k2,perspectiveX,Y,zoom,chromaticAberration | 0,90,0,0,0,0,0,1,0 |
| 26–28 | grain, monochrome mix, highlight shoulder strength | 0 |
| 30–31 | mask type (0 off,1 radial,2 gradient,3 luminance), mask strength | 0,1 |
| 32–48 | 17 点全局亮度曲线，x=i/16 | y=i/16 |
| 49–58 | mask x,y,radius,feather,angle,localExposureEV,localSaturation,invert,lumaLow,lumaHigh | .5,.5,.5,.2,0,0,0,0,0,1 |
| 64–87 | 8 个色相区，每区 hue degrees,saturation delta,lightness delta | 0 |
| 96–112 | 红通道 17 点曲线 | i/16 |
| 113–129 | 绿通道 17 点曲线 | i/16 |
| 130–146 | 蓝通道 17 点曲线 | i/16 |
| 160–168 | 行优先 3×3 通道混合矩阵 | 单位阵 |
| 172–177 | 阴影 RGB、高光 RGB 偏移（编码空间） | 0 |

其余索引保留，初始化为 0。HSL 色相中心均匀分布 0°,45°,…315°，区间之间线性插值；17 点曲线分段线性插值。局部蒙版仅控制局部曝光和局部饱和度，叠加在全局处理之上。逆几何映射的图外像素为黑色；可调 zoom 裁去边角。

## 文件用途

`natural.imgarg` 是目标配置；每张导出旁的 `.imgarg` 存储已求出的处理参数、目标范围和处理后的实测统计。使用 `--fixed --target sidecar.imgarg` 重放；不加 `--fixed` 则把其中参数作为基础风格继续自动匹配。文件内不嵌入原始图像、不嵌入原文件路径，也不保存私有相机元数据。
