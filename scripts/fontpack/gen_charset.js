#!/usr/bin/env node
/**
 * gen_charset.js — 字体包字符集生成（E12）
 *
 * 产出（写入脚本同目录）：
 *   charset-16.txt / charset-24.txt ：ASCII 可见字符 + GB2312 一级汉字全表 + 常用中文标点
 *   charset-32.txt                 ：标题子集 ~500（数字+大小写 ASCII + 一级表前 400 常用 + 界面词手补）
 *
 * GB2312 一级表按编码区 0xB0A1-0xD7F9 算法生成（任务约定区间）：
 *   区码 16..55（高字节 0xB0..0xD7），末行 0xD7 只到 0xF9（=「座」，D7FA/FB 为 PUA 非汉字，实测 Node GBK 解码验证）。
 *   每个 GB 双字节码经 TextDecoder('gbk') 解码，仅收录「恰好解码为 1 个 BMP 汉字 (U+4E00..U+9FFF)」的码位。
 *
 * 输出文件为单行无分隔符的字符序列（含空格 0x20），消费方（build-fonts.sh）读取时只剔除 \n / \r，不可按空白剥离。
 *
 * 用法：node gen_charset.js   （可选 --out-dir <dir>，默认脚本所在目录）
 */
'use strict';

const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
let outDir = __dirname;
{
  const i = args.indexOf('--out-dir');
  if (i !== -1 && args[i + 1]) outDir = path.resolve(args[i + 1]);
}

const gbk = new TextDecoder('gbk');

/** ASCII 可见字符 0x20(空格)..0x7E */
function asciiPrintable() {
  let s = '';
  for (let c = 0x20; c <= 0x7e; c++) s += String.fromCharCode(c);
  return s;
}

/** GB2312 一级汉字全表（编码区 0xB0A1-0xD7F9 算法生成），返回字符串（按编码序） */
function gb2312Level1() {
  let s = '';
  const buf = Buffer.alloc(2);
  for (let hi = 0xb0; hi <= 0xd7; hi++) {
    const loEnd = hi === 0xd7 ? 0xf9 : 0xfe; // 末行只到 0xF9（座）；D7FA+ 为 PUA
    for (let lo = 0xa1; lo <= loEnd; lo++) {
      buf[0] = hi;
      buf[1] = lo;
      const ch = gbk.decode(buf);
      if (ch.length !== 1) continue;
      const cp = ch.codePointAt(0);
      if (cp >= 0x4e00 && cp <= 0x9fff) s += ch;
    }
  }
  return s;
}

/** 常用中文标点 / 符号（GB2312 A1 符号区精选，宋体全覆盖） */
const CJK_PUNCT = '，。、；：？！“”‘’（）《》〈〉【】〔〕「」『』—…·～￥％＋－×÷＝°℃↑↓←→';

/** 标题档极简标点（32px 只做标题，无需全套） */
const TITLE_PUNCT = '，。、：；！？·…—（）“”';

/**
 * 32px 标题档界面词手补表（Web 配置页 / 设备状态条可能出现的标题字）。
 * 与「一级表前 400」去重后并入；补字原则 = 界面高频 + 项目域词（地图名/设备管理/E11 降级话术）。
 */
const TITLE_EXTRA_WORDS = [
  // 通用界面动作
  '设置', '确认', '取消', '返回', '保存', '删除', '编辑', '搜索', '添加', '切换', '选择', '预览', '应用',
  '新增', '移除', '刷新', '恢复', '重启', '升级', '更新', '下载', '上传', '同步',
  // 设备 / 网络
  '设备', '网络', '连接', '断开', '服务器', '地址', '密码', '配对', '绑定', '注册', '命名', '在线', '离线',
  '版本', '固件', '缓存', '存储', '超时', '失败', '错误', '重试', '成功', '未知', '默认', '自定义',
  // 显示 / 状态
  '时钟', '亮度', '音量', '背景', '音乐', '主题', '装扮', '发型', '脸型', '表情', '动作', '地图',
  '宠物', '名字', '备注', '页', '首页', '末页', '上', '下', '左', '右', '开', '关', '是', '否',
  '待机', '休眠', '唤醒', '充电', '低电', '温度', '过热', '加载', '请稍候', '中',
  // 项目域词（26 张时钟地图相关 + 冒险岛地名）
  '码头', '售票', '升降', '港', '通道', '岛', '村', '城', '山', '路', '探险',
  '冒险岛', '天空之城', '神秘岛', '玩具城', '神木村', '阿里安特', '武陵', '圣地', '金银岛',
  '离别之山', '枫叶', '编年史', '秋月', '开始', '结束', '倒计时', '上午', '下午',
];

function uniqueChars(s) {
  const seen = new Set();
  let out = '';
  for (const ch of s) {
    if (seen.has(ch)) continue;
    seen.add(ch);
    out += ch;
  }
  return out;
}

// ---------- 16 / 24 档：全量表 ----------
const ascii = asciiPrintable();
const level1 = gb2312Level1();
const full = uniqueChars(ascii + level1 + CJK_PUNCT);

// ---------- 32 档：标题子集 ----------
const first400 = [...level1].slice(0, 400).join('');
let titleExtra = '';
for (const w of TITLE_EXTRA_WORDS) titleExtra += w;
const title = uniqueChars(ascii + TITLE_PUNCT + first400 + titleExtra);

// ---------- 落盘 ----------
function write(name, s) {
  const p = path.join(outDir, name);
  fs.writeFileSync(p, s + '\n', 'utf8');
  const han = [...s].filter((c) => {
    const cp = c.codePointAt(0);
    return cp >= 0x4e00 && cp <= 0x9fff;
  }).length;
  console.log(
    `${name}: 总字符 ${s.length}（汉字 ${han}，非汉字 ${s.length - han}），${Buffer.byteLength(s, 'utf8')} 字节`
  );
  return p;
}

fs.mkdirSync(outDir, { recursive: true });
write('charset-16.txt', full);
write('charset-24.txt', full);
write('charset-32.txt', title);
console.log(`GB2312 一级表实测汉字数: ${[...level1].length}（编码区 0xB0A1-0xD7F9）`);
