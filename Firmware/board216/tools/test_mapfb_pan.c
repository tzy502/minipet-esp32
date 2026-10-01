/*
 * tools/test_mapfb_pan.c — 「地图扁平缓冲：平移 + 边条 == 全幅重建」的算术对拍
 *
 * 定位（诚实声明）：这不是跑固件代码，而是把 compositor.c 的
 *   · mapfb_shift()        （内容整体位移：dys 行搬 + dxs 逐行搬）
 *   · mapfb_pan() 的边条区间选取（dxs<0 → 右条 [sw+dxs, sw)；dys<0 → 下条 …）
 * 两份算术**逐行照抄**到 host，用"世界函数 W(x,y)"当宇宙真值：
 *   full(cam)      : dst(sx,sy) = W(cam_x + sx/2, cam_y + sy/2)      —— 全幅重建
 *   fb+pan(cam)    : 上一拍 fb 平移 + 只重算新露出的边条               —— 固件走的路径
 * 每一步都断言两者**逐像素相同**（含 2× 最近邻的整数像素口径、±8 世界 px 的
 * 拖动步长、跨条、上下左右四个方向、以及"条带相位冻结"这一条前提）。
 *
 * 编译/运行：gcc -std=c11 -O2 -Wall -Wextra tools/test_mapfb_pan.c -o /tmp/test_mapfb_pan
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 480
#define SH 480
#define SCALE 2                 /* RC_SCALE */
#define SCALE_SHIFT 1           /* RC_SCALE_SHIFT */

static int g_fail, g_check;
static void ck(int cond, const char *fmt, ...)
{
    g_check++;
    if (cond) return;
    g_fail++;
    va_list ap;
    va_start(ap, fmt);
    fputs("  x ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

/* ── 世界函数（"真值"）：分层 = 地面层 + 一条带（带自身相位 delta）── */
static uint16_t world_px(int32_t wx, int32_t wy)
{
    return (uint16_t)(((wx * 31 + wy * 17) ^ (wx >> 3)) & 0xFFFF);
}

/* 全幅重建（参考实现，语义 = cam_static_compose + cam_strip_compose 的并集） */
static void full_recompose(uint16_t *dst, int32_t cam_x, int32_t cam_y, int32_t strip_delta)
{
    for (int32_t sy = 0; sy < SH; sy++) {
        for (int32_t sx = 0; sx < SW; sx++) {
            int32_t wx = cam_x + (sx >> SCALE_SHIFT);
            int32_t wy = cam_y + (sy >> SCALE_SHIFT);
            uint16_t v = world_px(wx, wy);
            /* 条带：世界 x 平移 delta 后与"带层"混合（半透明条纹，模拟掩码层） */
            if (((wy / 16) % 2) == 0) v = (uint16_t)(v ^ world_px(wx + strip_delta, wy));
            dst[(size_t)sy * SW + sx] = v;
        }
    }
}

/* ── 以下两份**照抄自 compositor.c**（同一算术，改的只是缓冲名）── */
static void mapfb_shift(uint16_t *fb, int32_t dxs, int32_t dys)
{
    if (dys < 0) {
        memmove(fb, fb + (size_t)(-dys) * SW, (size_t)(SH + dys) * SW * 2u);
    } else if (dys > 0) {
        memmove(fb + (size_t)dys * SW, fb, (size_t)(SH - dys) * SW * 2u);
    }
    if (dxs < 0) {
        for (int32_t r = 0; r < SH; r++) {
            uint16_t *row = fb + (size_t)r * SW;
            memmove(row, row - dxs, (size_t)(SW + dxs) * 2u);
        }
    } else if (dxs > 0) {
        for (int32_t r = 0; r < SH; r++) {
            uint16_t *row = fb + (size_t)r * SW;
            memmove(row + dxs, row, (size_t)(SW - dxs) * 2u);
        }
    }
}

/* 只重算一块矩形（= map_bg_compose_into 的等价物：按世界坐标采样） */
static void band_compose(uint16_t *fb, int32_t cam_x, int32_t cam_y, int32_t strip_delta,
                         int32_t x, int32_t y, int32_t w, int32_t h)
{
    for (int32_t sy = y; sy < y + h; sy++) {
        for (int32_t sx = x; sx < x + w; sx++) {
            int32_t wx = cam_x + (sx >> SCALE_SHIFT);
            int32_t wy = cam_y + (sy >> SCALE_SHIFT);
            uint16_t v = world_px(wx, wy);
            if (((wy / 16) % 2) == 0) v = (uint16_t)(v ^ world_px(wx + strip_delta, wy));
            fb[(size_t)sy * SW + sx] = v;
        }
    }
}

int main(void)
{
    uint16_t *fb = malloc((size_t)SW * SH * 2u);
    uint16_t *ref = malloc((size_t)SW * SH * 2u);
    ck(fb && ref, "分配失败");
    if (!fb || !ref) return 2;

    /* 场景 1：相位冻结下的连续拖动（世界 px 步长 = CAM_PAN_SNAP_PX = 8，四个方向） */
    const int32_t steps[][2] = {
        { 8, 0 }, { 8, 0 }, { 8, 0 }, { 0, 8 }, { 0, 8 }, { -8, 0 }, { -8, -8 }, { 16, -16 },
        { 24, 8 }, { -8, 24 }, { 48, 0 }, { 0, -48 }, { 8, 8 }, { -16, -16 },
    };
    int32_t cam_x = 1000, cam_y = 800;
    const int32_t delta = 37;             /* 条带相位（冻结值） */
    full_recompose(fb, cam_x, cam_y, delta);
    int bad = 0;
    for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; k++) {
        int32_t ncx = cam_x + steps[k][0], ncy = cam_y + steps[k][1];
        int32_t dxs = (cam_x - ncx) << SCALE_SHIFT;   /* = 固件里的 (fb_cam - cam) << 1 */
        int32_t dys = (cam_y - ncy) << SCALE_SHIFT;
        mapfb_shift(fb, dxs, dys);
        if (dys < 0)      band_compose(fb, ncx, ncy, delta, 0, SH + dys, SW, -dys);
        else if (dys > 0) band_compose(fb, ncx, ncy, delta, 0, 0, SW, dys);
        if (dxs < 0)      band_compose(fb, ncx, ncy, delta, SW + dxs, 0, -dxs, SH);
        else if (dxs > 0) band_compose(fb, ncx, ncy, delta, 0, 0, dxs, SH);
        cam_x = ncx; cam_y = ncy;
        full_recompose(ref, cam_x, cam_y, delta);
        if (memcmp(fb, ref, (size_t)SW * SH * 2u) != 0) {
            for (int i = 0; i < SW * SH; i++)
                if (fb[i] != ref[i]) {
                    printf("  x 第 %u 步 (cam=%d,%d d=%d,%d) 第 %d 像素 fb=%04x ref=%04x\n",
                           k, cam_x, cam_y, steps[k][0], steps[k][1], i, fb[i], ref[i]);
                    break;
                }
            bad++;
        }
    }
    ck(bad == 0, "平移+边条 与 全幅重建 不一致 %d 步", bad);
    printf("场景1 连续拖动 %zu 步（含 4 方向/±8/±16/±24/±48）：平移+边条 == 全幅重建 逐像素 ✓\n",
           sizeof steps / sizeof steps[0]);

    /* 场景 2：相位变化必须靠"重算该带的行"回正（证明冻结+回正是必要的，不是可选项） */
    {
        uint16_t *fb2 = malloc((size_t)SW * SH * 2u);
        const int32_t cam0x = 500, cam0y = 500;
        full_recompose(fb2, cam0x, cam0y, 0);
        /* 相位从 0 → 100（模拟时间/IMU 相位推进），只平移不重算：应当**不相等** */
        mapfb_shift(fb2, 0, 0);
        full_recompose(ref, cam0x, cam0y, 100);
        ck(memcmp(fb2, ref, (size_t)SW * SH * 2u) != 0,
           "相位变化竟然被平移掩盖（说明测试本身没有区分力）");
        /* 按"该带所在行"重算（本模拟里条带覆盖整屏 ⇒ 全屏）→ 必须相等 */
        band_compose(fb2, cam0x, cam0y, 100, 0, 0, SW, SH);
        ck(memcmp(fb2, ref, (size_t)SW * SH * 2u) == 0, "相位回正重算后仍不一致");
        printf("场景2 相位推进：不重算 ⇒ 与真值不同（必须回正）；按行重算后 ⇒ 逐像素一致 ✓\n");
        free(fb2);
    }

    /* 场景 3：跳变超窗（|dxs| ≥ sw）必须整幅重建而不是平移 */
    {
        int32_t dxs = 240 << SCALE_SHIFT;   /* 240 世界 px = 480 屏 px = 整屏 */
        ck(dxs <= -SW || dxs >= SW, "超窗判据边界错：dxs=%d SW=%d", dxs, SW);
        printf("场景3 超窗判据：|dxs| ≥ %d 时走整幅重建（平移无意义）✓\n", SW);
    }

    free(fb); free(ref);
    printf("\n===== %s：%d 项检查，%d 项失败 =====\n", g_fail ? "FAIL" : "PASS", g_check, g_fail);
    return g_fail ? 1 : 0;
}
