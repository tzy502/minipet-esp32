/**
 * device-sim.mjs — 设备侧最小模拟器（自测用，不进生产）
 *
 * 为什么需要它：需求 E6/E8 的用户可见验收是「服务器页面点播放 → 设备真的收到 BGM 指令」。
 * 真机受网络/路由器限制时这条链在 CI 与本地都无法验证（Web 冒烟只能点到服务端收下指令
 * 为止）。本模拟器只做设备协议的三件事——hello 注册 / poll 长轮询取指令 / 回报收到的
 * 指令——足以把「Web 按钮 → 服务端 CommandQueue → 设备收到」整条链跑通并留证据。
 *
 * 用法：
 *   node Server/tools/device-sim.mjs --base http://127.0.0.1:38099 --seconds 90
 * 环境变量：SIM_UUID / SIM_NAME / SIM_SECONDS
 *
 * 输出：每收到一条指令打印 `[recv] type=... value=...`，退出时打印收到条数与汇总，
 * 便于脚本断言（grep '\[recv\]'）。
 */
const argv = process.argv.slice(2)
const arg = (n, d = null) => {
  const i = argv.indexOf(n)
  return i >= 0 && argv[i + 1] && !argv[i + 1].startsWith('--') ? argv[i + 1] : d
}
const BASE = (arg('--base', process.env.SIM_BASE || 'http://127.0.0.1:38099')).replace(/\/+$/, '')
const SECONDS = Number(arg('--seconds', process.env.SIM_SECONDS || 60))
const UUID = arg('--uuid', process.env.SIM_UUID || 'sim-esp32-0001')
const NAME = arg('--name', process.env.SIM_NAME || 'SIM-ESP32')

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

async function post(path, body) {
  const r = await fetch(BASE + path, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  })
  const text = await r.text()
  let json = null
  try { json = JSON.parse(text) } catch { /* 非 JSON 原样返回 */ }
  return { status: r.status, json, text }
}

async function main() {
  const hello = await post('/api/device/hello', {
    proto: 1,
    uuid: UUID,
    name: NAME,
    firmware: 'sim-0.0.1',
    fw: 'sim-0.0.1',
    profile: { w: 480, h: 480, shape: 'round', psram: 8, audio: true, touch: true, imu: true, rtc: true },
  })
  if (hello.status !== 200 || !hello.json) {
    console.error(`[sim] hello 失败 status=${hello.status} body=${hello.text.slice(0, 300)}`)
    process.exit(2)
  }
  const deviceId = hello.json.deviceId || hello.json.id
  console.log(`[sim] hello ok deviceId=${deviceId} proto=${hello.json.proto ?? '?'}`)

  const started = Date.now()
  let since = 0
  let got = 0
  let polls = 0
  const seen = []
  while ((Date.now() - started) / 1000 < SECONDS) {
    polls++
    let res
    try {
      res = await fetch(`${BASE}/api/device/poll?deviceId=${encodeURIComponent(deviceId)}&since=${since}`, {
        // 长轮询：服务端 hold 到有指令或超时；客户端超时给足
        signal: AbortSignal.timeout(30000),
      })
    } catch (e) {
      console.error(`[sim] poll 异常：${e.message}`)
      await sleep(1000)
      continue
    }
    if (!res.ok) {
      console.error(`[sim] poll HTTP ${res.status}`)
      await sleep(1000)
      continue
    }
    let body
    try { body = await res.json() } catch { continue }
    const cmds = Array.isArray(body?.commands) ? body.commands
      : (body?.cmd ? [body.cmd] : [])
    for (const c of cmds) {
      got++
      const desc = `type=${c.type ?? c.t} value=${c.value ?? c.v ?? ''} n=${c.n ?? ''}`.trim()
      console.log(`[recv] ${desc}`)
      seen.push(desc)
      if (typeof c.seq === 'number' && c.seq > since) since = c.seq
    }
    if (typeof body?.seq === 'number' && body.seq > since) since = body.seq
    // 心跳上报（让 Web 侧设备卡显示在线）
    await post('/api/device/event', {
      proto: 1, deviceId, type: 'heartbeat', ts: Date.now(),
      data: { online: true, a: 88, fw: 'sim-0.0.1' },
    }).catch(() => {})
  }
  console.log(`[sim] 结束：poll ${polls} 次，收到指令 ${got} 条`)
  if (seen.length) console.log(`[sim] 指令汇总：${seen.join(' | ')}`)
  process.exit(0)
}

main().catch((e) => { console.error('[sim] 致命：', e); process.exit(1) })
