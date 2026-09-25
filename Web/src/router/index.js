import { createRouter, createWebHistory } from 'vue-router'

// 六路由（软件设计第三节 / E4）：五页常驻菜单 + 设备详情（从设备卡片进入）
const routes = [
  { path: '/', name: 'dashboard', component: () => import('../views/DashboardView.vue'), meta: { title: '设备总览' } },
  { path: '/materials', name: 'materials', component: () => import('../views/MaterialsView.vue'), meta: { title: '素材浏览' } },
  { path: '/paperdoll', name: 'paperdoll', component: () => import('../views/PaperdollView.vue'), meta: { title: '纸娃娃编辑' } },
  { path: '/music', name: 'music', component: () => import('../views/MusicView.vue'), meta: { title: '曲库管理' } },
  { path: '/settings', name: 'settings', component: () => import('../views/SettingsView.vue'), meta: { title: '设置' } },
  { path: '/device/:id', name: 'device-detail', component: () => import('../views/DeviceDetailView.vue'), props: true, meta: { title: '设备详情' } },
  { path: '/:pathMatch(.*)*', redirect: '/' },
]

const router = createRouter({
  history: createWebHistory(),
  routes,
})

router.afterEach((to) => {
  document.title = to.meta?.title ? `${to.meta.title} · MiniPet` : 'MiniPet 管理台'
})

export default router
