/**
 * mob-push-smoke.mjs —— Web「素材 → 怪物」tab 推送链路的专项冒烟（CDP 驱动真实 Chrome，无第三方依赖）
 *
 * 为什么单独一条：`ui-smoke.mjs` 覆盖的是设置/设备/曲库那些轮次的改动；怪物推送是本轮
 * （2026-10-02「怪物资产可用」）新增的用户入口，需要一条**只验它**的、可重复跑的验收。
 *
 * 跑法（两步）：
 *   # ① 预览服务：dist 产物 + 真实服务端 API（API_TARGET 指到被测服务端）
 *   API_TARGET=http://192.168.3.46:38090 PORT=5199 node scripts/preview-with-live-api.mjs
 *   # ② 冒烟（默认对 100100 做正例、100000 做空壳负例；--device 指定目标设备）
 *   node scripts/mob-push-smoke.mjs --base http://127.0.0.1:5199 --device dev-xxxxxx
 *
 * 断言：
 *   ① 怪物 tab 渲染出素材格子，且 📤 **可点**（本轮改动前是 disabled）
 *   ② 弹窗类型标签 = 怪物
 *   ③ 正例（真身编号，默认 100100）：推送 → HTTP 202 → 弹窗出现"已受理" + "切成该实体"
 *   ④ 负例（空壳编号，默认 100000）：推送 → 服务端 400，弹窗回显
 *      "在 WZ 里没有可导出动作"（同步预检文案，不再是"已受理但后台静默失败"）
 *   ⑤ 截图 /tmp/mob_web_push.png 留证
 *
 * 退出码 0 = 全通。
 */
import { spawn } from 'node:child_process'
import fs from 'node:fs'

const argv = process.argv.slice(2)
const arg = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d }
const BASE = arg('--base', process.env.WEB_BASE || 'http://127.0.0.1:5199')
const DEVICE = arg('--device', '')          // 空 = 弹窗里选第一个设备
const GOOD = arg('--good', '100100')        // 有动作的真身怪
const SHELL = arg('--shell', '100000')      // 空壳编号（预检应 400）
const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
const PORT = Number(arg('--cdp-port', '9333'))
const SHOT = arg('--out', '/tmp/mob_web_push.png')

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
let fails = 0
function ck(cond, msg, extra = '') {
  console.log(`[${cond ? 'PASS' : 'FAIL'}] ${msg}${extra ? ' —— ' + extra : ''}`)
  if (!cond) fails++
}

class Cdp {
  constructor(ws) { this.ws = ws; this.id = 0; this.waits = new Map() }
  static async connect(url) {
    const ws = new WebSocket(url)
    await new Promise((res, rej) => {
      ws.addEventListener('open', res, { once: true })
      ws.addEventListener('error', rej, { once: true })
    })
    const c = new Cdp(ws)
    ws.addEventListener('message', (ev) => {
      const m = JSON.parse(ev.data)
      if (m.id && c.waits.has(m.id)) { c.waits.get(m.id)(m); c.waits.delete(m.id) }
    })
    return c
  }
  send(method, params = {}) {
    const id = ++this.id
    return new Promise((res) => { this.waits.set(id, res); this.ws.send(JSON.stringify({ id, method, params })) })
  }
  async js(expr) {
    const r = await this.send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })
    return r?.result?.result?.value
  }
}

/** 点开某一格（按素材 id 精确定位）的推送按钮，返回是否找到该格 */
const clickPushFor = (id) => `(() => {
  const cell = [...document.querySelectorAll('.thumb-cell')]
    .find(c => (c.querySelector('.thumb-id')?.textContent || '').trim() === ${JSON.stringify(id)});
  if (!cell) return false;
  const b = cell.querySelector('.push');
  if (!b || b.disabled) return false;
  b.click(); return true })()`

const modalText = `(() => {
  const m = [...document.querySelectorAll('.n-modal')].find(e => e.textContent.includes('推送到设备'));
  return m ? m.textContent.replace(/\\s+/g, ' ') : '' })()`

const closeModal = `(() => {
  const m = [...document.querySelectorAll('.n-modal')].find(e => e.textContent.includes('推送到设备'));
  const b = m && [...m.querySelectorAll('button')].find(e => e.textContent.includes('关闭'));
  if (b) b.click(); return !!b })()`

async function pushViaModal(cdp, wantId) {
  const opened = await cdp.js(clickPushFor(wantId))
  if (!opened) return { opened: false }
  await sleep(900)
  const tag = await cdp.js(`(() => {
    const m = [...document.querySelectorAll('.n-modal')].find(e => e.textContent.includes('推送到设备'));
    return m ? (m.querySelector('.n-tag')?.textContent || '').trim() : '' })()`)
  if (DEVICE) {
    await cdp.js(`(() => {
      const m = [...document.querySelectorAll('.n-modal')].find(e => e.textContent.includes('推送到设备'));
      const inp = m.querySelector('.n-select'); if (inp) inp.click(); return true })()`)
    await sleep(700)
    await cdp.js(`(() => {
      const o = [...document.querySelectorAll('.n-base-select-option')]
        .find(e => e.textContent.includes(${JSON.stringify(DEVICE)}));
      if (o) o.click(); return !!o })()`)
    await sleep(400)
  } else {
    await cdp.js(`(() => {
      const m = [...document.querySelectorAll('.n-modal')].find(e => e.textContent.includes('推送到设备'));
      const inp = m.querySelector('.n-select'); if (inp) inp.click(); return true })()`)
    await sleep(700)
    await cdp.js(`(() => { const o = document.querySelector('.n-base-select-option'); if (o) o.click(); return !!o })()`)
    await sleep(400)
  }
  await cdp.js(`(() => {
    const m = [...document.querySelectorAll('.n-modal')].find(e => e.textContent.includes('推送到设备'));
    const b = [...m.querySelectorAll('button')].find(e => e.textContent.includes('推送到该设备'));
    if (b) b.click(); return !!b })()`)
  await sleep(4500)
  const txt = await cdp.js(modalText)
  await cdp.js(closeModal)
  await sleep(400)
  return { opened: true, tag, text: txt }
}

const chrome = spawn(CHROME, [
  '--headless=new', `--remote-debugging-port=${PORT}`, '--user-data-dir=/tmp/mp-chrome-mob',
  '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--window-size=1440,1000', 'about:blank',
], { stdio: 'ignore' })

let cdp
for (let i = 0; i < 40 && !cdp; i++) {
  await sleep(400)
  try {
    const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json()
    const page = list.find((t) => t.type === 'page')
    if (page) cdp = await Cdp.connect(page.webSocketDebuggerUrl)
  } catch { /* 等 Chrome 起来 */ }
}
if (!cdp) { console.error('Chrome/CDP 没起来'); chrome.kill(); process.exit(2) }

try {
  await cdp.send('Page.enable')
  await cdp.send('Runtime.enable')
  await cdp.send('Page.navigate', { url: `${BASE}/materials` })
  await sleep(3500)

  // ① 切到「怪物」tab
  await cdp.js(`(() => {
    const t = [...document.querySelectorAll('.n-tabs-tab')].find(e => e.textContent.trim().startsWith('怪物'));
    if (t) t.click(); return !!t })()`)
  await sleep(2500)

  const grid = await cdp.js(`(() => {
    const cells = [...document.querySelectorAll('.thumb-cell')];
    if (!cells.length) return { cells: 0 };
    const btn = cells[0].querySelector('.push');
    return { cells: cells.length, disabled: btn ? btn.disabled : null } })()`)
  ck(grid.cells > 0, '怪物 tab 渲染出素材格子', `cells=${grid.cells}`)
  ck(grid.disabled === false, '📤 推送按钮可点（本轮改动点）', `disabled=${grid.disabled}`)

  // ③ 正例：真身编号
  const good = await pushViaModal(cdp, GOOD)
  ck(good.opened === true, `正例 ${GOOD}：弹窗打开并可推送`)
  ck((good.tag || '').includes('怪物'), '弹窗类型标签 = 怪物', `tag=${good.tag}`)
  ck(/已受理|HTTP 202/.test(good.text || ''), `正例 ${GOOD}：服务端受理（202）`,
     (good.text || '').slice(0, 110))
  ck(/切成该实体|切换/.test(good.text || ''), '成功文案点明"设备端切成该实体"')

  // ④ 负例：空壳编号 → 同步预检 400
  const shell = await pushViaModal(cdp, SHELL)
  if (shell.opened) {
    ck(/没有可导出动作|HTTP 400/.test(shell.text || ''), `负例 ${SHELL}：明确报错（不再是"已受理"）`,
       (shell.text || '').slice(0, 130))
  } else {
    console.log(`[SKIP] 负例 ${SHELL}：列表里没这一格（该 id 不在当前目录）`)
  }

  // ⑤ 截图留证
  const shot = await cdp.send('Page.captureScreenshot', { format: 'png' })
  fs.writeFileSync(SHOT, Buffer.from(shot.result.data, 'base64'))
  console.log(`截图 → ${SHOT}`)
} finally {
  try { cdp.ws.close() } catch {}
  chrome.kill()
}

console.log(fails ? `\nFAIL（${fails} 项）` : '\nPASS（Web 怪物推送链路全通）')
process.exit(fails ? 1 : 0)
