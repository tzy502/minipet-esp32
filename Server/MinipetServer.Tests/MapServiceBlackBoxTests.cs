using MinipetServer.Services;

namespace MinipetServer.Tests;

/// <summary>
/// M1 黑盒测试③：MapService — LoadMap + ClampCamera + RenderViewport 返回非空位图
/// （地图 100000000 = Maple Island，Map1 分区）。
/// </summary>
public class MapServiceBlackBoxTests
{
    [Fact]
    public void LoadMap_ClampCamera_RenderViewport_ReturnsNonEmptyBitmap()
    {
        if (!WzTestHarness.DataAvailable)
        {
            Console.WriteLine($"[SKIP] WZ 数据目录不存在: {WzTestHarness.DataPath}");
            return;
        }

        var wz = WzTestHarness.Wz;
        var service = new MapService(wz, new CacheManager());

        var map = service.LoadMap("100000000");
        Assert.NotNull(map);
        Assert.Equal("100000000", map!.Id);

        var (cx, cy) = MapService.GetMapCenter(map);
        var (camX, camY) = MapService.ClampCamera(map, cx, cy, 1f, 800, 600);

        var bitmap = service.RenderViewport(map, camX, camY, 1f, 0, 800, 600);
        Assert.NotNull(bitmap);
        Assert.Equal(800, bitmap!.Width);
        Assert.Equal(600, bitmap.Height);
    }
}
