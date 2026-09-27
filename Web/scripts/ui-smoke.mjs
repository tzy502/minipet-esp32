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
 *   T2 服务端无指令端点 → 25 个表情按钮全部 disabled + 端点缺失提示（mock 实例下 → 端点在位 + 可点）
 *   T3 素材页/设备页推送入口与 202/错误分支
 *   T4 IMU 灵敏度：服务端无字段 → 输入禁用且不随 PUT 下发（mock 实例下 → 启用）
 *   T5 随机台词气泡：服务端无 speech 段 → 禁用占位（mock 实例下 → 配置段在位）
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

    // ── T2/T4：设备详情（真实服务端：端点缺失 / 字段缺失）────────────────
    await cdp.goto(`${BASE}/device/dev-693ea4`)
    await cdp.eval(PAGE_HELPERS)
    await cdp.waitFor(`window.__mini.exprButtons().length === 25`, 25000, '25 个表情按钮渲染')
    await cdp.waitFor(`document.body.innerText.includes('端点缺失') || document.body.innerText.includes('端点在位')`, 20000, '端点探测完成')
    const expr = await cdp.eval(`(() => {
      const bs = window.__mini.exprButtons();
      return { n: bs.length, disabled: bs.filter(b => b.disabled).length, tag: (document.body.innerText.match(/端点缺失|端点在位|端点入参不符|未探测到/)||[''])[0],
               alert: document.body.innerText.includes('服务端尚未提供设备指令端点') };
    })()`)
    record(
      'T2 端点缺失 → 25 表情按钮全禁用 + 提示',
      expr.n === 25 && expr.disabled === 25 && expr.tag === '端点缺失' && expr.alert,
      `按钮=${expr.n} 禁用=${expr.disabled} 状态=${expr.tag} 提示=${expr.alert}`,
    )
    await cdp.shot(path.join(SHOT_DIR, 't2-detail-endpoint-missing.png'))

    // 打开「覆盖全局阈值」→ 前 4 项可编辑，灵敏度仍禁用（服务端无字段）
    const sens1 = await cdp.eval(`(() => {
      const cb = window.__mini.checkboxByText('覆盖全局阈值'); if (cb) cb.click();
      return true;
    })()`)
    await sleep(500)
    const thState = await cdp.eval(`(() => {
      const card = window.__mini.thresholdCard();
      const inputs = [...card.querySelectorAll('input')];
      const sens = window.__mini.sensitivityInput();
      return { total: inputs.length, enabled: inputs.filter(i=>!i.disabled).length, sensDisabled: !!sens?.disabled, sensValue: sens?.value, hint: card.innerText.includes('没有灵敏度字段') || card.innerText.includes('暂无 imuSensitivity') };
    })()`)
    record(
      'T4 服务端无 imuSensitivity → 灵敏度输入禁用+说明',
      sens1 && thState.sensDisabled === true && thState.enabled >= 4 && thState.hint,
      JSON.stringify(thState),
    )
    await cdp.shot(path.join(SHOT_DIR, 't4-detail-sensitivity.png'))

    // ── T3：设备详情页推送卡片存在 ───────────────────────────────────────
    const pushCard = await cdp.eval(`(() => {
      const c = window.__mini.cardByTitle('素材推送');
      return { exists: !!c, text: c ? c.innerText.slice(0, 200) : '' };
    })()`)
    record('T3 设备详情有推送卡片', pushCard.exists && /push/.test(pushCard.text), pushCard.text.replace(/\n/g, ' ').slice(0, 110))

    // ── T5：设置页台词占位 + T4 设置页灵敏度占位 ─────────────────────────
    await cdp.goto(`${BASE}/settings`)
    await cdp.eval(PAGE_HELPERS)
    await cdp.waitFor(`document.body.innerText.includes('随机台词气泡')`, 20000, '设置页加载')
    const st = await cdp.eval(`(() => {
      const card = window.__mini.cardByTitle('随机台词气泡');
      const devCard = window.__mini.cardByTitle('设备阈值');
      const sens = devCard ? [...devCard.querySelectorAll('.n-form-item')].find(i=>i.textContent.includes('IMU 灵敏度'))?.querySelector('input') : null;
      return { tag: card ? (card.innerText.includes('待服务端支持') ? '待服务端支持' : '配置段在位') : 'no-card',
               needAlert: document.body.innerText.includes('当前服务端没有台词配置段'),
               sensDisabled: !!sens?.disabled, devTag: devCard?.innerText.includes('没有灵敏度字段') || devCard?.innerText.includes('暂无 imuSensitivity') };
    })()`)
    record('T5 设置页台词禁用占位', st.tag === '待服务端支持' && st.needAlert, `tag=${st.tag} 需求说明=${st.needAlert}`)
    record('T4 设置页灵敏度禁用占位', st.sensDisabled === true && st.devTag === true, `disabled=${st.sensDisabled} 说明=${st.devTag}`)
    await cdp.shot(path.join(SHOT_DIR, 't45-settings-placeholder.png'))

    // ── T3：素材页推送入口（map 可推 / mob 禁用）─────────────────────────
    await cdp.goto(`${BASE}/materials`)
    await cdp.waitFor(`document.querySelectorAll('button.push').length > 0`, 25000, '素材页推送按钮')
    const mat = await cdp.eval(`(() => {
      const btns = [...document.querySelectorAll('button.push')];
      return { n: btns.length, enabled: btns.filter(b=>!b.disabled).length, disabled: btns.filter(b=>b.disabled).length };
    })()`)
    record('T3 素材页 map tab 推送按钮可点', mat.n > 0 && mat.disabled === 0 && mat.enabled === mat.n, JSON.stringify(mat))

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
        return { n: bs.length, disabled: bs.filter(b=>b.disabled).length,
                 tag: (document.body.innerText.match(/端点缺失|端点在位|端点入参不符|未探测到/)||[''])[0] };
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
                 unsupportedAlert: all.includes('没有灵敏度字段') };
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
        return { tag: card ? (card.innerText.includes('待服务端支持') ? '待服务端支持' : '配置段在位') : 'no-card',
                 sensDisabled: !!sens?.disabled, lines: ta ? ta.value : '' };
      })()`)
      record('T5 mock：speech 段在位 → 表单启用', m5.tag === '配置段在位' && !m5.sensDisabled && /加油/.test(m5.lines), JSON.stringify(m5))
      await cdp.shot(path.join(SHOT_DIR, 't5-mock-speech.png'))
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
