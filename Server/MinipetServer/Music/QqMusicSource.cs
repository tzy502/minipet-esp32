using MinipetServer.Config;

namespace MinipetServer.Music;

/// <summary>
/// QQ 音乐源（E8 / M10，2026-09-27 从占位实现升级为网关客户端形态）：
///   · 取链：QqGatewayClient 调本地 node 网关（QqGatewayProcess 拉起/健康检查/重启 1 次）
///     → 只接受 128k 明文 MP3 → 服务端 HttpClient 流式转发（直链绝不下发设备）；
///   · cookie：Web 导入（POST /api/admin/music/sources/qq/cookie，记 CookieSavedAtUtc）
///     → 过期告警（>7 天 → Health Degraded + /music/sources 的 cookieStale=true，E4）；
///   · 降级：网关脚本缺失/进程起不来/健康检查失败 → Degraded + 明确原因（缺什么、放哪、
///     怎么配），取流抛 BgmSourceUnavailableException（BgmRouter 同源降级，不跨源、不造假）。
///
/// 重要事实（不要在报告里含糊）：网关**本体**（Rain120/qq-music-api + 适配层）不在本仓库、
/// Dockerfile 也未内置，所以本镜像里 qq 源恒为 Degraded 直到运维把网关脚本放进
/// data/qq-gateway/index.js（或配 QqMusic.GatewayScript / MINIPET_QQ_GATEWAY）。
/// 客户端契约见 QqGatewayClient 类头注释。
/// </summary>
public sealed class QqMusicSource : IMusicSource
{
    /// <summary>cookie 有效期告警阈值（E4：QQ 网页 cookie 典型寿命数天，7 天起视为可疑）。</summary>
    public static readonly TimeSpan CookieStaleThreshold = TimeSpan.FromDays(7);

    private readonly ConfigService _cfg;
    private readonly QqGatewayProcess _gateway;
    private readonly QqGatewayClient _client;

    public QqMusicSource(ConfigService cfg, QqGatewayProcess gateway, QqGatewayClient client)
    {
        _cfg = cfg;
        _gateway = gateway;
        _client = client;
    }

    public string Name => "qq";
    public bool IsEnabled => _cfg.Current.QqMusic.Enabled;

    /// <summary>
    /// cookie 新鲜度：(Stale, AgeDays)。cookie 为空 → (false, null)；
    /// 有 cookie 但无导入时间（老配置/手改 JSON）→ (true, null) 保守告警；
    /// 超过 <see cref="CookieStaleThreshold"/> → (true, 天数)。
    /// </summary>
    public static (bool Stale, int? AgeDays) CookieFreshness(QqMusicConfig qq)
    {
        if (string.IsNullOrWhiteSpace(qq.Cookie)) return (false, null);
        if (qq.CookieSavedAtUtc is not { } saved) return (true, null);
        var age = DateTime.UtcNow - saved;
        return (age > CookieStaleThreshold, (int)Math.Max(0, age.TotalDays));
    }

    public MusicSourceHealth Health()
    {
        var qq = _cfg.Current.QqMusic;
        if (!qq.Enabled)
        {
            return new MusicSourceHealth
            {
                State = MusicSourceState.Disabled,
                Detail = "未启用（配置 QqMusic.Enabled=false；Web「曲库」页可开启）",
            };
        }

        var reasons = new List<string>();
        var (stale, ageDays) = CookieFreshness(qq);
        if (string.IsNullOrWhiteSpace(qq.Cookie))
            reasons.Add("cookie 未导入（Web「曲库」页粘贴网页版 cookie）");
        else if (stale)
            reasons.Add(ageDays.HasValue
                ? $"cookie 已保存 {ageDays} 天（>{CookieStaleThreshold.TotalDays:F0} 天阈值）可能已失效，请重新导入"
                : "cookie 无导入时间（旧配置），有效期未知，建议重新导入");

        var st = _gateway.Status;
        if (!st.ScriptFound)
        {
            reasons.Add("需外部 node 网关：" + (st.LastError ?? "未找到网关脚本") +
                        "——网关本体（Rain120/qq-music-api + 适配层）不在本仓库/镜像内；" +
                        "装好后放到 data/qq-gateway/index.js，或配 QqMusic.GatewayScript / MINIPET_QQ_GATEWAY 环境变量");
        }
        else if (!st.Running)
        {
            // LastError 自带"进程未运行/额度用尽"等细节，这里不再套一层前缀（防长串嵌套）
            reasons.Add($"网关不可用：{st.LastError ?? "进程未运行"}（脚本 {st.ScriptPath}）");
        }
        else if (!st.Healthy)
        {
            reasons.Add($"网关健康检查失败（{st.LastError ?? "未知原因"}；pid {st.Pid}）");
        }
        else if (!st.CookieLoaded)
        {
            reasons.Add("网关未加载 cookie（网关 /health 报 cookie=false）");
        }

        if (reasons.Count == 0)
        {
            return new MusicSourceHealth
            {
                State = MusicSourceState.Ok,
                Detail = $"网关就绪（127.0.0.1:{qq.GatewayPort}，pid {st.Pid}，重启 {st.Restarts} 次）+ cookie 有效" +
                         (ageDays.HasValue ? $"（导入 {ageDays} 天）" : ""),
            };
        }
        return new MusicSourceHealth
        {
            State = MusicSourceState.Degraded,
            Detail = string.Join("；", reasons),
        };
    }

    /// <summary>
    /// 曲库列表 = 网关搜索 QqMusic.SearchKeyword（QQ 无"全库"概念，必须有关键词/歌单；
    /// 未配置关键词 → 返回空列表，不臆造默认歌单）。网关不可用/搜索失败 → 空列表 + 日志
    /// （原因由 Health() 给出，Web 曲库页并排显示健康标签）。
    /// </summary>
    public async Task<IReadOnlyList<MusicTrackInfo>> ListTracksAsync(CancellationToken ct = default)
    {
        var qq = _cfg.Current.QqMusic;
        if (!qq.Enabled) return Array.Empty<MusicTrackInfo>();
        if (string.IsNullOrWhiteSpace(qq.SearchKeyword))
        {
            Console.Error.WriteLine("[QqMusicSource] 未配置 QqMusic.SearchKeyword → QQ 曲库列表为空（QQ 源无全库概念）");
            return Array.Empty<MusicTrackInfo>();
        }
        try
        {
            return await _client.SearchAsync(qq.SearchKeyword.Trim(), 30, ct).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[QqMusicSource] 曲库列表失败: {ex.Message}");
            return Array.Empty<MusicTrackInfo>();
        }
    }

    /// <summary>取流：前置检查（启用/cookie/网关）→ 网关取链 + 128k 明文 MP3 校验 + 服务端流式转发。</summary>
    public Task<MusicStream> GetTrackStreamAsync(string trackId, CancellationToken ct = default)
    {
        var qq = _cfg.Current.QqMusic;
        if (!qq.Enabled)
            throw new BgmSourceUnavailableException(Name, "QQ 音源已停用（配置 QqMusic.Enabled=false）");
        if (string.IsNullOrWhiteSpace(qq.Cookie))
            throw new BgmSourceUnavailableException(Name, "QQ cookie 未导入（Web「曲库」页导入后重试）");
        var st = _gateway.Status;
        if (!st.ScriptFound || !st.Running || !st.Healthy)
            throw new BgmSourceUnavailableException(Name,
                "QQ 网关未就绪（" + (st.LastError ?? "需外部 node 网关") + "；详见 GET /api/admin/music/sources 的 health.detail）");
        return _client.OpenStreamAsync(trackId, ct);
    }
}
