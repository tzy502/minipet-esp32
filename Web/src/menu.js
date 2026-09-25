/** App 侧边菜单装配（拆出 SFC 保持 App.vue 清爽）。 */
import { computed } from 'vue'

const ICONS = {
  dashboard: '🐾',
  materials: '🗺️',
  paperdoll: '🧵',
  music: '🎵',
  settings: '⚙️',
}

export function useMenu({ route, h, RouterLink }) {
  const items = [
    { key: '/', label: '设备总览', icon: ICONS.dashboard },
    { key: '/materials', label: '素材浏览', icon: ICONS.materials },
    { key: '/paperdoll', label: '纸娃娃编辑', icon: ICONS.paperdoll },
    { key: '/music', label: '曲库管理', icon: ICONS.music },
    { key: '/settings', label: '设置', icon: ICONS.settings },
  ]

  const menuOptions = items.map((it) => ({
    key: it.key,
    icon: () => h('span', { class: 'menu-emoji' }, it.icon),
    label: () => h(RouterLink, { to: it.key }, { default: () => it.label }),
  }))

  // 设备详情页归到「设备总览」高亮
  const activeKey = computed(() => (route.path.startsWith('/device/') ? '/' : route.path))

  return { menuOptions, activeKey }
}
