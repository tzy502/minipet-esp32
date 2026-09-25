# Server — PC 服务端

技术栈：C# / .NET（复用桌面版 mapleStoryMiniPet 的 Services 核心 10.4K 行）

职责：
- WZ 数据解析 → 布局导出器（部件图 RGB565 + origin / 布局表 / 地图烘焙 / 视差条带）
- 设备管理（manifest 版本 / 素材包 / 状态指令 / 设备表）
- BGM 流式下发（IMusicSource 可插拔：WZ 曲库 / QQ 音乐）
- Web API + 静态托管（Vue 前端构建产物）

迁移说明：从桌面版纯 copy Services 层，清理 17 处 Avalonia Dispatcher 引用后纳入本仓库。
