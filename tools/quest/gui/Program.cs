using System.Diagnostics;
using System.Drawing;
using System.Text.Json;
using System.Windows.Forms;

namespace SkyRecompQuestPackager;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        var root = FindBundleRoot();
        if (root is null)
        {
            MessageBox.Show(
                "This executable must remain inside the extracted SkyRecomp Quest Packager folder.",
                "SkyRecomp Quest Packager", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 2;
        }

        using var form = new PackagerForm(root);
        if (args.Contains("--self-test", StringComparer.OrdinalIgnoreCase))
        {
            Console.WriteLine("Quest Packager executable self-test passed.");
            return form.RunSelfTest() ? 0 : 1;
        }
        Application.Run(form);
        return 0;
    }

    private static string? FindBundleRoot()
    {
        var starts = new[] { AppContext.BaseDirectory, Environment.CurrentDirectory };
        foreach (var start in starts)
        {
            var directory = new DirectoryInfo(start);
            while (directory is not null)
            {
                if (File.Exists(Path.Combine(directory.FullName, "tools", "quest", "build-personal-apk.ps1")))
                    return directory.FullName;
                directory = directory.Parent;
            }
        }
        return null;
    }
}

internal sealed class PackagerForm : Form
{
    private readonly string root;
    private readonly TextBox game = new();
    private readonly TextBox sdk = new();
    private readonly TextBox java = new();
    private readonly CheckBox install = new();
    private readonly Button installTools = new();
    private readonly Button removeTools = new();
    private readonly Button build = new();
    private readonly Button output = new();
    private readonly Label status = new();
    private readonly TextBox log = new();
    private Process? process;

    internal PackagerForm(string bundleRoot)
    {
        root = bundleRoot;
        Text = "SkyRecomp Quest Packager";
        ClientSize = new Size(820, 590);
        MinimumSize = new Size(700, 500);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9);

        AddLabel("Build your personal SkyRecomp Quest APK", 20, 16, 15, true);
        AddLabel("Select the folder containing your legally extracted exefs and romfs folders.\nGame files stay on this PC and are never uploaded.", 22, 52);
        AddPathRow("Extracted game:", game, 105);
        AddPathRow("Android SDK:", sdk, 142);
        AddPathRow("Java/JDK:", java, 179);

        sdk.Text = Environment.GetEnvironmentVariable("ANDROID_HOME")
            ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk");
        java.Text = Environment.GetEnvironmentVariable("JAVA_HOME") ?? "";
        LoadPrivateToolchain();

        installTools.Text = "Install required tools";
        installTools.SetBounds(150, 216, 160, 30);
        installTools.Click += InstallToolsClicked;
        Controls.Add(installTools);

        removeTools.Text = "Remove installed tools";
        removeTools.SetBounds(315, 216, 165, 30);
        removeTools.Click += RemoveToolsClicked;
        Controls.Add(removeTools);

        install.Text = "Install to connected Quest after build";
        install.AutoSize = true;
        install.Location = new Point(490, 222);
        Controls.Add(install);

        build.Text = "Build personal APK";
        build.SetBounds(150, 258, 170, 36);
        build.Click += BuildClicked;
        Controls.Add(build);

        output.Text = "Open output folder";
        output.SetBounds(330, 258, 150, 36);
        output.Click += (_, _) => OpenOutput();
        Controls.Add(output);

        status.Text = "Ready";
        status.AutoSize = true;
        status.Location = new Point(500, 268);
        Controls.Add(status);

        log.Multiline = true;
        log.ReadOnly = true;
        log.ScrollBars = ScrollBars.Both;
        log.WordWrap = false;
        log.Font = new Font("Consolas", 8.5f);
        log.SetBounds(22, 311, 768, 254);
        log.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(log);
        FormClosing += (_, e) => { if (process is { HasExited: false }) e.Cancel = true; };
    }

    internal bool RunSelfTest() =>
        Text == "SkyRecomp Quest Packager" && build.Enabled && Controls.Contains(game) &&
        File.Exists(Path.Combine(root, "tools", "quest", "build-personal-apk.ps1")) &&
        File.Exists(Path.Combine(root, "tools", "quest", "install-toolchain.ps1")) &&
        File.Exists(Path.Combine(root, "tools", "quest", "remove-toolchain.ps1"));

    private async void InstallToolsClicked(object? sender, EventArgs e)
    {
        var answer = MessageBox.Show(this,
            "This downloads Microsoft OpenJDK 21 and the official Android SDK tools. " +
            "Continuing also accepts the Android SDK license required by sdkmanager. Continue?",
            "Install required tools", MessageBoxButtons.YesNo, MessageBoxIcon.Information);
        if (answer != DialogResult.Yes) return;

        var script = Path.Combine(root, "tools", "quest", "install-toolchain.ps1");
        var info = PowerShellInfo(script, new[] { "-Destination", Path.Combine(root, "toolchain") });
        log.Clear();
        build.Enabled = installTools.Enabled = removeTools.Enabled = false;
        status.Text = "Installing tools...";
        var exit = await RunProcess(info);
        LoadPrivateToolchain();
        status.Text = exit == 0 ? "Tools installed" : $"Tool installation failed (exit {exit})";
        build.Enabled = installTools.Enabled = removeTools.Enabled = true;
    }

    private async void RemoveToolsClicked(object? sender, EventArgs e)
    {
        var privateRoot = Path.GetFullPath(Path.Combine(root, "toolchain"));
        var answer = MessageBox.Show(this,
            $"Remove the packager's private Java and Android tools?\n\n{privateRoot}\n\n" +
            "Game files and generated APKs will not be removed.",
            "Remove installed tools", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
        if (answer != DialogResult.Yes) return;

        log.Clear();
        build.Enabled = installTools.Enabled = removeTools.Enabled = false;
        status.Text = "Removing tools...";
        var script = Path.Combine(root, "tools", "quest", "remove-toolchain.ps1");
        var exit = await RunProcess(PowerShellInfo(script, Array.Empty<string>()));
        if (exit == 0)
        {
            if (sdk.Text.StartsWith(privateRoot, StringComparison.OrdinalIgnoreCase)) sdk.Clear();
            if (java.Text.StartsWith(privateRoot, StringComparison.OrdinalIgnoreCase)) java.Clear();
        }
        status.Text = exit == 0 ? "Private tools removed" : $"Tool removal failed (exit {exit})";
        build.Enabled = installTools.Enabled = removeTools.Enabled = true;
    }

    private void AddLabel(string text, int left, int top, float size = 9, bool semibold = false)
    {
        var label = new Label { Text = text, AutoSize = true, Location = new Point(left, top) };
        if (size != 9 || semibold) label.Font = new Font("Segoe UI", size, semibold ? FontStyle.Bold : FontStyle.Regular);
        Controls.Add(label);
    }

    private void AddPathRow(string caption, TextBox box, int top)
    {
        AddLabel(caption, 22, top + 5);
        box.SetBounds(150, top, 560, 25);
        box.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(box);
        var browse = new Button { Text = "Browse..." };
        browse.SetBounds(715, top - 1, 75, 27);
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
        foreach (var item in new[] { ("game folder", game.Text), ("Android SDK", sdk.Text), ("Java/JDK", java.Text) })
        {
            if (!Directory.Exists(item.Item2))
            {
                MessageBox.Show(this, $"The {item.Item1} path is missing or invalid.", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
        }

        var backend = Path.Combine(root, "tools", "quest", "build-personal-apk.ps1");
        var args = new List<string> { "-GameRoot", game.Text, "-AndroidSdk", sdk.Text, "-JavaHome", java.Text };
        if (install.Checked) args.Add("-Install");
        var info = PowerShellInfo(backend, args);

        log.Clear();
        build.Enabled = false;
        status.Text = "Building...";
        try
        {
            var exit = await RunProcess(info);
            status.Text = exit == 0 ? "Build completed" : $"Build failed (exit {exit})";
        }
        catch (Exception ex)
        {
            Append(ex.Message);
            status.Text = "Build failed";
        }
        finally { build.Enabled = true; }
    }

    private ProcessStartInfo PowerShellInfo(string script, IEnumerable<string> arguments)
    {
        var info = new ProcessStartInfo("powershell.exe")
        {
            WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true
        };
        foreach (var arg in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script })
            info.ArgumentList.Add(arg);
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

    private void LoadPrivateToolchain()
    {
        var config = Path.Combine(root, "toolchain", "toolchain.json");
        if (!File.Exists(config)) return;
        using var document = JsonDocument.Parse(File.ReadAllText(config));
        sdk.Text = document.RootElement.GetProperty("androidSdk").GetString() ?? sdk.Text;
        java.Text = document.RootElement.GetProperty("javaHome").GetString() ?? java.Text;
    }

    private void Append(string? line)
    {
        if (string.IsNullOrEmpty(line)) return;
        if (InvokeRequired) { BeginInvoke(() => Append(line)); return; }
        log.AppendText(line + Environment.NewLine);
    }

    private void OpenOutput()
    {
        var path = Path.Combine(root, "android", "app", "build", "outputs", "apk", "personal", "release");
        if (Directory.Exists(path)) Process.Start(new ProcessStartInfo("explorer.exe", path) { UseShellExecute = true });
    }
}
