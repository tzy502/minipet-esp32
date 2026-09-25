<script setup>
import { h, computed } from 'vue'
import { useRoute, useRouter, RouterLink } from 'vue-router'
import {
  NConfigProvider,
  NLayout,
  NLayoutSider,
  NLayoutHeader,
  NLayoutContent,
  NMenu,
  zhCN,
  dateZhCN,
  NMessageProvider,
  NDialogProvider,
  NNotificationProvider,
} from 'naive-ui'
import { useMenu } from './menu'

const route = useRoute()
const router = useRouter()

// 侧边菜单五项（设备详情页不在菜单，通过卡片进入）
const { menuOptions, activeKey } = useMenu({ route, h, RouterLink })

const pageTitle = computed(() => route.meta?.title ?? '')

function goHome() {
  router.push('/')
}
</script>

<template>
  <n-config-provider :locale="zhCN" :date-locale="dateZhCN" class="app-root">
    <n-message-provider>
      <n-dialog-provider>
        <n-notification-provider>
          <n-layout position="absolute">
            <n-layout-sider
              bordered
              collapse-mode="width"
              :collapsed-width="0"
              :width="200"
              show-trigger="bar"
              :native-scrollbar="false"
              class="app-sider"
            >
              <div class="brand" @click="goHome">
                <span class="brand-name">MiniPet</span>
                <span class="brand-sub">ESP32 管理台</span>
              </div>
              <n-menu :options="menuOptions" :value="activeKey" :root-indent="18" />
            </n-layout-sider>
            <n-layout>
              <n-layout-header bordered class="app-header">
                <span class="app-title">{{ pageTitle }}</span>
              </n-layout-header>
              <n-layout-content class="app-content" :native-scrollbar="false">
                <router-view />
              </n-layout-content>
            </n-layout>
          </n-layout>
        </n-notification-provider>
      </n-dialog-provider>
    </n-message-provider>
  </n-config-provider>
</template>

<style>
html,
body,
#app {
  height: 100%;
  margin: 0;
}
.app-root {
  height: 100%;
}
.brand {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 18px 12px 10px;
  cursor: pointer;
  user-select: none;
}
.brand-name {
  font-size: 20px;
  font-weight: 700;
  letter-spacing: 1px;
}
.brand-sub {
  font-size: 12px;
  opacity: 0.55;
  margin-top: 2px;
}
.app-header {
  height: 48px;
  display: flex;
  align-items: center;
  padding: 0 24px;
}
.app-title {
  font-size: 15px;
  font-weight: 600;
}
.app-content {
  padding: 20px 24px 40px;
}
.menu-emoji {
  font-size: 15px;
  line-height: 1;
}
</style>
