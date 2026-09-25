#!/usr/bin/env node
/**
 * convert.js — lv_font_conv dump 输出 → 服务端 FontPackWriter 友好中间 JSON（kind=4 FONT 源）
 *
 * 输入：lv_font_conv --format dump --full-info 产出的 font_info.json
 *   （pixels 为 FreeType 8bpp 灰度 [h][w]；metrics 为 FreeType 整数像素量）
 * 输出格式（对齐 docs/ai/algorithm-asset-format.md 第六节字段命名）：
 *   {
 *     "size_px": 24, "bpp": 4, "glyph_count": N, ...meta,
 *     "glyphs": [ { "unicode", "w", "h", "advance", "off_x", "bearing_y", "bitmap" } ]
 *   }
 *   - bitmap：4bpp hex，行按字节对齐（行字节数 = ceil(w/2)），每像素 4bit，行内左像素在高半字节
 *   - advance(u8) = freetype.metrics.horiAdvance；off_x(i8) = horiBearingX；bearing_y(i8) = horiBearingY
 *
 * 用法：node convert.js <dump_font_info.json> <size_px> <输出路径> [字体源描述]
 */
'use strict';

const fs = require('fs');
const path = require('path');

const [input, sizeArg, output, sourceDesc] = process.argv.slice(2);
if (!input || !sizeArg || !output) {
  console.error('用法: node convert.js <dump_font_info.json> <size_px> <输出路径> [字体源描述]');
  process.exit(1);
}
const sizePx = parseInt(sizeArg, 10);

const dump = JSON.parse(fs.readFileSync(input, 'utf8'));

const glyphs = [];
let bitmapBytesTotal = 0;

for (const g of dump.glyphs) {
  const m = (g.freetype && g.freetype.metrics) || {};
  const w = m.width | 0;
  const h = m.height | 0;
  const advance = m.horiAdvance | 0;
  const offX = m.horiBearingX | 0;
  const bearingY = m.horiBearingY | 0;

  if (advance < 0 || advance > 255) throw new Error(`advance 越界 u8: ${advance} @U+${g.code.toString(16)}`);
  if (offX < -128 || offX > 127) throw new Error(`off_x 越界 i8: ${offX} @U+${g.code.toString(16)}`);
  if (bearingY < -128 || bearingY > 127) throw new Error(`bearing_y 越界 i8: ${bearingY} @U+${g.code.toString(16)}`);

  const rowBytes = (w + 1) >> 1;
  bitmapBytesTotal += rowBytes * h;

  let hex = '';
  if (w > 0 && h > 0) {
    const px = g.pixels;
    if (!Array.isArray(px) || px.length !== h) throw new Error(`像素行数不符 @U+${g.code.toString(16)}`);
    for (let y = 0; y < h; y++) {
      const row = px[y];
      if (row.length !== w) throw new Error(`像素行宽不符 y=${y} @U+${g.code.toString(16)}`);
      for (let bx = 0; bx < rowBytes; bx++) {
        const hi = row[bx * 2] >> 4; // 左像素高半字节
        const lo = bx * 2 + 1 < w ? row[bx * 2 + 1] >> 4 : 0; // 行尾奇数宽补 0
        hex += ((hi << 4) | lo).toString(16).padStart(2, '0');
      }
    }
  }

  glyphs.push({ unicode: g.code, w, h, advance, off_x: offX, bearing_y: bearingY, bitmap: hex });
}

glyphs.sort((a, b) => a.unicode - b.unicode);

const binPayloadEstimate = bitmapBytesTotal + 12 * glyphs.length; // 位图区 + 12B/glyph 索引（格式第六节）
const out = {
  size_px: sizePx,
  bpp: 4,
  glyph_count: glyphs.length,
  source: sourceDesc || 'unknown',
  bitmap_layout: '4bpp，行按字节对齐（行字节数=ceil(w/2)），行内左像素在高半字节，hex 每字节 2 字符',
  bin_payload_bytes_estimate: binPayloadEstimate,
  ascent: dump.ascent,
  descent: dump.descent,
  glyphs,
};

fs.mkdirSync(path.dirname(path.resolve(output)), { recursive: true });
fs.writeFileSync(output, JSON.stringify(out) + '\n', 'utf8');

const jsonBytes = fs.statSync(output).size;
console.log(
  `font-${sizePx}.json: glyphs=${glyphs.length} 中间JSON=${(jsonBytes / 1024).toFixed(1)}KB ` +
    `位图区=${(bitmapBytesTotal / 1024).toFixed(1)}KB bin包估算=${(binPayloadEstimate / 1024).toFixed(1)}KB(含12B/glyph索引)`
);
