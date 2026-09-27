using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using Makaretu.Dns;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using MinipetServer.Config;

namespace MinipetServer.Services;

/// <summary>
/// E14 mDNS 服务广告：让设备在「配网页服务器地址留空」时自动发现本服务端。
///
/// ── 契约（以固件为准，勿单方面改）─────────────────────────────────────────
/// 固件 <c>Firmware/main/net/mdns_discover.c</c> / <c>.h</c>：
///   · 查询服务类型常量 <c>MP_MDNS_SERVICE="_minipet"</c> + <c>MP_MDNS_PROTO="_tcp"</c>
///     → 即 <c>_minipet._tcp.local</c>；
///   · 先 <c>mdns_query_ptr("_minipet","_tcp",1100ms,4,&amp;res)</c>；应答里没带 A 记录时，
///     再用 <c>mdns_query_a(r-&gt;hostname, 1100ms, &amp;v4)</c> 补一次；
///   · 拿到 IP 后拼 <c>http://&lt;ip&gt;:&lt;port&gt;</c>，<c>port==0</c> 时缺省 38090；
///   · 只在两种时机发现：① NVS srv_url 为空（配网页留空）② 手输地址 hello 连不上兜底一次。
/// 所以服务端必须做到：广告 PTR/SRV/TXT **且** SRV 的 target 能被 A 记录解析。
/// 本实现把 PTR 查询的应答做成**单包 PTR+SRV+TXT+A**（等价 Avahi/Bonjour 的应答形状），
/// 让固件第一条路径就命中（r-&gt;addr 直接有 IPv4）；同时单独应答 hostname 的 A 查询，
/// 覆盖固件的「A 补查」第二条路径。两条路径都通。
///
/// ── 为什么不直接用 Makaretu 的 ServiceDiscovery.Advertise(ServiceProfile) ──
/// 本仓库引用的 <c>Makaretu.Dns 2.0.1</c> 是**纯 DNS 对象模型**包，实测其 70 个导出类型里
/// **没有** <c>ServiceDiscovery</c>/<c>ServiceProfile</c>（它们在另一个包
/// <c>Makaretu.Dns.Multicast</c> 里，该包停在 0.27.0）。而 0.27.0 传递依赖
/// <c>Tmds.LibC 0.2.0</c>（网卡枚举的原生 libc 绑定），其 runtimes 只有
/// linux-arm / linux-arm64 / linux-x64 —— **没有 macOS/Windows 资产**，本机（macOS）自测
/// 直接跑不起来，NAS 镜像也多一份原生依赖。故只借用已引用的 Makaretu.Dns 做**报文编解码**
/// （<see cref="Message"/>/<see cref="ResourceRecord"/> 及名字压缩），组播收发自己用
/// <see cref="Socket"/> 实现：约 200 行、零新依赖、应答内容完全可控。
///
/// ── 端口口径（见 ResolvePort）─────────────────────────────────────────────
/// 设备在局域网上按「主机对外可达端口」连接。开发/直跑用实际监听端口；容器里监听 8080、
/// 对外映射 38090（.env MINIPET_PORT），此时必须广告**对外端口**，否则设备拼出的
/// http://&lt;ip&gt;:8080 连不上。优先级：配置 Mdns.Port &gt; 环境变量 MINIPET_PORT &gt;
/// 实际监听端口（IServerAddressesFeature / ASPNETCORE_URLS / ASPNETCORE_HTTP_PORTS）&gt; 38090。
///
/// ── 降级（硬要求）────────────────────────────────────────────────────────
/// mDNS 起不来（端口被占 / 无可用网卡 / 权限 / 网络受限）**绝不影响服务端启动**：
/// <see cref="Start"/> 内部全量 try/catch，失败只记一条 warning 并置 <see cref="LastError"/>。
/// 另外受配置 <c>Mdns.Enabled</c>（默认 true，老配置文件缺该字段=开）控制。
///
/// ── 部署前提（写进注释，避免线上白排查）──────────────────────────────────
/// 容器 **bridge 网络收不到也发不出局域网组播**，且广告的 A 记录会是容器内网 IP
/// （设备不可达）→ 需要 <c>network_mode: host</c>（docs/ai/keys-touch-handoff.md §7.1 第 5 条）。
/// 本类不主动探测这一点，只在端口口径不一致时打一条 warning 提示。
/// </summary>
public sealed class MdnsAdvertiser : IDisposable
{
    // ── 与固件逐字对齐的常量 ────────────────────────────────────────────────
    /// <summary>服务名（固件 MP_MDNS_SERVICE）。</summary>
    public const string Service = "_minipet";
    /// <summary>协议（固件 MP_MDNS_PROTO）。</summary>
    public const string Proto = "_tcp";
    /// <summary>PTR 查询名（固件 mdns_query_ptr(MP_MDNS_SERVICE, MP_MDNS_PROTO, ...)）。</summary>
    public const string ServiceType = Service + "." + Proto + ".local";

    /// <summary>服务类型枚举名（_services._dns-sd._udp.local，仅给 dns-sd -B 排障用，固件不查）。</summary>
    public const string ServiceEnumeration = "_services._dns-sd._udp.local";

    /// <summary>mDNS 组播地址 / 端口（RFC 6762）。</summary>
    public static readonly IPAddress MulticastGroup = IPAddress.Parse("224.0.0.251");
    public const int MulticastPort = 5353;

    /// <summary>默认 HTTP 端口（与固件 MDNS_DEFAULT_PORT、.env MINIPET_PORT 同值）。</summary>
    public const int DefaultHttpPort = 38090;

    /// <summary>PTR/SRV/TXT 记录 TTL（RFC 6762 §10：建议 75 分钟）。</summary>
    public static readonly TimeSpan ServiceTtl = TimeSpan.FromSeconds(4500);
    /// <summary>A 记录 TTL（RFC 6762 §10：host 记录建议 120 秒）。</summary>
    public static readonly TimeSpan HostTtl = TimeSpan.FromSeconds(120);
    /// <summary>TXT 协议版本（对应固件 MP_PROTO_VER；当前固件不读，预留给版本协商）。</summary>
    public const string TxtVersion = "ver=1";
    /// <summary>TXT 端点前缀（keys-touch-handoff §7.1 建议值）。</summary>
    public const string TxtPath = "path=/api/device";
    /// <summary>TXT 展示名（Web/排障可见）。</summary>
    public const string TxtName = "name=MiniPet";

    private const int MaxPacketBytes = 4096;          // 本服务应答 ~300B；4096 足够且不浪费
    private const int AnnounceCount = 3;              // RFC 6762 §8.3：公告至少发 2 次
    /// <summary>同一查询的抑制窗口（重复投递抑制，RFC 6762 §7.4）。</summary>
    private static readonly TimeSpan DuplicateWindow = TimeSpan.FromSeconds(1);

    private readonly ConfigService _cfg;
    private readonly ILogger<MdnsAdvertiser> _log;
    private readonly IServer? _server;

    private readonly object _gate = new();
    private readonly List<IPAddress> _joined = new();
    private readonly Dictionary<string, DateTime> _recentQueries = new(StringComparer.Ordinal);
    private Socket? _socket;
    private CancellationTokenSource? _cts;
    private bool _started;
    private bool _disposed;

    /// <summary>构造时传 IServer=null 也可（DI 未注册时用默认值）：端口退回环境变量口径。</summary>
    public MdnsAdvertiser(ConfigService cfg, ILogger<MdnsAdvertiser> log, IServer? server = null)
    {
        _cfg = cfg;
        _log = log;
        _server = server;
    }

    /// <summary>正在广告中（false = 未启用/启动失败，见 <see cref="LastError"/>）。</summary>
    public bool IsRunning { get; private set; }

    /// <summary>启动失败原因（null = 无错误）。失败不影响服务端其它功能。</summary>
    public string? LastError { get; private set; }

    /// <summary>本次要广告的档案（端口/网卡解析成功即非 null；是否真的在播看 <see cref="IsRunning"/>）。</summary>
    public MdnsAdvertisement? Advertisement { get; private set; }

    /// <summary>端口来源说明（日志/排障用，如「配置 Mdns.Port」「环境变量 MINIPET_PORT」）。</summary>
    public string PortSource { get; private set; } = "(未解析)";

    /// <summary>一行状态（/api/health 展示用）。</summary>
    public string StatusText => IsRunning
        ? $"advertising {Advertisement?.ServiceInstanceName} port={Advertisement?.Port} host={Advertisement?.HostName} ({PortSource})"
        : (LastError is null ? "disabled" : $"degraded: {LastError}");

    // ── 启动 ────────────────────────────────────────────────────────────────
    /// <summary>
    /// 启动广告（幂等、**永不抛**）。调用点：Program.cs 的 ApplicationStarted 回调
    /// （此时监听端口才是确定的；也保证不阻塞 HTTP 启动路径）。
    /// </summary>
    public void Start()
    {
        if (_disposed) return;
        lock (_gate)
        {
            if (_started) return;
            _started = true;
        }
        try { StartCore(); }
        catch (Exception ex) { Degrade($"启动异常：{ex.Message}"); }
    }

    private void StartCore()
    {
        var cfg = _cfg.Current.Mdns;
        if (!cfg.Enabled)
        {
            LastError = null;
            PortSource = "未启用";
            _log.LogInformation("[mDNS] 已按配置关闭（Mdns.Enabled=false）：设备将只能靠配网页手输地址");
            return;
        }

        // ① 端口口径（见类头）
        var (port, source) = ResolvePort(cfg.Port, ListeningUrls());
        PortSource = source;

        // ② 主机名 / 实例名
        //    主机名刻意不用裸 <hostname>.local：那多半被系统 mDNS 守护（macOS mDNSResponder /
        //    avahi）以同一名字广告，两边 A 记录集合不一致会触发 RFC 6762 名字冲突（系统守护
        //    可能把自己改名成 xxx-2.local）。加 minipet- 前缀独占一个名字，零冲突。
        var hostLabel = SanitizeHostLabel(Dns.GetHostName());
        var hostName = $"minipet-{hostLabel}.local";
        var instanceName = $"MiniPet-{hostLabel}";

        // ③ 网卡与 A 记录地址
        var ifaces = SelectInterfaces(cfg.Interface);
        if (ifaces.Count == 0)
        {
            Degrade(string.IsNullOrWhiteSpace(cfg.Interface)
                ? "没有可用于 mDNS 的网卡（需已启用、非回环、支持组播、有非 link-local IPv4）"
                : $"配置的网卡地址不可用：Mdns.Interface={cfg.Interface}（本机没有该 IPv4）");
            return;
        }
        var addresses = ChooseAddresses(ifaces);
        if (addresses.Count == 0) { Degrade("没有可广告的 IPv4 地址"); return; }

        Advertisement = new MdnsAdvertisement
        {
            InstanceName = instanceName,
            ServiceType = ServiceType,
            Port = (ushort)port,
            HostName = hostName,
            Addresses = addresses,
            Txt = new[] { TxtVersion, TxtPath, TxtName },
        };

        // ④ 套接字：0.0.0.0:5353 + SO_REUSEADDR/SO_REUSEPORT（与系统 mDNS 守护共存）
        var sock = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        try
        {
            sock.ExclusiveAddressUse = false;            // Unix 上同时放开 SO_REUSEADDR + SO_REUSEPORT
            sock.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            sock.Bind(new IPEndPoint(IPAddress.Any, MulticastPort));
            sock.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastLoopback, true);
            sock.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastTimeToLive, 255); // RFC 6762 §11
        }
        catch (Exception ex)
        {
            try { sock.Dispose(); } catch { /* 关不掉也不影响：下面已降级返回 */ }
            Degrade($"UDP {MulticastPort} 绑定失败：{ex.Message}（端口被独占 / 权限不足）");
            return;
        }

        // ⑤ 逐网卡加入组播组（部分虚拟网卡会失败：跳过即可，全失败才算降级）
        //    注意按**网卡**去重：一块网卡挂多个 IPv4 时入组两次会让同一查询收到两份 → 重复应答。
        var joins = PrimaryPerNic(ifaces);
        var joined = new List<IPAddress>();
        foreach (var iface in joins)
        {
            try
            {
                sock.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.AddMembership,
                    new MulticastOption(MulticastGroup, iface.Address));
                joined.Add(iface.Address);
            }
            catch (Exception ex)
            {
                _log.LogDebug("[mDNS] 网卡 {Nic}({Address}) 加入组播失败，跳过：{Message}",
                    iface.Nic, iface.Address, ex.Message);
            }
        }
        if (joined.Count == 0)
        {
            try { sock.Dispose(); } catch { /* 降级路径 */ }
            Degrade("所有网卡加入 224.0.0.251 组播组均失败（虚拟网卡/权限受限）");
            return;
        }

        _socket = sock;
        lock (_gate) { _joined.Clear(); _joined.AddRange(joined); }
        _cts = new CancellationTokenSource();
        var token = _cts.Token;
        IsRunning = true;
        LastError = null;

        _ = Task.Run(() => ReceiveLoopAsync(sock, token), CancellationToken.None);   // 收查询 → 应答
        _ = Task.Run(() => Announce(sock, token), CancellationToken.None);           // 开播公告（3 次，不阻塞启动）

        _log.LogInformation(
            "[mDNS] 已广告 {Service} instance={Instance} host={Host} port={Port}（端口来源：{Source}）地址=[{Addresses}] 网卡=[{Nics}]",
            ServiceType, Advertisement.ServiceInstanceName, hostName, port, source,
            string.Join(", ", addresses), string.Join(", ", joined));

        if (source.StartsWith("环境变量", StringComparison.Ordinal) && !ListeningUrls().Any(u => u.Contains($":{port}")))
        {
            // 典型场景：容器 bridge 网络 8080 监听 + 38090 映射。端口对了，但组播出不了容器、
            // A 记录也是容器内网 IP —— 需要 network_mode: host（docs/ai/keys-touch-handoff.md §7.1）。
            _log.LogWarning(
                "[mDNS] 广告端口 {Port} 与容器内监听端口不一致：若本服务跑在 Docker bridge 网络，" +
                "组播与 A 记录都到不了局域网，需改用 network_mode: host（或让监听端口=对外端口）", port);
        }
    }

    private void Degrade(string reason)
    {
        LastError = reason;
        IsRunning = false;
        _log.LogWarning("[mDNS] 未启用广告（不影响服务端启动）：{Reason}", reason);
    }

    /// <summary>IServer 实际监听地址（ApplicationStarted 后才有值；拿不到返回空）。</summary>
    private IReadOnlyList<string> ListeningUrls()
    {
        try
        {
            var feature = _server?.Features.Get<IServerAddressesFeature>();
            if (feature?.Addresses is { Count: > 0 } addrs) return addrs.ToList();
        }
        catch { /* 拿不到就走环境变量口径 */ }
        return Array.Empty<string>();
    }

    // ── 端口口径（静态纯函数：自测可直接调）────────────────────────────────
    /// <summary>
    /// 解析「广告端口」——即设备该连的对外端口。优先级：
    /// ① 配置 <c>Mdns.Port</c>（&gt;0，人工钉死，优先级最高）；
    /// ② 环境变量 <c>MINIPET_PORT</c>（部署层定义的对外端口：.env/.env.example 的唯一端口项，
    ///    compose 里 ":38090:8080" 的左边；容器内监听 8080 时，设备必须连 38090）；
    /// ③ 实际监听端口（IServerAddressesFeature 里的 http URL；本地 dotnet run 的真相）；
    /// ④ <c>ASPNETCORE_URLS</c> / <c>ASPNETCORE_HTTP_PORTS</c> / <c>ASPNETCORE_HTTPS_PORTS</c> 兜底；
    /// ⑤ <see cref="DefaultHttpPort"/>（与固件缺省同值）。
    /// </summary>
    public static (int Port, string Source) ResolvePort(
        int configuredPort, IReadOnlyList<string> listeningUrls, Func<string, string?>? getEnv = null)
    {
        getEnv ??= Environment.GetEnvironmentVariable;

        if (configuredPort is > 0 and <= 65535) return (configuredPort, "配置 Mdns.Port");

        if (int.TryParse((getEnv("MINIPET_PORT") ?? "").Trim(), out var envPort) && envPort is > 0 and <= 65535)
            return (envPort, "环境变量 MINIPET_PORT（部署层对外端口）");

        foreach (var url in listeningUrls)
        {
            var p = PortFromUrl(url);
            if (p is > 0 and <= 65535) return (p.Value, $"实际监听端口（{url}）");
        }

        foreach (var key in new[] { "ASPNETCORE_URLS", "ASPNETCORE_HTTP_PORTS", "ASPNETCORE_HTTPS_PORTS" })
        {
            var raw = (getEnv(key) ?? "").Trim();
            if (raw.Length == 0) continue;
            foreach (var part in raw.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            {
                var p = PortFromUrl(part);
                if (p is > 0 and <= 65535) return (p.Value, $"环境变量 {key}");
            }
        }

        return (DefaultHttpPort, $"缺省值（{DefaultHttpPort}）");
    }

    /// <summary>从 "http://0.0.0.0:38090" / "http://+:8080" / "8080" 里取端口；取不到返回 null。</summary>
    private static int? PortFromUrl(string url)
    {
        if (string.IsNullOrWhiteSpace(url)) return null;
        var s = url.Trim();
        if (int.TryParse(s, out var bare) && bare is > 0 and <= 65535) return bare;   // ASPNETCORE_HTTP_PORTS 裸端口形态
        if (!s.Contains("://", StringComparison.Ordinal)) s = "http://" + s;
        return Uri.TryCreate(s, UriKind.Absolute, out var uri) && uri.Port is > 0 and <= 65535 ? uri.Port : null;
    }

    /// <summary>
    /// 主机标签清洗：小写、仅保留 [a-z0-9-]（DNS label 合法字符），去掉首尾 '-'，最长 63。
    /// 例："a502deMacBook-Pro.local" → "a502demacbook-pro"；空/非法 → "minipet"。
    /// </summary>
    public static string SanitizeHostLabel(string? raw)
    {
        var src = (raw ?? "").Trim();
        var cut = src.IndexOf('.');
        if (cut > 0) src = src[..cut];                       // 去掉已有域后缀
        var sb = new System.Text.StringBuilder(src.Length);
        foreach (var ch in src.ToLowerInvariant())
        {
            if (ch is >= 'a' and <= 'z' or >= '0' and <= '9' or '-') sb.Append(ch);
            else if (ch is '_' or ' ') sb.Append('-');
        }
        var label = sb.ToString().Trim('-');
        if (label.Length == 0) label = "minipet";
        return label.Length > 63 ? label[..63] : label;
    }

    // ── 网卡选择（静态纯函数：自测可直接调）────────────────────────────────
    /// <summary>一块可用网卡上的一个 IPv4 地址。</summary>
    public sealed record MdnsInterface(string Nic, IPAddress Address, bool HasGateway);

    /// <summary>
    /// 枚举可用于 mDNS 的网卡地址：已启用、非回环、支持组播、有 IPv4、排除 169.254/16。
    /// <paramref name="onlyAddress"/> 非空时只保留该 IPv4（配置 <c>Mdns.Interface</c>，多网卡 NAS 选路用）。
    /// **排序有意义**：有默认网关的网卡（=真正连局域网的网卡）排最前——设备（固件 first_ipv4）
    /// 取应答里第一条 A 记录，排错就把 docker0/虚拟网卡的地址给了设备。
    /// </summary>
    public static IReadOnlyList<MdnsInterface> SelectInterfaces(string? onlyAddress = null)
    {
        IPAddress? only = null;
        if (!string.IsNullOrWhiteSpace(onlyAddress)) IPAddress.TryParse(onlyAddress.Trim(), out only);

        var result = new List<MdnsInterface>();
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            try
            {
                if (nic.OperationalStatus != OperationalStatus.Up) continue;
                if (nic.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                if (!nic.SupportsMulticast) continue;

                var props = nic.GetIPProperties();
                bool hasGateway = false;
                try
                {
                    hasGateway = props.GatewayAddresses.Any(g =>
                        g.Address.AddressFamily == AddressFamily.InterNetwork && !g.Address.Equals(IPAddress.Any));
                }
                catch { /* 个别网卡查网关会抛：按无网关处理 */ }

                foreach (var ua in props.UnicastAddresses)
                {
                    var addr = ua.Address;
                    if (addr.AddressFamily != AddressFamily.InterNetwork) continue;
                    if (IPAddress.IsLoopback(addr)) continue;
                    if (IsLinkLocal(addr)) continue;
                    if (only is not null && !only.Equals(addr)) continue;
                    result.Add(new MdnsInterface(nic.Name, addr, hasGateway));
                }
            }
            catch { /* 单块网卡异常不影响其它网卡 */ }
        }

        return result
            .OrderByDescending(i => i.HasGateway)
            .ThenBy(i => i.Nic, StringComparer.Ordinal)
            .ToList();
    }

    private static bool IsLinkLocal(IPAddress addr)
    {
        var b = addr.GetAddressBytes();
        return b.Length == 4 && b[0] == 169 && b[1] == 254;
    }

    /// <summary>
    /// 每块网卡取一个代表地址（入组/指定出口用）：同一网卡多 IPv4 时只算一次，
    /// 避免「一次查询收到 N 份 → 回 N 份」的应答放大。
    /// </summary>
    public static IReadOnlyList<MdnsInterface> PrimaryPerNic(IReadOnlyList<MdnsInterface> interfaces)
        => interfaces.GroupBy(i => i.Nic, StringComparer.Ordinal).Select(g => g.First()).ToList();

    /// <summary>
    /// 选广告哪些地址：有默认网关的网卡存在时**只广告它们**（避免把 docker0/虚拟网卡地址
    /// 卖给设备，设备连不上就白发现一次）；都没有网关（少见的静态路由拓扑）才全给。
    /// </summary>
    public static IReadOnlyList<IPAddress> ChooseAddresses(IReadOnlyList<MdnsInterface> interfaces)
    {
        var gateway = interfaces.Where(i => i.HasGateway).Select(i => i.Address).Distinct().ToList();
        return gateway.Count > 0 ? gateway : interfaces.Select(i => i.Address).Distinct().ToList();
    }

    // ── 收查询 → 应答 ───────────────────────────────────────────────────────
    private async Task ReceiveLoopAsync(Socket sock, CancellationToken ct)
    {
        var buffer = new byte[MaxPacketBytes];
        var from = new IPEndPoint(IPAddress.Any, 0);
        while (!ct.IsCancellationRequested)
        {
            SocketReceiveFromResult r;
            try
            {
                r = await sock.ReceiveFromAsync(buffer, SocketFlags.None, from).ConfigureAwait(false);
            }
            catch (ObjectDisposedException) { break; }                       // Dispose 关套接字
            catch (OperationCanceledException) { break; }
            catch (SocketException ex)
            {
                if (ct.IsCancellationRequested) break;
                _log.LogDebug("[mDNS] 收包失败（继续）：{Message}", ex.Message);
                continue;
            }
            try { HandlePacket(buffer.AsSpan(0, r.ReceivedBytes).ToArray(), (IPEndPoint)r.RemoteEndPoint); }
            catch (Exception ex) { _log.LogDebug("[mDNS] 丢弃无法解析的报文：{Message}", ex.Message); }
        }
    }

    private void HandlePacket(byte[] packet, IPEndPoint from)
    {
        var adv = Advertisement;
        if (adv is null) return;

        var msg = (Message)new Message().Read(packet);
        if (!msg.IsQuery) return;                       // 响应/公告（含自己发的回声）一律不处理
        if (IsDuplicateQuery(packet, from)) return;     // 同一查询被重复投递（多网卡入组会投 N 份）→ 只答一次

        // 传统单播查询（源端口非 5353）：应答必须**单播回源**、TTL ≤ 10s、不带 cache-flush 位（RFC 6762 §6.7）
        bool legacyUnicast = from.Port != MulticastPort;

        var answers = new List<ResourceRecord>();
        foreach (var q in msg.Questions)
        {
            foreach (var rec in MatchRecords(adv, q.Name?.ToString() ?? "", q.Type))
                if (!answers.Any(x => SameRecord(x, rec))) answers.Add(rec);
        }
        if (answers.Count == 0) return;                 // 不是问我们 → 保持安静（mDNS 礼仪）

        var outgoing = legacyUnicast ? LegacyShape(answers) : answers;
        var resp = msg.CreateResponse();
        resp.AA = true;                                 // 权威应答（本机就是这些记录的唯一来源）
        resp.Answers.AddRange(outgoing);
        SendPacket(resp.ToByteArray(), legacyUnicast ? from : null);
    }

    /// <summary>查询名/类型 → 应答记录。**PTR 查询一次给全 PTR+SRV+TXT+A**（固件首条路径就拿到 IPv4）；
    /// hostname 的 A 查询单独应答（固件「A 补查」路径）。</summary>
    private static IReadOnlyList<ResourceRecord> MatchRecords(MdnsAdvertisement adv, string name, DnsType type)
    {
        bool any = type == DnsType.ANY;
        var all = adv.ToRecords();

        if (Eq(name, adv.ServiceType))
        {
            // 服务浏览：PTR + SRV + TXT + A 全给（等价 Avahi 的应答形状）
            return any || type == DnsType.PTR ? all : Array.Empty<ResourceRecord>();
        }
        if (Eq(name, ServiceEnumeration))
        {
            // 服务类型枚举（dns-sd -B 排障用）
            return any || type == DnsType.PTR
                ? new ResourceRecord[]
                  {
                      new PTRRecord
                      {
                          Name = new DomainName(ServiceEnumeration),
                          DomainName = new DomainName(adv.ServiceType),
                          TTL = MdnsAdvertiser.ServiceTtl,
                          Class = DnsClass.IN,
                      },
                  }
                : Array.Empty<ResourceRecord>();
        }
        if (Eq(name, adv.ServiceInstanceName))
        {
            return any || type is DnsType.SRV or DnsType.TXT
                ? all.Where(r => r is SRVRecord or TXTRecord or ARecord).ToList()
                : Array.Empty<ResourceRecord>();
        }
        if (Eq(name, adv.HostName))
        {
            return any || type == DnsType.A ? all.Where(r => r is ARecord).ToList() : Array.Empty<ResourceRecord>();
        }
        return Array.Empty<ResourceRecord>();
    }

    private static bool Eq(string a, string b) => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// 重复查询抑制（RFC 6762 §7.4）：多网卡各入一次组播组时，内核会把**同一份查询**投递多次
    /// （实测 2 网卡 = 收到 2 份）→ 不拦就回 2 份，白占带宽还可能让设备的 result 列表出现重复项。
    /// 以「源端点 + 报文指纹」为键，1s 窗口内只答第一次；字典按窗口清理，规模恒定。
    /// </summary>
    private bool IsDuplicateQuery(byte[] packet, IPEndPoint from)
    {
        var key = $"{from}|{System.IO.Hashing.XxHash64.HashToUInt64(packet):x16}";
        var now = DateTime.UtcNow;
        lock (_gate)
        {
            if (_recentQueries.TryGetValue(key, out var at) && now - at < DuplicateWindow) return true;
            if (_recentQueries.Count >= 64)
            {
                foreach (var stale in _recentQueries.Where(kv => now - kv.Value >= DuplicateWindow).Select(kv => kv.Key).ToList())
                    _recentQueries.Remove(stale);
            }
            _recentQueries[key] = now;
            return false;
        }
    }

    /// <summary>
    /// 同一份记录？——必须比到 **rdata**（Type+Name+数据）。只比 Type+Name 会把同一主机的
    /// 多条 A 记录（多网卡多 IP）当成重复丢掉，实测就是这样漏掉了第二个地址。
    /// </summary>
    private static bool SameRecord(ResourceRecord a, ResourceRecord b)
        => a.Type == b.Type
           && Eq(a.Name.ToString(), b.Name.ToString())
           && Convert.ToHexString(a.GetData()) == Convert.ToHexString(b.GetData());

    /// <summary>传统单播应答形状：TTL 压到 10s 且清掉 cache-flush 位（RFC 6762 §6.7 / §10.2）。</summary>
    private static List<ResourceRecord> LegacyShape(IEnumerable<ResourceRecord> records)
    {
        var list = new List<ResourceRecord>();
        foreach (var r in records)
        {
            var clone = (ResourceRecord)r.Clone();
            clone.TTL = TimeSpan.FromSeconds(Math.Min(10, clone.TTL.TotalSeconds));
            clone.Class = DnsClass.IN;
            list.Add(clone);
        }
        return list;
    }

    // ── 公告 / 告别 / 发包 ─────────────────────────────────────────────────
    private void Announce(Socket sock, CancellationToken ct)
    {
        var adv = Advertisement;
        if (adv is null) return;
        var packet = BuildResponse(adv.ToRecords()).ToByteArray();
        for (int i = 0; i < AnnounceCount; i++)                    // RFC 6762 §8.3：间隔 1s 连发
        {
            if (ct.IsCancellationRequested) return;
            SendPacket(packet, null);
            if (i < AnnounceCount - 1)
            {
                try { Task.Delay(TimeSpan.FromSeconds(1), ct).GetAwaiter().GetResult(); }
                catch (OperationCanceledException) { return; }
            }
        }
    }

    private static Message BuildResponse(IEnumerable<ResourceRecord> records)
    {
        var msg = new Message { QR = true, AA = true, Opcode = MessageOperation.Query };
        msg.Answers.AddRange(records);
        return msg;
    }

    /// <summary>组播发包：逐已入组网卡指定出口各发一份（多网卡 NAS 上设备接哪块都能收到）。</summary>
    private void SendPacket(byte[] payload, IPEndPoint? unicastTo)
    {
        var sock = _socket;
        if (sock is null) return;
        var group = new IPEndPoint(MulticastGroup, MulticastPort);

        if (unicastTo is not null)                                  // 传统单播：只回源
        {
            try { sock.SendTo(payload, unicastTo); }
            catch (Exception ex) { _log.LogDebug("[mDNS] 单播应答失败：{Message}", ex.Message); }
            return;
        }

        List<IPAddress> joined;
        lock (_gate) joined = _joined.ToList();

        bool sent = false;
        foreach (var addr in joined)
        {
            try
            {
                // IP_MULTICAST_IF 取网络序 in_addr（.NET 的 byte[] 重载原样下发）
                sock.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastInterface, addr.GetAddressBytes());
                sock.SendTo(payload, group);
                sent = true;
            }
            catch (Exception ex) { _log.LogDebug("[mDNS] 组播发送失败（出口 {Address}）：{Message}", addr, ex.Message); }
        }
        if (!sent)   // 指定出口全失败（平台不支持该选项等）→ 退回系统默认路由发一次，聊胜于无
        {
            try { sock.SendTo(payload, group); }
            catch (Exception ex) { _log.LogDebug("[mDNS] 组播发送失败（默认路由）：{Message}", ex.Message); }
        }
    }

    // ── 停止 ────────────────────────────────────────────────────────────────
    /// <summary>DI 在宿主退出时调用：先发 goodbye（TTL=0，RFC 6762 §10.1）再关套接字。永不抛。</summary>
    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        try
        {
            var adv = Advertisement;
            if (IsRunning && adv is not null)
                SendPacket(BuildResponse(adv.ToRecords(goodBye: true)).ToByteArray(), null);
        }
        catch (Exception ex) { _log.LogDebug("[mDNS] goodbye 发送失败：{Message}", ex.Message); }

        try { _cts?.Cancel(); } catch { /* 关停路径不抛 */ }
        try { _socket?.Dispose(); } catch { /* 同上 */ }
        _socket = null;
        try { _cts?.Dispose(); } catch { /* 同上 */ }
        _cts = null;
        if (IsRunning) _log.LogInformation("[mDNS] 已停止广告（goodbye 已发送）");
        IsRunning = false;
    }
}

/// <summary>
/// mDNS 广告档案（纯数据，不碰网络）：一条 <c>_minipet._tcp</c> 服务实例 + 主机 A 记录。
/// 自测可直接断言 <see cref="ServiceInstanceName"/> / <see cref="Port"/> / <see cref="Addresses"/>，
/// 并 <see cref="ToRecords"/> 出真实 DNS 记录做逐条校验。
/// </summary>
public sealed class MdnsAdvertisement
{
    /// <summary>实例名（PTR 目标的第一个 label），如 "MiniPet-a502demacbook-pro"。</summary>
    public required string InstanceName { get; init; }
    /// <summary>服务类型（固定 "_minipet._tcp.local"，与固件 MP_MDNS_SERVICE/PROTO 对齐）。</summary>
    public required string ServiceType { get; init; }
    /// <summary>HTTP 对外端口（设备拼 http://&lt;ip&gt;:&lt;port&gt; 用它）。</summary>
    public required ushort Port { get; init; }
    /// <summary>SRV 的 target（host 名），如 "minipet-a502demacbook-pro.local"。</summary>
    public required string HostName { get; init; }
    /// <summary>A 记录地址（SRV target 必须能被这些记录解析）。</summary>
    public required IReadOnlyList<IPAddress> Addresses { get; init; }
    /// <summary>TXT 条目（ver=1 / path=/api/device / name=MiniPet）。</summary>
    public required IReadOnlyList<string> Txt { get; init; }

    /// <summary>"MiniPet-x._minipet._tcp.local"（固件 r-&gt;instance_name 看到的就是它）。</summary>
    public string ServiceInstanceName => $"{InstanceName}.{ServiceType}";

    /// <summary>
    /// 构造 DNS 记录集：PTR（服务类型 → 实例）、SRV（实例 → 主机:端口）、TXT、A（主机 → 地址）。
    /// <paramref name="goodBye"/>=true 时全部 TTL=0（告别包）；PTR 属共享记录，不设 cache-flush 位。
    /// </summary>
    public IReadOnlyList<ResourceRecord> ToRecords(bool goodBye = false, bool cacheFlush = true, TimeSpan? ttlOverride = null)
    {
        var svc = new DomainName(ServiceType);
        var inst = new DomainName(ServiceInstanceName);
        var host = new DomainName(HostName);

        var svcTtl = goodBye ? TimeSpan.Zero : ttlOverride ?? MdnsAdvertiser.ServiceTtl;
        var hostTtl = goodBye ? TimeSpan.Zero : ttlOverride ?? MdnsAdvertiser.HostTtl;
        // cache-flush 位 = DNS class 字段最高位（0x8001）：唯一记录（SRV/TXT/A）置位，共享记录（PTR）不置
        var flushClass = cacheFlush ? (DnsClass)((ushort)DnsClass.IN | 0x8000) : DnsClass.IN;

        return new ResourceRecord[]
        {
            new PTRRecord { Name = svc, DomainName = inst, TTL = svcTtl, Class = DnsClass.IN },
            new SRVRecord
            {
                Name = inst, Target = host, Port = Port, Priority = 0, Weight = 0,
                TTL = svcTtl, Class = flushClass,
            },
            new TXTRecord { Name = inst, Strings = new List<string>(Txt), TTL = svcTtl, Class = flushClass },
        }.Concat(Addresses.Select(a => (ResourceRecord)new ARecord
        {
            Name = host, Address = a, TTL = hostTtl, Class = flushClass,
        })).ToList();
    }
}
