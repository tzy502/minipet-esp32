import { defineStore } from 'pinia'
import { getDevice, listDevices, pair, updateDevice } from '../api/client'

/**
 * 设备列表（E4 首页卡片 / E13 设备表）。
 * - 5s 轮询在线态（IsOnline = 90s 心跳窗口，服务端口径）；
 * - 轮询失败不清空已有列表（保持卡片 + 顶部告警，不白屏）；
 * - 换装动作：PUT devices/{id} 显式传 petConfig（manifest rev+1，设备下次 poll 生效）。
 * - petConfigById：卡片缩略图要按「该设备真实装扮」合成（T1）——列表端点只回
 *   hasPetConfig 布尔，不含 petConfig 本体，故按设备懒加载详情并缓存（每设备一次），
 *   换装保存后置失效重取。
 */
export const useDevicesStore = defineStore('devices', {
  state: () => ({
    devices: [],
    loading: false, // 首载 loading；轮询静默
    error: '', // 最近一次请求错误（页面渲染 NAlert/NResult）
    lastOkAt: null,
    petConfigById: {}, // deviceId → petConfig(appearance JSON) | null（null = 无自定义装扮）
    _timer: null,
  }),
  getters: {
    onlineCount: (s) => s.devices.filter((d) => d.online).length,
  },
  actions: {
    async fetchAll({ silent = false } = {}) {
      if (!silent) this.loading = true
      try {
        const data = await listDevices()
        this.devices = data?.devices ?? []
        // hasPetConfig=false 的设备（含刚被清空装扮的）剔除缓存 → 缩略图回落 seed（默认宠物）
        const keep = new Set(this.devices.filter((d) => d.hasPetConfig).map((d) => d.deviceId))
        for (const k of Object.keys(this.petConfigById)) if (!keep.has(k)) delete this.petConfigById[k]
        this.error = ''
        this.lastOkAt = Date.now()
      } catch (e) {
        this.error = e?.serverError || '设备列表请求失败'
        if (!silent) this.devices = []
        throw e // 首载失败让页面 NResult 兜底；轮询失败吞掉由 error 字段呈现
      } finally {
        this.loading = false
      }
    },
    startPolling(intervalMs = 5000) {
      this.stopPolling()
      this._timer = setInterval(() => {
        this.fetchAll({ silent: true }).catch(() => {})
      }, intervalMs)
    },
    stopPolling() {
      if (this._timer) {
        clearInterval(this._timer)
        this._timer = null
      }
    },
    async pairDevice(code, name) {
      const data = await pair(code, name)
      this.fetchAll({ silent: true }).catch(() => {})
      return data?.device
    },
    /**
     * 取设备 petConfig（缩略图按真实装扮合成用，T1）。
     * 缓存命中直接返回（每设备一次请求，5s 轮询不会重复拉）；force=true 重新拉
     * （换装保存后调用）。设备不存在/请求失败时向上抛，由调用方回落 seed 缩略图。
     */
    async ensurePetConfig(deviceId, { force = false } = {}) {
      if (!deviceId) return null
      if (!force && deviceId in this.petConfigById) return this.petConfigById[deviceId]
      const data = await getDevice(deviceId)
      const petConfig = data?.device?.petConfig ?? null
      this.petConfigById = { ...this.petConfigById, [deviceId]: petConfig }
      return petConfig
    },
    /** 换宠换装：petConfig 传对象=覆盖，传 null=清空回默认宠物（E13 按设备隔离）。 */
    async applyPetConfig(deviceId, petConfig) {
      const data = await updateDevice(deviceId, { petConfig })
      await this.fetchAll({ silent: true }).catch(() => {})
      await this.ensurePetConfig(deviceId, { force: true }).catch(() => {})
      return data?.device
    },
  },
})
