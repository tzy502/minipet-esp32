/**
 * mdns_discover.h — mDNS 服务发现：设备自动找到服务端（E14）
 *
 * 需求原文（E14）：「mDNS：设备可发现服务端（配网页手输地址的补充，可选启用）」。
 *
 * 定位：**手输地址优先**。只有 NVS "srv_url" 为空、或已配置但本次连不上时，
 * 才用 mDNS 找一次；找到即作为本次运行的兜底地址（不写回 NVS，避免把
 * 手输地址覆盖掉、也避免 DHCP 换 IP 后永远用旧值）。
 *
 * 服务端侧需广告的服务名（接口需求，见 docs/ai/keys-touch-handoff.md）：
 *   _minipet._tcp.local   （PTR/SRV/TXT；TXT 建议 path=/api/device）
 *
 * 约束：
 *   - 超时短（默认 ~2.2s，最多两次查询），失败静默返回 false
 *   - 不新建常驻任务（mdns 组件自带任务，用完 mdns_free 释放）
 *   - 无 PSRAM/无内部堆常驻占用（用完即还）
 */
#ifndef MP_MDNS_DISCOVER_H
#define MP_MDNS_DISCOVER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 服务名（服务端必须广告这个；设备按此查询） */
#define MP_MDNS_SERVICE   "_minipet"
#define MP_MDNS_PROTO     "_tcp"

/**
 * mDNS 发现服务端一次。
 * @param url_out  成功后写入 "http://<ip>:<port>"（NUL 结尾，无尾斜杠）
 * @param cap      url_out 容量（>=32）
 * @return true = 找到（已写 url_out 并打一条 INFO 日志）；false = 未找到（静默）
 *
 * 调用前提：STA 已拿到 IP（拿到 IP 前 mDNS 查询无出口，直接返回 false）。
 * 阻塞最长 ~2.2s（两次查询各 1.1s），只应在启动自检的服务端探测路径调用。
 */
bool mp_mdns_discover_server(char *url_out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MP_MDNS_DISCOVER_H */
