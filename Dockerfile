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
# 本次前端产物的指纹（final stage 断言 /app/wwwroot 就是这一份用；只传指纹文件，不重复拷 dist）。
# 格式自己 printf 定死（"<sha256> <相对路径>"）：webbuild 是 busybox sha256sum、final 是
# GNU coreutils sha256sum，两者输出空格数不必依赖 —— 只取哈希列，两边逐字节可比。
RUN set -eux; \
    cd dist; \
    for f in $(find . -type f | sort); do printf '%s %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done > /tmp/dist.sha256; \
    wc -l < /tmp/dist.sha256

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
# 问题1 同类防复发（2026-09-27 层序修复）：publish 会把源码树里的 wwwroot
# （Server/MinipetServer/wwwroot，.gitignore 的本地构建残留，被上面 COPY Server/ 带进
# 构建上下文）当内容一起发布出去。本机实证（同版本源码）：
#   dotnet publish Server/MinipetServer/MinipetServer.csproj -c Debug -o /tmp/mp-publish-check
#   → /tmp/mp-publish-check/wwwroot/index.html 引 assets/index-B3Oa4XgL.js（旧代）
# 原 Dockerfile 顺序「先 COPY dist 到 ./wwwroot，再 COPY publish ./」时，这一坨旧 UI 会
# 目录合并覆盖掉本次前端构建产物 → 线上容器跑旧 Web UI。这里从源头剔除，
# 保证 publish 产物永远不含 wwwroot（final stage 的断言是第二道防线）。
RUN set -eux; \
    rm -rf /app/publish/wwwroot; \
    test ! -e /app/publish/wwwroot

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

# .NET 发布产物（上面 serverbuild 已剔除 wwwroot —— 防止旧 UI 目录合并覆盖新产物）
COPY --from=serverbuild /app/publish ./

# Vue 构建产物 → wwwroot（api 直接托管，同源无 CORS/nginx）
COPY --from=webbuild /src/dist ./wwwroot
# 本次前端产物指纹（webbuild 产出；只传指纹文件，不重复拷 dist 膨胀镜像）
COPY --from=webbuild /tmp/dist.sha256 /tmp/dist.sha256

# ── 构建期断言 [assert-wwwroot]（防复发；风格同 serverbuild 的 SkiaSharp 断言）──────
# ① wwwroot/index.html 引用的 JS 必须在本次前端构建产物（/tmp/dist.sha256 指纹表）里真实存在：
#    旧 UI 被目录合并回来时，index.html 指向的 assets/index-<旧hash>.js 在新产物里不存在 → 构建失败；
# ② wwwroot 的文件集 + 每个文件的 sha256 必须与本次 dist 指纹表逐字节一致（多一个旧代文件即失败）；
# ③ 占位首页路径（Web/ 无 package.json / 构建未产出 dist）显式打印警告，不做 JS 断言但 ② 仍生效。
RUN set -eux; \
    ref="$(grep -oE 'assets/[A-Za-z0-9._-]+\.js' ./wwwroot/index.html | head -n1 || true)"; \
    if [ -n "$ref" ]; then \
      ls -l "./wwwroot/$ref"; \
      grep -F "$ref" /tmp/dist.sha256; \
    else \
      echo '警告：wwwroot/index.html 未引用 assets/*.js —— 占位首页路径（本次前端构建无产物）'; \
    fi; \
    (cd ./wwwroot && for f in $(find . -type f | sort); do printf '%s %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done) > /tmp/wwwroot.sha256; \
    echo "本次前端产物文件数：$(wc -l < /tmp/dist.sha256)"; \
    test "$(cat /tmp/dist.sha256)" = "$(cat /tmp/wwwroot.sha256)"; \
    echo 'wwwroot 断言通过：镜像内 UI = 本次前端构建产物'; \
    rm -f /tmp/wwwroot.sha256

ENV TZ=Asia/Shanghai

# aspnet:9.0 基础镜像已设 ASPNETCORE_HTTP_PORTS=8080，容器内监听 8080；
# 对外端口由部署层映射（compose: "${MINIPET_PORT:-38090}:8080"）
EXPOSE 8080

ENTRYPOINT ["dotnet", "MinipetServer.dll"]
