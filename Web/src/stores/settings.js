import { defineStore } from 'pinia'
import { getSettings, putSettings, validateWzPath } from '../api/client'

/**
 * 全局设置（E3 双通道之 Web 通道）。
 * config 即后端 appsettings.json 的 camelCase 投影（wz/qqMusic/bgm/device/clock），
 * PUT 回传同构 JSON（未在页面编辑的段——如 qqMusic/bgm——原样带回，不丢字段）。
 */
export const useSettingsStore = defineStore('settings', {
  state: () => ({
    config: null, // 加载到的完整配置
    wzPathExists: null, // GET 附带：当前配置里 WZ 路径是否存在
    note: '',
    loading: false,
    saving: false,
    error: '',
    validate: { status: 'idle', message: '' }, // idle | checking | ok | fail | unsupported
  }),
  actions: {
    async load() {
      this.loading = true
      this.error = ''
      try {
        const data = await getSettings()
        this.config = data?.config ?? null
        this.wzPathExists = !!data?.wzPathExists
        this.note = data?.note ?? ''
      } catch (e) {
        this.error = e?.serverError || '设置加载失败'
        throw e
      } finally {
        this.loading = false
      }
    },
    async save(nextConfig) {
      this.saving = true
      try {
        const data = await putSettings(nextConfig)
        this.config = data?.config ?? nextConfig
        this.wzPathExists = !!data?.wzPathExists
        return data
      } finally {
        this.saving = false
      }
    },
    async validatePath(path) {
      this.validate = { status: 'checking', message: '' }
      try {
        const r = await validateWzPath(path)
        this.validate = r.supported
          ? { status: r.ok ? 'ok' : 'fail', message: r.message }
          : { status: 'unsupported', message: '独立校验端点未上线：保存时服务端会对 WZ 路径做存在性硬校验' }
        return r
      } catch (e) {
        this.validate = { status: 'fail', message: e?.serverError || '校验请求失败' }
        throw e
      }
    },
  },
})
