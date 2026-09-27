using System.Net;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using MinipetServer.Config;

namespace MinipetServer.Music;

/// <summary>
/// QQ 网关客户端（E8 / M10）——服务端 ↔ 本地 node 网关的**唯一**接口层。
///
/// ═════════════ 网关契约（本客户端按此实现；网关本体不在本仓库）═════════════
/// 上游选型（docs/ai/README.md §QQ 接入调研）：Rain120/qq-music-api（Node/Koa2，活跃）。
/// 它**不是**现成可用的契约实现，需要一个薄适配层把它包成下面三个端点后，由
/// QqGatewayProcess 以 `node index.js --port 3300` 拉起（容器内 127.0.0.1，仅本进程访问）：
///
///   GET  /health
///        → 200 {"ok":true,"cookie":true|false,"version":"..."}
///   GET  /song/url?id={songmid}&quality=128
///        → 200 {"url":"https://...mp3","br":128,"format":"mp3","encrypted":false,"size":123456}
///        · br 必须是 128（kbps）；format 必须是 "mp3"；encrypted 必须为 false
///          —— 只锁 128k 明文 MP3（requirements-analysis.md E8：设备端 minimp3 只吃明文 MP3，
///          m4a/flac/加密 eist 一律拒绝）
///   GET  /search?key={关键词}&limit={n}
///        → 200 {"songs":[{"mid":"...","name":"...","singer":"...","interval":240}]}
///
/// 直链（url 字段）**绝不下发设备**：只在本进程内被 HttpClient 打开并流式转发
/// （vkey 短时效，设备端拿到也会过期；且直链归属账号 cookie，泄漏即盗用）。
///
/// ═════════════ 未安装网关时的行为（明确降级，不造假）═════════════
/// 网关脚本探测不到 → QqGatewayProcess.Status.ScriptFound=false → QqMusicSource.Health()
/// 返回 Degraded + 原因（缺什么、放哪里、怎么配），列表/取流一律抛
/// BgmSourceUnavailableException（BgmRouter 同源降级，不跨源）。
/// </summary>
public sealed class QqGatewayClient : IDisposable
{
    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    private readonly HttpClient _http = new(new SocketsHttpHandler
    {
        // 直链是 CDN：连接池短、超时短——网关本身是本机 127.0.0.1，不需要长连接复用
        PooledConnectionLifetime = TimeSpan.FromMinutes(2),
        AllowAutoRedirect = true,
    })
    {
        Timeout = TimeSpan.FromSeconds(20),
    };

    private readonly ConfigService _cfg;

    public QqGatewayClient(ConfigService cfg)
    {
        _cfg = cfg;
    }

    private int Port => _cfg.Current.QqMusic.GatewayPort is > 0 and < 65536 ? _cfg.Current.QqMusic.GatewayPort : 3300;
    private string BaseUrl => $"http://127.0.0.1:{Port}";

    /// <summary>网关健康 + cookie 注入状态（失败 = 未安装/未启动/不可达，异常信息即原因）。</summary>
    public async Task<QqGatewayHealth> ProbeAsync(CancellationToken ct = default)
    {
        try
        {
            using var res = await _http.GetAsync($"{BaseUrl}/health", ct).ConfigureAwait(false);
            if (!res.IsSuccessStatusCode)
                return new QqGatewayHealth { Ok = false, Reason = $"网关 /health 返回 {(int)res.StatusCode}" };
            var body = await res.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            var node = JsonNode.Parse(body);
            bool ok = node?["ok"]?.GetValue<bool>() ?? true;
            bool cookie = node?["cookie"]?.GetValue<bool>() ?? false;
            return new QqGatewayHealth
            {
                Ok = ok,
                CookieLoaded = cookie,
                Reason = ok ? (cookie ? null : "网关未加载 cookie") : "网关自报 ok=false",
            };
        }
        catch (Exception ex)
        {
            return new QqGatewayHealth { Ok = false, Reason = $"网关不可达（{BaseUrl}）：{ex.Message}" };
        }
    }

    /// <summary>把 Web 导入的 cookie 推给网关（网关重启后需要重新注入）。失败返回 false，不抛。</summary>
    public async Task<bool> SyncCookieAsync(string cookie, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(cookie)) return false;
        try
        {
            using var content = new StringContent(JsonSerializer.Serialize(new { cookie }, JsonOpts),
                System.Text.Encoding.UTF8, "application/json");
            using var res = await _http.PostAsync($"{BaseUrl}/user/setCookie", content, ct).ConfigureAwait(false);
            return res.IsSuccessStatusCode;
        }
        catch { return false; }
    }

    /// <summary>曲目搜索（曲库页/设备选择器展示；网关不可用抛 BgmSourceUnavailableException）。</summary>
    public async Task<IReadOnlyList<MusicTrackInfo>> SearchAsync(string keyword, int limit, CancellationToken ct = default)
    {
        try
        {
            var url = $"{BaseUrl}/search?key={Uri.EscapeDataString(keyword)}&limit={Math.Clamp(limit, 1, 100)}";
            using var res = await _http.GetAsync(url, ct).ConfigureAwait(false);
            if (!res.IsSuccessStatusCode) throw new BgmSourceUnavailableException("qq", $"网关 /search 返回 {(int)res.StatusCode}");
            var body = await res.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            var parsed = JsonSerializer.Deserialize<SearchResponse>(body, JsonOpts);
            return (parsed?.Songs ?? new List<SearchSong>()).Select(s => new MusicTrackInfo
            {
                Id = s.Mid ?? "",
                Title = string.IsNullOrWhiteSpace(s.Singer) ? s.Name ?? "" : $"{s.Name} - {s.Singer}",
                Category = "QQ 搜索",
                Source = "qq",
                Bytes = 0,
            }).Where(t => !string.IsNullOrEmpty(t.Id)).ToList();
        }
        catch (BgmSourceUnavailableException) { throw; }
        catch (Exception ex) { throw new BgmSourceUnavailableException("qq", $"网关取曲库失败：{ex.Message}"); }
    }

    /// <summary>
    /// 取链 + 校验 + 流式转发（只接受 128k 明文 MP3）。
    /// 直链只存在于本方法栈内：返回的 Stream 是 HTTP 响应体（ResponseHeadersRead，不整载内存）。
    /// 校验失败（br≠128 / 非 mp3 / encrypted）抛 BgmSourceUnavailableException，绝不降级成"随便发个链接"。
    /// </summary>
    public async Task<MusicStream> OpenStreamAsync(string trackId, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(trackId)) throw new ArgumentException("trackId 为空", nameof(trackId));

        QqSongUrl? info;
        try
        {
            var url = $"{BaseUrl}/song/url?id={Uri.EscapeDataString(trackId)}&quality=128";
            using var res = await _http.GetAsync(url, ct).ConfigureAwait(false);
            if (!res.IsSuccessStatusCode) throw new BgmSourceUnavailableException("qq", $"网关 /song/url 返回 {(int)res.StatusCode}");
            var body = await res.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            info = JsonSerializer.Deserialize<QqSongUrl>(body, JsonOpts)
                   ?? throw new BgmSourceUnavailableException("qq", "网关 /song/url 响应为空");
        }
        catch (BgmSourceUnavailableException) { throw; }
        catch (Exception ex) { throw new BgmSourceUnavailableException("qq", $"网关取链失败：{ex.Message}"); }

        if (string.IsNullOrWhiteSpace(info.Url))
            throw new BgmSourceUnavailableException("qq", $"曲目 {trackId} 无可用直链（VIP/版权/风控，网关未返回 url）");
        if (info.Encrypted == true)
            throw new BgmSourceUnavailableException("qq", $"曲目 {trackId} 直链为加密流（encrypted=true），设备端 minimp3 无法解码 → 拒绝");
        if (info.Br is > 0 and not 128)
            throw new BgmSourceUnavailableException("qq", $"曲目 {trackId} 仅 {info.Br}kbps（E8 只锁 128k；更高码率不可用）");
        if (!string.IsNullOrEmpty(info.Format) && !string.Equals(info.Format, "mp3", StringComparison.OrdinalIgnoreCase))
            throw new BgmSourceUnavailableException("qq", $"曲目 {trackId} 格式 {info.Format}（E8 只锁明文 MP3）");

        HttpResponseMessage? direct = null;
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, info.Url);
            req.Headers.Referrer = new Uri("https://y.qq.com/");
            direct = await _http.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
            if (!direct.IsSuccessStatusCode)
                throw new BgmSourceUnavailableException("qq", $"直链下载失败：HTTP {(int)direct.StatusCode}");

            var mediaType = direct.Content.Headers.ContentType?.MediaType ?? "";
            if (mediaType is not ("audio/mpeg" or "audio/mp3" or "audio/x-mpeg" or "application/octet-stream"))
                throw new BgmSourceUnavailableException("qq", $"直链返回 {mediaType}（非 MP3 音频流，拒绝下发）");

            var stream = await direct.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
            var title = QueryTitle(info.Url);
            return new MusicStream
            {
                Stream = new ForwardStream(stream, direct), // 随流释放 HttpResponseMessage（连接归还池）
                MimeType = "audio/mpeg",
                Track = new MusicTrackInfo
                {
                    Id = trackId,
                    Title = title,
                    Category = "QQ 网关",
                    Source = "qq",
                    Bytes = direct.Content.Headers.ContentLength ?? info.Size ?? 0,
                },
            };
        }
        catch (BgmSourceUnavailableException)
        {
            direct?.Dispose();
            throw;
        }
        catch (Exception ex)
        {
            direct?.Dispose();
            throw new BgmSourceUnavailableException("qq", $"直链取流失败：{ex.Message}");
        }
    }

    private static string QueryTitle(string url)
    {
        try
        {
            var name = Path.GetFileName(new Uri(url).AbsolutePath);
            return string.IsNullOrEmpty(name) ? "QQ 曲目" : Uri.UnescapeDataString(name);
        }
        catch { return "QQ 曲目"; }
    }

    public void Dispose() => _http.Dispose();

    /// <summary>流 + 其底层 HttpResponseMessage 一起释放（HttpClient 流式响应的正确回收姿势）。</summary>
    private sealed class ForwardStream : Stream
    {
        private readonly Stream _inner;
        private readonly IDisposable _owner;
        public ForwardStream(Stream inner, IDisposable owner) { _inner = inner; _owner = owner; }
        public override bool CanRead => _inner.CanRead;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => _inner.Length;
        public override long Position { get => _inner.Position; set => throw new NotSupportedException(); }
        public override void Flush() => _inner.Flush();
        public override int Read(byte[] buffer, int offset, int count) => _inner.Read(buffer, offset, count);
        public override int Read(Span<byte> buffer) => _inner.Read(buffer);
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken ct = default) => _inner.ReadAsync(buffer, ct);
        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken ct) => _inner.ReadAsync(buffer, offset, count, ct);
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        protected override void Dispose(bool disposing)
        {
            if (disposing) { _inner.Dispose(); _owner.Dispose(); }
            base.Dispose(disposing);
        }
    }

    // ── 网关 DTO（契约见类头注释）──────────────────────────────────────────

    private sealed class SearchResponse
    {
        [JsonPropertyName("songs")] public List<SearchSong>? Songs { get; set; }
    }

    private sealed class SearchSong
    {
        [JsonPropertyName("mid")] public string? Mid { get; set; }
        [JsonPropertyName("name")] public string? Name { get; set; }
        [JsonPropertyName("singer")] public string? Singer { get; set; }
        [JsonPropertyName("interval")] public int Interval { get; set; }
    }

    private sealed class QqSongUrl
    {
        [JsonPropertyName("url")] public string? Url { get; set; }
        [JsonPropertyName("br")] public int Br { get; set; }
        [JsonPropertyName("format")] public string? Format { get; set; }
        [JsonPropertyName("encrypted")] public bool? Encrypted { get; set; }
        [JsonPropertyName("size")] public long? Size { get; set; }
    }
}

/// <summary>网关探测/健康结果（QqGatewayClient.ProbeAsync）。</summary>
public sealed class QqGatewayHealth
{
    public bool Ok { get; init; }
    public bool CookieLoaded { get; init; }
    public string? Reason { get; init; }
}
