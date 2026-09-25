using MinipetServer.Models;
using MinipetServer.Services;

namespace MinipetServer.Tests;

/// <summary>
/// M1 黑盒测试②：PaperdollService 默认装扮实时合成 —
/// RenderFrame(walk1, 0, "default") 返回非空位图（默认外观 = 男体 2000 / 发 30000 / 脸 20000，对齐桌面版默认）。
/// </summary>
public class PaperdollServiceBlackBoxTests
{
    [Fact]
    public void RenderFrame_DefaultAppearance_Walk1Frame0_ReturnsNonEmptyBitmap()
    {
        if (!WzTestHarness.DataAvailable)
        {
            Console.WriteLine($"[SKIP] WZ 数据目录不存在: {WzTestHarness.DataPath}");
            return;
        }

        var wz = WzTestHarness.Wz;
        var cache = new CacheManager();
        var service = new PaperdollService(wz, cache, new SpriteService(cache));

        var appearance = new CharacterAppearance
        {
            Gender = 0,
            BodyId = 2000,
            Hair = new ItemInfo { Id = "30000" },
            Face = new ItemInfo { Id = "20000" },
        };

        var (frame, originX, originY, frameW, frameH) =
            service.RenderFrame(appearance, "walk1", 0, "default");

        Assert.NotNull(frame);
        Assert.True(frame!.Width > 1 && frame.Height > 1, $"位图应为有效尺寸，实际 {frame.Width}x{frame.Height}");
        Assert.True(frameW > 0 && frameH > 0, "union canvas 尺寸应 > 0");
    }
}
