# syntax=docker/dockerfile:1
# =====================================================================
# MiniPet-ESP32 单容器镜像（E3 定稿，docs/ai/deployment-design.md 顶部横幅）
#
#   api（ASP.NET Core 9）直接托管：Vue 产物（wwwroot）+ API + BGM 流
#   QQ 音乐网关 = 本容器内的 node 子进程 → 最终层附带 node 运行时
#
# 多阶段：
#   webbuild    node:22-alpine      构建 Web/（无 package.json 时自动跳过）
#   serverbuild sdk:9.0             dotnet publish MinipetServer
#   final       aspnet:9.0          运行时（libfontconfig1 供 SkiaSharp）
#
# 注意：WZ 数据不进镜像（部署时 :ro 挂载 /wz）；多架构（amd64/arm64）
#   由 buildx 分别构建本文件，publish 不指定 RID（可移植产物含双架构
#   SkiaSharp native，deps.json 运行时自动选择）。
# =====================================================================

# ---------- Stage 1: Web 构建 ----------
# 说明：Dockerfile 无法条件 COPY，故整目录 COPY（node_modules/dist 已被
# .dockerignore 排除）；Web/ 目前还没有 package.json（前端并行开发中），
# 下面的 RUN 会在缺失时放一个占位首页而不是让构建失败。
# package.json 出现后此文件零改动。
# 前端构建固定跑在构建机原生架构（$BUILDPLATFORM）：buildx 跨架构时 QEMU 模拟下
# Node/V8 JIT 偶发 SIGILL（exit 132，2026-09-26 dfe6b79 CI 实证）；产物是纯静态文件
# 与目标架构无关，拷进 final stage 即可。
FROM --platform=$BUILDPLATFORM node:22-alpine AS webbuild
WORKDIR /src
COPY Web/ ./
RUN set -eux; \
    if [ ! -f package.json ]; then \
      echo 'Web/ 无 package.json，跳过前端构建（放占位首页）'; \
      mkdir -p dist; \
      printf '<!doctype html><meta charset="utf-8"><title>MiniPet</title><p>Web UI not built yet.' > dist/index.html; \
    else \
      if [ -f package-lock.json ]; then npm ci; else npm install; fi; \
      npm run build --if-present; \
      [ -d dist ] || { \
        echo '前端构建未产出 dist/，放占位首页'; \
        mkdir -p dist; \
        printf '<!doctype html><meta charset="utf-8"><title>MiniPet</title><p>Web UI build produced no dist/.' > dist/index.html; \
      }; \
    fi

# ---------- Stage 2: Server 发布 ----------
FROM mcr.microsoft.com/dotnet/sdk:9.0 AS serverbuild
WORKDIR /src
# 先只拷 sln + 各 csproj 做 restore，命中层缓存；Server/ 新增项目时需同步补一行
# 注意：COPY 目标必须保留 Server/ 目录结构（./ 会拍平到 /src 根，
# 导致 dotnet restore Server/Minipet.sln 找不到文件 —— MSB1009）
COPY Server/Minipet.sln Server/
COPY Server/MinipetServer/MinipetServer.csproj Server/MinipetServer/
COPY Server/MinipetServer.Tests/MinipetServer.Tests.csproj Server/MinipetServer.Tests/
COPY Server/tools/Exporter/Exporter.csproj Server/tools/Exporter/
RUN dotnet restore Server/Minipet.sln
COPY Server/ ./Server/
RUN dotnet publish Server/MinipetServer/MinipetServer.csproj \
      -c Release \
      -o /app/publish
# 问题1 防复发断言（2026-09-26）：linux native 必须在 publish 产物里，
# 缺失直接构建失败（此前 libSkiaSharp.so 缺失导致线上缩略图全 500）
RUN set -eux; \
    ls /app/publish/runtimes/linux-x64/native/libSkiaSharp.so; \
    ls /app/publish/runtimes/linux-arm64/native/libSkiaSharp.so

# ---------- Stage 3: 运行时 ----------
FROM mcr.microsoft.com/dotnet/aspnet:9.0 AS final
WORKDIR /app

# libfontconfig1 ：SkiaSharp 文本渲染在 Linux 上的 native 依赖（正确包名 libfontconfig1）
# libstdc++6 / libatomic1 ：node 二进制的动态链接依赖（arm64 也需要 libatomic1）
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libfontconfig1 \
        libstdc++6 \
        libatomic1 \
    && rm -rf /var/lib/apt/lists/*

# node 运行时（QQ 网关子进程用）。
# node:22-bookworm-slim 中 node 装在 /usr/local/bin/node（nodejs.org 官方
# 二进制，libnode 已链入、无独立 libnode.so，仅需上面安装的系统库），
# 这里统一放置到 /usr/bin/node 供 QqGatewayProcess 拉起。
COPY --from=node:22-bookworm-slim /usr/local/bin/node /usr/bin/node

# Vue 构建产物 → wwwroot（api 直接托管，同源无 CORS/nginx）
COPY --from=webbuild /src/dist ./wwwroot

# .NET 发布产物
COPY --from=serverbuild /app/publish ./

ENV TZ=Asia/Shanghai

# aspnet:9.0 基础镜像已设 ASPNETCORE_HTTP_PORTS=8080，容器内监听 8080；
# 对外端口由部署层映射（compose: "${MINIPET_PORT:-38090}:8080"）
EXPOSE 8080

ENTRYPOINT ["dotnet", "MinipetServer.dll"]
