# 处理流水线与实现约定

本文描述 v1.0.0 的 `process.comp`，与 [README 的统计公式](../README.md#metrics) 配合使用。所有参数索引见 [FORMAT.md](../FORMAT.md)。以下 `clamp`、`lerp` 和 `smoothstep` 均采用 GLSL 语义；有效输入下，`smoothstep(a,b,x)` 为 $t^2(3-2t)$，其中 $t=\operatorname{clamp}((x-a)/(b-a),0,1)$。

输入/驻留结果为 linear sRGB；调整过程中可进入编码域，色调 pass 结束时解码回线性。边界采样夹到最近像素。参数 API 拒绝非有限数，但没有统一约束所有有限值；调用方需保持合理取值，如正 Gamma/zoom、有序蒙版上下界、非退化投影参数。

## Pass 0：逆几何

按输出像素中心定义 $uv=((x+0.5)/W,(y+0.5)/H)$，依次执行：

1. 中心缩放：$\mathbf n=(2uv-1)/\max(zoom,0.01)$。
2. 平面旋转：$\mathbf n\leftarrow R(\theta)\mathbf n$，$\theta=rotation\cdot\pi/180$；图像 y 轴朝下。
3. 透视除法：$\mathbf n\leftarrow\mathbf n/\max(0.1,1+p_xn_x+p_yn_y)$。
4. Panini 逆映射：使用 [README 所列公式](../README.md#processing)，`panini=0` 时跳过。
5. 径向映射：$r^2=\mathbf n\cdot\mathbf n$，$\mathbf n\leftarrow\mathbf n(1+k_1r^2+k_2r^4)$。
6. 映射回像素：$\mathbf q=(\mathbf n/2+1/2)(W,H)-1/2$，双线性采样。
7. 色差参数 $c_a$ 非零时，红通道在 $\mathbf n(1+c_a)$、蓝通道在 $\mathbf n(1-c_a)$ 处重新采样。

主坐标在输入图像外时结果为黑色。输出宽高保持不变；“裁切”是中心缩放，不生成任意裁剪尺寸。参数处在奇异投影范围时，不能靠有限数检查保证有效映射，建议非负且适度的 `panini`，并检查黑边。

## Pass 1：双边降噪

公式见 [README](../README.md#processing)。使用固定 5×5 邻域，空间权重分母为 5，颜色距离在 linear RGB 中计算；强度同时影响值域标准差和原图/滤波结果混合比例。

这是单次局部滤波，没有多尺度降噪、坏点检测器、噪声 profile 或非局部补丁匹配。即使某些异常像素被平滑，也不应把它描述为专用坏点修复。

## Pass 2：色调与色彩

本 pass 始终运行，顺序会影响结果。

### 2.1 蒙版、曝光、白平衡、通道混合

蒙版以 pass 输入的编码亮度 $Y'$ 和输出归一化位置 $uv$ 计算。令 $f=\max(feather,0.001)$、中心 $\mathbf c=(mask_x,mask_y)$：

- 径向：$m=1-\operatorname{smoothstep}(\max(0,r-f),r,\|uv-\mathbf c\|)$，$r=mask\_radius$。
- 渐变：$m=\operatorname{smoothstep}(-f,f,(uv-\mathbf c)\cdot(\cos\theta,\sin\theta))$。
- 亮度：$m=\operatorname{smoothstep}(l,l+f,Y')[1-\operatorname{smoothstep}(u-f,u,Y')]$。
- 反选开启时 $m\leftarrow1-m$；最终 $w=\operatorname{clamp}(m\cdot strength,0,1)$。类型 0 直接使用 $w=0$。

曝光和 RGB 增益见 README；随后按行优先 3×3 参数矩阵进行通道混合。蒙版只作用于局部曝光和局部饱和度，不能给所有效果添加任意蒙版堆栈。

### 2.2 阴影、高光、黑白点与肩部

以完成通道混合后的线性亮度 $y=\max(\mathbf c\cdot(0.2126,0.7152,0.0722),10^{-7})$ 计算：

$$
e_s=shadows\cdot e^{-6y},\qquad
e_h=highlights\cdot\operatorname{smoothstep}(0.15,1,y),\qquad
\mathbf c\leftarrow\mathbf c\,2^{e_s+e_h}.
$$

然后：

$$
\mathbf c\leftarrow\max\left(\frac{\mathbf c-black}{\max(0.01,white-black)},0\right).
$$

`shoulder>0` 且最大通道超过 0.60 时使用 README 的统一 RGB 比例压缩。该策略在此步保持 RGB 比例，后续编码域裁剪/色彩操作仍会改变颜色。

### 2.3 编码域对比度、Gamma、曲线和去雾

进入 sRGB 编码域后：

$$
\mathbf c\leftarrow\operatorname{clamp}((E(\mathbf c)-0.5)contrast+0.5,0,1),\quad
\mathbf c\leftarrow\mathbf c^{1/\max(0.1,gamma)}.
$$

17 点曲线在 $x_i=i/16$ 定义，区间内线性插值。全局曲线先根据 $y=\max(\mathbf c\cdot\mathbf w,10^{-6})$ 计算比例 $T(y)/y$ 作用于 RGB，再分别执行 R/G/B 曲线。它不是 CIELAB L* 曲线，节点也没有自动强制单调性。

去雾参数 $d_h$ 使用：

$$
\mathbf c\leftarrow\operatorname{clamp}
 \left(\frac{\mathbf c-0.06d_h}{1-0.12d_h},0,1\right).
$$

这是对比度近似，不估计大气光或透射率；不得使用使分母为零的参数，也不应把它当作基于物理散射的恢复算法。

### 2.4 HSL、黑白、分离色调、暗角与颗粒

将裁剪 RGB 转换为 HSL。8 个色相区位于 $0^\circ,45^\circ,\ldots,315^\circ$，根据当前色相在相邻区间插值出 $(\delta h,\delta s,\delta l)$，首尾循环：

$$
h'=\operatorname{fract}(h+(hue+\delta h)/360),
$$

$$
s'=\operatorname{clamp}
  (s\cdot saturation\cdot[1+vibrance(1-s)]
     \cdot(1+\delta s)\cdot(1+w\,localSaturation),0,1),\quad
l'=\operatorname{clamp}(l+\delta l,0,1).
$$

自然饱和度对当前低饱和颜色施加更强作用，但没有肤色检测/保护。HSL 转回 RGB 后，依次：

1. 以 `monochrome` 在当前 RGB 与编码域亮度灰阶之间混合。
2. 阴影 RGB 偏移乘 $1-\operatorname{smoothstep}(0.15,0.65,l)$，高光偏移乘 $\operatorname{smoothstep}(0.35,0.85,l)$；这里 $l$ 为当前编码亮度。
3. 暗角乘 $2^{-v\,r^2}$，$r=\operatorname{clamp}(\|1.414(uv-0.5)\|,0,1)$。
4. 颗粒加入 $(\operatorname{fract}(\sin((x,y)\cdot(12.9898,78.233))43758.5453)-0.5)\,grain\,0.06$。
5. RGB 截到 $[0,1]$，解码回线性。

颗粒是基于像素坐标的确定性伪随机扰动，无时间种子；不同驱动的三角函数近似可能改变结果，不保证跨设备逐字节一致。

## Pass 3：锐化与局部对比度

$B_n$ 为 3×3 均值，$B_f$ 为采样间隔 12 像素的稀疏 3×3 均值；不是完整 25×25 均值。令 $\mathbf d=\mathbf c-B_n$：

$$
g=\operatorname{smoothstep}(0.001,0.012,\|\mathbf d\|),\qquad
\mathbf c'=\operatorname{clamp}
 (\mathbf c+sharpen\cdot\mathbf d\,g+0.3\,clarity(\mathbf c-B_f),0,1).
$$

门限可减少平坦区域锐化，但没有反卷积、边缘晕环抑制或多尺度频带分解。结果对分辨率敏感；预览参数应在全尺寸图上复核。

## 对应来源

- 双边滤波思想：Tomasi & Manduchi (1998)，但距离度量、固定核与强度映射为本文所列实现。
- Panini 投影族：Sharpless, Postle & German (2010)，本项目采用有限参数的逆采样形式。
- 其余调整和默认常量：本项目工程定义，不声称来自上述论文。
- 完整引用和 BibTeX 见 [README](../README.md#references) 与 [references.bib](references.bib)。
