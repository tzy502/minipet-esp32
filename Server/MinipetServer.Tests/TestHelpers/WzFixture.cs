using MinipetServer.Services;
using Xunit;

namespace MinipetServer.Tests.TestHelpers;

/// <summary>
/// WZ 测试夹具：WzService 进程内单例（WZ 目录加载一次全程复用，模仿 M1 测试的共享初始化模式）。
/// 数据路径：env <c>WZ_DATA_PATH</c>，缺省 /Volumes/SSD/mxd/mxd/Data；目录缺失 → 用 <see cref="WzFactAttribute"/> 的测试自动 Skip。
/// </summary>
public static class WzFixture
{
    public static readonly string WzDataPath =
        Environment.GetEnvironmentVariable("WZ_DATA_PATH") is { Length: > 0 } env ? env
        : "/Volumes/SSD/mxd/mxd/Data";

    public static bool DataAvailable => Directory.Exists(WzDataPath);

    private static readonly Lazy<WzService> _instance = new(() =>
    {
        if (!DataAvailable)
            throw new InvalidOperationException($"WZ 数据目录不存在: {WzDataPath}");
        var wz = new WzService();
        var (ok, err) = wz.LoadWz(string.Empty, WzDataPath);
        if (!ok)
            throw new InvalidOperationException($"WZ 加载失败: {err} path={WzDataPath}");
        return wz;
    }, LazyThreadSafetyMode.ExecutionAndPublication);

    /// <summary>加载完成的 WzService 单例（首次访问触发加载；失败抛异常 = 测试失败）。</summary>
    public static WzService Wz => _instance.Value;

    /// <summary>
    /// 渲染用 PaperdollService（构造方式对齐 M1 PaperdollServiceBlackBoxTests：
    /// CacheManager + SpriteService(cache)，磁盘缓存目录缺省）。
    /// </summary>
    public static PaperdollService CreatePaperdoll()
    {
        var cache = new CacheManager();
        return new PaperdollService(Wz, cache, new SpriteService(cache));
    }
}

/// <summary>WZ 数据缺失时自动 Skip 的 Fact（xunit 2.x 的动态 Skip 惯用法）。</summary>
public sealed class WzFactAttribute : FactAttribute
{
    public WzFactAttribute()
    {
        if (!WzFixture.DataAvailable)
            Skip = $"WZ 数据不存在: {WzFixture.WzDataPath}（可用 env WZ_DATA_PATH 覆盖）";
    }
}
