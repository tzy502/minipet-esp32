import { defineStore } from 'pinia'
import { listDevices, pair, updateDevice } from '../api/client'

/**
 * 设备列表（E4 首页卡片 / E13 设备表）。
 * - 5s 轮询在线态（IsOnline = 90s 心跳窗口，服务端口径）；
 * - 轮询失败不清空已有列表（保持卡片 + 顶部告警，不白屏）；
 * - 换装动作：PUT devices/{id} 显式传 petConfig（manifest rev+1，设备下次 poll 生效）。
 */
export const useDevicesStore = defineStore('devices', {
  state: () => ({
    devices: [],
    loading: false, // 首载 loading；轮询静默
    error: '', // 最近一次请求错误（页面渲染 NAlert/NResult）
    lastOkAt: null,
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
    /** 换宠换装：petConfig 传对象=覆盖，传 null=清空回默认宠物（E13 按设备隔离）。 */
    async applyPetConfig(deviceId, petConfig) {
      const data = await updateDevice(deviceId, { petConfig })
      await this.fetchAll({ silent: true }).catch(() => {})
      return data?.device
    },
  },
})
