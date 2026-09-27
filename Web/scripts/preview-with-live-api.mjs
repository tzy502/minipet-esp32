/**
 * 本地预览/自测服务（仅开发用，不进生产构建）：
 *   node scripts/preview-with-live-api.mjs
 *
 * 作用：把 `npm run build` 产物（dist/）+ 真实服务端 API 拼成可跑的前端，
 * 无需 dotnet 也能验收 Web 行为（vite dev 的 proxy 目标是 localhost:8080，
 * 本脚本直接代理到真实服务端，避免为验收改动任何配置）。
 *
 * 环境变量：
 *   PORT        监听端口（默认 5199）
 *   API_TARGET  后端基址（默认 http://<NAS_IP>:38090）
 *   MINIPET_MOCK=1
 *       —— 模拟「服务端已补齐 T2/T4/T5/T6 缺口」后的响应，用于验证前端的
 *          「探测到就启用」分支（真实服务端目前没有这些端点/字段）：
 *          1) POST /api/admin/devices/{id}/command → 202 { ok, seq, type, value }
 *             bgm（T6）：value 白名单 play/pause/resume/stop/next/prev/vol，
 *             vol 需 n∈[0,100]；其余值 400「bgm 的 value 非法」——
 *             探测哨兵 __probe__ 正落此分支，用于验证「已认 bgm type 只是拒了 value」
 *             时前端判「端点在位」而非「端点缺失」。
 *          2) GET  /api/admin/settings → 真实响应 + device.imuSensitivity + speech 段
 *          3) GET  /api/admin/devices/{id} → 真实响应 + thresholds.imuSensitivity
 *             （服务端真加了字段时，详情页与设置页都会带出来）
 *          4) PUT  /api/admin/settings → 200 回显（不落盘，绝不动真实服务端配置）
 *   MOCK_PUSH_FAIL=400|404|503
 *       —— 故障注入：POST /api/admin/devices/{id}/push 直接回该状态码 + 服务端同款
 *          { "error": ... } 响应体，用于在真实浏览器里验证前端推送的错误分支
 *          （真实服务端的 400/404 取决于入参与设备是否存在，UI 上无法自然构造）。
 */
import http from 'node:http'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const DIST = path.resolve(__dirname, '..', 'dist')
const PORT = Number(process.env.PORT || 5199)
const API_TARGET = process.env.API_TARGET || 'http://<NAS_IP>:38090'
const MOCK = process.env.MINIPET_MOCK === '1'
const MOCK_PUSH_FAIL = Number(process.env.MOCK_PUSH_FAIL || 0)

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
}

async function readBody(req) {
  const chunks = []
  for await (const c of req) chunks.push(c)
  return Buffer.concat(chunks)
}

function send(res, status, body, headers = {}) {
  res.writeHead(status, headers)
  res.end(body)
}

/** 代理到真实服务端（含 /api/admin/thumb 的 PNG 字节流）。 */
async function proxy(req, res, url) {
  const target = API_TARGET + url.pathname + url.search
  const init = { method: req.method, headers: { 'content-type': req.headers['content-type'] || 'application/json' } }
  if (req.method !== 'GET' && req.method !== 'HEAD') init.body = await readBody(req)
  try {
    const r = await fetch(target, init)
    const buf = Buffer.from(await r.arrayBuffer())
    const ct = r.headers.get('content-type') || 'application/octet-stream'
    send(res, r.status, buf, { 'content-type': ct })
  } catch (e) {
    send(res, 502, JSON.stringify({ error: `proxy failed: ${e.message}` }), { 'content-type': 'application/json' })
  }
}

function serveStatic(res, pathname) {
  const rel = pathname === '/' ? 'index.html' : pathname.replace(/^\/+/, '')
  let file = path.join(DIST, rel)
  if (!file.startsWith(DIST) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    file = path.join(DIST, 'index.html') // SPA fallback
  }
  const body = fs.readFileSync(file)
  send(res, 200, body, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' })
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://127.0.0.1:${PORT}`)

  if (!url.pathname.startsWith('/api/')) return serveStatic(res, url.pathname)

  // 故障注入：推送错误分支（400/404/503）
  if (MOCK_PUSH_FAIL && /^\/api\/admin\/devices\/[^/]+\/push$/.test(url.pathname) && req.method === 'POST') {
    const msg =
      MOCK_PUSH_FAIL === 400 ? 'kind 必须是 map 或 npc'
      : MOCK_PUSH_FAIL === 404 ? `设备不存在：${decodeURIComponent(url.pathname.split('/')[4] || '')}`
      : 'WZ 未加载（到「设置」页配置后重试）'
    return send(res, MOCK_PUSH_FAIL, JSON.stringify({ error: msg, injected: true }), { 'content-type': 'application/json' })
  }

  if (MOCK) {
    // 1) 设备指令端点（T2/T5）：约定的目标形态
    const cmd = url.pathname.match(/^\/api\/admin\/devices\/([^/]+)\/command$/)
    if (cmd && req.method === 'POST') {
      const body = JSON.parse((await readBody(req)).toString() || '{}')
      if (!body.type || !body.value) {
        return send(res, 400, JSON.stringify({ error: 'type 与 value 必填' }), { 'content-type': 'application/json' })
      }
      // 1b) bgm（T6）：模拟「服务端已按接口清单放行 bgm」后的形态——
      //     value 白名单 + vol 走 n；哨兵值 __probe__ 落到 value 非法分支（400），
      //     前端必须据此判定「bgm 这个 type 已认」→ 端点在位（而非端点缺失）。
      if (body.type === 'bgm') {
        const values = ['play', 'pause', 'resume', 'stop', 'next', 'prev', 'vol']
        if (!values.includes(body.value)) {
          return send(res, 400, JSON.stringify({ error: `bgm 的 value 非法：${body.value}（可用：${values.join('/')}）`, mocked: true }), {
            'content-type': 'application/json',
          })
        }
        if (body.value === 'vol' && !(body.n >= 0 && body.n <= 100)) {
          return send(res, 400, JSON.stringify({ error: 'bgm vol 需要 n∈[0,100]', mocked: true }), { 'content-type': 'application/json' })
        }
        return send(res, 202, JSON.stringify({ ok: true, seq: 42, type: 'bgm', value: body.value, n: body.n, mocked: true }), {
          'content-type': 'application/json',
        })
      }
      return send(res, 202, JSON.stringify({ ok: true, seq: 42, type: body.type, value: body.value, mocked: true }), {
        'content-type': 'application/json',
      })
    }
    // 2) 设置读：真实响应 + 补齐 T4/T5 字段
    if (url.pathname === '/api/admin/settings' && req.method === 'GET') {
      const r = await fetch(API_TARGET + '/api/admin/settings')
      const data = await r.json()
      data.config = data.config || {}
      data.config.device = { ...(data.config.device || {}), imuSensitivity: 1.0 }
      data.config.speech = { enabled: true, idleSec: 60, lines: ['今天也要加油哦', '摸摸头～'] }
      data.note = `${data.note || ''}（MINIPET_MOCK：已模拟 device.imuSensitivity + speech 段）`
      return send(res, 200, JSON.stringify(data), { 'content-type': 'application/json' })
    }
    // 3) 设备详情：真实响应 + thresholds.imuSensitivity（服务端加字段后详情页会带出来）
    const devDetail = url.pathname.match(/^\/api\/admin\/devices\/([^/]+)$/)
    if (devDetail && req.method === 'GET') {
      const r = await fetch(API_TARGET + url.pathname)
      if (r.status !== 200) return send(res, r.status, Buffer.from(await r.arrayBuffer()), { 'content-type': 'application/json' })
      const data = await r.json()
      if (data?.device?.thresholds) data.device.thresholds = { ...data.device.thresholds, imuSensitivity: 1.0 }
      return send(res, 200, JSON.stringify(data), { 'content-type': 'application/json' })
    }
    // 4) 设置写：回显，不落盘
    if (url.pathname === '/api/admin/settings' && req.method === 'PUT') {
      const body = JSON.parse((await readBody(req)).toString() || '{}')
      return send(res, 200, JSON.stringify({ ok: true, config: body, wzPathExists: true, mocked: true }), {
        'content-type': 'application/json',
      })
    }
  }

  return proxy(req, res, url)
})

server.listen(PORT, '127.0.0.1', () => {
  console.log(`[preview] dist=${DIST}`)
  console.log(`[preview] http://127.0.0.1:${PORT}/  → /api 代理到 ${API_TARGET}${MOCK ? '（MINIPET_MOCK=1）' : ''}`)
})
