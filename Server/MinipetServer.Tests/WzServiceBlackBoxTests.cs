using System.Linq;
using MinipetServer.Services;

namespace MinipetServer.Tests;

/// <summary>
/// M1 黑盒测试①：WZ 加载 — LoadWz 成功 + 能读 Base.wz 节点（zmap.img 层序表）。
/// </summary>
public class WzServiceBlackBoxTests
{
    [Fact]
    public void LoadWz_Succeeds_And_CanReadBaseWzNode()
    {
        if (!WzTestHarness.DataAvailable)
        {
            Console.WriteLine($"[SKIP] WZ 数据目录不存在: {WzTestHarness.DataPath}");
            return;
        }

        var wz = WzTestHarness.Wz;

        // 加载成功（懒加载内已执行 LoadWz；此处校验其结果状态）
        Assert.True(wz.IsLoaded, "LoadWz 后 IsLoaded 应为 true");
        Assert.Null(wz.LastError);

        // 能读 Base.wz 节点：zmap.img（纸娃娃层序表，随 Base.wz 一并提供）可读出层序
        var zmap = wz.GetZmapOrder();
        Assert.NotEmpty(zmap);
        Assert.Contains("body", zmap);

        // 附加探针：Character 目录可枚举（目录模式挂载验证）
        var children = wz.GetDirectoryChildren("Character/Hair");
        Assert.NotEmpty(children);
        Assert.True(children.All(c => c.Id.Length > 0));
    }
}
