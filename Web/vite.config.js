import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 开发期 /api → 本地 ASP.NET Core（Server/MinipetServer）；生产同源托管进 wwwroot，无代理。
// 注意：dotnet run 默认 launchSettings 端口为 5059；若用 MINIPET_PORT=8080 起（或容器内）则走 8080。
export default defineConfig({
  plugins: [vue()],
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
    },
  },
})
