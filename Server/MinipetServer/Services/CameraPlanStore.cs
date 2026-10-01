using System.IO;
using System.Text.Json;
using MinipetServer.Config;
using MinipetServer.Device;

namespace MinipetServer.Services;

/// <summary>
/// 相机机位（"选镜头"）记录：data/camera-positions.json，per 设备 + per 地图 存 (x,y)。
///
/// 为什么落在服务端而不是只存设备：用户口径「服务端是主口径，本地卡只是辅助」——
/// 设备用 NVS 记住最后一次机位只是为了断网时能用；真正的"我选过哪里"必须能在
/// Web 重新打开时回填（换机/清 NVS/重刷固件都不该丢），所以这里才是唯一事实源。
///
/// 幂等：同 (deviceId, mapId) 反复写只更新数值与时间戳；文件整份原子替换
/// （StorageUtil.AtomicWriteAllText，temp+Move），读侧永远看不到半截 JSON。
/// 容错：文件损坏/字段缺失按空处理（不抛给端点）——机位记录丢了只是要重选一次，
/// 不该让整个「选镜头」页 500。
/// </summary>
public sealed class CameraPlanStore
{
    /// <summary>一条机位记录。Vw/Vh 记下写入时的整图尺寸：地图被重推/换口径后
    /// 尺寸会变，旧坐标可能越界——调用方据此判断"这条记录是否还适用于当前这张图"。</summary>
    public sealed class Position
    {
        public int X { get; set; }
        public int Y { get; set; }
        public int Vw { get; set; }
        public int Vh { get; set; }
        public DateTime UpdatedUtc { get; set; }
    }

    /// <summary>单设备的机位表 + 该设备最后操作过的地图（Web 重新打开时优先选中它）。</summary>
    public sealed class DevicePlan
    {
        public string? LastMapId { get; set; }
        public DateTime? LastUpdatedUtc { get; set; }
        public Dictionary<string, Position> Positions { get; set; } = new(StringComparer.Ordinal);
    }

    private sealed class FileModel
    {
        public string Note { get; set; } =
            "Web「选镜头」机位记录：devices[deviceId].positions[mapId] = {x,y}（整图世界系，0 = 地图 bbox 左上角）。"
            + "服务端为主口径，设备 NVS 只是断网辅助。";
        public Dictionary<string, DevicePlan> Devices { get; set; } = new(StringComparer.Ordinal);
    }

    private readonly ServerPaths _paths;
    private readonly object _gate = new();
    private FileModel _model;

    /// <summary>落盘 JSON 选项：在仓库统一口径上加"中文不转义"——这份文件是给人看的
    /// （用户明确要"可读回、能手工核对"），同 DeviceAssetService 写 manifest 的做法。</summary>
    private static readonly JsonSerializerOptions FileJsonOpts = new(StorageUtil.JsonOpts)
    {
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public CameraPlanStore(ServerPaths paths)
    {
        _paths = paths ?? throw new ArgumentNullException(nameof(paths));
        _model = Load();
    }

    private FileModel Load()
    {
        try
        {
            var loaded = StorageUtil.ReadJson<FileModel>(_paths.CameraPositionsFile);
            if (loaded != null)
            {
                loaded.Devices ??= new Dictionary<string, DevicePlan>(StringComparer.Ordinal);
                foreach (var plan in loaded.Devices.Values)
                    plan.Positions ??= new Dictionary<string, Position>(StringComparer.Ordinal);
                return loaded;
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[CameraPlan] {_paths.CameraPositionsFile} 解析失败，按空表处理: {ex.Message}");
        }
        return new FileModel();
    }

    /// <summary>整张机位表快照（deviceId → mapId → 坐标）——Web 打开页面时一次拉回填。</summary>
    public Dictionary<string, Dictionary<string, Position>> All()
    {
        lock (_gate)
        {
            return _model.Devices.ToDictionary(
                kv => kv.Key,
                kv => kv.Value.Positions.ToDictionary(p => p.Key, p => Clone(p.Value), StringComparer.Ordinal),
                StringComparer.Ordinal);
        }
    }

    /// <summary>某设备全部机位 + 最后操作过的地图。</summary>
    public (string? lastMapId, Dictionary<string, Position> positions) Get(string deviceId)
    {
        lock (_gate)
        {
            if (!_model.Devices.TryGetValue(deviceId, out var plan))
                return (null, new Dictionary<string, Position>(StringComparer.Ordinal));
            return (plan.LastMapId,
                plan.Positions.ToDictionary(p => p.Key, p => Clone(p.Value), StringComparer.Ordinal));
        }
    }

    /// <summary>单条机位；没有记录返回 null（Web 据此显示"未记录"而不是 0,0）。</summary>
    public Position? Get(string deviceId, string mapId)
    {
        lock (_gate)
        {
            if (!_model.Devices.TryGetValue(deviceId, out var plan)) return null;
            return plan.Positions.TryGetValue(mapId, out var pos) ? Clone(pos) : null;
        }
    }

    /// <summary>
    /// 写入一条机位（幂等 upsert）并记 LastMapId。返回落盘后的记录副本；
    /// 写盘失败抛异常（端点转 500）——静默失败会让用户以为存下了。
    /// </summary>
    public Position Save(string deviceId, string mapId, int x, int y, int vw, int vh)
    {
        Position saved;
        lock (_gate)
        {
            if (!_model.Devices.TryGetValue(deviceId, out var plan))
            {
                plan = new DevicePlan();
                _model.Devices[deviceId] = plan;
            }
            saved = new Position
            {
                X = x, Y = y, Vw = vw, Vh = vh,
                UpdatedUtc = DateTime.UtcNow,
            };
            plan.Positions[mapId] = saved;
            plan.LastMapId = mapId;
            plan.LastUpdatedUtc = saved.UpdatedUtc;
            StorageUtil.AtomicWriteAllText(_paths.CameraPositionsFile,
                JsonSerializer.Serialize(_model, FileJsonOpts));
        }
        return Clone(saved);
    }

    private static Position Clone(Position p) => new()
    {
        X = p.X, Y = p.Y, Vw = p.Vw, Vh = p.Vh, UpdatedUtc = p.UpdatedUtc,
    };
}
