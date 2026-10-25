using System.Runtime.InteropServices;
using System.Text;

namespace ImageBatchProcessorGui;
internal static unsafe class Native {
  const string Dll = "ImageBatchProcessorCore.dll";
  [StructLayout(LayoutKind.Sequential)]
  internal struct Host {
    public uint Width, Height;
    public IntPtr Pixels;
  }
  [StructLayout(LayoutKind.Sequential)]
  internal struct Parameters {
    public fixed float Values[256];
  }
  [StructLayout(LayoutKind.Sequential)]
  internal struct Analysis {
    public fixed double Metrics[23];
    public ulong Samples;
    public fixed uint Histogram[1024];
  }
  [StructLayout(LayoutKind.Sequential)]
  internal struct Target {
    public Parameters Parameters;
    public fixed double Ranges[8];
    public Analysis Reference;
  }
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_Create([MarshalAs(UnmanagedType.LPUTF8Str)] string? shaders,
                                        out IntPtr context);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern void IBP_Destroy(IntPtr context);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern IntPtr IBP_DeviceInfo(IntPtr context);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  static extern IntPtr IBP_LastError();
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern void IBP_DefaultParams(out Parameters p);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern void IBP_DefaultTarget(out Target t);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_LoadImage([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
                                           int half, out Host image);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern void IBP_FreeHostImage(ref Host image);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_SaveImage([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
                                           in Host image, int quality);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int UploadImage(IntPtr context, in Host image, out ulong handle);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int UploadImageBatchMode(IntPtr context, [In] Host[] images, uint count,
                                                  [Out] ulong[] handles);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int DownloadImage(IntPtr context, ulong handle, out Host image);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_ProcessBatch(IntPtr context, [In] ulong[] handles,
                                              [In] Parameters[] parameters, uint count);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_Analyze(IntPtr context, ulong handle, out Analysis result);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_ReleaseImage(IntPtr context, ulong handle);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_SuggestParams(in Analysis analysis, in Target target,
                                               out Parameters p);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_SaveTarget([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
                                            in Target target);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_LoadTarget([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
                                            out Target target);
  [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
  internal static extern int IBP_WriteAnalysisJson([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
                                                   in Analysis a);
  internal static void Check(int r) {
    if (r != 0)
      throw new InvalidOperationException(Marshal.PtrToStringUTF8(IBP_LastError()));
  }
  internal static float Get(Parameters p, int i) => p.Values[i];
  internal static double GetRange(Target t, int i) => t.Ranges[i];
  internal static void SetRange(ref Target t, int i, double v) {
    fixed(double *p = t.Ranges) p[i] = v;
  }
  internal static void Set(ref Parameters p, int i, float v) {
    fixed(float *f = p.Values) f[i] = v;
  }
  internal static string Describe(Analysis a) =>
      $"Median {a.Metrics[1]:F3} · P99 {a.Metrics[5]:F3} · Highlights clipped {a.Metrics[11]:P2}\r\nContrast {a.Metrics[6]:F3} · Colourfulness {a.Metrics[13]:F3} · Entropy {a.Metrics[7]:F2}\r\nCIELAB L* {a.Metrics[17]:F1}, a* {a.Metrics[18]:F1}, b* {a.Metrics[19]:F1}";
}
internal sealed class MainForm : Form {
  IntPtr context;
  Native.Target target;
  readonly Dictionary<int, NumericUpDown> controls = new();
  readonly Dictionary<int, NumericUpDown> ranges = new();
  readonly ListBox files = new() { Dock = DockStyle.Fill };
  readonly PictureBox before = new() { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom,
                                       BackColor = Color.FromArgb(25, 28, 32) };
  readonly PictureBox after = new() { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom,
                                      BackColor = Color.FromArgb(25, 28, 32) };
  readonly Label status = new() { AutoSize = false, Dock = DockStyle.Bottom, Height = 100,
                                  Padding = new Padding(12), Text = "Ready" };
  readonly ProgressBar progress = new() { Dock = DockStyle.Bottom, Height = 8 };
  readonly CheckBox auto = new() { Text = "按目标自动匹配", Checked = true, AutoSize = true };
  readonly Button cancel = new() { Text = "取消批处理", Enabled = false, AutoSize = true };
  readonly List<Control> actions = new();
  CancellationTokenSource? cancellation;
  bool busy;
  public MainForm(string? initialPath = null) {
    Text = "ImageBatchProcessor · RAW 工作室";
    Width = 1480;
    Height = 940;
    MinimumSize = new Size(1050, 720);
    Font = new Font("Microsoft YaHei UI", 9);
    BackColor = Color.FromArgb(245, 246, 248);
    Native.Check(Native.IBP_Create(null, out context));
    Native.IBP_DefaultTarget(out target);
    var bar = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 54, Padding = new Padding(8),
                                    WrapContents = false };
    AddButton(bar, "添加照片", AddFiles);
    AddButton(bar, "添加目录", AddFolder);
    AddButton(bar, "读取目标", LoadTarget);
    AddButton(bar, "保存目标", SaveTarget);
    AddButton(bar, "更新预览", () => _ = Preview());
    AddButton(bar, "批量导出", () => _ = Export());
    AddButton(bar, "重置", () => {
      Native.IBP_DefaultTarget(out target);
      SyncControls();
    });
    bar.Controls.Add(auto);
    bar.Controls.Add(cancel);
    cancel.Click += (_, _) => cancellation?.Cancel();
    var split = new SplitContainer { Dock = DockStyle.Fill, FixedPanel = FixedPanel.Panel1,
                                     SplitterDistance = 240, Width = 1200 };
    split.Panel1.Controls.Add(files);
    files.SelectedIndexChanged += async (_, _) => {
      if (!busy)
        await Preview();
    };
    files.FormattingEnabled = true;
    files.Format += (_, e) => {
      if (e.ListItem is string f)
        e.Value = Path.GetFileName(f);
    };
    var work = new SplitContainer { Dock = DockStyle.Fill, FixedPanel = FixedPanel.Panel2,
                                    SplitterDistance = 780, Width = 1110 };
    var previews = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 2 };
    previews.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
    previews.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
    previews.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
    previews.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
    previews.Controls.Add(new Label { Text = "解码原图", Dock = DockStyle.Fill,
                                      TextAlign = ContentAlignment.MiddleCenter },
                          0, 0);
    previews.Controls.Add(new Label { Text = "处理结果", Dock = DockStyle.Fill,
                                      TextAlign = ContentAlignment.MiddleCenter },
                          1, 0);
    previews.Controls.Add(before, 0, 1);
    previews.Controls.Add(after, 1, 1);
    work.Panel1.Controls.Add(previews);
    var tabs = new TabControl { Dock = DockStyle.Fill };
    var basic = Page(tabs, "基本");
    Add(basic, "曝光 EV", 0, -5, 5, .05m);
    Add(basic, "对比度", 1, .1m, 2, .025m);
    Add(basic, "阴影 EV", 2, -2, 2, .05m);
    Add(basic, "高光 EV", 3, -2, 2, .05m);
    Add(basic, "黑点", 4, -.1m, .3m, .005m);
    Add(basic, "白点", 5, .1m, 3, .025m);
    Add(basic, "饱和度", 6, 0, 2, .025m);
    Add(basic, "自然饱和度", 7, -1, 1, .025m);
    Add(basic, "色相 °", 8, -180, 180, 1);
    Add(basic, "冷暖", 9, -2, 2, .05m);
    Add(basic, "色调", 10, -2, 2, .05m);
    Add(basic, "Gamma", 11, .1m, 3, .025m);
    Add(basic, "锐化", 12, 0, 2, .05m);
    Add(basic, "降噪", 13, 0, 1, .05m);
    Add(basic, "清晰度", 14, -1, 1, .05m);
    Add(basic, "去雾", 15, -1, 1, .05m);
    Add(basic, "暗角", 16, -2, 2, .05m);
    Add(basic, "颗粒", 26, 0, 1, .05m);
    Add(basic, "黑白混合", 27, 0, 1, .05m);
    Add(basic, "高光肩部", 28, 0, 1, .05m);
    var geometry = Page(tabs, "几何");
    Add(geometry, "Panini d", 17, 0, 3, .05m);
    Add(geometry, "水平视角 °", 18, 10, 160, 1);
    Add(geometry, "旋转 °", 19, -180, 180, .1m);
    Add(geometry, "径向 k1", 20, -1, 1, .005m);
    Add(geometry, "径向 k2", 21, -1, 1, .005m);
    Add(geometry, "透视 X", 22, -.8m, .8m, .01m);
    Add(geometry, "透视 Y", 23, -.8m, .8m, .01m);
    Add(geometry, "缩放裁切", 24, .2m, 4, .025m);
    Add(geometry, "色差", 25, -.02m, .02m, .0005m);
    var hsl = Page(tabs, "HSL");
    string[] hues = { "红", "橙", "黄绿", "绿", "青", "蓝", "紫", "洋红" };
    for (int i = 0; i < 8; i++) {
      Add(hsl, hues[i] + " 色相", 64 + i * 3, -180, 180, 1);
      Add(hsl, hues[i] + " 饱和度", 65 + i * 3, -1, 1, .025m);
      Add(hsl, hues[i] + " 明度", 66 + i * 3, -.5m, .5m, .01m);
    }
    var curves = Page(tabs, "曲线");
    foreach (var item in new[] { ("亮度", 32), ("红", 96), ("绿", 113), ("蓝", 130) })
      for (int i = 0; i < 17; i++)
        Add(curves, $"{item.Item1} {i / 16.0:F3}", item.Item2 + i, 0, 1, .01m);
    var mixer = Page(tabs, "混色");
    for (int i = 0; i < 9; i++)
      Add(mixer, $"矩阵 {i / 3 + 1},{i % 3 + 1}", 160 + i, -2, 2, .025m);
    for (int i = 0; i < 6; i++)
      Add(mixer, $"{(i < 3 ? "阴影" : "高光")} {"RGB"[i % 3]}", 172 + i, -.3m, .3m, .01m);
    var local = Page(tabs, "局部");
    Add(local, "蒙版 0关/1径向/2渐变/3亮度", 30, 0, 3, 1);
    Add(local, "蒙版强度", 31, 0, 1, .025m);
    Add(local, "中心 X", 49, 0, 1, .01m);
    Add(local, "中心 Y", 50, 0, 1, .01m);
    Add(local, "半径", 51, .01m, 2, .01m);
    Add(local, "羽化", 52, .001m, 1, .01m);
    Add(local, "角度 °", 53, -180, 180, 1);
    Add(local, "局部曝光 EV", 54, -5, 5, .05m);
    Add(local, "局部饱和度", 55, -1, 1, .025m);
    Add(local, "反选", 56, 0, 1, 1);
    Add(local, "亮度下界", 57, 0, 1, .01m);
    Add(local, "亮度上界", 58, 0, 1, .01m);
    var goal = Page(tabs, "目标");
    string[] labels = { "中位亮度下界", "中位亮度上界", "P99 下界",       "P99 上界",
                        "高光剪切上限", "暗部剪切上限", "色彩丰富度下界", "色彩丰富度上界" };
    for (int i = 0; i < 8; i++) {
      var row = new FlowLayoutPanel { Width = 290, Height = 32 };
      row.Controls.Add(new Label { Text = labels[i], Width = 155 });
      var number = new NumericUpDown { Minimum = 0, Maximum = 1, DecimalPlaces = 3,
                                       Increment = .005m, Width = 108 };
      ranges[i] = number;
      row.Controls.Add(number);
      goal.Controls.Add(row);
    }
    work.Panel2.Controls.Add(tabs);
    split.Panel2.Controls.Add(work);
    Controls.Add(split);
    Controls.Add(status);
    Controls.Add(progress);
    Controls.Add(bar);
    status.Text = Marshal.PtrToStringUTF8(Native.IBP_DeviceInfo(context));
    SyncControls();
    Shown += (_, _) => {
      split.SplitterDistance = (int)(split.ClientSize.Width * .18);
      work.SplitterDistance = (int)(work.ClientSize.Width * .65);
      if (initialPath != null && File.Exists(initialPath)) {
        files.Items.Add(initialPath);
        files.SelectedIndex = 0;
      }
    };
    FormClosing += (_, e) => {
      if (busy) {
        cancellation?.Cancel();
        e.Cancel = true;
        status.Text = "正在结束当前照片，请稍后关闭。";
      } else {
        before.Image?.Dispose();
        after.Image?.Dispose();
        Native.IBP_Destroy(context);
        context = IntPtr.Zero;
      }
    };
  }
  void AddButton(FlowLayoutPanel panel, string text, Action action) {
    var b = new Button { Text = text, AutoSize = true, Height = 32 };
    b.Click += (_, _) => action();
    panel.Controls.Add(b);
    actions.Add(b);
  }
  static FlowLayoutPanel Page(TabControl tabs, string name) {
    var page = new TabPage(name);
    var flow = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown,
                                     WrapContents = false, AutoScroll = true };
    page.Controls.Add(flow);
    tabs.TabPages.Add(page);
    return flow;
  }
  void Add(FlowLayoutPanel panel, string title, int index, decimal min, decimal max,
           decimal increment) {
    var row = new FlowLayoutPanel { Width = 290, Height = 32 };
    row.Controls.Add(
        new Label { Text = title, Width = 155, TextAlign = ContentAlignment.MiddleLeft });
    var field = new NumericUpDown { Minimum = min, Maximum = max, Increment = increment,
                                    DecimalPlaces = increment < .001m ? 4 : 3, Width = 108 };
    row.Controls.Add(field);
    controls[index] = field;
    panel.Controls.Add(row);
  }
  void SyncControls() {
    foreach (var (i, c) in controls)
      c.Value = Math.Clamp((decimal)Native.Get(target.Parameters, i), c.Minimum, c.Maximum);
    foreach (var (i, c) in ranges)
      c.Value = Math.Clamp((decimal)Native.GetRange(target, i), 0, 1);
  }
  Native.Parameters ReadControls() {
    var p = target.Parameters;
    foreach (var (i, c) in controls)
      Native.Set(ref p, i, (float)c.Value);
    return p;
  }
  Native.Target ReadTarget() {
    var t = target;
    t.Parameters = ReadControls();
    foreach (var (i, c) in ranges)
      Native.SetRange(ref t, i, (double)c.Value);
    return t;
  }
  void AddFiles() {
    using var d =
        new OpenFileDialog { Multiselect = true,
                             Filter = "RAW " +
                                      "与图像|*.nef;*.nrw;*.cr2;*.cr3;*.arw;*.dng;*.raf;*.rw2;*." +
                                      "orf;*.pef;*.jpg;*.tif;*.png|所有文件|*.*" };
    if (d.ShowDialog() == DialogResult.OK)
      foreach (var f in d.FileNames)
        if (!files.Items.Contains(f))
          files.Items.Add(f);
  }
  void AddFolder() {
    using var d = new FolderBrowserDialog();
    if (d.ShowDialog() == DialogResult.OK) {
      var ext = new HashSet<string>(new[] { ".nef", ".nrw", ".cr2", ".cr3", ".arw", ".dng", ".raf",
                                            ".rw2", ".orf", ".pef", ".raw" },
                                    StringComparer.OrdinalIgnoreCase);
      foreach (var f in Directory.GetFiles(d.SelectedPath)
                   .Where(f => ext.Contains(Path.GetExtension(f)))
                   .Order())
        if (!files.Items.Contains(f))
          files.Items.Add(f);
    }
  }
  void LoadTarget() {
    using var d = new OpenFileDialog { Filter = "图像目标|*.imgarg" };
    if (d.ShowDialog() == DialogResult.OK)
      try {
        Native.Check(Native.IBP_LoadTarget(d.FileName, out target));
        SyncControls();
        status.Text = "目标已载入";
      } catch (Exception ex) {
        ShowError(ex);
      }
  }
  void SaveTarget() {
    using var d = new SaveFileDialog { Filter = "图像目标|*.imgarg", FileName = "target.imgarg" };
    if (d.ShowDialog() == DialogResult.OK)
      try {
        target = ReadTarget();
        Native.Check(Native.IBP_SaveTarget(d.FileName, in target));
        status.Text = "目标已保存";
      } catch (Exception ex) {
        ShowError(ex);
      }
  }
  void Busy(bool value) {
    busy = value;
    foreach (var a in actions)
      a.Enabled = !value;
    files.Enabled = !value;
    auto.Enabled = !value;
    cancel.Enabled = value;
  }
  void ShowError(Exception ex) {
    status.Text = ex.Message;
    MessageBox.Show(this, ex.Message, "处理失败", MessageBoxButtons.OK, MessageBoxIcon.Error);
  }
  static Bitmap BitmapFrom(in Native.Host h) {
    string p = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".jpg");
    try {
      Native.Check(Native.IBP_SaveImage(p, in h, 92));
      using var b = new Bitmap(p);
      return new Bitmap(b);
    } finally {
      if (File.Exists(p))
        File.Delete(p);
    }
  }
  async Task Preview() {
    if (busy || files.SelectedItem is not string path)
      return;
    Busy(true);
    var t = ReadTarget();
    bool automatic = auto.Checked;
    status.Text = "正在解码和计算预览…";
    try {
      var result = await Task.Run(() => {
        Native.Host input = default, output = default;
        ulong id = 0;
        try {
          Native.Check(Native.IBP_LoadImage(path, 1, out input));
          Native.Check(Native.UploadImage(context, in input, out id));
          Native.Check(Native.IBP_Analyze(context, id, out var analysis));
          var p = t.Parameters;
          if (automatic)
            Native.Check(Native.IBP_SuggestParams(in analysis, in t, out p));
          var original = BitmapFrom(in input);
          Native.Check(Native.IBP_ProcessBatch(context, new[] { id }, new[] { p }, 1));
          Native.Check(Native.IBP_Analyze(context, id, out var final));
          Native.Check(Native.DownloadImage(context, id, out output));
          var edited = BitmapFrom(in output);
          return (original, edited, final);
        } finally {
          if (id != 0)
            Native.IBP_ReleaseImage(context, id);
          Native.IBP_FreeHostImage(ref input);
          Native.IBP_FreeHostImage(ref output);
        }
      });
      before.Image?.Dispose();
      after.Image?.Dispose();
      before.Image = result.original;
      after.Image = result.edited;
      status.Text = Native.Describe(result.final);
    } catch (Exception ex) {
      ShowError(ex);
    } finally {
      Busy(false);
    }
  }
  async Task Export() {
    if (busy || files.Items.Count == 0)
      return;
    using var dialog =
        new FolderBrowserDialog { Description = "选择导出目录（JPEG 95 + 参数文件）" };
    if (dialog.ShowDialog() != DialogResult.OK)
      return;
    var paths = files.Items.Cast<string>().ToArray();
    var directory = dialog.SelectedPath;
    var t = ReadTarget();
    bool automatic = auto.Checked;
    Busy(true);
    cancellation = new();
    progress.Maximum = paths.Length;
    progress.Value = 0;
    int ok = 0, fail = 0;
    var messages = new List<string>();
    var report = new Progress<string>(s => {
      status.Text = s;
      progress.Value = Math.Min(progress.Maximum, progress.Value + 1);
    });
    try {
      await Task.Run(() => {
        foreach (var source in paths) {
          if (cancellation.IsCancellationRequested)
            break;
          Native.Host input = default, output = default;
          ulong id = 0;
          try {
            string stem = Path.Combine(directory, Path.GetFileNameWithoutExtension(source));
            if (File.Exists(stem + ".jpg"))
              throw new IOException("输出已存在：" + stem);
            Native.Check(Native.IBP_LoadImage(source, 0, out input));
            Native.Check(Native.UploadImage(context, in input, out id));
            Native.IBP_FreeHostImage(ref input);
            Native.Check(Native.IBP_Analyze(context, id, out var analysis));
            var p = t.Parameters;
            if (automatic)
              Native.Check(Native.IBP_SuggestParams(in analysis, in t, out p));
            Native.Check(Native.IBP_ProcessBatch(context, new[] { id }, new[] { p }, 1));
            Native.Check(Native.IBP_Analyze(context, id, out var final));
            Native.Check(Native.DownloadImage(context, id, out output));
            Native.Check(Native.IBP_SaveImage(stem + ".jpg", in output, 95));
            var recipe = t;
            recipe.Parameters = p;
            recipe.Reference = final;
            Native.Check(Native.IBP_SaveTarget(stem + ".imgarg", in recipe));
            Native.Check(Native.IBP_WriteAnalysisJson(stem + ".analysis.json", in final));
            ok++;
          } catch (Exception ex) {
            fail++;
            messages.Add(source + ": " + ex.Message);
          } finally {
            if (id != 0)
              Native.IBP_ReleaseImage(context, id);
            Native.IBP_FreeHostImage(ref input);
            Native.IBP_FreeHostImage(ref output);
          }
          ((IProgress<string>)report)
              .Report($"已处理 {ok + fail}/{paths.Length} · {Path.GetFileName(source)}");
        }
      });
      File.WriteAllLines(Path.Combine(directory, "gui-batch.log"), messages);
      status.Text = $"完成 {ok} 张，失败 {fail} 张" +
                    (cancellation.IsCancellationRequested ? "，已取消后续任务" : "");
    } catch (Exception ex) {
      ShowError(ex);
    } finally {
      cancellation.Dispose();
      cancellation = null;
      Busy(false);
    }
  }
}
internal static class Program {
  [STAThread]
  static void Main(string[] args) {
    ApplicationConfiguration.Initialize();
    try {
      Application.Run(new MainForm(args.FirstOrDefault()));
    } catch (Exception ex) {
      MessageBox.Show(ex.ToString(), "ImageBatchProcessor 初始化失败");
    }
  }
}
