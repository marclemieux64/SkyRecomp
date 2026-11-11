using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Forms;

namespace SkyRecompQuestPackager;

internal static class Program
{
    internal const string AppName = "Skyrecomp-VR APK Builder";

    [STAThread]
    private static int Main(string[] args)
    {
        // GDI+ text rendering is required for the launcher font, which is loaded privately.
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(true);
        Application.SetHighDpiMode(HighDpiMode.SystemAware);
        try
        {
            var root = EnsurePayload();
            Theme.Load(root);
            using var form = new BuilderForm(root);
            var shot = Array.FindIndex(args, a => a.Equals("--screenshot", StringComparison.OrdinalIgnoreCase));
            if (shot >= 0 && shot + 1 < args.Length) { form.SaveScreenshot(args[shot + 1]); return 0; }
            if (args.Contains("--self-test", StringComparer.OrdinalIgnoreCase)) return form.RunSelfTest() ? 0 : 1;
            Application.Run(form);
            return 0;
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, AppName, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 2;
        }
    }

    private static string EnsurePayload()
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
        {
            var directory = new DirectoryInfo(start);
            while (directory is not null)
            {
                if (File.Exists(Path.Combine(directory.FullName, "tools", "quest", "build-personal-apk.ps1")))
                    return directory.FullName;
                directory = directory.Parent;
            }
        }

        var assembly = Assembly.GetExecutingAssembly();
        using var payload = assembly.GetManifestResourceStream("SkyRecomp.Payload.zip")
            ?? throw new InvalidOperationException("The embedded builder payload is missing.");
        var version = assembly.GetName().Version?.ToString(3) ?? "0.2.0";
        var root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            AppName, "payload", version);
        // The stamp identifies this exact executable, so a rebuilt builder with
        // the same version number still refreshes its extracted sources.
        var marker = Path.Combine(root, ".ready");
        var stamp = $"{AppName} {version} {assembly.ManifestModule.ModuleVersionId}";
        if (!File.Exists(marker) || File.ReadAllText(marker) != stamp)
        {
            Directory.CreateDirectory(root);
            File.Delete(marker);
            using var archive = new ZipArchive(payload, ZipArchiveMode.Read);
            // Sources an earlier version unpacked and this one no longer ships are removed (the build picks up
            // every source file it finds); build outputs are left alone.
            var manifest = Path.Combine(root, ".files");
            var shipped = archive.Entries.Where(e => e.Name.Length > 0).Select(e => e.FullName).ToHashSet();
            if (File.Exists(manifest))
                foreach (var stale in File.ReadAllLines(manifest).Where(name => name.Length > 0 && !shipped.Contains(name)))
                {
                    var path = Path.GetFullPath(Path.Combine(root, stale));
                    if (path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.Ordinal)) File.Delete(path);
                }
            archive.ExtractToDirectory(root, true);
            File.WriteAllLines(manifest, shipped);
            File.WriteAllText(marker, stamp);
        }
        return root;
    }
}

// Colours, rounding and font of the in-game option menu (runtime/ui/menu.mm style_init).
internal static class Theme
{
    internal static readonly Color Gold = Color.FromArgb(238, 198, 92);
    internal static readonly Color GoldBright = Color.FromArgb(255, 228, 118);
    internal static readonly Color Ivory = Color.FromArgb(246, 241, 227);
    internal static readonly Color Sage = Color.FromArgb(182, 188, 176);
    internal static readonly Color Panel = Color.FromArgb(13, 23, 26);
    internal static readonly Color PanelHot = Color.FromArgb(22, 42, 44);
    internal static readonly Color GoldDim = Color.FromArgb(158, 125, 51);
    internal static readonly Color WindowBg = Color.FromArgb(15, 23, 20);
    internal static readonly Color ChildBg = Color.FromArgb(23, 33, 28);
    internal static readonly Color FrameBg = Color.FromArgb(33, 48, 41);
    internal static readonly Color Button = Color.FromArgb(36, 56, 46);
    internal static readonly Color ButtonHot = Color.FromArgb(38, 92, 64);
    internal static readonly Color ButtonDown = Color.FromArgb(54, 128, 87);
    private static PrivateFontCollection? collection;
    private static FontFamily family = new("Segoe UI");

    internal static void Load(string root)
    {
        try
        {
            var file = Path.Combine(root, "fonts", "Baloo2-SemiBold.ttf");
            if (!File.Exists(file)) return;
            collection = new PrivateFontCollection();
            collection.AddFontFile(file);
            family = collection.Families[0];
        }
        catch (Exception) { /* keep the system font */ }
    }

    internal static Font Font(float size)
    {
        var style = family.IsStyleAvailable(FontStyle.Regular) ? FontStyle.Regular : FontStyle.Bold;
        return new Font(family, size, style);
    }

    internal static GraphicsPath Rounded(RectangleF r, float radius)
    {
        var d = radius * 2;
        var path = new GraphicsPath();
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }
}

// The option menu's button: a rounded dark green block that turns green under the pointer.
internal sealed class PillButton : Button
{
    private bool hot, down;

    internal PillButton()
    {
        SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.UserPaint |
                 ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        BackColor = Color.Transparent;
        Font = Theme.Font(10.5f);
        Cursor = Cursors.Hand;
    }

    protected override void OnMouseEnter(EventArgs e) { hot = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hot = down = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        InvokePaintBackground(this, e);
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
        var color = !Enabled ? Color.FromArgb(26, 38, 32) : down ? Theme.ButtonDown : hot || Focused ? Theme.ButtonHot : Theme.Button;
        using var path = Theme.Rounded(new RectangleF(0.5f, 0.5f, Width - 1, Height - 1), 8);
        using var fill = new SolidBrush(color);
        g.FillPath(fill, path);
        using var text = new SolidBrush(Enabled ? Theme.Ivory : Color.FromArgb(158, 163, 148));
        using var format = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
        g.DrawString(Text, Font, text, new RectangleF(0, 1, Width, Height), format);
    }
}

// Text that lets the mouse through, so the window can be dragged by its heading.
internal sealed class PassLabel : Label
{
    protected override void WndProc(ref Message m)
    {
        if (m.Msg == 0x84) { m.Result = (IntPtr)(-1); return; }  // WM_NCHITTEST: HTTRANSPARENT
        base.WndProc(ref m);
    }
}

// Minimise / close, drawn in the window itself (there is no system title bar).
internal sealed class GlyphButton : Control
{
    private readonly bool close;
    private bool hot;

    internal GlyphButton(bool closeGlyph)
    {
        close = closeGlyph;
        SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.UserPaint |
                 ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        BackColor = Color.Transparent;
        Size = new Size(34, 26);
    }

    protected override void OnMouseEnter(EventArgs e) { hot = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hot = false; Invalidate(); base.OnMouseLeave(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        if (hot)
        {
            using var path = Theme.Rounded(new RectangleF(0.5f, 0.5f, Width - 1, Height - 1), 8);
            using var fill = new SolidBrush(close ? Color.FromArgb(150, 48, 40) : Theme.ButtonHot);
            g.FillPath(fill, path);
        }
        using var pen = new Pen(hot ? Theme.Ivory : Theme.Sage, 1.6f);
        float cx = Width / 2f, cy = Height / 2f, r = 4.5f;
        if (close)
        {
            g.DrawLine(pen, cx - r, cy - r, cx + r, cy + r);
            g.DrawLine(pen, cx - r, cy + r, cx + r, cy - r);
        }
        else g.DrawLine(pen, cx - r, cy + 3, cx + r, cy + 3);
    }
}

// The build log: wrapped monospace lines in a rounded panel with the menu's slim gold scroll bar.
internal sealed class LogView : Control
{
    private const int Pad = 10, BarWidth = 8;
    private readonly List<string> lines = new();
    private readonly List<(int Line, int Start, int Length)> rows = new();
    private int top, columns = 80, rowHeight = 14;
    private float charWidth = 7;
    private bool follow = true, barHot, dragging;
    private int dragOffset;

    internal LogView()
    {
        SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                 ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
        BackColor = Color.Transparent;
        Font = new Font("Consolas", 9f);
        var menu = new ContextMenuStrip();
        menu.Items.Add("Copy log", null, (_, _) => CopyAll());
        ContextMenuStrip = menu;
    }

    private int VisibleRows => Math.Max(1, (Height - Pad * 2) / rowHeight);
    private int MaxTop => Math.Max(0, rows.Count - VisibleRows);

    internal void Clear() { lines.Clear(); rows.Clear(); top = 0; follow = true; Invalidate(); }

    internal void AddLine(string text)
    {
        lines.Add(text);
        Wrap(lines.Count - 1);
        Settle();
    }

    internal void ReplaceLastLine(string text)
    {
        if (lines.Count == 0) { AddLine(text); return; }
        var last = lines.Count - 1;
        rows.RemoveAll(row => row.Line == last);
        lines[last] = text;
        Wrap(last);
        Settle();
    }

    internal void CopyAll()
    {
        if (lines.Count > 0) Clipboard.SetText(string.Join(Environment.NewLine, lines));
    }

    private void Wrap(int line)
    {
        var length = lines[line].Length;
        if (length == 0) { rows.Add((line, 0, 0)); return; }
        for (var start = 0; start < length; start += columns) rows.Add((line, start, Math.Min(columns, length - start)));
    }

    private void Settle()
    {
        top = follow ? MaxTop : Math.Min(top, MaxTop);
        Invalidate();
    }

    private void Measure()
    {
        charWidth = TextRenderer.MeasureText("MMMMMMMMMMMMMMMMMMMM", Font, Size.Empty, TextFormatFlags.NoPadding).Width / 20f;
        rowHeight = Font.Height + 1;
        var fit = Math.Max(20, (int)((Width - Pad * 2 - BarWidth - 8) / Math.Max(1f, charWidth)));
        if (fit == columns && rows.Count > 0) return;
        columns = fit;
        rows.Clear();
        for (var i = 0; i < lines.Count; i++) Wrap(i);
    }

    protected override void OnResize(EventArgs e) { base.OnResize(e); Measure(); Settle(); }
    protected override void OnHandleCreated(EventArgs e) { base.OnHandleCreated(e); Measure(); }

    private Rectangle Thumb()
    {
        if (rows.Count <= VisibleRows) return Rectangle.Empty;
        var track = Height - Pad * 2;
        var size = Math.Max(28, track * VisibleRows / rows.Count);
        var y = Pad + (int)((long)(track - size) * top / Math.Max(1, MaxTop));
        return new Rectangle(Width - BarWidth - 6, y, BarWidth, size);
    }

    private void ScrollTo(int row)
    {
        top = Math.Clamp(row, 0, MaxTop);
        follow = top >= MaxTop;
        Invalidate();
    }

    protected override void OnMouseWheel(MouseEventArgs e) { ScrollTo(top - Math.Sign(e.Delta) * 3); base.OnMouseWheel(e); }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        Focus();
        var thumb = Thumb();
        if (e.Button == MouseButtons.Left && !thumb.IsEmpty && e.X >= thumb.Left - 6)
        {
            if (!thumb.Contains(thumb.Left, e.Y)) ScrollTo(top + (e.Y < thumb.Top ? -VisibleRows : VisibleRows));
            else { dragging = true; dragOffset = e.Y - thumb.Top; Capture = true; }
        }
        base.OnMouseDown(e);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        var thumb = Thumb();
        if (dragging && !thumb.IsEmpty)
        {
            var range = Height - Pad * 2 - thumb.Height;
            ScrollTo(range <= 0 ? 0 : (int)((long)(e.Y - dragOffset - Pad) * MaxTop / range));
        }
        var over = !thumb.IsEmpty && e.X >= thumb.Left - 6;
        if (over != barHot) { barHot = over; Invalidate(); }
        base.OnMouseMove(e);
    }

    protected override void OnMouseUp(MouseEventArgs e) { dragging = false; Capture = false; base.OnMouseUp(e); }
    protected override void OnMouseLeave(EventArgs e) { if (barHot) { barHot = false; Invalidate(); } base.OnMouseLeave(e); }

    protected override bool IsInputKey(Keys keyData) => true;

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.Control && e.KeyCode == Keys.C) CopyAll();
        else if (e.KeyCode == Keys.PageUp) ScrollTo(top - VisibleRows);
        else if (e.KeyCode == Keys.PageDown) ScrollTo(top + VisibleRows);
        else if (e.KeyCode == Keys.Up) ScrollTo(top - 1);
        else if (e.KeyCode == Keys.Down) ScrollTo(top + 1);
        else if (e.KeyCode == Keys.Home) ScrollTo(0);
        else if (e.KeyCode == Keys.End) ScrollTo(MaxTop);
        base.OnKeyDown(e);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        InvokePaintBackground(this, e);
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using (var panel = Theme.Rounded(new RectangleF(0.5f, 0.5f, Width - 1, Height - 1), 10))
        using (var fill = new SolidBrush(Theme.ChildBg))
            g.FillPath(fill, panel);
        var y = Pad;
        for (var i = top; i < rows.Count && y + rowHeight <= Height - Pad + 2; i++, y += rowHeight)
        {
            var row = rows[i];
            if (row.Length == 0) continue;
            var text = lines[row.Line];
            var color = text.StartsWith("== ", StringComparison.Ordinal) ? Theme.Gold : Theme.Ivory;
            TextRenderer.DrawText(g, text.Substring(row.Start, row.Length), Font, new Point(Pad, y), color, Theme.ChildBg,
                TextFormatFlags.NoPadding | TextFormatFlags.NoPrefix);
        }
        var thumb = Thumb();
        if (thumb.IsEmpty) return;
        using var bar = Theme.Rounded(thumb, BarWidth / 2f);
        using var gold = new SolidBrush(barHot || dragging ? Theme.Gold : Theme.GoldDim);
        g.FillPath(gold, bar);
    }
}

// A thin gold bar that sweeps while work is running, so a quiet log never looks like a hang.
internal sealed class PulseBar : Control
{
    private float phase;
    private bool active;

    internal PulseBar()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
    }

    internal bool Active
    {
        get => active;
        set { active = value; phase = 0; Invalidate(); }
    }

    internal void Advance() { phase = (phase + 0.012f) % 1f; Invalidate(); }

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Theme.Panel);
        if (!active || Width <= 0) return;
        var span = Width * 0.22f;
        var x = phase * (Width + span) - span;
        using var brush = new LinearGradientBrush(new RectangleF(x - 1, 0, span + 2, Math.Max(1, Height)),
            Color.FromArgb(40, Theme.Gold), Theme.GoldBright, 0f);
        e.Graphics.FillRectangle(brush, x, 0, span, Height);
    }
}

internal sealed class BuilderForm : Form
{
    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

    private const int TitleHeight = 48, Grip = 6;
    private readonly List<Rectangle> fieldFrames = new();  // rounded backgrounds painted behind borderless text fields

    private readonly string root;
    private readonly string toolchainRoot;
    private readonly TextBox game = new();
    private readonly Button install = new PillButton();
    private readonly Label hint = new();
    private readonly Button build = new PillButton();
    private readonly Button removeTools = new PillButton();
    private readonly Button output = new PillButton();
    private readonly Label toolStatus = new();
    private readonly Label status = new();
    private readonly LogView log = new();
    private readonly PulseBar pulse = new();
    private readonly System.Windows.Forms.Timer ticker = new() { Interval = 33 };
    private readonly Stopwatch elapsed = new();
    private string activity = "";
    private string headsetNote = "";  // the build script's last "HEADSET:" line: what happened with the headset
    private bool liveProgress;  // the log's last line is a progress counter that the next one replaces
    private Process? process;

    internal BuilderForm(string payloadRoot)
    {
        root = payloadRoot;
        toolchainRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            Program.AppName, "toolchain");
        Text = Program.AppName;
        FormBorderStyle = FormBorderStyle.None;
        ClientSize = new Size(820, 535);
        MinimumSize = new Size(700, 470);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9);
        Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!);
        BackColor = Theme.WindowBg;
        ForeColor = Theme.Ivory;
        DoubleBuffered = true;
        ResizeRedraw = true;

        var close = new GlyphButton(true) { Location = new Point(820 - 16 - 34, 12), Anchor = AnchorStyles.Top | AnchorStyles.Right };
        close.Click += (_, _) => Close();
        Controls.Add(close);
        var minimize = new GlyphButton(false) { Location = new Point(820 - 16 - 34 - 38, 12), Anchor = AnchorStyles.Top | AnchorStyles.Right };
        minimize.Click += (_, _) => WindowState = FormWindowState.Minimized;
        Controls.Add(minimize);

        AddLabel(Program.AppName, 20, 12, 17, true);
        AddLabel("Choose the folder containing your legally extracted exefs and romfs folders.\n" +
                 "The game code (exefs) is built into the Quest app; the game data (romfs) is copied to the headset.", 22, 52);
        AddPathRow("Extracted game:", game, 105);

        toolStatus.AutoSize = true;
        toolStatus.Location = new Point(150, 143);
        toolStatus.BackColor = Color.Transparent;
        toolStatus.ForeColor = Theme.Sage;
        toolStatus.Font = Theme.Font(10);
        Controls.Add(toolStatus);
        UpdateToolStatus();

        hint.Text = "Plug your Quest in by USB first and the game is installed on it automatically.";
        hint.AutoSize = true;
        hint.Location = new Point(150, 172);
        hint.BackColor = Color.Transparent;
        hint.ForeColor = Theme.Sage;
        hint.Font = Theme.Font(10);
        Controls.Add(hint);

        build.Text = "Build and install";
        build.SetBounds(150, 211, 170, 36);
        build.Click += BuildClicked;
        Controls.Add(build);

        output.Text = "Open output folder";
        install.Text = "Install on headset";
        install.SetBounds(328, 211, 160, 36);
        install.Click += InstallClicked;
        Controls.Add(install);

        output.SetBounds(496, 211, 150, 36);
        output.Click += (_, _) => OpenOutput();
        Controls.Add(output);

        removeTools.Text = "Remove tools";
        removeTools.SetBounds(654, 211, 136, 36);
        removeTools.Click += RemoveToolsClicked;
        Controls.Add(removeTools);

        status.Text = "Ready";
        status.AutoSize = true;
        status.Location = new Point(22, 254);
        status.BackColor = Color.Transparent;
        status.ForeColor = Theme.Gold;
        status.Font = Theme.Font(10);
        Controls.Add(status);

        pulse.SetBounds(22, 280, 768, 4);
        pulse.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(pulse);
        ticker.Tick += (_, _) =>
        {
            pulse.Advance();
            status.Text = $"{activity}   {(int)elapsed.Elapsed.TotalMinutes:00}:{elapsed.Elapsed.Seconds:00}";
        };
        log.SetBounds(22, 292, 768, 218);
        log.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(log);
        FormClosing += (_, e) => { if (process is { HasExited: false }) e.Cancel = true; };
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        var round = 2;  // DWMWA_WINDOW_CORNER_PREFERENCE: rounded corners (Windows 11)
        DwmSetWindowAttribute(Handle, 33, ref round, sizeof(int));
    }

    protected override CreateParams CreateParams
    {
        get
        {
            var parameters = base.CreateParams;
            parameters.Style |= 0x20000;       // WS_MINIMIZEBOX: the taskbar button still minimises
            parameters.ClassStyle |= 0x20000;  // CS_DROPSHADOW
            return parameters;
        }
    }

    // Without a system frame the window says itself where it can be dragged (the heading) and resized (the edges).
    protected override void WndProc(ref Message m)
    {
        base.WndProc(ref m);
        if (m.Msg != 0x84 || (int)m.Result != 1) return;  // WM_NCHITTEST over the bare client area
        var lp = m.LParam.ToInt64();
        var p = PointToClient(new Point((short)(lp & 0xFFFF), (short)((lp >> 16) & 0xFFFF)));
        bool left = p.X < Grip, right = p.X >= ClientSize.Width - Grip, up = p.Y < Grip, bottom = p.Y >= ClientSize.Height - Grip;
        var hit = up && left ? 13 : up && right ? 14 : bottom && left ? 16 : bottom && right ? 17
                : left ? 10 : right ? 11 : up ? 12 : bottom ? 15 : p.Y < TitleHeight ? 2 : 1;
        m.Result = (IntPtr)hit;
    }

    // The option menu's flat dark panel with a thin gold edge; text fields sit on rounded darker-green frames.
    protected override void OnPaintBackground(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.WindowBg);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using (var field = new SolidBrush(Theme.FrameBg))
            foreach (var frame in fieldFrames)
            {
                using var path = Theme.Rounded(frame, 8);
                g.FillPath(field, path);
            }
        using var separator = new Pen(Color.FromArgb(77, 92, 77));
        g.DrawLine(separator, 22, TitleHeight, ClientSize.Width - 22, TitleHeight);
        using var edge = Theme.Rounded(new RectangleF(0.5f, 0.5f, ClientSize.Width - 1, ClientSize.Height - 1), 8);
        using var gold = new Pen(Color.FromArgb(120, Theme.GoldDim), 1f);
        g.DrawPath(gold, edge);
    }

    internal void SaveScreenshot(string path)
    {
        Show();
        if (Environment.GetEnvironmentVariable("SKYRECOMP_BUILDER_DEMO") is not null)  // sample log lines for the picture
        {
            Append("Checking the private build tools...");
            Append("== Step 2/6: Translating the game code to C (4 parts, several minutes)");
            for (var i = 0; i < 40; i++) Append($"Analysing part {i}... (the first progress line can take a minute)");
            Append("== Step 3/6: Compiling the game code for the headset (about 2000 files, the longest step)");
            Append("[1/2075] Building C object");
            Append("[812/2075] Building C object CMakeFiles/skygame.dir/generated/main/main_0811.c.o");
            SetActivity("Step 3/6: Compiling the game code for the headset (about 2000 files, the longest step)   04:12");
            pulse.Active = true;
            for (var i = 0; i < 30; i++) pulse.Advance();
        }
        Application.DoEvents();
        using var bitmap = new Bitmap(ClientSize.Width, ClientSize.Height);
        DrawToBitmap(bitmap, new Rectangle(Point.Empty, ClientSize));
        bitmap.Save(path, ImageFormat.Png);
    }

    internal bool RunSelfTest() => Text == Program.AppName && build.Enabled && Controls.Contains(game) &&
        Controls.OfType<TextBox>().Count() == 1 && Controls.Contains(log) &&
        !Controls.OfType<Label>().Any(label => label.Text.Contains("Android SDK:") || label.Text.Contains("Java/JDK:")) &&
        File.Exists(Path.Combine(root, "tools", "quest", "build-personal-apk.ps1")) &&
        File.Exists(Path.Combine(root, "tools", "quest", "install-toolchain.ps1")) &&
        File.Exists(Path.Combine(root, "tools", "quest", "remove-toolchain.ps1"));

    private void AddLabel(string text, int left, int top, float size = 9, bool semibold = false)
    {
        var label = new PassLabel
        {
            Text = text, AutoSize = true, Location = new Point(left, top), BackColor = Color.Transparent,
            ForeColor = semibold ? Theme.Gold : Theme.Ivory, Font = Theme.Font(size == 9 ? 10 : size)
        };
        Controls.Add(label);
    }

    private void AddPathRow(string caption, TextBox box, int top)
    {
        AddLabel(caption, 22, top + 1);
        fieldFrames.Add(new Rectangle(150, top - 2, 556, 30));
        box.SetBounds(162, top + 5, 532, 18);
        box.BackColor = Theme.FrameBg;
        box.ForeColor = Theme.Ivory;
        box.BorderStyle = BorderStyle.None;
        box.Font = new Font("Segoe UI", 9.75f);
        box.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(box);
        var browse = new PillButton { Text = "Browse..." };
        browse.SetBounds(712, top - 2, 78, 30);
        browse.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        browse.Click += (_, _) =>
        {
            using var dialog = new FolderBrowserDialog { SelectedPath = Directory.Exists(box.Text) ? box.Text : "" };
            if (dialog.ShowDialog(this) == DialogResult.OK) box.Text = dialog.SelectedPath;
        };
        Controls.Add(browse);
    }

    private async void BuildClicked(object? sender, EventArgs e)
    {
        if (!Directory.Exists(game.Text))
        {
            MessageBox.Show(this, "Select the extracted game folder first.", Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        var result = "";
        try
        {
            SetBusy(true);
            log.Clear();
            SetActivity("Checking the build tools");
            Append("Checking the private build tools...");
            if (!TryReadToolchain(out var androidSdk, out var javaHome))
            {
                var answer = MessageBox.Show(this,
                    "The private Android build tools are not installed. Download and install them now?\n\n" +
                    "Continuing downloads Microsoft OpenJDK and Google's Android SDK, and accepts the Android SDK licenses.",
                    "Prepare build tools", MessageBoxButtons.YesNo, MessageBoxIcon.Information);
                if (answer != DialogResult.Yes) { result = "Ready"; return; }
                SetActivity("Installing the build tools (a large download)");
                var installer = Path.Combine(root, "tools", "quest", "install-toolchain.ps1");
                var installExit = await RunProcess(PowerShellInfo(installer, new[] { "-Destination", toolchainRoot }));
                if (installExit != 0 || !TryReadToolchain(out androidSdk, out javaHome))
                    throw new InvalidOperationException($"Tool installation failed (exit {installExit}).");
                UpdateToolStatus();
            }

            SetActivity("Starting the build");
            var backend = Path.Combine(root, "tools", "quest", "build-personal-apk.ps1");
            var args = new List<string> { "-GameRoot", game.Text, "-AndroidSdk", androidSdk, "-JavaHome", javaHome };
            args.Add("-Install");
            headsetNote = "";
            var exit = await RunProcess(PowerShellInfo(backend, args));
            result = exit != 0 ? $"Build failed (exit {exit}) - see the log"
                : headsetNote.Length > 0 ? "APK built. " + headsetNote : "Build completed";
        }
        catch (Exception ex) { Append(ex.Message); result = "Build failed"; }
        finally { SetBusy(false); if (result.Length > 0) status.Text = result; }
    }

    // Install the APK that is already built (and the game files, once) on the connected headset.
    private async void InstallClicked(object? sender, EventArgs e)
    {
        if (!Directory.Exists(game.Text))
        {
            MessageBox.Show(this, "Select the extracted game folder first.", Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        var result = "";
        try
        {
            SetBusy(true);
            log.Clear();
            if (!TryReadToolchain(out var androidSdk, out var javaHome))
            {
                result = "Build the APK first";
                Append("The build tools are not installed yet: click Build and install first.");
                return;
            }
            SetActivity("Looking for the headset");
            headsetNote = "";
            var backend = Path.Combine(root, "tools", "quest", "build-personal-apk.ps1");
            var exit = await RunProcess(PowerShellInfo(backend,
                new[] { "-GameRoot", game.Text, "-AndroidSdk", androidSdk, "-JavaHome", javaHome, "-InstallOnly" }));
            result = exit != 0 ? $"Installation failed (exit {exit}) - see the log" : headsetNote;
        }
        catch (Exception ex) { Append(ex.Message); result = "Installation failed"; }
        finally { SetBusy(false); if (result.Length > 0) status.Text = result; }
    }

    private async void RemoveToolsClicked(object? sender, EventArgs e)
    {
        var answer = MessageBox.Show(this,
            $"Remove the builder's private Java and Android tools?\n\n{toolchainRoot}\n\n" +
            "Game files and generated APKs will not be removed.",
            "Remove installed tools", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
        if (answer != DialogResult.Yes) return;
        try
        {
            SetBusy(true);
            log.Clear();
            SetActivity("Removing the build tools");
            var script = Path.Combine(root, "tools", "quest", "remove-toolchain.ps1");
            var exit = await RunProcess(PowerShellInfo(script, new[] { "-Destination", toolchainRoot }));
            status.Text = exit == 0 ? "Private tools removed" : $"Removal failed (exit {exit})";
            UpdateToolStatus();
        }
        catch (Exception ex) { Append(ex.Message); status.Text = "Removal failed"; }
        finally { SetBusy(false); }
    }

    private bool TryReadToolchain(out string androidSdk, out string javaHome)
    {
        androidSdk = javaHome = "";
        var config = Path.Combine(toolchainRoot, "toolchain.json");
        if (!File.Exists(config)) return false;
        using var document = JsonDocument.Parse(File.ReadAllText(config));
        androidSdk = document.RootElement.GetProperty("androidSdk").GetString() ?? "";
        javaHome = document.RootElement.GetProperty("javaHome").GetString() ?? "";
        return File.Exists(Path.Combine(javaHome, "bin", "java.exe")) &&
               File.Exists(Path.Combine(androidSdk, "ndk", "27.1.12297006", "build", "cmake", "android.toolchain.cmake"));
    }

    private void UpdateToolStatus()
    {
        toolStatus.Text = TryReadToolchain(out _, out _) ? "Build tools: installed privately" : "Build tools: installed automatically on first build";
        removeTools.Enabled = Directory.Exists(toolchainRoot);
    }

    private ProcessStartInfo PowerShellInfo(string script, IEnumerable<string> arguments)
    {
        var info = new ProcessStartInfo("powershell.exe")
        {
            WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true
        };
        foreach (var arg in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script }) info.ArgumentList.Add(arg);
        foreach (var arg in arguments) info.ArgumentList.Add(arg);
        return info;
    }

    private async Task<int> RunProcess(ProcessStartInfo info)
    {
        process = new Process { StartInfo = info, EnableRaisingEvents = true };
        process.OutputDataReceived += (_, a) => Append(a.Data);
        process.ErrorDataReceived += (_, a) => Append(a.Data);
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await process.WaitForExitAsync();
        return process.ExitCode;
    }

    private void SetActivity(string text)
    {
        activity = text;
        status.Text = text;
    }

    private void SetBusy(bool busy)
    {
        pulse.Active = busy;
        liveProgress = false;
        if (busy) { elapsed.Restart(); ticker.Start(); }
        else { ticker.Stop(); elapsed.Stop(); }
        build.Enabled = install.Enabled = output.Enabled = !busy;
        removeTools.Enabled = !busy && Directory.Exists(toolchainRoot);
    }

    // Counters that repeat thousands of times (code generation, compiler steps, download bars) update one live
    // line instead of flooding the log.
    private static readonly System.Text.RegularExpressions.Regex ProgressLine = new(
        @"^\s*(\[\d+/\d+\]|\[[=\s]*\]\s*\d+%|#+\s|\S+: progress \d+/\d+|\d+(\.\d+)?%)",
        System.Text.RegularExpressions.RegexOptions.Compiled);

    private void Append(string? line)
    {
        if (string.IsNullOrWhiteSpace(line)) return;
        if (InvokeRequired) { BeginInvoke(() => Append(line)); return; }
        line = line.TrimEnd();
        if (line.StartsWith("HEADSET: ", StringComparison.Ordinal))
        {
            line = line[9..];
            headsetNote = line.Split('.')[0] + ".";
            headsetNote = char.ToUpperInvariant(headsetNote[0]) + headsetNote[1..];
        }
        if (line.StartsWith("== ", StringComparison.Ordinal))  // a step heading from the build scripts (drawn in gold)
        {
            SetActivity(line[3..].Trim());
            log.AddLine("");
            liveProgress = false;
        }
        var progress = ProgressLine.IsMatch(line);
        if (line.Length > 400) line = line[..400] + "...";
        if (progress && liveProgress) log.ReplaceLastLine(line);
        else log.AddLine(line);
        liveProgress = progress;
    }

    private void OpenOutput()
    {
        var path = Path.Combine(root, "android", "app", "build", "outputs", "apk", "personal", "release");
        if (Directory.Exists(path)) Process.Start(new ProcessStartInfo("explorer.exe", path) { UseShellExecute = true });
    }
}
