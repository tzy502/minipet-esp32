# E14 服务端 mDNS 广告（`_minipet._tcp`）实施与自测报告

- 日期：2026-09-27
- 仓库：`/Users/<USER>/IdeaProjects/minipet-esp32`
- 执行：子 agent（模型 `deepseek-flash`，DSH 工具链）；dotnet 为可执行文件 `"/Volumes/SSD/C#/dotnet"`
- 改动范围：**仅 `Server/MinipetServer/`**（另加本报告 `docs/ai/`）。未碰 `Firmware/`、`Web/`、根目录 compose/.env/Dockerfile；未做任何 git commit/push/checkout/stash
- 需求：E14 最后一环 —— 固件侧 `mdns_discover.c` 早已能发现，服务端从未广告

## 结论摘要

| 项 | 结果 |
|---|---|
| Release 构建 | `"/Volumes/SSD/C#/dotnet" build Server/MinipetServer/MinipetServer.csproj -c Release` → **0 Error**（66 Warning 全部是既有文件的，本次新增/改动文件 0 warning） |
| 服务广告 | 起本机实例（`MINIPET_DATA_DIR=/tmp/mdns_test`，`ASPNETCORE_URLS=http://0.0.0.0:38210`）→ 自制 python 查询器收到 **PTR+SRV+TXT+A 单包应答**，端口/IP 正确（原始输出见 ③） |
| 独立第三方验证 | macOS `dns-sd`（系统 mDNSResponder，**另一套 mDNS 实现**）能 browse 到实例、lookup 到 `host:port` 与 TXT、解析到 A 记录 |
| 单测级断言 | 直连出货代码路径的 harness：21 条断言 **0 FAIL**（含 `ServiceInstanceName`/`Port`/`Addresses` 非空） |
| 降级 | 关开关 / 网卡不可用 → 服务端照常启动、HTTP 正常、只落一条 warning（原始输出见 ④） |
| 真机 | ⚠️ **未验证**（需 ESP32 在场，见 ⑤） |

---

## ① 改了什么

| 文件 | 变更 |
|---|---|
| `Server/MinipetServer/Services/MdnsAdvertiser.cs` | **新增**（697 行）：mDNS 响应器（组播收发 + 记录构造 + 降级 + `IDisposable`），含纯数据档案类型 `MdnsAdvertisement` |
| `Server/MinipetServer/Config/ConfigService.cs` | +29 行：新增 `MdnsConfig`（`Enabled`/`Port`/`Interface`）并挂进 `MinipetConfig`；`Normalize` 补默认与端口夹取；`Replace` 对老前端回传做了保留 |
| `Server/MinipetServer/Program.cs` | +9 行：DI 单例注册 + `ApplicationStarted` 回调启动 + `/api/health` 暴露 `mdns` 状态 |
| `Server/MinipetServer/MinipetServer.csproj` | **未改**（`Makaretu.Dns 2.0.1` 是原有引用；本方案**零新增依赖**） |

### 1.1 契约对齐（以固件为准）

`Services/MdnsAdvertiser.cs` 顶部与常量逐字对齐 `Firmware/main/net/mdns_discover.h/.c`：

```csharp
public const string Service = "_minipet";        // 固件 MP_MDNS_SERVICE
public const string Proto   = "_tcp";            // 固件 MP_MDNS_PROTO
public const string ServiceType = Service + "." + Proto + ".local";  // 固件 mdns_query_ptr 查的名字
// TXT：ver=1（固件 MP_PROTO_VER）、path=/api/device、name=MiniPet
```

### 1.2 为什么**没有**用 `ServiceDiscovery.Advertise(ServiceProfile)`

任务书给的候选方案是 Makaretu 的 `ServiceDiscovery.Advertise(ServiceProfile)`，实测不可行（**跑过的证据**，不是推断）：

1. 反射扫描已引用的 `Makaretu.Dns 2.0.1` 全部 70 个导出类型 → **没有** `ServiceDiscovery` / `ServiceProfile`（该包只是 DNS 对象模型）；
2. 这两个类型在另一个包 `Makaretu.Dns.Multicast` 里，而它**停在 0.27.0**（NuGet 实测：`Found 39 version(s) … Nearest version: 0.27.0`）；
3. `Makaretu.Dns.Multicast 0.27.0` 传递依赖 `Tmds.LibC 0.2.0`（网卡枚举的原生 libc 绑定），其 runtimes 只有 **linux-arm / linux-arm64 / linux-x64，没有 macOS/Windows 资产** → 本机（macOS）自测直接跑不起来，NAS 镜像也多一份原生依赖风险。

所以：**只借用已引用的 `Makaretu.Dns` 做报文编解码**（`Message` / `PTRRecord` / `SRVRecord` / `TXTRecord` / `ARecord`，含 DNS 名字压缩），组播收发用 `Socket` 自己实现（约 200 行）。收益：零新依赖、应答内容完全可控（能把 PTR+SRV+TXT+A 塞进**同一个应答包**，这正是固件 `first_ipv4(r->addr)` 最省事的情况）。

### 1.3 关键代码

记录集（`MdnsAdvertisement.ToRecords`）——PTR 是共享记录不置 cache-flush，SRV/TXT/A 是唯一记录置位；TTL 按 RFC 6762 §10（服务 4500s / 主机 120s）：

```csharp
new PTRRecord { Name = svc,  DomainName = inst, TTL = svcTtl, Class = DnsClass.IN },
new SRVRecord { Name = inst, Target = host, Port = Port, Priority = 0, Weight = 0, TTL = svcTtl, Class = flushClass },
new TXTRecord { Name = inst, Strings = new List<string>(Txt), TTL = svcTtl, Class = flushClass },
// 每条 Addresses 一条 ARecord（宿主主机名必须能被 A 解析 —— 固件的「A 补查」路径靠它）
```

应答匹配（`MatchRecords`）——**PTR 查询一次给全四类记录**，另外单独应答 hostname 的 A 查询（覆盖固件第二条路径）：

```csharp
if (Eq(name, adv.ServiceType))   return any || type == DnsType.PTR ? all : empty;   // PTR+SRV+TXT+A
if (Eq(name, adv.ServiceEnumeration)) …                                              // _services._dns-sd._udp.local（dns-sd 排障）
if (Eq(name, adv.ServiceInstanceName)) return … SRV|TXT|ANY → SRV+TXT+A;
if (Eq(name, adv.HostName))            return … A|ANY       → A 记录;
```

其它按 RFC 6762 的细节：公告 3 次 ×1s（§8.3）、退出时 goodbye（TTL=0，§10.1）、传统单播查询（源端口≠5353）单播回源且 TTL≤10s 不带 flush（§6.7）、组播 TTL=255（§11）、重复查询 1s 抑制（§7.4）。

### 1.4 注册与启动（`Program.cs`）

```csharp
builder.Services.AddSingleton<MdnsAdvertiser>();   // IDisposable：宿主退出 → DI 调 Dispose → 发 goodbye
...
var mdns = app.Services.GetRequiredService<MdnsAdvertiser>();
app.Lifetime.ApplicationStarted.Register(() => mdns.Start());   // 监听端口确定后才开播；不阻塞启动
...
app.MapGet("/api/health", … mdns = mdns.StatusText …);          // advertising … / disabled / degraded: 原因
```

刻意**不用 hosted service**（照 `SpeechScheduler`/`QqGatewayProcess` 的单例风格）：构造只做依赖装配，开播挂 `ApplicationStarted`，`Start()` 内部全量 try/catch，**永不抛**。

### 1.5 配置（默认开，老文件缺字段=开）

```csharp
public sealed class MdnsConfig {
    public bool   Enabled   { get; set; } = true;   // 缺字段时 STJ 保留初始值 → 老配置默认开
    public int    Port      { get; set; }           // 0 = 自动（见 ②）
    public string Interface { get; set; } = "";     // 空 = 自动（优先有默认网关的网卡）
}
```

`Replace()` 里 `if (incoming.Mdns is not null) c.Mdns = incoming.Mdns;` —— Web 全量回传若不带该段（老前端），保留现行值，不会把开关/端口重置成默认。首次启动落种的配置文件实测已含 `"Mdns": { "Enabled": true, "Port": 0, "Interface": "" }`。

---

## ② 端口口径的判断依据

**证据链（全部只读仓库文件）**：

| 来源 | 内容 |
|---|---|
| `.env.example` | `MINIPET_PORT=38090`，注释「**对外访问端口**…ESP32 配网页填 `http://<NAS_IP>:此端口`」 |
| `docker-compose.example.yml` | `ports: - "${MINIPET_PORT:-38090}:8080"`（左对外 / 右容器内） |
| `Dockerfile` | `aspnet:9.0` 基础镜像 `ASPNETCORE_HTTP_PORTS=8080`、`EXPOSE 8080`、`ENTRYPOINT dotnet MinipetServer.dll` |
| `Server/MinipetServer/appsettings.json` | 「端口属部署层 .env（MINIPET_PORT，默认 38090），**不入 appsettings**」 |
| `docs/ai/keys-touch-handoff.md` §7.1 | 「广告 `_minipet._tcp`，端口 = 服务端 HTTP 监听端口（默认 **38090**）」；并明确容器需 `network_mode: host` |

**判断**：设备是**从局域网连**，所以广告端口必须是「对外可达端口」，而不是容器内的 8080。实现按优先级取（`MdnsAdvertiser.ResolvePort`，纯函数、可单测）：

1. 配置 `Mdns.Port`（>0，人工钉死；容器里最稳的写法）
2. 环境变量 `MINIPET_PORT`（部署层定义的对外端口；若部署把它传进容器，这就是正确答案）
3. 实际监听端口（`IServerAddressesFeature.Addresses`，`ApplicationStarted` 后取；本地 `dotnet run` 的真相）
4. `ASPNETCORE_URLS` / `ASPNETCORE_HTTP_PORTS` / `ASPNETCORE_HTTPS_PORTS` 兜底解析
5. `38090`（与固件 `MDNS_DEFAULT_PORT` 同值）

选 2 高于 3 的理由：容器里监听 8080、对外 38090 时，用监听端口会广告成设备连不上的 8080。**代码里已写清该依据**（类头「端口口径」段 + `ResolvePort` XML 注释）。

**两个如实记录的部署事实**（本次**未改**，超出改动范围，但会影响线上）：
1. `docker-compose.example.yml` 目前**没有把 `MINIPET_PORT` 传进容器 `environment`**（它只用于端口映射）→ 容器内读不到 2，只能落到 3/4 = 8080。要让容器广告对外端口，需在 compose 的 `environment` 加 `- MINIPET_PORT=${MINIPET_PORT:-38090}`，或在 `data/config/appsettings.json` 写 `"Mdns": { "Port": 38090 }`。
2. Docker **bridge 网络下组播出不了容器、A 记录也会是容器内网 IP** → 必须 `network_mode: host`。为此代码在「广告端口 ≠ 实际监听端口」时会额外打一条 warning 点名这件事（实测见 ④ 场景 1）。

---

## ③ mDNS 实测（原始输出）

### 3.1 启动（本机实例，38210，避开被占的 38090/38095/38096/38097/38099/5059）

```bash
cd /Users/<USER>/IdeaProjects/minipet-esp32
MINIPET_DATA_DIR=/tmp/mdns_test ASPNETCORE_URLS=http://0.0.0.0:38210 \
  "/Volumes/SSD/C#/dotnet" run --project Server/MinipetServer/MinipetServer.csproj \
  -c Release --no-build --no-launch-profile
```

服务端日志（原始）：

```
      Now listening on: http://0.0.0.0:38210
      Application started. Press Ctrl+C to shut down.
      [mDNS] 已广告 _minipet._tcp.local instance=MiniPet-502demac-mini._minipet._tcp.local host=minipet-502demac-mini.local port=38210（端口来源：实际监听端口（http://0.0.0.0:38210））地址=[<DEV_PC_IP>, <LAN_IP>] 网卡=[<DEV_PC_IP>, <LAN_IP>]
```

`curl http://127.0.0.1:38210/api/health`（原始）：

```json
{"ok":true,"service":"minipet-server","proto":1,"timeUtc":"2026-09-27T07:10:05.018905Z","wzPathExists":false,"wzDataPath":"/wz/Data","devices":0,"qqEnabled":false,"mdns":"advertising MiniPet-502demac-mini._minipet._tcp.local port=38210 host=minipet-502demac-mini.local (实际监听端口（http://0.0.0.0:38210）)"}
```

### 3.2 python 最小 mDNS 查询器（自写，只用标准库）

脚本落在 `/tmp/mdns_query.py`（临时文件，已清理；核心逻辑如下，可直接复跑）。它模拟固件两条路径：① `_minipet._tcp.local` PTR 查询；② 若①没有 A，按 SRV target 补查 A。两种源端口各跑一遍（5353 = 固件形态；随机 = 传统单播查询）。

```python
import socket, struct, time
GROUP, PORT = "224.0.0.251", 5353
def enc_name(n):
    out = b""
    for l in n.rstrip(".").split("."):
        if l: out += bytes([len(l)]) + l.encode()
    return out + b"\x00"
def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(("8.8.8.8", 80))
    return s.getsockname()[0]
def make_sock(bind_port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try: s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except OSError: pass
    s.bind(("", bind_port))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 struct.pack("4s4s", socket.inet_aton(GROUP), socket.inet_aton(local_ip())))
    s.settimeout(2.0); return s
def query(name, qtype, bind_port):
    s = make_sock(bind_port)
    pkt = struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0) + enc_name(name) + struct.pack(">HH", qtype, 1)
    print("  → 发送查询", name, "type", qtype, "源端口", s.getsockname()[1], "报文", pkt.hex())
    s.sendto(pkt, (GROUP, PORT))
    while True:                       # 再用 parse()（名字压缩/各类型 rdata 解析）逐条打印
        try: data, addr = s.recvfrom(9000)
        except socket.timeout: break
        print("  ← 收到", len(data), "B from", addr)
        for r in parse(data): print("      ", r)
    s.close()
query("_minipet._tcp.local", 12, 5353)       # ① PTR（固件 mdns_query_ptr）
query("minipet-<host>.local", 1, 5353)       # ② A 补查（固件 mdns_query_a）
```

> 完整脚本还带 DNS 报文解析（名字压缩指针、PTR/SRV/TXT/A rdata 解码）与两种源端口模式，输出见下。

### 3.3 查询原始输出（逐字粘贴，未删改）

```
本机 IPv4（默认路由网卡）= <DEV_PC_IP>

=== PTR 查询（源端口 5353（固件形态 → 期望服务端组播应答））===
  → 发送查询 _minipet._tcp.local type=PTR 源端口=5353 目标=224.0.0.251:5353 报文=000000000001000000000000085f6d696e69706574045f746370056c6f63616c00000c0001
  ← 收到 37B from <DEV_PC_IP>:5353 flags=0x0000 QR=0 AA=0 records=0
  ← 收到 195B from <DEV_PC_IP>:5353 flags=0x8400 QR=1 AA=1 records=5
       [ANSWER] _minipet._tcp.local PTR ttl=4500 flush=False → MiniPet-502demac-mini._minipet._tcp.local   rdata=154d696e695065742d35303264656d61632d6d696e69c00c
       [ANSWER] MiniPet-502demac-mini._minipet._tcp.local SRV ttl=4500 flush=True → pri=0 weight=0 port=38210 target=minipet-502demac-mini.local   rdata=000000009542156d696e697065742d35303264656d61632d6d696e69c01a
       [ANSWER] MiniPet-502demac-mini._minipet._tcp.local TXT ttl=4500 flush=True → ver=1 / path=/api/device / name=MiniPet   rdata=057665723d3110706174683d2f6170692f6465766963650c6e616d653d4d696e69506574
       [ANSWER] minipet-502demac-mini.local A ttl=120 flush=True → <DEV_PC_IP>   rdata=c0a80360
       [ANSWER] minipet-502demac-mini.local A ttl=120 flush=True → <LAN_IP>   rdata=c0a8036c
  ← 收到 195B from <LAN_IP>:5353 flags=0x8400 QR=1 AA=1 records=5
       [ANSWER] _minipet._tcp.local PTR ttl=4500 flush=False → MiniPet-502demac-mini._minipet._tcp.local   rdata=154d696e695065742d35303264656d61632d6d696e69c00c
       [ANSWER] MiniPet-502demac-mini._minipet._tcp.local SRV ttl=4500 flush=True → pri=0 weight=0 port=38210 target=minipet-502demac-mini.local   rdata=000000009542156d696e697065742d35303264656d61632d6d696e69c01a
       [ANSWER] MiniPet-502demac-mini._minipet._tcp.local TXT ttl=4500 flush=True → ver=1 / path=/api/device / name=MiniPet   rdata=057665723d3110706174683d2f6170692f6465766963650c6e616d653d4d696e69506574
       [ANSWER] minipet-502demac-mini.local A ttl=120 flush=True → <DEV_PC_IP>   rdata=c0a80360
       [ANSWER] minipet-502demac-mini.local A ttl=120 flush=True → <LAN_IP>   rdata=c0a8036c

=== 按 SRV target 补查 A（模拟固件 mdns_query_a，源端口 5353（固件形态 → 期望服务端组播应答））===
  → 发送查询 minipet-502demac-mini.local type=A 源端口=5353 目标=224.0.0.251:5353 报文=000000000001000000000000156d696e697065742d35303264656d61632d6d696e69056c6f63616c0000010001
  ← 收到 45B from <DEV_PC_IP>:5353 flags=0x0000 QR=0 AA=0 records=0
  ← 收到 77B from <DEV_PC_IP>:5353 flags=0x8400 QR=1 AA=1 records=2
       [ANSWER] minipet-502demac-mini.local A ttl=120 flush=True → <DEV_PC_IP>   rdata=c0a80360
       [ANSWER] minipet-502demac-mini.local A ttl=120 flush=True → <LAN_IP>   rdata=c0a8036c
  ← 收到 77B from <LAN_IP>:5353 flags=0x8400 QR=1 AA=1 records=2
       [ANSWER] minipet-502demac-mini.local A ttl=120 flush=True → <DEV_PC_IP>   rdata=c0a80360
       [ANSWER] minipet-502demac-mini.local A ttl=120 flush=True → <LAN_IP>   rdata=c0a8036c

=== PTR 查询（源端口随机（传统单播查询 → 期望服务端单播回源））===
  → 发送查询 _minipet._tcp.local type=PTR 源端口=54198 目标=224.0.0.251:5353 报文=000000000001000000000000085f6d696e69706574045f746370056c6f63616c00000c0001
  ← 收到 195B from <DEV_PC_IP>:5353 flags=0x8400 QR=1 AA=1 records=5
       [ANSWER] _minipet._tcp.local PTR ttl=10 flush=False → MiniPet-502demac-mini._minipet._tcp.local   rdata=154d696e695065742d35303264656d61632d6d696e69c00c
       [ANSWER] MiniPet-502demac-mini._minipet._tcp.local SRV ttl=10 flush=False → pri=0 weight=0 port=38210 target=minipet-502demac-mini.local   rdata=000000009542156d696e697065742d35303264656d61632d6d696e69c01a
       [ANSWER] MiniPet-502demac-mini._minipet._tcp.local TXT ttl=10 flush=False → ver=1 / path=/api/device / name=MiniPet   rdata=057665723d3110706174683d2f6170692f6465766963650c6e616d653d4d696e69506574
       [ANSWER] minipet-502demac-mini.local A ttl=10 flush=False → <DEV_PC_IP>   rdata=c0a80360
       [ANSWER] minipet-502demac-mini.local A ttl=10 flush=False → <LAN_IP>   rdata=c0a8036c

=== 按 SRV target 补查 A（模拟固件 mdns_query_a，源端口随机（传统单播查询 → 期望服务端单播回源））===
  → 发送查询 minipet-502demac-mini.local type=A 源端口=63003 目标=224.0.0.251:5353 报文=000000000001000000000000156d696e697065742d35303264656d61632d6d696e69056c6f63616c0000010001
  ← 收到 77B from <DEV_PC_IP>:5353 flags=0x8400 QR=1 AA=1 records=2
       [ANSWER] minipet-502demac-mini.local A ttl=10 flush=False → <DEV_PC_IP>   rdata=c0a80360
       [ANSWER] minipet-502demac-mini.local A ttl=10 flush=False → <LAN_IP>   rdata=c0a8036c

=== 结论 ===
PTR=3 SRV=3 TXT=3 A=12
  SRV: MiniPet-502demac-mini._minipet._tcp.local → pri=0 weight=0 port=38210 target=minipet-502demac-mini.local
  SRV: MiniPet-502demac-mini._minipet._tcp.local → pri=0 weight=0 port=38210 target=minipet-502demac-mini.local
  SRV: MiniPet-502demac-mini._minipet._tcp.local → pri=0 weight=0 port=38210 target=minipet-502demac-mini.local
  A  : minipet-502demac-mini.local → <DEV_PC_IP>
  A  : minipet-502demac-mini.local → <LAN_IP>
  …（A 记录按每次应答重复计）
服务端广告验证：PASS（PTR+SRV+A 齐备，端口可解析）
```

**读法**：① 一次 PTR 查询就拿到 PTR+SRV+TXT+**A**（固件第一条路径即可拼出 `http://<DEV_PC_IP>:38210`）；② A 补查路径也通（77B 单包，含两条 A）；③ 传统单播查询按 RFC 6762 §6.7 单播回源、TTL=10、无 flush 位；④ 同一应答从 en0(<DEV_PC_IP>)/en1(<LAN_IP>) 各发一份 = 多网卡各一份，不是重复放大（去重前后实测：修复前 4~6 份/查询 → 修复后 2 份/查询）。

### 3.4 独立第三方验证：macOS `dns-sd`（系统 mDNSResponder，非本次代码）

```
$ dns-sd -B _minipet._tcp local
Browsing for _minipet._tcp.local
Timestamp     A/R    Flags  if Domain               Service Type         Instance Name
15:12:25.480  Add        3  15 local.               _minipet._tcp.       MiniPet-502demac-mini
15:12:25.480  Add        2   7 local.               _minipet._tcp.       MiniPet-502demac-mini

$ dns-sd -L MiniPet-502demac-mini _minipet._tcp local
15:12:33.968  MiniPet-502demac-mini._minipet._tcp.local. can be reached at minipet-502demac-mini.local.:38210 (interface 7) Flags: 1
 ver=1 path=/api/device name=MiniPet

$ dns-sd -G v4 minipet-502demac-mini.local
Timestamp     A/R  Flags         IF  Hostname                     Address         TTL
15:12:39.986  Add  40000003      15  minipet-502demac-mini.local. <DEV_PC_IP>   120
15:12:39.986  Add  40000003      15  minipet-502demac-mini.local. <LAN_IP>  120
```

意义：这**不是我的代码**在自证 —— 系统的第二套 mDNS 实现能完整解析出「实例名 + 主机:端口 + TXT + A」，说明报文字段合规（名字压缩、cache-flush 位、TTL 分类都被独立实现接受了）。

### 3.5 单测级断言（直连出货代码路径；21 条断言 0 FAIL）

用临时 console 工程 `ProjectReference` 到 `MinipetServer.csproj`，调 `MdnsAdvertiser.Start()` 后用真实广告档案断言（节选原始输出）：

```
=== 1. 端口口径 ResolvePort（纯函数）===
  [PASS] 实际监听端口优先（无环境变量）  → 38210 / 实际监听端口（http://0.0.0.0:38210）
  [PASS] MINIPET_PORT 压过监听端口  → 38090 / 环境变量 MINIPET_PORT（部署层对外端口）
  [PASS] 配置 Mdns.Port 优先级最高  → 39001 / 配置 Mdns.Port
  [PASS] ASPNETCORE_HTTP_PORTS 兜底（容器内 8080）  → 8080 / 环境变量 ASPNETCORE_HTTP_PORTS
  [PASS] 全空 → 38090（与固件缺省同值）  → 38090 / 缺省值（38090）
=== 3. 网卡选择 ===
  [PASS] 枚举到可用网卡  → en0=<DEV_PC_IP>(gw=True), en1=<LAN_IP>(gw=True)
=== 4. 走真实代码路径构造广告（Start + Advertisement 断言）===
  [PASS] 配置默认 Enabled=true（老配置文件缺字段也是 true）
  [PASS] ServiceInstanceName 非空且形如 <inst>._minipet._tcp.local  → MiniPet-502demac-mini._minipet._tcp.local
  [PASS] Port 非空且在合法区间  → 38090
  [PASS] Addresses 非空  → <DEV_PC_IP>, <LAN_IP>
  [PASS] PTR + SRV + TXT + A 四类记录齐备
  [PASS] SRV.Target 有对应 A 记录 / TXT 含 ver=1|path=/api/device|name=MiniPet / goodbye 全部 TTL=0
全部断言通过（0 FAIL）
```

---

## ④ 降级验证（原始输出，逐场景重启实测）

驱动脚本：对每个场景改 `data/config/appsettings.json` 的 `Mdns` 段（或给环境变量）后**重启**实例，收集日志 + `/api/health` + 真实 mDNS 查询。**只杀本测试的 `bin/Release/net9.0/MinipetServer.dll` 进程**，用户常驻实例（`Debug`/`:5059`）全程未动。

```
【minipet_port】端口口径①：环境变量 MINIPET_PORT=38090（模拟容器「内 8080 / 外 38090」）
  环境：MINIPET_PORT=38090 + 监听 38210
      Now listening on: http://0.0.0.0:38210
      [mDNS] 已广告 … port=38090（端口来源：环境变量 MINIPET_PORT（部署层对外端口））
      [mDNS] 广告端口 38090 与容器内监听端口不一致：若本服务跑在 Docker bridge 网络，组播与 A 记录都到不了局域网，需改用 network_mode: host（或让监听端口=对外端口）
  --- /api/health ---
  ok=True  mdns=advertising … port=38090 … (环境变量 MINIPET_PORT（部署层对外端口）)
  --- mDNS 实测（广告端口应为 38090）---
       [ANSWER] … SRV ttl=4500 flush=True → pri=0 weight=0 port=38090 target=minipet-502demac-mini.local
  服务端广告验证：PASS（PTR+SRV+A 齐备，端口可解析）

【cfg_port】端口口径②：配置 Mdns.Port=39001 优先于环境变量 MINIPET_PORT=38090
      [mDNS] 已广告 … port=39001（端口来源：配置 Mdns.Port）
  ok=True  mdns=advertising … port=39001 … (配置 Mdns.Port)
  --- mDNS 实测 ---  SRV … port=39001 …   服务端广告验证：PASS

【disabled】降级①：配置关闭 mDNS（Enabled=false）——服务端照常起，且不再广告
      Now listening on: http://0.0.0.0:38210
      [mDNS] 已按配置关闭（Mdns.Enabled=false）：设备将只能靠配网页手输地址
  ok=True  mdns=disabled
  --- mDNS 查询（应完全无应答）---
    ← 收到 37B from <DEV_PC_IP>:5353 flags=0x0000 QR=0 AA=0 records=0     （只有本机查询回声，无人应答）
    !! 未收到任何应答

【bad_iface】降级②：Mdns.Interface 指向本机不存在的 IPv4（走与「绑定失败」同一条 Degrade 分支）
      Now listening on: http://0.0.0.0:38210
      [mDNS] 未启用广告（不影响服务端启动）：配置的网卡地址不可用：Mdns.Interface=203.0.113.7（本机没有该 IPv4）
  ok=True  mdns=degraded: 配置的网卡地址不可用：Mdns.Interface=203.0.113.7（本机没有该 IPv4）
    !! 未收到任何应答

【coexist】对照：UDP 5353 已被系统 mDNSResponder/QQBrowser 占用时，本服务仍能共存并成功广告
      [mDNS] 已广告 … port=38210（端口来源：实际监听端口（http://0.0.0.0:38210））
  ok=True  mdns=advertising …
```

**关于「端口占用」的如实说明**：本机 **5353 早就被别人占着**（`lsof -iUDP:5353` → `QQBrowser`，另有系统 mDNSResponder），而本服务用 `SO_REUSEADDR + SO_REUSEPORT` **绑定成功并正常广告**（coexist 场景）——这是**比降级更强的结论**。但因此**无法在本机构造「5353 被独占」**：连 python 刻意不设 reuse 的独占绑定都直接 `OSError: [Errno 48] Address already in use`（实测）。所以「绑定失败」分支我**没有在本机真实触发过**，它由同一 `Degrade()` 代码路径的 `bad_iface` 场景覆盖（同样是 catch → 一条 warning → 服务端照常起、无广告）。该分支在 Linux 容器（无组播/权限受限/独占占用）下才会真实出现。

另外实测确认的关闭路径：`SIGTERM` → 宿主优雅退出 → `[mDNS] 已停止广告（goodbye 已发送）`（`IDisposable` + RFC 6762 §10.1 告别包生效）。

---

## ⑤ 未做 / 待验证项

**未做（明确不做，附理由）**

- 未做 IPv6/AAAA（固件 `first_ipv4()` 只认 IPv4；纯 IPv6 网络不在 E14 范围）
- 未做探测/冲突解决（RFC 6762 §8.1 probing、§9 conflict resolution）：主机名刻意用 `minipet-<host>.local` 独占名字（不占用系统守护已广告的 `<host>.local`），冲突概率极低；真出现冲突时表现为设备连不上而回落手输地址（不比现状差）。若日后要跑多实例同机同名，需要补
- 未做已知答案抑制（KA，§7.1）：设备只在启动/兜底时查一次，流量可忽略
- 未做 mDNS 配置**热重载**：`Mdns.*` 只在 `Start()` 读一次（`ApplicationStarted`），改开关/端口需**重启服务端**（Web 设置页改了不会立即生效）
- 未跑 `docker build`（本机未验证镜像构建；改动未动 csproj/依赖，Dockerfile 阶段不受影响——**这条属只读推断**）

**待真机/现场验证（需设备在场或 NAS 环境）**

1. **ESP32 真机 + 配网页服务器地址留空 → 上电应自动发现**（E14 验收的核心场景）：看设备串口是否出现 `I (xxxx) mdns: 发现服务端: http://<ip>:<port> instance=... host=...`，以及随后 `hello` 成功
2. **真机第二条路径**：手输错地址 → hello 失败 → 兜底 mDNS 发现一次
3. **NAS + Docker 现场**：需 `network_mode: host`，并把对外端口传进容器（见 ② 的两条部署事实）；bridge 网络下预期**发现失败**（组播出不去），本报告只做了代码侧 warning，无现场验证
4. 真机与本服务端**同一二层广播域**、UDP 5353 未被 AP/防火墙隔离（`docs/ai/keys-touch-handoff.md` §7.1 前提）

---

## 附：复现命令

```bash
# 构建
"/Volumes/SSD/C#/dotnet" build Server/MinipetServer/MinipetServer.csproj -c Release

# 起本机实例（38210；避开 38090/38095/38096/38097/38099）
cd /Users/<USER>/IdeaProjects/minipet-esp32
MINIPET_DATA_DIR=/tmp/mdns_test ASPNETCORE_URLS=http://0.0.0.0:38210 \
  "/Volumes/SSD/C#/dotnet" run --project Server/MinipetServer/MinipetServer.csproj \
  -c Release --no-build --no-launch-profile      # 注意 --no-launch-profile，否则 launchSettings 会抢 5059

# 查询（python 脚本见 3.2；两种源端口各跑一遍）
python3 /tmp/mdns_query.py --both

# 独立实现交叉验证（macOS 自带）
dns-sd -B _minipet._tcp local
dns-sd -L MiniPet-<host> _minipet._tcp local
```

## 附：「实际跑过」vs「只读代码/文档推断」

| 结论 | 性质 |
|---|---|
| Release 构建 0 error；新增文件 0 warning | ✅ 实跑 |
| 广告报文 PTR/SRV/TXT/A、端口、A 记录、单播/组播两种应答 | ✅ 实跑（python 原始输出） |
| 报文被独立 mDNS 实现（mDNSResponder/dns-sd）接受 | ✅ 实跑 |
| 端口优先级 5 条分支、默认 Enabled=true、记录集内容、goodbye TTL=0 | ✅ 实跑（单测 harness） |
| 关开关 / 网卡不可用 时不阻断启动 | ✅ 实跑 |
| 5353 被占用时能共存（本机 QQBrowser/mDNSResponder 占着） | ✅ 实跑 |
| 「5353 被独占 → 绑定失败」分支 | ⚠️ **未能在本机触发**（macOS 上无法独占绑定 5353）；由同路径的网卡失败场景覆盖 |
| `Makaretu.Dns 2.0.1` 无 ServiceDiscovery；`Makaretu.Dns.Multicast` 停在 0.27.0 且缺 macOS 原生资产 | ✅ 实跑（反射 + NuGet 还原） |
| 容器 bridge 收不到组播、compose 未传 MINIPET_PORT | 📖 只读代码/文档推断（未起 Docker 验证） |
| 真机设备发现 | ❌ 未做（无设备） |
