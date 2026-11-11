using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Text.Json;
using System.Text.RegularExpressions;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Platform.Storage;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;
using Avalonia.Threading;

namespace SkyRecompQuestBuilder;

internal static class Program
{
    internal const string AppName = "Skyrecomp-VR APK Builder";
    internal const string DataFolder = "skyrecomp-vr-apk-builder";
    internal static string[] Args = Array.Empty<string>();
    internal static string Root = "";

    [STAThread]
    private static int Main(string[] args)
    {
        Args = args;
        try
        {
            Root = EnsurePayload();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 2;
        }
        // The self-test needs no display: it checks that the embedded sources unpack and hold the build scripts.
        if (args.Contains("--self-test", StringComparer.OrdinalIgnoreCase)) return SelfTest() ? 0 : 1;
        return AppBuilder.Configure<App>().UsePlatformDetect().StartWithClassicDesktopLifetime(args);
    }

    internal static string DataRoot()
    {
        var xdg = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return Path.Combine(!string.IsNullOrEmpty(xdg) && xdg.StartsWith('/') ? xdg : Path.Combine(home, ".local", "share"), DataFolder);
    }

    internal static string Script(string name) => Path.Combine(Root, "tools", "quest", "linux", name);

    private static bool SelfTest() =>
        File.Exists(Script("build-personal-apk.sh")) && File.Exists(Script("install-toolchain.sh")) &&
        File.Exists(Script("remove-toolchain.sh")) && File.Exists(Path.Combine(Root, "android", "gradlew")) &&
        File.Exists(Path.Combine(Root, "android", "engine", "CMakeLists.txt")) &&
        File.Exists(Path.Combine(Root, "tools", "gen.py"));

    private static string EnsurePayload()
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
        {
            var directory = new DirectoryInfo(start);
            while (directory is not null)
            {
                if (File.Exists(Path.Combine(directory.FullName, "tools", "quest", "linux", "build-personal-apk.sh")))
                    return directory.FullName;
                directory = directory.Parent;
            }
        }

        var assembly = Assembly.GetExecutingAssembly();
        using var payload = assembly.GetManifestResourceStream("SkyRecomp.Payload.zip")
            ?? throw new InvalidOperationException("The embedded builder payload is missing.");
        var version = assembly.GetName().Version?.ToString(3) ?? "0.2.0";
        var root = Path.Combine(DataRoot(), "payload", version);
        // The stamp identifies this exact executable, so a rebuilt builder with the same version number still
        // refreshes its extracted sources.
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

// Colours of the in-game option menu (runtime/ui/menu.mm style_init), as in the Windows builder.
internal static class Look
{
    internal static readonly Color Gold = Color.FromRgb(238, 198, 92);
    internal static readonly Color GoldDim = Color.FromRgb(158, 125, 51);
    internal static readonly Color Ivory = Color.FromRgb(246, 241, 227);
    internal static readonly Color Sage = Color.FromRgb(182, 188, 176);
    internal static readonly Color WindowBg = Color.FromRgb(15, 23, 20);
    internal static readonly Color ChildBg = Color.FromRgb(23, 33, 28);
    internal static readonly Color FrameBg = Color.FromRgb(33, 48, 41);
    internal static readonly Color Button = Color.FromRgb(36, 56, 46);
    internal static readonly Color ButtonHot = Color.FromRgb(38, 92, 64);
    internal static readonly Color ButtonDown = Color.FromRgb(54, 128, 87);
    internal static readonly Color Separator = Color.FromRgb(77, 92, 77);
    internal static readonly FontFamily Font =
        new("avares://skyrecomp-vr-apk-builder/Assets#Baloo 2 SemiBold, avares://skyrecomp-vr-apk-builder/Assets#Baloo 2");
    internal static readonly FontFamily Mono = new("DejaVu Sans Mono, Liberation Mono, Noto Sans Mono, monospace");

    internal static IBrush Brush(Color color) => new SolidColorBrush(color);
}

internal sealed class App : Application
{
    public override void Initialize()
    {
        RequestedThemeVariant = ThemeVariant.Dark;
        Styles.Add(new FluentTheme());
        // The Fluent controls take their state colours from these resources.
        var colors = new Dictionary<string, Color>
        {
            ["ButtonBackground"] = Look.Button, ["ButtonBackgroundPointerOver"] = Look.ButtonHot,
            ["ButtonBackgroundPressed"] = Look.ButtonDown, ["ButtonBackgroundDisabled"] = Color.FromRgb(26, 38, 32),
            ["ButtonForeground"] = Look.Ivory, ["ButtonForegroundPointerOver"] = Look.Ivory,
            ["ButtonForegroundPressed"] = Look.Ivory, ["ButtonForegroundDisabled"] = Color.FromRgb(158, 163, 148),
            ["ButtonBorderBrush"] = Colors.Transparent, ["ButtonBorderBrushPointerOver"] = Colors.Transparent,
            ["ButtonBorderBrushPressed"] = Colors.Transparent, ["ButtonBorderBrushDisabled"] = Colors.Transparent,
            ["TextControlBackground"] = Look.FrameBg, ["TextControlBackgroundPointerOver"] = Look.FrameBg,
            ["TextControlBackgroundFocused"] = Look.FrameBg, ["TextControlBackgroundDisabled"] = Look.FrameBg,
            ["TextControlForeground"] = Look.Ivory, ["TextControlForegroundPointerOver"] = Look.Ivory,
            ["TextControlForegroundFocused"] = Look.Ivory,
            ["TextControlBorderBrush"] = Colors.Transparent, ["TextControlBorderBrushPointerOver"] = Colors.Transparent,
            ["TextControlBorderBrushFocused"] = Look.GoldDim,
            ["TextControlSelectionHighlightColor"] = Look.ButtonDown,
            ["ScrollBarThumbFill"] = Look.GoldDim, ["ScrollBarThumbFillPointerOver"] = Look.Gold,
            ["ScrollBarThumbFillPressed"] = Look.Gold, ["ScrollBarPanningThumbBackground"] = Look.GoldDim,
            ["ScrollBarThumbBackgroundColor"] = Look.GoldDim,
            ["ScrollBarTrackFill"] = Colors.Transparent, ["ScrollBarTrackFillPointerOver"] = Colors.Transparent,
            ["ScrollBarBackground"] = Colors.Transparent, ["ScrollBarBackgroundPointerOver"] = Colors.Transparent,
            ["ScrollBarButtonArrowForeground"] = Colors.Transparent,
            ["ScrollBarButtonArrowForegroundPointerOver"] = Colors.Transparent,
            ["ScrollBarButtonBackgroundPointerOver"] = Colors.Transparent,
        };
        foreach (var (key, color) in colors) Resources[key] = Look.Brush(color);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new BuilderWindow(Program.Root);
        base.OnFrameworkInitializationCompleted();
    }
}

internal sealed class BuilderWindow : Window
{
    private static readonly Regex ProgressLine = new(
        @"^\s*(\[\d+/\d+\]|\[[=\s]*\]\s*\d+%|#+\s|\S+: progress \d+/\d+|\d+(\.\d+)?%)", RegexOptions.Compiled);

    private readonly string root;
    private readonly string toolchainRoot;
    private readonly TextBox game = new();
    private readonly Button build = MakeButton("Build and install");
    private readonly Button install = MakeButton("Install on headset");
    private readonly Button output = MakeButton("Open output folder");
    private readonly Button removeTools = MakeButton("Remove tools");
    private readonly TextBlock toolStatus = MakeText("", Look.Sage);
    private readonly TextBlock status = MakeText("Ready", Look.Gold);
    private readonly ProgressBar pulse = new();
    private readonly TextBox log = new();
    private readonly List<string> lines = new();
    private readonly DispatcherTimer ticker = new() { Interval = TimeSpan.FromMilliseconds(200) };
    private readonly Stopwatch elapsed = new();
    private string activity = "";
    private string headsetNote = "";  // the build script's last "HEADSET:" line: what happened with the headset
    private bool liveProgress, logDirty, busy;
    private Process? process;

    internal BuilderWindow(string payloadRoot)
    {
        root = payloadRoot;
        toolchainRoot = Path.Combine(Program.DataRoot(), "toolchain");
        Title = Program.AppName;
        Width = 820;
        Height = 535;
        MinWidth = 700;
        MinHeight = 470;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        SystemDecorations = SystemDecorations.BorderOnly;  // the heading below is the title bar
        Background = Look.Brush(Look.WindowBg);
        FontFamily = Look.Font;
        FontSize = 14;
        Foreground = Look.Brush(Look.Ivory);
        try { Icon = new WindowIcon(AssetLoader.Open(new Uri("avares://skyrecomp-vr-apk-builder/Assets/icon.png"))); }
        catch (Exception) { /* no icon */ }

        // ---- heading: title, minimise, close; drag the window by it
        var title = MakeText(Program.AppName, Look.Gold);
        title.FontSize = 23;
        var windowButtons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, HorizontalAlignment = HorizontalAlignment.Right };
        windowButtons.Children.Add(MakeGlyph("–", Look.ButtonHot, () => WindowState = WindowState.Minimized));
        windowButtons.Children.Add(MakeGlyph("✕", Color.FromRgb(150, 48, 40), Close));
        var heading = new Grid { Background = Brushes.Transparent, Height = 36 };
        heading.Children.Add(title);
        heading.Children.Add(windowButtons);
        heading.PointerPressed += (_, e) =>
        {
            if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) BeginMoveDrag(e);
        };

        var intro = MakeText("Choose the folder containing your legally extracted exefs and romfs folders.\n" +
                             "The game code (exefs) is built into the Quest app; the game data (romfs) is copied to the headset.", Look.Ivory);
        intro.Margin = new Thickness(0, 10, 0, 12);

        // ---- the game folder
        game.CornerRadius = new CornerRadius(8);
        game.FontFamily = FontFamily.Default;
        game.VerticalContentAlignment = VerticalAlignment.Center;
        game.Watermark = "/path/to/extracted";
        var browse = MakeButton("Browse...");
        browse.Margin = new Thickness(8, 0, 0, 0);
        browse.Click += async (_, _) =>
        {
            var picked = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Extracted game folder" });
            var path = picked.Count > 0 ? picked[0].TryGetLocalPath() : null;
            if (path is not null) game.Text = path;
        };
        var gameLabel = MakeText("Extracted game:", Look.Ivory);
        gameLabel.Width = 128;
        gameLabel.VerticalAlignment = VerticalAlignment.Center;
        var gameRow = new DockPanel();
        DockPanel.SetDock(gameLabel, Dock.Left);
        DockPanel.SetDock(browse, Dock.Right);
        gameRow.Children.Add(gameLabel);
        gameRow.Children.Add(browse);
        gameRow.Children.Add(game);

        toolStatus.Margin = new Thickness(128, 10, 0, 0);
        var hint = MakeText("Plug your Quest in by USB first and the game is installed on it automatically.", Look.Sage);
        hint.Margin = new Thickness(128, 6, 0, 12);

        // ---- actions
        build.Click += async (_, _) => await BuildClicked();
        install.Click += async (_, _) => await InstallClicked();
        output.Click += (_, _) => OpenOutput();
        removeTools.Click += async (_, _) => await RemoveToolsClicked();
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(128, 0, 0, 0) };
        actions.Children.AddRange(new Control[] { build, install, output, removeTools });

        status.Margin = new Thickness(0, 10, 0, 4);
        pulse.IsIndeterminate = false;
        pulse.Height = 4;
        pulse.MinHeight = 4;
        pulse.Foreground = Look.Brush(Look.Gold);
        pulse.Background = Look.Brush(Look.ChildBg);
        pulse.Margin = new Thickness(0, 0, 0, 8);

        // ---- log
        log.IsReadOnly = true;
        log.AcceptsReturn = true;
        log.TextWrapping = TextWrapping.Wrap;
        log.FontFamily = Look.Mono;
        log.FontSize = 12;
        log.CornerRadius = new CornerRadius(10);
        log.Padding = new Thickness(10);
        foreach (var key in new[] { "TextControlBackground", "TextControlBackgroundPointerOver", "TextControlBackgroundFocused" })
            log.Resources[key] = Look.Brush(Look.ChildBg);
        log.Resources["TextControlBorderBrushFocused"] = Brushes.Transparent;

        var top = new StackPanel();
        top.Children.AddRange(new Control[]
        {
            heading,
            new Border { Height = 1, Background = Look.Brush(Look.Separator), Margin = new Thickness(0, 4, 0, 0) },
            intro, gameRow, toolStatus, hint, actions, status, pulse
        });
        var layout = new DockPanel { Margin = new Thickness(22, 10, 22, 22) };
        DockPanel.SetDock(top, Dock.Top);
        layout.Children.Add(top);
        layout.Children.Add(log);
        Content = new Border
        {
            BorderBrush = Look.Brush(Color.FromArgb(120, Look.GoldDim.R, Look.GoldDim.G, Look.GoldDim.B)),
            BorderThickness = new Thickness(1),
            Child = layout
        };

        ticker.Tick += (_, _) =>
        {
            if (busy) status.Text = $"{activity}   {(int)elapsed.Elapsed.TotalMinutes:00}:{elapsed.Elapsed.Seconds:00}";
            if (!logDirty) return;
            logDirty = false;
            log.Text = string.Join('\n', lines);
            log.CaretIndex = log.Text.Length;  // keeps the newest line in view
        };
        ticker.Start();
        UpdateToolStatus();
        Closing += (_, e) => { if (process is { HasExited: false }) e.Cancel = true; };

        var shot = Array.FindIndex(Program.Args, a => a.Equals("--screenshot", StringComparison.OrdinalIgnoreCase));
        if (shot >= 0 && shot + 1 < Program.Args.Length)
            Opened += (_, _) => DispatcherTimer.RunOnce(() => SaveScreenshot(Program.Args[shot + 1]), TimeSpan.FromMilliseconds(600));
    }

    private void SaveScreenshot(string path)
    {
        if (Environment.GetEnvironmentVariable("SKYRECOMP_BUILDER_DEMO") is not null)  // sample log lines for the picture
        {
            Append("Checking the private build tools...");
            Append("== Step 2/6: Translating the game code to C (4 parts, several minutes)");
            for (var i = 0; i < 40; i++) Append($"Analysing part {i}... (the first progress line can take a minute)");
            Append("== Step 3/6: Compiling the game code for the headset (about 2000 files, the longest step)");
            Append("[812/2075] Building C object CMakeFiles/skygame.dir/generated/main/main_0811.c.o");
            pulse.IsIndeterminate = true;
            log.Text = string.Join('\n', lines);
            log.CaretIndex = log.Text.Length;
        }
        DispatcherTimer.RunOnce(() =>
        {
            using var bitmap = new RenderTargetBitmap(new PixelSize((int)Bounds.Width, (int)Bounds.Height));
            bitmap.Render(this);
            bitmap.Save(path);
            Close();
        }, TimeSpan.FromMilliseconds(400));
    }

    private static TextBlock MakeText(string text, Color color) =>
        new() { Text = text, Foreground = Look.Brush(color), VerticalAlignment = VerticalAlignment.Center };

    private static Button MakeButton(string text) => new()
    {
        Content = text, CornerRadius = new CornerRadius(8), Padding = new Thickness(18, 7, 18, 7),
        FontSize = 14.5, Cursor = new Cursor(StandardCursorType.Hand), VerticalAlignment = VerticalAlignment.Stretch
    };

    private static Control MakeGlyph(string glyph, Color hover, Action click)
    {
        var text = new TextBlock
        {
            Text = glyph, FontFamily = FontFamily.Default, FontSize = 14, Foreground = Look.Brush(Look.Sage),
            HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center
        };
        var box = new Border { Width = 34, Height = 26, CornerRadius = new CornerRadius(8), Background = Brushes.Transparent, Child = text };
        box.PointerEntered += (_, _) => { box.Background = Look.Brush(hover); text.Foreground = Look.Brush(Look.Ivory); };
        box.PointerExited += (_, _) => { box.Background = Brushes.Transparent; text.Foreground = Look.Brush(Look.Sage); };
        box.PointerPressed += (_, e) => e.Handled = true;  // (not a window drag)
        box.PointerReleased += (_, _) => click();
        return box;
    }

    // ---- actions
    private async Task BuildClicked()
    {
        if (!Directory.Exists(game.Text))
        {
            await Ask("Select the extracted game folder first.", false);
            return;
        }
        var result = "";
        try
        {
            SetBusy(true);
            ClearLog();
            SetActivity("Checking the build tools");
            Append("Checking the private build tools...");
            if (!TryReadToolchain(out var androidSdk, out var javaHome))
            {
                var yes = await Ask("The private Android build tools are not installed. Download and install them now?\n\n" +
                                    "Continuing downloads Microsoft OpenJDK and Google's Android SDK, and accepts the Android SDK licenses.", true);
                if (!yes) { result = "Ready"; return; }
                SetActivity("Installing the build tools (a large download)");
                var installExit = await Run(Program.Script("install-toolchain.sh"), toolchainRoot);
                if (installExit != 0 || !TryReadToolchain(out androidSdk, out javaHome))
                    throw new InvalidOperationException($"Tool installation failed (exit {installExit}).");
                UpdateToolStatus();
            }

            SetActivity("Starting the build");
            headsetNote = "";
            var exit = await Run(Program.Script("build-personal-apk.sh"),
                "--game", game.Text!, "--sdk", androidSdk, "--java", javaHome, "--install");
            result = exit != 0 ? $"Build failed (exit {exit}) - see the log"
                : headsetNote.Length > 0 ? "APK built. " + headsetNote : "Build completed";
        }
        catch (Exception ex) { Append(ex.Message); result = "Build failed"; }
        finally { SetBusy(false); if (result.Length > 0) status.Text = result; }
    }

    // Install the APK that is already built (and the game files, once) on the connected headset.
    private async Task InstallClicked()
    {
        if (!Directory.Exists(game.Text))
        {
            await Ask("Select the extracted game folder first.", false);
            return;
        }
        var result = "";
        try
        {
            SetBusy(true);
            ClearLog();
            if (!TryReadToolchain(out var androidSdk, out var javaHome))
            {
                result = "Build the APK first";
                Append("The build tools are not installed yet: click Build and install first.");
                return;
            }
            SetActivity("Looking for the headset");
            headsetNote = "";
            var exit = await Run(Program.Script("build-personal-apk.sh"),
                "--game", game.Text!, "--sdk", androidSdk, "--java", javaHome, "--install-only");
            result = exit != 0 ? $"Installation failed (exit {exit}) - see the log" : headsetNote;
        }
        catch (Exception ex) { Append(ex.Message); result = "Installation failed"; }
        finally { SetBusy(false); if (result.Length > 0) status.Text = result; }
    }

    private async Task RemoveToolsClicked()
    {
        var yes = await Ask($"Remove the builder's private Java and Android tools?\n\n{toolchainRoot}\n\n" +
                            "Game files and generated APKs will not be removed.", true);
        if (!yes) return;
        var result = "";
        try
        {
            SetBusy(true);
            ClearLog();
            SetActivity("Removing the build tools");
            var exit = await Run(Program.Script("remove-toolchain.sh"), toolchainRoot);
            result = exit == 0 ? "Private tools removed" : $"Removal failed (exit {exit})";
        }
        catch (Exception ex) { Append(ex.Message); result = "Removal failed"; }
        finally { SetBusy(false); status.Text = result; }
    }

    private void OpenOutput()
    {
        var path = Path.Combine(root, "android", "app", "build", "outputs", "apk", "personal", "release");
        if (!Directory.Exists(path)) { status.Text = "No APK has been built yet"; return; }
        try { Process.Start(new ProcessStartInfo("xdg-open") { ArgumentList = { path }, UseShellExecute = false }); }
        catch (Exception) { status.Text = path; }
    }

    // ---- helpers
    private bool TryReadToolchain(out string androidSdk, out string javaHome)
    {
        androidSdk = javaHome = "";
        var config = Path.Combine(toolchainRoot, "toolchain.json");
        if (!File.Exists(config)) return false;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(config));
            androidSdk = document.RootElement.GetProperty("androidSdk").GetString() ?? "";
            javaHome = document.RootElement.GetProperty("javaHome").GetString() ?? "";
        }
        catch (Exception) { return false; }
        return File.Exists(Path.Combine(javaHome, "bin", "java")) &&
               File.Exists(Path.Combine(androidSdk, "ndk", "27.1.12297006", "build", "cmake", "android.toolchain.cmake"));
    }

    private void UpdateToolStatus()
    {
        toolStatus.Text = TryReadToolchain(out _, out _) ? "Build tools: installed privately" : "Build tools: installed automatically on first build";
        removeTools.IsEnabled = !busy && Directory.Exists(toolchainRoot);
    }

    private void SetActivity(string text)
    {
        activity = text;
        status.Text = text;
    }

    private void SetBusy(bool value)
    {
        busy = value;
        pulse.IsIndeterminate = value;
        liveProgress = false;
        if (value) elapsed.Restart();
        else elapsed.Stop();
        build.IsEnabled = install.IsEnabled = output.IsEnabled = !value;
        UpdateToolStatus();
    }

    private void ClearLog()
    {
        lines.Clear();
        logDirty = true;
    }

    private async Task<int> Run(string script, params string[] arguments)
    {
        var info = new ProcessStartInfo("bash")
        {
            WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true
        };
        info.ArgumentList.Add(script);
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        // (inside an AppImage: the tools must not inherit its bundled library path)
        info.Environment.Remove("LD_LIBRARY_PATH");
        process = new Process { StartInfo = info, EnableRaisingEvents = true };
        process.OutputDataReceived += (_, a) => Dispatcher.UIThread.Post(() => Append(a.Data));
        process.ErrorDataReceived += (_, a) => Dispatcher.UIThread.Post(() => Append(a.Data));
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await process.WaitForExitAsync();
        return process.ExitCode;
    }

    // Counters that repeat thousands of times (code generation, compiler steps, download bars) update one live
    // line instead of flooding the log. Download bars redraw with carriage returns: the last state is the line.
    private void Append(string? line)
    {
        if (string.IsNullOrWhiteSpace(line)) return;
        line = line.TrimEnd();
        var redraw = line.LastIndexOf('\r');
        if (redraw >= 0) line = line[(redraw + 1)..];
        if (line.Length == 0) return;
        if (line.StartsWith("HEADSET: ", StringComparison.Ordinal))
        {
            line = line[9..];
            headsetNote = line.Split('.')[0] + ".";
            headsetNote = char.ToUpperInvariant(headsetNote[0]) + headsetNote[1..];
        }
        if (line.StartsWith("== ", StringComparison.Ordinal))  // a step heading from the build scripts
        {
            SetActivity(line[3..].Trim());
            lines.Add("");
            liveProgress = false;
        }
        var progress = ProgressLine.IsMatch(line);
        if (line.Length > 400) line = line[..400] + "...";
        if (progress && liveProgress && lines.Count > 0) lines[^1] = line;
        else lines.Add(line);
        liveProgress = progress;
        logDirty = true;
    }

    // A small question / notice in the builder's own style. Returns true for Yes.
    private async Task<bool> Ask(string message, bool question)
    {
        var dialog = new Window
        {
            Title = Program.AppName, Width = 480, SizeToContent = SizeToContent.Height, CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner, SystemDecorations = SystemDecorations.BorderOnly,
            Background = Look.Brush(Look.WindowBg), FontFamily = Look.Font, FontSize = 14
        };
        var text = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap, Foreground = Look.Brush(Look.Ivory) };
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(0, 18, 0, 0)
        };
        var answer = false;
        var yes = MakeButton(question ? "Yes" : "OK");
        yes.Click += (_, _) => { answer = true; dialog.Close(); };
        buttons.Children.Add(yes);
        if (question)
        {
            var no = MakeButton("No");
            no.Click += (_, _) => dialog.Close();
            buttons.Children.Add(no);
        }
        var panel = new StackPanel { Margin = new Thickness(24, 20, 24, 20) };
        panel.Children.Add(text);
        panel.Children.Add(buttons);
        dialog.Content = new Border
        {
            BorderBrush = Look.Brush(Look.GoldDim), BorderThickness = new Thickness(1), Child = panel
        };
        await dialog.ShowDialog(this);
        return answer;
    }
}
