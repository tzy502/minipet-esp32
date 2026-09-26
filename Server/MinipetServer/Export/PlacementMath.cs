using System;

namespace MiniPet.Export;

/// <summary>
/// 设备端摆放数学（导出器 ↔ 固件公共参考实现，固件照抄同一公式，勿各自发明）。
///
/// 契约（与 LayoutPackWriter.cs 顶部注释同口径）：
/// LAYOUT 的 piece x/y 已是「桌面版合成画布内」的 1x 绝对坐标，画布尺寸见
/// manifest-assets.json 对应 LAYOUT 条目的 "bounds":[w,h]。设备端渲染流程：
///   1. 画布 1x 合成：新建 bounds[w,h] 透明画布，piece 按帧内列表序（底→顶）blit 到 (x,y)；
///   2. 2x nearest 放大（禁双线性，保像素风）；
///   3. 摆放：水平居中、底对齐（缩放后画布底边距屏幕底边 <see cref="BottomMargin"/> px）。
///
/// 本类只给落点公式：<c>DstX = (screenW - canvasW*scale) / 2</c>、
/// <c>DstY = screenH - bottomMargin - canvasH*scale</c>（整数运算，均向零取整）。
/// 缩放后包围盒 = [DstX, DstX + canvasW*scale) × [DstY, DstY + canvasH*scale)；
/// 包围盒超出屏幕（负落点/右底越界）由设备端裁剪，公式本身不做 clamp（与桌面观感一致：
/// 主体以底边 40px 为锚，超宽画布左右对称溢出裁剪）。
/// </summary>
public static class PlacementMath
{
    /// <summary>放大倍数（nearest 邻近，整数倍无采样失真）。</summary>
    public const int Scale = 2;

    /// <summary>底对齐边距：缩放后画布底边距屏幕底边的像素数。</summary>
    public const int BottomMargin = 40;

    /// <summary>
    /// 计算缩放后画布在屏幕上的左上角落点（水平居中 / 底对齐 40px）。
    /// 纯整数数学，无副作用；固件照抄此公式（C 等价：
    /// dst_x = (screen_w - canvas_w*scale)/2; dst_y = screen_h - bottom_margin - canvas_h*scale;）。
    /// </summary>
    public static (int DstX, int DstY) PlaceOnScreen(int canvasW, int canvasH, int screenW, int screenH,
        int scale = Scale, int bottomMargin = BottomMargin)
    {
        if (scale <= 0) throw new ArgumentOutOfRangeException(nameof(scale));
        if (canvasW < 0 || canvasH < 0) throw new ArgumentOutOfRangeException(nameof(canvasW), "画布尺寸须非负");
        int dstW = canvasW * scale, dstH = canvasH * scale;
        return ((screenW - dstW) / 2, screenH - bottomMargin - dstH);
    }
}
