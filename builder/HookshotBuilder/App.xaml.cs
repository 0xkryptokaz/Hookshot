using System.IO;
using System.Text;
using System.Windows;

namespace HookshotBuilder;

public partial class App : Application
{
    // (RecompLauncher) --cli <make_game.py's options>: no window; its lines on stdout, and its result as the exit code
    protected override void OnStartup(StartupEventArgs e)
    {
        if (e.Args.Length > 0 && e.Args[0] == "--cli")
        {
            Environment.Exit(Cli(e.Args.Skip(1).ToList()));
            return;
        }
        base.OnStartup(e);
    }

    static int Cli(List<string> args)
    {
        var tools = Tools.Find();
        if (tools == null)
        {
            Console.Error.WriteLine("error The build tools were not found next to this program.");
            return 1;
        }
        using var stdout = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true };
        var done = new ManualResetEventSlim();
        string? failure = null;
        var runner = new Runner(tools, args);
        runner.Line += line => { lock (stdout) stdout.WriteLine(line); };
        runner.Finished += result => { failure = result; done.Set(); };
        runner.Start();
        done.Wait();
        if (failure != null) Console.Error.WriteLine("error " + failure);
        return failure == null ? 0 : 1;
    }
}
