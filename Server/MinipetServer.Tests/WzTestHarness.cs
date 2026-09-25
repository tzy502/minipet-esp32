using System;
using MinipetServer.Services;

namespace MinipetServer.Tests;

/// <summary>
/// M1 黑盒测试共享 WZ 环境：WZ_DATA_PATH（默认 /Volumes/SSD/mxd/mxd/Data）指向桌宠 WZ 数据目录。
/// 全部测试共用一个懒加载 WzService 单例（LoadWzFolder 全量加载开销大；WzService 内部 _wzLock 串行化保证线程安全）。
/// 数据目录缺失时各测试软跳过（直接返回，测试通过但不做断言）。
/// </summary>
public static class WzTestHarness
{
    public static readonly string DataPath =
        Environment.GetEnvironmentVariable("WZ_DATA_PATH") ?? "/Volumes/SSD/mxd/mxd/Data";

    public static bool DataAvailable => System.IO.Directory.Exists(DataPath);

    private static readonly Lazy<WzService> _wz = new(() =>
    {
        var wz = new WzService();
        var (ok, err) = wz.LoadWz("", DataPath);
        Console.WriteLine($"[WzTestHarness] LoadWz ok={ok} err={err}");
        return wz;
    });

    /// <summary>已加载就绪的 WzService（首次调用触发 LoadWz，后续复用）。</summary>
    public static WzService Wz => _wz.Value;
}
