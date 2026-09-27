using System.Diagnostics;
using System.IO;
using MinipetServer.Config;

namespace MinipetServer.Music;

/// <summary>
/// QQ 网关 node 子进程生命周期（E3 定稿：单容器，网关是 api 容器内的 node 子进程，
/// 监听 127.0.0.1:{GatewayPort}，仅本进程访问；Dockerfile 最终层已带 /usr/bin/node）。
///
/// 职责（对齐 docs/ai/software-design.md 2.5：QqGatewayProcess = 拉起/停止/健康检查/自动重启 1 次）：
///   · 按配置 Enabled 拉起/停止；脚本路径探测顺序见 ResolveScriptPath；
///   · 健康检查 = QqGatewayClient.ProbeAsync（HTTP /health），由后台定时器每 30s 一次，
///     结果缓存进 Status —— IMusicSource.Health() 是同步方法，绝不能在里面做网络 IO；
///   · 不健康且进程已死 → **自动重启 1 次**：重启额度在每次探测成功时重置为 1，
///     连续失败则不再重启（源置灰 = Degraded，等人工修或下一轮额度恢复）；
///   · 进程退出回收（Process.Exited + Dispose 杀进程树），防僵尸/端口占用。
///
/// 网关脚本本体（Rain120/qq-music-api + 适配层，契约见 QqGatewayClient 顶部注释）
/// **不在本仓库**，镜像也未内置：探测不到脚本时 Status.ScriptFound=false，
/// qq 源 Health() 返回 Degraded 并写明"缺什么/放哪里/怎么配"，不做任何假实现。
/// </summary>
public sealed class QqGatewayProcess : IDisposable
{
    /// <summary>健康巡检周期（Health() 读缓存，巡检负责刷新）。</summary>
    private static readonly TimeSpan ProbeInterval = TimeSpan.FromSeconds(30);

    /// <summary>重启额度回补窗口：额度用尽后每隔该时长回补 1 次。
    /// 没有回补会出现死局——网关置灰后即便运维把脚本修好，也永远不会再被拉起；
    /// 有了回补则"每窗口最多自动重启 1 次"，既守住 E8 降级语义又可自愈。</summary>
    private static readonly TimeSpan RestartBudgetRefill = TimeSpan.FromMinutes(5);

    private readonly ConfigService _cfg;
    private readonly QqGatewayClient _client;
    private readonly object _gate = new();

    private Process? _proc;
    private string? _procScript;   // 当前进程实际用的脚本（配置换脚本 → 重启）
    private string? _lastExit;     // 最近一次进程退出信息（独立字段：不复用 _lastError，防错误串自嵌套）
    private Timer? _timer;
    private int _restartBudget = 1;      // 每次探测成功 / 配置变更重置，按 RestartBudgetRefill 回补；失败消耗 → 用尽即置灰
    private DateTime? _lastRestartUtc;   // 最近一次消耗额度的时刻（回补判定）
    private bool _disposed;

    // ── 状态（Health() 读取；只在锁内更新）────────────
    private bool _scriptFound;
    private string? _scriptPath;
    private DateTime? _startedAtUtc;
    private DateTime? _lastProbeUtc;
    private bool _healthy;
    private bool _cookieLoaded;
    private string? _lastError;
    private int _restarts;
    private int _pid;

    public QqGatewayProcess(ConfigService cfg, QqGatewayClient client)
    {
        _cfg = cfg;
        _client = client;
        _cfg.Changed += OnConfigChanged;
        _timer = new Timer(_ => SafeTick(), null, TimeSpan.Zero, ProbeInterval);
    }

    /// <summary>已解析的网关脚本路径（null = 未找到）。</summary>
    public string? ScriptPath { get { lock (_gate) return _scriptPath; } }

    /// <summary>快照状态（供 QqMusicSource.Health 组装 Degraded 原因；无网络 IO）。</summary>
    public QqGatewayStatus Status
    {
        get
        {
            lock (_gate)
            {
                return new QqGatewayStatus
                {
                    Enabled = _cfg.Current.QqMusic.Enabled,
                    ScriptFound = _scriptFound,
                    ScriptPath = _scriptPath,
                    Running = _proc is { HasExited: false },
                    Healthy = _healthy,
                    CookieLoaded = _cookieLoaded,
                    Pid = _pid,
                    Restarts = _restarts,
                    StartedAtUtc = _startedAtUtc,
                    LastProbeUtc = _lastProbeUtc,
                    LastError = _lastError,
                };
            }
        }
    }

    /// <summary>手动触发一次巡检（admin 端点/测试用；同步等一次探测结果）。</summary>
    public void EnsureHealthyNow() => SafeTick();

    private void OnConfigChanged(ConfigChangedEventArgs e)
    {
        // 开关切换/端口/脚本路径变化 → 立刻巡检（不等下一个 30s 周期）
        bool relevant = e.Old.QqMusic.Enabled != e.New.QqMusic.Enabled
                        || e.Old.QqMusic.GatewayPort != e.New.QqMusic.GatewayPort
                        || !string.Equals(e.Old.QqMusic.GatewayScript, e.New.QqMusic.GatewayScript, StringComparison.Ordinal)
                        || !string.Equals(e.Old.QqMusic.Cookie, e.New.QqMusic.Cookie, StringComparison.Ordinal);
        if (!relevant) return;
        lock (_gate) _restartBudget = 1; // 配置变更 = 人工干预 → 重置重启额度（否则修好也拉不起来）
        _ = Task.Run(SafeTick);
    }

    private void SafeTick()
    {
        try { Tick(); }
        catch (Exception ex)
        {
            lock (_gate) _lastError = ex.Message;
            Console.Error.WriteLine($"[QqGateway] 巡检异常: {ex.Message}");
        }
    }

    private void Tick()
    {
        if (_disposed) return;
        var qq = _cfg.Current.QqMusic;

        if (!qq.Enabled)
        {
            StopProcess("配置停用");
            lock (_gate)
            {
                _healthy = false;
                _cookieLoaded = false;
                _lastError = null;
                _lastProbeUtc = DateTime.UtcNow;
            }
            return;
        }

        string? script = ResolveScriptPath(qq.GatewayScript, out var searched);
        lock (_gate)
        {
            _scriptFound = script != null;
            _scriptPath = script;
            if (script == null) _lastError = $"未找到网关脚本（已探测: {string.Join(" / ", searched)}）";
        }
        if (script == null)
        {
            StopProcess("无网关脚本");
            return;
        }

        // 1) 进程不在（未启动 / 崩了）→ 用重启额度拉起；
        //    脚本路径变更（GatewayScript 改配置）→ 先停旧进程再按新脚本拉起
        bool running;
        string? runningScript;
        lock (_gate) { running = _proc is { HasExited: false }; runningScript = _procScript; }
        if (running && runningScript != null && !string.Equals(runningScript, script, StringComparison.Ordinal))
        {
            StopProcess($"网关脚本变更（{runningScript} → {script}）");
            running = false;
        }
        if (!running)
        {
            if (!TryConsumeRestart($"进程未运行（上次退出 {LastExitReason()}）")) return;
            if (!StartProcess(script, qq.GatewayPort)) return;
        }

        // 2) 健康探测（含 cookie 注入）
        var health = _client.ProbeAsync().GetAwaiter().GetResult();
        lock (_gate)
        {
            _lastProbeUtc = DateTime.UtcNow;
            _healthy = health.Ok;
            _cookieLoaded = health.CookieLoaded;
            _lastError = health.Ok ? null : health.Reason;
            if (health.Ok) _restartBudget = 1; // 恢复：额度重置
        }

        if (health.Ok && !health.CookieLoaded && !string.IsNullOrWhiteSpace(qq.Cookie))
        {
            // 网关活着但没吃 cookie（刚重启）→ 重新注入
            bool ok = _client.SyncCookieAsync(qq.Cookie).GetAwaiter().GetResult();
            lock (_gate) _cookieLoaded = ok;
            if (!ok) Console.Error.WriteLine("[QqGateway] cookie 注入网关失败");
        }

        // 3) 活着但不健康（挂死/端口占用）→ 杀进程 + 重启 1 次
        if (!health.Ok)
        {
            lock (_gate) running = _proc is { HasExited: false };
            if (running && TryConsumeRestart($"健康检查失败（{health.Reason}）"))
            {
                StopProcess("健康检查失败");
                if (StartProcess(script, qq.GatewayPort))
                {
                    var retry = _client.ProbeAsync().GetAwaiter().GetResult();
                    lock (_gate)
                    {
                        _healthy = retry.Ok;
                        _cookieLoaded = retry.CookieLoaded;
                        _lastError = retry.Ok ? null : retry.Reason ?? "重启后仍不健康";
                        _lastProbeUtc = DateTime.UtcNow;
                        if (retry.Ok) _restartBudget = 1;
                    }
                }
            }
        }
    }

    /// <summary>消耗一次重启额度（额度用尽 → 记录原因并返回 false，源置灰；每 RestartBudgetRefill 回补 1 次）。</summary>
    private bool TryConsumeRestart(string reason)
    {
        lock (_gate)
        {
            if (_restartBudget <= 0 && _lastRestartUtc is { } last
                && DateTime.UtcNow - last > RestartBudgetRefill)
            {
                _restartBudget = 1;
                Console.WriteLine("[QqGateway] 重启额度已回补（距上次重启超过回补窗口）");
            }
            if (_restartBudget <= 0)
            {
                _lastError = $"{reason}；自动重启额度已用尽（自动重启 1 次规则）→ qq 源置灰";
                _healthy = false;
                return false;
            }
            _restartBudget--;
            _lastRestartUtc = DateTime.UtcNow;
            return true;
        }
    }

    private bool StartProcess(string script, int port)
    {
        try
        {
            var node = ResolveNodePath();
            var psi = new ProcessStartInfo
            {
                FileName = node,
                WorkingDirectory = Path.GetDirectoryName(script) ?? AppContext.BaseDirectory,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            psi.ArgumentList.Add(script);
            psi.ArgumentList.Add("--port");
            psi.ArgumentList.Add(port.ToString());
            // cookie 经环境变量给网关（不进命令行，避免 ps/日志泄漏）
            psi.Environment["MINIPET_QQ_COOKIE"] = _cfg.Current.QqMusic.Cookie ?? "";
            psi.Environment["QQ_GATEWAY_PORT"] = port.ToString();

            var proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
            proc.OutputDataReceived += (_, e) => { if (!string.IsNullOrWhiteSpace(e.Data)) Console.WriteLine($"[QqGateway] {e.Data}"); };
            proc.ErrorDataReceived += (_, e) => { if (!string.IsNullOrWhiteSpace(e.Data)) Console.Error.WriteLine($"[QqGateway] {e.Data}"); };
            proc.Exited += (_, _) =>
            {
                lock (_gate) _lastExit = $"code {SafeExitCode(proc)}";
                Console.Error.WriteLine($"[QqGateway] 网关进程退出（code {SafeExitCode(proc)}）");
            };
            if (!proc.Start()) { lock (_gate) _lastError = "node 进程启动失败"; return false; }
            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();

            lock (_gate)
            {
                _proc = proc;
                _procScript = script;
                _pid = proc.Id;
                _startedAtUtc = DateTime.UtcNow;
                _restarts++;
                _lastError = null;
            }
            Console.WriteLine($"[QqGateway] 已拉起 node 网关 pid={proc.Id} script={script} port={port}（第 {_restarts} 次）");
            // 给 node 一点启动时间（listen 需要 ~百毫秒；探针本身有 20s 超时，这里只等一拍）
            Thread.Sleep(300);
            return true;
        }
        catch (Exception ex)
        {
            lock (_gate) _lastError = $"网关启动异常：{ex.Message}";
            Console.Error.WriteLine($"[QqGateway] 启动网关失败: {ex.Message}");
            return false;
        }
    }

    private void StopProcess(string reason)
    {
        Process? proc;
        lock (_gate)
        {
            proc = _proc;
            _proc = null;
            _procScript = null;
            _pid = 0;
            _healthy = false;
            _startedAtUtc = null;
        }
        if (proc == null) return;
        try
        {
            if (!proc.HasExited)
            {
                proc.Kill(entireProcessTree: true); // 容器 SIGTERM 传播：主子进程一起收
                proc.WaitForExit(3000);
            }
            Console.WriteLine($"[QqGateway] 网关进程已停止（{reason}）");
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[QqGateway] 停止网关进程异常: {ex.Message}");
        }
        finally { proc.Dispose(); }
    }

    private string? LastExitReason()
    {
        lock (_gate) return _lastExit ?? "（未启动成功）";
    }

    private static int SafeExitCode(Process p)
    {
        try { return p.ExitCode; } catch { return -1; }
    }

    /// <summary>
    /// node 可执行文件：MINIPET_NODE 环境变量 → PATH 上的 node → /usr/bin/node（Dockerfile 落点）。
    /// </summary>
    private static string ResolveNodePath()
    {
        var env = Environment.GetEnvironmentVariable("MINIPET_NODE");
        if (!string.IsNullOrWhiteSpace(env) && File.Exists(env)) return env;
        if (File.Exists("/usr/bin/node")) return "/usr/bin/node";
        return "node";
    }

    /// <summary>
    /// 网关脚本探测：配置 GatewayScript → MINIPET_QQ_GATEWAY 环境变量 →
    /// data/qq-gateway/index.js → 应用目录 qq-gateway/index.js。
    /// searched 回填全部候选（Health 的 Degraded 原因里原样给出，便于定位）。
    /// </summary>
    private static string? ResolveScriptPath(string configured, out List<string> searched)
    {
        var candidates = new List<string>();
        if (!string.IsNullOrWhiteSpace(configured)) candidates.Add(configured.Trim());
        var env = Environment.GetEnvironmentVariable("MINIPET_QQ_GATEWAY");
        if (!string.IsNullOrWhiteSpace(env)) candidates.Add(env.Trim());
        var dataDir = Environment.GetEnvironmentVariable("MINIPET_DATA_DIR");
        if (!string.IsNullOrWhiteSpace(dataDir)) candidates.Add(Path.Combine(dataDir, "qq-gateway", "index.js"));
        candidates.Add(Path.Combine(AppContext.BaseDirectory, "qq-gateway", "index.js"));
        candidates.Add(Path.Combine(Environment.CurrentDirectory, "qq-gateway", "index.js"));
        searched = candidates;
        return candidates.FirstOrDefault(File.Exists);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _cfg.Changed -= OnConfigChanged;
        _timer?.Dispose();
        _timer = null;
        StopProcess("服务退出");
        _client.Dispose();
    }
}

/// <summary>网关状态快照（QqMusicSource.Health 用；纯数据，无副作用）。</summary>
public sealed class QqGatewayStatus
{
    public bool Enabled { get; init; }
    public bool ScriptFound { get; init; }
    public string? ScriptPath { get; init; }
    public bool Running { get; init; }
    public bool Healthy { get; init; }
    public bool CookieLoaded { get; init; }
    public int Pid { get; init; }
    public int Restarts { get; init; }
    public DateTime? StartedAtUtc { get; init; }
    public DateTime? LastProbeUtc { get; init; }
    public string? LastError { get; init; }
}
