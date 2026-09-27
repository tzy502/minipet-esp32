/**
 * UI 冒烟自测（CDP 驱动真实 Chrome，无第三方依赖；仅开发用，不进生产构建）
 *
 *   # 1) 先起预览服务（真实服务端 API）
 *   node scripts/preview-with-live-api.mjs                 # → http://127.0.0.1:5199
 *   # 2)（可选）再起一个「模拟服务端已补齐 T2/T4/T5」的实例，验证「探测到就启用」分支
 *   PORT=5198 MINIPET_MOCK=1 node scripts/preview-with-live-api.mjs
 *   # 3) 自测（--push 才会真的调 push 端点，默认只读）
 *   node scripts/ui-smoke.mjs --base http://127.0.0.1:5199 --mock-base http://127.0.0.1:5198 --push
 *
 * 断言覆盖本轮的 Web 改动：
 *   T1 首页卡片缩略图 = 该设备 petConfig 拼出的外观串 URL（不再是 deviceId → seed）
 *   T2 服务端指令端点**已上线** → 25 个表情按钮可点（mock 实例同样 → 端点在位 + 可点）
 *   T3 素材页/设备页推送入口与 202/错误分支；设备选择器分组「★收藏 / 🕘最近」（E4）；
 *      收藏同步探测：真实服务端判「仅本机（端点缺失）」，mock（§T7 端点在位）判「已同步服务端」
 *   T4 IMU 灵敏度：服务端 device.imuSensitivity 在位 → 输入启用且随 PUT 下发
 *      （mock 实例下同样启用；旧断言「服务端无字段 → 禁用」已按 2026-09-27 实测更新）
 *   T5 随机台词气泡：服务端 speech 段在位 → 表单启用、逐行字节校验、mock 下真实 PUT 往返
 *   T6 曲库页「设备播放控制」卡（E8 附加）：服务端未放行 bgm → 播放/暂停/切歌/音量按钮
 *      全禁用 + 卡内接口需求（真实服务端实测 400「type 非法：bgm」）；
 *      mock 实例（服务端按 §T6 放行 bgm）→ 探针把「400 bgm 的 value 非法」判为端点在位、
 *      按钮全启用、点击真实发出 POST .../command {type:"bgm",...} 并显示 seq。
 *      ⚠ 真实实例上会点一次「存为设备偏好」——请求体用该设备**当前**音量（幂等，不改动实际偏好）。
 *   T1(本轮) QQ 卡 cookie 有效期告警 + 网关降级原因：真实实例（未导入 cookie + 网关未启用）不误报；
 *      mock 注入 cookieStale=true/ageDays=9/scriptFound=false → 黄色告警 + 降级原因渲染。
 *   T2(本轮) QQ 曲库关键词入口：真实实例只做 UI 断言（**不点保存，不动线上配置**）；
 *      mock 实例真实保存一次（mock PUT 只回显不落盘）。
 */
import { spawn } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const argv = process.argv.slice(2)
const arg = (name, dflt = null) => {
  const i = argv.indexOf(name)
  return i >= 0 && argv[i + 1] && !argv[i + 1].startsWith('--') ? argv[i + 1] : dflt
}
const BASE = arg('--base', 'http://127.0.0.1:5199')
const MOCK_BASE = arg('--mock-base', null)
const PUSH_FAIL_BASE = arg('--push-fail-base', null)
const ALLOW_PUSH = argv.includes('--push')
const SHOT_DIR = arg('--shots', '/tmp/minipet-ui-shots')
const CDP_PORT = Number(arg('--cdp-port', '9333'))
const CHROME = process.env.CHROME_BIN || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const results = []
const record = (id, ok, detail) => {
  results.push({ id, ok, detail })
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${id}  ${detail}`)
}

// ── CDP 最小客户端 ────────────────────────────────────────────────────────
class Cdp {
  constructor(ws) {
    this.ws = ws
    this.id = 0
    this.pending = new Map()
    this.events = []
    ws.addEventListener('message', (ev) => {
      const msg = JSON.parse(ev.data)
      if (msg.id && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id)
        this.pending.delete(msg.id)
        msg.error ? reject(new Error(JSON.stringify(msg.error))) : resolve(msg.result)
      } else if (msg.method) {
        this.events.push(msg)
      }
    })
  }
  send(method, params = {}) {
    const id = ++this.id
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject })
      this.ws.send(JSON.stringify({ id, method, params }))
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id)
          reject(new Error(`CDP timeout: ${method}`))
        }
      }, 30000)
    })
  }
  async eval(expression) {
    const r = await this.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })
    if (r.exceptionDetails) throw new Error(`eval error: ${r.exceptionDetails.text} ${r.exceptionDetails.exception?.description || ''}`)
    return r.result?.value
  }
  async waitFor(expression, timeoutMs = 15000, label = expression) {
    const t0 = Date.now()
    while (Date.now() - t0 < timeoutMs) {
      try {
        if (await this.eval(expression)) return true
      } catch { /* 导航中 evaluate 可能失败，重试 */ }
      await sleep(250)
    }
    throw new Error(`waitFor 超时：${label}`)
  }
  async goto(url, settleMs = 800) {
    await this.send('Page.enable')
    await this.send('Page.navigate', { url })
    await sleep(settleMs)
  }
  async shot(file) {
    try {
      const r = await this.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true })
      fs.mkdirSync(path.dirname(file), { recursive: true })
      fs.writeFileSync(file, Buffer.from(r.data, 'base64'))
    } catch (e) {
      console.log(`  (截图失败 ${file}: ${e.message})`)
    }
  }
}

async function getPageWs(port) {
  for (let i = 0; i < 60; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${port}/json/list`)
      const list = await r.json()
      const page = list.find((t) => t.type === 'page' && t.webSocketDebuggerUrl)
      if (page) return page.webSocketDebuggerUrl
    } catch { /* 还没起来 */ }
    await sleep(300)
  }
  throw new Error('Chrome CDP 端口未就绪')
}

function launchChrome(port) {
  const userDataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'minipet-cdp-'))
  const proc = spawn(
    CHROME,
    [
      '--headless',
      '--disable-gpu',
      '--no-sandbox',
      '--no-first-run',
      '--no-default-browser-check',
      '--disable-extensions',
      '--hide-scrollbars',
      '--window-size=1400,2000',
      `--user-data-dir=${userDataDir}`,
      `--remote-debugging-port=${port}`,
      'about:blank',
    ],
    { stdio: 'ignore', detached: false },
  )
  return { proc, userDataDir }
}

// ── 页面内取数小工具（注入到页面执行）──────────────────────────────────────
const PAGE_HELPERS = `
window.__mini = {
  cardByTitle: (t) => [...document.querySelectorAll('.n-card')].find(c => (c.querySelector('.n-card-header__main')||{}).textContent?.includes(t)),
  exprButtons: () => [...document.querySelectorAll('button')].filter(b => b.querySelector('.expr-key')),
  thresholdCard: () => window.__mini.cardByTitle('阈值（IMU'),
  sensitivityInput: () => {
    const card = window.__mini.thresholdCard();
    if (!card) return null;
    const labels = [...card.querySelectorAll('.n-form-item')];
    const item = labels.find(i => i.textContent.includes('IMU 灵敏度'));
    return item ? item.querySelector('input') : null;
  },
  checkboxByText: (t) => [...document.querySelectorAll('.n-checkbox')].find(c => c.textContent.includes(t)),
  bgmCard: () => window.__mini.cardByTitle('设备播放控制'),
  bgmBtn: (k) => document.querySelector('button[data-bgm="' + k + '"]'),
  bgmButtons: () => [...document.querySelectorAll('button[data-bgm]')].filter(b => b.dataset.bgm !== 'pref').map(b => ({ k: b.dataset.bgm, disabled: b.disabled })),
  bgmTag: () => {
    const c = window.__mini.bgmCard();
    if (!c) return 'no-card';
    const m = c.innerText.match(/端点在位|端点不支持 bgm|端点入参不符|未探测到/);
    return m ? m[0] : '';
  },
  cmdCard: () => window.__mini.cardByTitle('表情 / 气泡调试'),
  /** 指令卡右上角状态标签（⚠ 不能扫 body 全文：卡片说明里的「收藏端点缺失」等字样会抢先命中）。 */
  cmdTag: () => {
    const c = window.__mini.cmdCard();
    if (!c) return 'no-card';
    const t = c.querySelector('.n-card-header__extra .n-tag');
    return t ? t.innerText.trim() : '';
  },
  bodyText: () => document.body.innerText,
  toastText: () => [...document.querySelectorAll('.n-message__content')].map(e => e.textContent).join(' | '),
};
'ok'
`

/**
 * 走 UI 完整链路推一条地图（搜索 200000100 → 卡片 📤 → 弹窗选设备 → 「推送到该设备」）。
 * 返回弹窗文案 + toast 文案。会真实调用服务端 push 端点（202 或注入的错误码）。
 */
async function pushViaUi(cdp, base) {
  await cdp.goto(`${base}/materials`)
  await cdp.eval(PAGE_HELPERS)
  await cdp.waitFor(`document.querySelectorAll('button.push').length > 0`, 25000, '素材页推送按钮')
  await cdp.eval(`(() => {
    const inp = document.querySelector('.search input');
    inp.value = '200000100';
    inp.dispatchEvent(new Event('input', { bubbles: true }));
    return true;
  })()`)
  await sleep(1200) // 搜索防抖 300ms + 过滤渲染
  const clicked = await cdp.eval(`(() => {
    const cell = [...document.querySelectorAll('.thumb-cell')].find(c => c.innerText.includes('200000100'));
    if (!cell) return { ok: false, why: '未找到 200000100 卡片' };
    const b = cell.querySelector('button.push');
    if (!b) return { ok: false, why: '该卡片无推送按钮' };
    if (b.disabled) return { ok: false, why: '推送按钮禁用' };
    b.click();
    return { ok: true };
  })()`)
  if (!clicked.ok) return { text: `打开弹窗失败：${clicked.why}`, toast: '' }
  await cdp.waitFor(`!!document.querySelector('.n-modal')`, 10000, '推送弹窗')
  await cdp.waitFor(`(() => { const m=document.querySelector('.n-modal'); const b=[...m.querySelectorAll('button')].find(b=>b.textContent.includes('推送到该设备')); return !!b && !b.disabled })()`, 15000, '推送按钮可点（等设备列表选中）')
  await cdp.eval(`(() => { const m=document.querySelector('.n-modal'); [...m.querySelectorAll('button')].find(b=>b.textContent.includes('推送到该设备')).click(); return true })()`)
  await sleep(4000)
  return cdp.eval(`(() => { const m=document.querySelector('.n-modal'); return { text: m ? m.innerText.replace(/\\n/g,' | ').slice(0,400) : '', toast: window.__mini.toastText() } })()`)
}

async function main() {
  console.log(`Chrome: ${CHROME}`)
  console.log(`BASE=${BASE}  MOCK_BASE=${MOCK_BASE || '(未提供)'}  ALLOW_PUSH=${ALLOW_PUSH}`)
  const { proc } = launchChrome(CDP_PORT)
  const wsUrl = await getPageWs(CDP_PORT)
  const ws = new WebSocket(wsUrl)
  await new Promise((res, rej) => {
    ws.addEventListener('open', res, { once: true })
    ws.addEventListener('error', rej, { once: true })
  })
  const cdp = new Cdp(ws)
  await cdp.send('Runtime.enable')
  await cdp.send('Page.enable')
  await cdp.send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 2000, deviceScaleFactor: 1, mobile: false })

  try {
    // ── T1：首页卡片缩略图 = 真实装扮外观串 ──────────────────────────────
    await cdp.goto(`${BASE}/`)
    await cdp.waitFor(`document.querySelector('.n-avatar img, img.n-avatar-img, .n-avatar') !== null`, 20000, '首页卡片出现')
    await cdp.waitFor(`!!document.querySelector('img[src*="thumb?type=paperdoll"]')`, 20000, '缩略图 URL 落地')
    const cardThumb = await cdp.eval(`(() => {
      const img = document.querySelector('img[src*="thumb?type=paperdoll"]');
      const dev = [...document.querySelectorAll('*')].map(e=>e.textContent).join(' ');
      return { src: img ? img.getAttribute('src') : null, hasDeviceId: document.body.innerText.includes('dev-693ea4') };
    })()`)
    const decoded = decodeURIComponent(cardThumb.src || '')
    record(
      'T1 首页缩略图按外观串合成',
      /id=g:0\|ear:humanEar\|body:2000\|hair:42540\|face:22035/.test(decoded) && !/id=dev-693ea4/.test(decoded),
      `src=${decoded.slice(0, 120)}…`,
    )
    await cdp.shot(path.join(SHOT_DIR, 't1-dashboard.png'))

    // ── T2：设备详情（真实服务端：指令端点**已上线** → 25 表情按钮可点）──────
    await cdp.goto(`${BASE}/device/dev-693ea4`)
    await cdp.eval(PAGE_HELPERS)
    await cdp.waitFor(`window.__mini.exprButtons().length === 25`, 25000, '25 个表情按钮渲染')
    await cdp.waitFor(`document.body.innerText.includes('端点缺失') || document.body.innerText.includes('端点在位')`, 20000, '端点探测完成')
    const expr = await cdp.eval(`(() => {
      const bs = window.__mini.exprButtons();
      return { n: bs.length, disabled: bs.filter(b => b.disabled).length, tag: window.__mini.cmdTag(),
               alert: document.body.innerText.includes('本次探测判定设备指令端点不可用') };
    })()`)
    record(
      'T2 真实实例：端点在位 → 25 表情按钮全可点（旧「服务端尚未提供」断言已按实测更新）',
      expr.n === 25 && expr.disabled === 0 && expr.tag === '端点在位' && !expr.alert,
      `按钮=${expr.n} 禁用=${expr.disabled} 状态=${expr.tag} 旧禁用提示=${expr.alert}`,
    )
    await cdp.shot(path.join(SHOT_DIR, 't2-detail-endpoint-ok.png'))

    // 打开「覆盖全局阈值」→ 5 项（含灵敏度）全部可编辑（服务端 imuSensitivity 已在上位）
    const sens1 = await cdp.eval(`(() => {
      const cb = window.__mini.checkboxByText('覆盖全局阈值'); if (cb) cb.click();
      return true;
    })()`)
    await sleep(500)
    const thState = await cdp.eval(`(() => {
      const card = window.__mini.thresholdCard();
      const inputs = [...card.querySelectorAll('input')];
      const sens = window.__mini.sensitivityInput();
      return { total: inputs.length, enabled: inputs.filter(i=>!i.disabled).length, sensDisabled: !!sens?.disabled, sensValue: sens?.value,
               hint: card.innerText.includes('没有灵敏度字段') || card.innerText.includes('本次设备响应的') };
    })()`)
    record(
      'T4 真实实例：imuSensitivity 在位 → 灵敏度输入可编辑 + 占位说明消失',
      sens1 && thState.sensDisabled === false && thState.enabled >= 5 && thState.hint === false,
      JSON.stringify(thState),
    )
    await cdp.shot(path.join(SHOT_DIR, 't4-detail-sensitivity.png'))

    // ── T3/E4：设备选择器含「★ 收藏 / 🕘 最近」（读本机 localStorage，与素材页星标同源）──
    await cdp.eval(`(() => {
      localStorage.setItem('minipet.materials.favorites', JSON.stringify({ map: ['200000100'], mob: [], npc: [] }));
      localStorage.setItem('minipet.materials.recent', JSON.stringify({ map: ['200000111'], mob: [], npc: [] }));
      return true;
    })()`)
    await cdp.goto(`${BASE}/device/dev-693ea4`)
    await cdp.eval(PAGE_HELPERS)
    await cdp.waitFor(`!!window.__mini.cardByTitle('素材推送')`, 25000, '素材推送卡')
    const grouped = await cdp.eval(`(() => {
      const card = window.__mini.cardByTitle('素材推送');
      const sel = card.querySelector('.n-select');
      if (!sel) return { ok: false, why: 'no-select' };
      sel.querySelector('.n-base-selection')?.dispatchEvent(new MouseEvent('click', { bubbles: true }));
      return { ok: true };
    })()`)
    await sleep(1200)
    const groupText = await cdp.eval(`document.body.innerText`)
    record(
      'T3/E4 设备选择器分组「★ 收藏 / 🕘 最近使用」（喂给设备选择器）',
      grouped.ok && /★ 收藏/.test(groupText) && /🕘 最近使用/.test(groupText),
      `收藏组=${/★ 收藏/.test(groupText)} 最近组=${/🕘 最近使用/.test(groupText)} 打开选择器=${grouped.ok}${grouped.why ? ' ' + grouped.why : ''}`,
    )
    await cdp.shot(path.join(SHOT_DIR, 't3-e4-selector-groups.png'))
    await cdp.eval(`document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`)

    // ── T3：设备详情页推送卡片存在 ───────────────────────────────────────
    const pushCard = await cdp.eval(`(() => {
      const c = window.__mini.cardByTitle('素材推送');
      return { exists: !!c, text: c ? c.innerText.slice(0, 400) : '' };
    })()`)
    record('T3 设备详情有推送卡片', pushCard.exists && /push/.test(pushCard.text), pushCard.text.replace(/\n/g, ' ').slice(0, 110))

    // ── T6：曲库页「设备播放控制」卡（真实服务端：未放行 bgm）──────────────
    await cdp.goto(`${BASE}/music`)
    await cdp.eval(PAGE_HELPERS)
    await cdp.waitFor(
      `['端点在位','端点不支持 bgm','未探测到'].includes(window.__mini.bgmTag())`,
      25000,
      'bgm 控制卡探测完成',
    )
    const bgmReal = await cdp.eval(`(() => {
      const card = window.__mini.bgmCard();
      const bs = window.__mini.bgmButtons();
      const txt = card ? card.innerText : '';
      return {
        card: !!card,
        tag: window.__mini.bgmTag(),
        btns: bs.length,
        keys: bs.map(b => b.k).join(','),
        disabled: bs.filter(b => b.disabled).length,
        prefEnabled: !(window.__mini.bgmBtn('pref') || { disabled: true }).disabled,
        need: txt.includes('/api/admin/devices/') && txt.includes('command') && txt.includes('§T6'),
        readonly: txt.includes('BGM 偏好：源'),
        online: document.body.innerText.includes('设备离线') || document.body.innerText.includes('设备在线'),
      };
    })()`)
    record(
      'T6 真实实例：未放行 bgm → 6 播放键+下发音量 全禁用 + 接口需求',
      bgmReal.card && bgmReal.tag === '端点不支持 bgm' && bgmReal.btns === 7 && bgmReal.disabled === 7 &&
        bgmReal.keys === 'play,pause,resume,stop,prev,next,vol' && bgmReal.need && bgmReal.readonly && bgmReal.online,
      JSON.stringify(bgmReal),
    )
    record('T6 真实实例：「存为设备偏好」可用（PUT 通道已在位）', bgmReal.prefEnabled === true, `prefEnabled=${bgmReal.prefEnabled}`)
    await cdp.shot(path.join(SHOT_DIR, 't6-music-bgm-missing.png'))

    // ── T1：QQ 卡消费服务端 cookieStale / gateway（真实实例：未导入 cookie + 网关未启用）──
    const t1 = await cdp.eval(`(() => {
      const card = window.__mini.cardByTitle('QQ 音源');
      const txt = card ? card.innerText : '';
      const tag = (txt.match(/网关：[^\\n]*/) || [''])[0];
      return { card: !!card, tag, stateTag: (txt.match(/健康：[^\\n]*/) || [''])[0],
               staleAlert: txt.includes('可能已失效，请重新导入'),
               degradedAlert: txt.includes('QQ 音源当前'),
               detail: txt.includes('未启用（配置 QqMusic.Enabled=false'),
               gwDetail: txt.includes('scriptFound=') && txt.includes('healthy='),
               external: txt.includes('不在本仓库、镜像也未内置') };
    })()`)
    record(
      'T1 真实实例：QQ 卡渲染 gateway 状态 + 降级原因（服务端 health.detail / gateway 对象）',
      t1.card && /网关：网关未启用/.test(t1.tag) && t1.degradedAlert && t1.detail && t1.gwDetail && t1.external,
      JSON.stringify(t1).slice(0, 320),
    )
    record(
      'T1 真实实例：未导入 cookie → 不误报过期告警（cookieStale=false）',
      t1.staleAlert === false, `staleAlert=${t1.staleAlert} 健康标签=${t1.stateTag}`,
    )
    await cdp.shot(path.join(SHOT_DIR, 't1-music-qq-cookie-gateway.png'))

    // ── T2（E4 缺口）：QQ 曲库关键词输入入口（真实实例：只做 UI 校验，不写真实配置）──────
    const kw = await cdp.eval(`(() => {
      const card = window.__mini.cardByTitle('QQ 音源');
      const inp = card ? card.querySelector('input') : null;
      const btn = card ? [...card.querySelectorAll('button')].find(b => b.textContent.includes('保存关键词')) : null;
      const before = inp ? inp.value : null;
      const dirtyBefore = card ? card.innerText.includes('未保存') : null;
      if (inp) { inp.value = '__smoke_kw__'; inp.dispatchEvent(new Event('input', { bubbles: true })); }
      return { hasInput: !!inp, hasBtn: !!btn, btnDisabledWhenClean: btn ? btn.disabled : null, before, dirtyBefore };
    })()`)
    await sleep(400)
    const kw2 = await cdp.eval(`(() => {
      const card = window.__mini.cardByTitle('QQ 音源');
      const btn = [...card.querySelectorAll('button')].find(b => b.textContent.includes('保存关键词'));
      return { dirtyAfter: card.innerText.includes('未保存'), btnEnabledWhenDirty: btn ? !btn.disabled : null,
               serverValueShown: /当前服务端值/.test(card.innerText) };
    })()`)
    record(
      'T2 真实实例：QQ 曲库关键词入口在位（写入 qqMusic.searchKeyword，脏标记 + 按钮启用）',
      kw.hasInput && kw.hasBtn && kw.before === '' && kw.btnDisabledWhenClean === true &&
        kw2.dirtyAfter === true && kw2.btnEnabledWhenDirty === true && kw2.serverValueShown === true,
      `input=${kw.hasInput} 按钮=${kw.hasBtn} 初始值="${kw.before}" 干净时禁用=${kw.btnDisabledWhenClean} 脏标记=${kw2.dirtyAfter} 脏时启用=${kw2.btnEnabledWhenDirty}`,
    )
    await cdp.shot(path.join(SHOT_DIR, 't2-music-keyword-input.png'))
    // 只丢弃本地输入态（真实实例**不点保存**，不动线上配置）；goto 会清掉 window.__mini，需重新注入
    await cdp.goto(`${BASE}/music`)
    await cdp.eval(PAGE_HELPERS)
    await cdp.waitFor(`window.__mini.bgmBtn('pref') !== null`, 25000, '回到曲库页（bgm 卡就绪）')

    // 真实点一次「存为设备偏好」：请求体音量 = 该设备**当前**偏好值（幂等，不改实际偏好）
    await cdp.eval(`(() => { window.__mini.bgmBtn('pref').click(); return true })()`)
    await sleep(2500)
    const prefRun = await cdp.eval(`(() => { const c = window.__mini.bgmCard(); return { toast: window.__mini.toastText(), text: c ? c.innerText : '' } })()`)
    record(
      'T6 真实实例：存为设备偏好 → PUT 200 + 真实请求/响应回显',
      /音量已写入设备偏好/.test(prefRun.toast) && /HTTP 200/.test(prefRun.text) &&
        /PUT \/api\/admin\/devices\/[^ ]+ body \{"bgm":\{"volume":\d+\}\}/.test(prefRun.text),
      `toast=${prefRun.toast} | ${(prefRun.text.match(/HTTP 200[^\n]*/) || [''])[0]}`.slice(0, 240),
    )
    await cdp.shot(path.join(SHOT_DIR, 't6-music-bgm-pref-saved.png'))

    // ── T5/E12 + T4：设置页（真实服务端：speech 段 + imuSensitivity **都已在上位**）──
    await cdp.goto(`${BASE}/settings`)
    await cdp.eval(PAGE_HELPERS)
    await cdp.waitFor(`document.body.innerText.includes('随机台词气泡')`, 20000, '设置页加载')
    const st = await cdp.eval(`(() => {
      const card = window.__mini.cardByTitle('随机台词气泡');
      const devCard = window.__mini.cardByTitle('设备阈值');
      const sens = devCard ? [...devCard.querySelectorAll('.n-form-item')].find(i=>i.textContent.includes('IMU 灵敏度'))?.querySelector('input') : null;
      const ta = card ? card.querySelector('textarea') : null;
      const sw = card ? card.querySelector('.n-switch') : null;
      const num = card ? card.querySelector('input') : null;
      return { tag: card ? card.innerText.match(/配置段在位（真实读写）|服务端未返回 speech 段/) : null,
               staleCopy: document.body.innerText.includes('当前服务端没有台词配置段') || document.body.innerText.includes('没有台词段'),
               sensDisabled: !!sens?.disabled, sensAlert: document.body.innerText.includes('本次 GET /api/admin/settings 的响应里没有'),
               taDisabled: !!ta?.disabled, idleDisabled: !!num?.disabled, idleValue: num ? num.value : null,
               tagText: card ? (card.innerText.split('\\n')[0] || '') : '' };
    })()`)
    record(
      'T5/E12 真实实例：speech 段在位 → 台词表单启用（开关/秒数/台词库）+ 过期文案已清除',
      Array.isArray(st.tag) && st.tag[0] === '配置段在位（真实读写）' && st.staleCopy === false &&
        st.taDisabled === false && st.idleDisabled === false && st.idleValue === '300',
      `标签=${st.tag} 过期文案=${st.staleCopy} 台词框禁用=${st.taDisabled} 秒数=${st.idleValue} 禁用=${st.idleDisabled}`,
    )
    record(
      'T4 真实实例：服务端已返回 imuSensitivity → 灵敏度输入启用（旧「没有该字段」说明消失）',
      st.sensDisabled === false && st.sensAlert === false,
      `disabled=${st.sensDisabled} 旧说明=${st.sensAlert}`,
    )
    await cdp.shot(path.join(SHOT_DIR, 't45-settings-enabled.png'))

    // T5 台词逐行校验（只读断言：不点保存 —— 真实实例的写往返在 mock 段做，避免动线上配置）
    const speechRound = await cdp.eval(`(() => {
      const card = window.__mini.cardByTitle('随机台词气泡');
      const ta = card.querySelector('textarea');
      ta.value = 'smoke 台词一\\nsmoke 台词二';
      ta.dispatchEvent(new Event('input', { bubbles: true }));
      return true;
    })()`)
    await sleep(300)
    const speechBytes = await cdp.eval(`(() => {
      const card = window.__mini.cardByTitle('随机台词气泡');
      const m = card.innerText.match(/共 \\d+ 条/);
      return m ? m[0] : '';
    })()`)
    record('T5 真实实例：台词逐行字节校验生效（显示条数统计）', speechRound && speechBytes === '共 2 条', `统计=${speechBytes}`)
    await cdp.eval(`(() => { const ta = window.__mini.cardByTitle('随机台词气泡').querySelector('textarea'); ta.value=''; ta.dispatchEvent(new Event('input',{bubbles:true})); return true })()`)
    await cdp.shot(path.join(SHOT_DIR, 't5-settings-speech.png'))

    // ── T3：素材页推送入口（map 可推 / mob 禁用）─────────────────────────
    await cdp.goto(`${BASE}/materials`)
    await cdp.waitFor(`document.querySelectorAll('button.push').length > 0`, 25000, '素材页推送按钮')
    const mat = await cdp.eval(`(() => {
      const btns = [...document.querySelectorAll('button.push')];
      return { n: btns.length, enabled: btns.filter(b=>!b.disabled).length, disabled: btns.filter(b=>b.disabled).length };
    })()`)
    record('T3 素材页 map tab 推送按钮可点', mat.n > 0 && mat.disabled === 0 && mat.enabled === mat.n, JSON.stringify(mat))

    // ── T3/E4：收藏同步探测（真实服务端：GET 未注册 → SPA fallback 200+HTML → 判「仅本机」）──
    await cdp.waitFor(`document.body.innerText.includes('收藏仅本机') || document.body.innerText.includes('收藏已同步服务端')`, 20000, '收藏同步探测完成')
    const favReal = await cdp.eval(`(() => {
      const all = document.body.innerText;
      const tag = (all.match(/收藏仅本机（端点缺失）|收藏仅本机（同步不可用）|收藏仅本机|收藏已同步服务端/) || [''])[0];
      return { tag, hasSyncBtn: [...document.querySelectorAll('button')].some(b => b.textContent.includes('同步到服务端')),
               spaNote: all.includes('SPA index.html') || all.includes('GET 路由未注册'),
               feedsSelector: all.includes('已喂给设备选择器') };
    })()`)
    record(
      'T3/E4 真实实例：收藏端点缺失（GET 走 SPA fallback 200+HTML）→ 标注「仅本机」且不给同步按钮',
      favReal.tag === '收藏仅本机（端点缺失）' && favReal.hasSyncBtn === false && favReal.feedsSelector === true,
      JSON.stringify(favReal),
    )
    // 星标照常可用（localStorage 兜底能力未退化）
    const favToggle = await cdp.eval(`(() => {
      const b = document.querySelector('button.star');
      if (!b) return { ok: false };
      const before = localStorage.getItem('minipet.materials.favorites');
      b.click();
      return { ok: true, before, after: localStorage.getItem('minipet.materials.favorites') };
    })()`)
    await sleep(300)
    record(
      'T3/E4 真实实例：星标仍写 localStorage（离线兜底未删）',
      favToggle.ok && favToggle.before !== favToggle.after && !!favToggle.after,
      `before=${(favToggle.before || '').slice(0, 60)} after=${(favToggle.after || '').slice(0, 60)}`,
    )
    await cdp.shot(path.join(SHOT_DIR, 't3-materials-favorites-sync.png'))

    // 纸娃娃 tab：推送按钮应为禁用态（服务端 push 无 paperdoll 分支）
    await cdp.eval(`(() => { const t=[...document.querySelectorAll('.n-tabs-tab')].find(t=>t.textContent.includes('纸娃娃部件')); t && t.click(); return true })()`)
    await sleep(1500)
    const matPd = await cdp.eval(`(() => { const b=[...document.querySelectorAll('button.push')]; return { n:b.length, disabled:b.filter(x=>x.disabled).length } })()`)
    record('T3 纸娃娃 tab 推送按钮禁用（端点仅 map/npc）', matPd.n > 0 && matPd.disabled === matPd.n, JSON.stringify(matPd))
    await cdp.shot(path.join(SHOT_DIR, 't3-materials-push.png'))

    // ── T3：真实推送（仅 --push 时执行；走 UI 完整链路）──────────────────
    if (ALLOW_PUSH) {
      const okState = await pushViaUi(cdp, BASE)
      record('T3 UI 真实推送 202 受理', /已受理|HTTP 202/.test(okState.text + okState.toast), (okState.text + ' || toast=' + okState.toast).slice(0, 240))
      await cdp.shot(path.join(SHOT_DIR, 't3-push-result.png'))
    } else {
      console.log('SKIP  T3 UI 真实推送（未加 --push，避免改动真实服务端资产）')
    }

    // ── T3：推送错误分支（故障注入实例，前端必须落到 503 文案 + 提示）────
    if (PUSH_FAIL_BASE) {
      const failState = await pushViaUi(cdp, PUSH_FAIL_BASE)
      const txt = failState.text + ' || toast=' + failState.toast
      record('T3 UI 推送失败分支（注入 503）', /HTTP 503/.test(txt) && /WZ 未加载/.test(txt), txt.slice(0, 240))
      await cdp.shot(path.join(SHOT_DIR, 't3-push-error-503.png'))
    } else {
      console.log('SKIP  T3 推送错误分支（未提供 --push-fail-base）')
    }

    // ── mock 实例：端点/字段补齐后前端自动启用 ──────────────────────────
    if (MOCK_BASE) {
      await cdp.goto(`${MOCK_BASE}/device/dev-693ea4`)
      await cdp.eval(PAGE_HELPERS)
      await cdp.waitFor(`document.body.innerText.includes('端点在位') || document.body.innerText.includes('端点缺失')`, 25000, 'mock 端点探测')
      const m2 = await cdp.eval(`(() => {
        const bs = window.__mini.exprButtons();
        return { n: bs.length, disabled: bs.filter(b=>b.disabled).length, tag: window.__mini.cmdTag() };
      })()`)
      record('T2 mock：端点上线 → 按钮启用', m2.tag === '端点在位' && m2.disabled === 0, JSON.stringify(m2))

      // 点「微笑」→ mock 202 → 前端提示指令已入队
      await cdp.eval(`(() => { const b = window.__mini.exprButtons().find(b => b.textContent.includes('微笑')); b.click(); return true })()`)
      await sleep(1200)
      const sent = await cdp.eval(`window.__mini.toastText()`)
      record('T2 mock：点击表情 → 202 入队提示', /指令已入队/.test(sent), sent || '(无 toast)')
      await cdp.shot(path.join(SHOT_DIR, 't2-mock-endpoint-ok.png'))

      const m4 = await cdp.eval(`(() => {
        const cb = window.__mini.checkboxByText('覆盖全局阈值');
        if (cb && !cb.classList.contains('n-checkbox--checked')) cb.click();
        return true;
      })()`)
      await sleep(600)
      const m4state = await cdp.eval(`(() => {
        const card = window.__mini.thresholdCard();
        const sens = window.__mini.sensitivityInput();
        const inputs = card ? [...card.querySelectorAll('input')] : [];
        const all = document.body.innerText;
        return { sensDisabled: !!sens?.disabled, hasSens: !!sens,
                 enabled: inputs.filter(i=>!i.disabled).length,
                 unsupportedAlert: all.includes('本次设备响应的') };
      })()`)
      record(
        'T4 mock：字段补齐 → 灵敏度启用（占位说明消失）',
        m4state.hasSens === true && m4state.sensDisabled === false && m4state.enabled === 5 && m4state.unsupportedAlert === false,
        JSON.stringify(m4state),
      )

      await cdp.goto(`${MOCK_BASE}/settings`)
      await cdp.eval(PAGE_HELPERS)
      await cdp.waitFor(`document.body.innerText.includes('随机台词气泡')`, 20000, 'mock 设置页')
      const m5 = await cdp.eval(`(() => {
        const card = window.__mini.cardByTitle('随机台词气泡');
        const devCard = window.__mini.cardByTitle('设备阈值');
        const sens = devCard ? [...devCard.querySelectorAll('.n-form-item')].find(i=>i.textContent.includes('IMU 灵敏度'))?.querySelector('input') : null;
        const ta = card ? [...card.querySelectorAll('textarea')][0] : null;
        return { tag: card ? (card.innerText.match(/配置段在位（真实读写）|服务端未返回 speech 段/) || ['no-card'])[0] : 'no-card',
                 sensDisabled: !!sens?.disabled, lines: ta ? ta.value : '' };
      })()`)
      record('T5 mock：speech 段在位 → 表单启用', /配置段在位/.test(m5.tag) && !m5.sensDisabled && /加油/.test(m5.lines), JSON.stringify(m5))
      await cdp.shot(path.join(SHOT_DIR, 't5-mock-speech.png'))

      // T5 mock：台词库改一行 → 保存（mock PUT 只回显、不落盘）→ toast + 请求体回显
      await cdp.eval(`(() => {
        const card = window.__mini.cardByTitle('随机台词气泡');
        const ta = card.querySelector('textarea');
        ta.value = 'mock 往返一'; ta.dispatchEvent(new Event('input', { bubbles: true }));
        return true;
      })()`)
      await sleep(300)
      await cdp.eval(`(() => { const b=[...document.querySelectorAll('button')].find(b=>b.textContent.includes('保存设置')); if(b) b.click(); return !!b })()`)
      await sleep(2000)
      const m5save = await cdp.eval(`window.__mini.toastText()`)
      record('T5 mock：PUT /admin/settings 台词往返（保存提示成功）', /设置已保存/.test(m5save), `toast=${m5save}`)

      // ── T1 mock：cookie 过期告警 + 网关降级原因（mock 注入 cookieStale=true/ageDays=9）──
      await cdp.goto(`${MOCK_BASE}/music`)
      await cdp.eval(PAGE_HELPERS)
      await cdp.waitFor(`(window.__mini.cardByTitle('QQ 音源')||{}).innerText?.includes('cookie')`, 25000, 'mock QQ 卡加载')
      const t1mock = await cdp.eval(`(() => {
        const card = window.__mini.cardByTitle('QQ 音源');
        const txt = card ? card.innerText : '';
        return { stale: /cookie 已保存 9 天（>7 天）可能已失效，请重新导入/.test(txt),
                 gwTag: (txt.match(/网关：[^\\n]*/) || [''])[0],
                 gwReason: txt.includes('未找到网关脚本'),
                 notInRepo: txt.includes('不在本仓库、镜像也未内置'),
                 ageCode: txt.includes('cookieAgeDays=9') && txt.includes('cookieStale=true') };
      })()`)
      record(
        'T1 mock：cookieStale=true/ageDays=9 → 黄色告警「已保存 9 天（>7 天）可能已失效，请重新导入」',
        t1mock.stale && t1mock.ageCode, JSON.stringify(t1mock).slice(0, 300),
      )
      record(
        'T1 mock：网关降级原因渲染（scriptFound=false → 未找到网关脚本 + 需服务端配好网关）',
        /网关：未找到网关脚本/.test(t1mock.gwTag) && t1mock.gwReason && t1mock.notInRepo,
        `tag=${t1mock.gwTag} 原因=${t1mock.gwReason} 不在仓库提示=${t1mock.notInRepo}`,
      )
      // T2 mock：关键词保存走既有 settings 通道（mock PUT 回显，不落盘）
      const kwMock = await cdp.eval(`(() => {
        const card = window.__mini.cardByTitle('QQ 音源');
        const inp = card.querySelector('input');
        inp.value = '久石让'; inp.dispatchEvent(new Event('input', { bubbles: true }));
        return inp.value;
      })()`)
      await sleep(300)
      await cdp.eval(`(() => { const b=[...window.__mini.cardByTitle('QQ 音源').querySelectorAll('button')].find(b=>b.textContent.includes('保存关键词')); if(b) b.click(); return !!b })()`)
      await sleep(2000)
      const kwToast = await cdp.eval(`window.__mini.toastText()`)
      record(
        'T2 mock：保存关键词 → PUT /admin/settings 全量回传 → toast 回显新值',
        /搜索关键词已保存：久石让/.test(kwToast), `输入=${kwMock} toast=${kwToast}`,
      )
      await cdp.shot(path.join(SHOT_DIR, 't1-t2-mock-qq-card.png'))

      // ── T3/T7 mock：收藏端点在位 → 探针判 ok + 自动合并服务端收藏 ──────────
      await cdp.goto(`${MOCK_BASE}/materials`)
      await cdp.waitFor(`document.body.innerText.includes('收藏已同步服务端') || document.body.innerText.includes('收藏仅本机')`, 25000, 'mock 收藏探测完成')
      const favMock = await cdp.eval(`(() => {
        const all = document.body.innerText;
        return { tag: (all.match(/收藏已同步服务端|收藏仅本机（端点缺失）|收藏仅本机/) || [''])[0],
                 hasSyncBtn: [...document.querySelectorAll('button')].some(b => b.textContent.includes('同步到服务端')),
                 merged: (localStorage.getItem('minipet.materials.favorites') || '').includes('200000100'),
                 mockTag: (all.match(/HTTP 200[^\\n]*|响应形态不符/) || [''])[0] };
      })()`)
      record(
        'T3/T7 mock：收藏端点在位 → tag「收藏已同步服务端」+ 同步按钮出现 + 服务端收藏已并入本机',
        favMock.tag === '收藏已同步服务端' && favMock.hasSyncBtn === true && favMock.merged === true,
        JSON.stringify(favMock),
      )
      await cdp.shot(path.join(SHOT_DIR, 't3-mock-favorites-synced.png'))

      // ── T6 mock：服务端按接口清单放行 bgm → 探针判「端点在位」、全键可点 ──
      await cdp.goto(`${MOCK_BASE}/music`)
      await cdp.eval(PAGE_HELPERS)
      await cdp.waitFor(
        `['端点在位','端点不支持 bgm','未探测到'].includes(window.__mini.bgmTag())`,
        25000,
        'mock bgm 控制卡探测',
      )
      const bgmMock = await cdp.eval(`(() => {
        const card = window.__mini.bgmCard();
        const bs = window.__mini.bgmButtons();
        return { tag: window.__mini.bgmTag(), btns: bs.length, disabled: bs.filter(b => b.disabled).length,
                 note: card ? (card.innerText.match(/哨兵值被拒[^\\n]*/) || [''])[0] : '' };
      })()`)
      record(
        'T6 mock：放行 bgm（哨兵被 value 白名单拒）→ 判端点在位 + 7 键全启用',
        bgmMock.tag === '端点在位' && bgmMock.btns === 7 && bgmMock.disabled === 0 && /哨兵值被拒/.test(bgmMock.note),
        JSON.stringify(bgmMock),
      )
      await cdp.shot(path.join(SHOT_DIR, 't6-mock-bgm-ok.png'))

      // 点「播放」→ mock 202 → 结果行显示真实请求体 + seq
      await cdp.eval(`(() => { window.__mini.bgmBtn('play').click(); return true })()`)
      await sleep(1500)
      const playRun = await cdp.eval(`(() => { const c = window.__mini.bgmCard(); return { toast: window.__mini.toastText(), text: c ? c.innerText : '' } })()`)
      record(
        'T6 mock：点播放 → POST {type:"bgm",value:"play"} → 202 seq 42',
        /播放已下发/.test(playRun.toast) && /"type":"bgm","value":"play"/.test(playRun.text) && /seq 42/.test(playRun.text),
        `toast=${playRun.toast} | ${(playRun.text.match(/HTTP 202[^\n]*/) || [''])[0]}`.slice(0, 240),
      )

      // 点「下发音量」→ mock 202（带 n）
      await cdp.eval(`(() => { window.__mini.bgmBtn('vol').click(); return true })()`)
      await sleep(1500)
      const volRun = await cdp.eval(`(() => { const c = window.__mini.bgmCard(); return { toast: window.__mini.toastText(), text: c ? c.innerText : '' } })()`)
      record(
        'T6 mock：点下发音量 → POST {type:"bgm",value:"vol",n:50} → 202 seq 42',
        /音量已下发/.test(volRun.toast) && /"value":"vol","n":\d+/.test(volRun.text) && /seq 42/.test(volRun.text),
        `toast=${volRun.toast} | ${(volRun.text.match(/HTTP 202[^\n]*/) || [''])[0]}`.slice(0, 240),
      )
      await cdp.shot(path.join(SHOT_DIR, 't6-mock-bgm-sent.png'))
    } else {
      console.log('SKIP  mock 实例检查（未提供 --mock-base）')
    }
  } finally {
    try { ws.close() } catch {}
    try { proc.kill('SIGKILL') } catch {}
  }

  const failed = results.filter((r) => !r.ok)
  console.log(`\n==== ${results.length - failed.length}/${results.length} PASS ====`)
  if (failed.length) {
    console.log('FAILED: ' + failed.map((f) => f.id).join(', '))
    process.exit(1)
  }
}

main().catch((e) => {
  console.error('冒烟脚本异常：', e)
  process.exit(2)
})
