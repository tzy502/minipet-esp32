using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using MinipetServer.Models;
using MinipetServer.Services;
using WzComparerR2.WzLib;

namespace MiniPet.Export;

/// <summary>
/// PaperdollService 部件管线的反射门面（M2 导出器专用）。
///
/// 背景：M1 服务层迁移按设计「原样迁入」PaperdollService，部件管线
/// （CollectPiecesForFrame → MaterializePieces → ResolveAnchors → AssignLayers →
/// HideFrontFaceLayersOnBackAction → ApplyLocks）与 GetBounds/GetFrameMove 均为 private
/// ——导出器边界（仅 Export/**）不可改其可见性，故经反射调用，方法名/字段名与桌面版逐字对齐
/// （两侧文件 diff 为空，M1 迁移不改内部结构）。若后续服务层重命名，本门面在初始化时即快速失败。
/// </summary>
internal static class PaperdollInternals
{
    private static readonly BindingFlags F = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;

    private static readonly MethodInfo MiCollect = typeof(PaperdollService).GetMethod("CollectPiecesForFrame", F)
        ?? throw Missing("CollectPiecesForFrame");
    private static readonly MethodInfo MiMaterialize = typeof(PaperdollService).GetMethod("MaterializePieces", F)
        ?? throw Missing("MaterializePieces");
    private static readonly MethodInfo MiResolveAnchors = typeof(PaperdollService).GetMethod("ResolveAnchors", F)
        ?? throw Missing("ResolveAnchors");
    private static readonly MethodInfo MiAssignLayers = typeof(PaperdollService).GetMethod("AssignLayers", F)
        ?? throw Missing("AssignLayers");
    private static readonly MethodInfo MiHideFront = typeof(PaperdollService).GetMethod("HideFrontFaceLayersOnBackAction", F)
        ?? throw Missing("HideFrontFaceLayersOnBackAction");
    private static readonly MethodInfo MiApplyLocks = typeof(PaperdollService).GetMethod("ApplyLocks", F)
        ?? throw Missing("ApplyLocks");
    private static readonly MethodInfo MiGetBounds = typeof(PaperdollService).GetMethod("GetBounds", F)
        ?? throw Missing("GetBounds");
    private static readonly MethodInfo MiGetFrameMove = typeof(PaperdollService).GetMethod("GetFrameMove", F)
        ?? throw Missing("GetFrameMove");

    private static readonly Type PieceType = typeof(PaperdollService).GetNestedType("Piece", BindingFlags.NonPublic)
        ?? throw Missing("Piece");
    private static readonly Type BoundsType = typeof(PaperdollService).GetNestedType("Bounds", BindingFlags.NonPublic)
        ?? throw Missing("Bounds");

    private static readonly FieldInfo FiCategory = Field("Category");
    private static readonly FieldInfo FiPieceName = Field("PieceName");
    private static readonly FieldInfo FiPiecePath = Field("PiecePath");
    private static readonly FieldInfo FiItemId = Field("ItemId");
    private static readonly FieldInfo FiZIndex = Field("ZIndex");
    private static readonly FieldInfo FiAnchorX = Field("AnchorX");
    private static readonly FieldInfo FiAnchorY = Field("AnchorY");
    private static readonly FieldInfo FiLocked = Field("Locked");
    private static readonly FieldInfo FiHidden = Field("Hidden");
    private static readonly FieldInfo FiFinalX = Field("FinalX");
    private static readonly FieldInfo FiFinalY = Field("FinalY");
    private static readonly FieldInfo FiResolvedLayer = Field("ResolvedLayer");

    private static readonly FieldInfo FiBoundsLeft = BoundsType.GetField("Left", F)!;
    private static readonly FieldInfo FiBoundsTop = BoundsType.GetField("Top", F)!;
    private static readonly FieldInfo FiBoundsW = BoundsType.GetField("W", F)!;
    private static readonly FieldInfo FiBoundsH = BoundsType.GetField("H", F)!;

    private static InvalidOperationException Missing(string name) =>
        new($"PaperdollService 内部结构不兼容（缺 {name}）——请核对 M1 迁移是否「原样迁入」");

    private static FieldInfo Field(string name) => PieceType.GetField(name, F) ?? throw Missing($"Piece.{name}");

    // ── 管线 ──────────────────────────────────────────────

    /// <summary>跑完整部件管线（与 RenderFrame 同序），返回锁定且未隐藏的部件视图。</summary>
    public static List<PieceView> RunPipeline(PaperdollService doll, string hash, CharacterAppearance a,
        string action, int frame, string expression, out UnionBounds bounds, out (int Dx, int Dy) move)
    {
        var b = MiGetBounds.Invoke(doll, new object[] { hash, a, action })!;
        bounds = new UnionBounds
        {
            Left = (int)FiBoundsLeft.GetValue(b)!,
            Top = (int)FiBoundsTop.GetValue(b)!,
            W = (int)FiBoundsW.GetValue(b)!,
            H = (int)FiBoundsH.GetValue(b)!,
        };

        var sources = MiCollect.Invoke(doll, new object[] { hash, a, action, frame, expression })!;
        var pieces = (System.Collections.IList)MiMaterialize.Invoke(doll, new object[] { sources, action, frame })!;
        MiResolveAnchors.Invoke(doll, new object[] { pieces });
        MiAssignLayers.Invoke(doll, new object[] { pieces });
        MiHideFront.Invoke(doll, new object[] { pieces, a, action, frame });
        MiApplyLocks.Invoke(doll, new object[] { hash, a, pieces });

        move = (0, 0);
        if (MiGetFrameMove.Invoke(doll, new object[] { hash, a, action, frame }) is Wz_Vector mv)
        {
            move = (mv.X, mv.Y);
        }

        var views = new List<PieceView>();
        foreach (var p in pieces)
        {
            bool locked = (bool)FiLocked.GetValue(p)!;
            bool hidden = (bool)FiHidden.GetValue(p)!;
            if (!locked || hidden) continue;
            views.Add(new PieceView
            {
                Category = (string)FiCategory.GetValue(p)!,
                PieceName = (string)FiPieceName.GetValue(p)!,
                PiecePath = (string)FiPiecePath.GetValue(p)!,
                ItemId = (string)FiItemId.GetValue(p)!,
                ZIndex = (int)FiZIndex.GetValue(p)!,
                AnchorX = (int)FiAnchorX.GetValue(p)!,
                AnchorY = (int)FiAnchorY.GetValue(p)!,
                FinalX = (int)FiFinalX.GetValue(p)!,
                FinalY = (int)FiFinalY.GetValue(p)!,
                ResolvedLayer = (string?)FiResolvedLayer.GetValue(p) ?? "",
            });
        }
        return views;
    }

    /// <summary>只收集部件（不走锚点/层）——表情变体位图登记用（仅需要 PiecePath 集合）。</summary>
    public static List<PieceView> CollectRaw(PaperdollService doll, string hash, CharacterAppearance a,
        string action, int frame, string expression)
    {
        var sources = MiCollect.Invoke(doll, new object[] { hash, a, action, frame, expression })!;
        var pieces = (System.Collections.IList)MiMaterialize.Invoke(doll, new object[] { sources, action, frame })!;
        var views = new List<PieceView>();
        foreach (var p in pieces)
        {
            views.Add(new PieceView
            {
                Category = (string)FiCategory.GetValue(p)!,
                PieceName = (string)FiPieceName.GetValue(p)!,
                PiecePath = (string)FiPiecePath.GetValue(p)!,
            });
        }
        return views;
    }

    public struct UnionBounds
    {
        public int Left, Top, W, H;
    }

    /// <summary>Piece 私有类的只读视图（字段经反射缓存读取）。</summary>
    public sealed class PieceView
    {
        public string Category = "";
        public string PieceName = "";
        public string PiecePath = "";
        public string ItemId = "";
        public int ZIndex;
        public int AnchorX, AnchorY;
        public int FinalX, FinalY;
        public string ResolvedLayer = "";
    }
}
