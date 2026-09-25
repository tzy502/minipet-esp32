using System;
using System.Collections.Generic;
using MinipetServer.Models;

namespace MinipetServer.Services
{
    /// <summary>
    /// 服务端迁移（M1）：桌面版 CacheManager（磁盘缓存）未迁入，此为进程内占位实现——
    /// 方法签名与桌面版对齐（便于后续替换为磁盘/Redis 版），语义为「进程生命周期内有效的内存缓存」。
    /// TODO：M2+ 如需跨进程/磁盘缓存，按桌面版 Services/CacheManager.cs 补齐落盘逻辑。
    /// </summary>
    public class CacheManager
    {
        private readonly object _lock = new();
        private readonly Dictionary<string, (byte[] pngData, byte[] configData)> _spriteStrips = new();
        private readonly Dictionary<string, BalloonTiles> _balloonTiles = new();
        private readonly Dictionary<string, string> _mapHierarchy = new();

        public bool IsSpriteCached(string mobId, string action)
        {
            lock (_lock) return _spriteStrips.ContainsKey(Key(mobId, action));
        }

        public List<string> GetCachedActions(string mobId)
        {
            var result = new List<string>();
            lock (_lock)
            {
                foreach (var key in _spriteStrips.Keys)
                {
                    var parts = key.Split('|', 2);
                    if (parts.Length == 2 && parts[0] == mobId) result.Add(parts[1]);
                }
            }
            return result;
        }

        public void SaveSpriteStrip(string mobId, string action, byte[] pngData, byte[] configData)
        {
            lock (_lock) _spriteStrips[Key(mobId, action)] = (pngData, configData);
        }

        public void DeleteSpriteStrip(string mobId, string action)
        {
            lock (_lock) _spriteStrips.Remove(Key(mobId, action));
        }

        public (byte[] pngData, byte[] configData)? LoadSpriteStrip(string mobId, string action)
        {
            lock (_lock)
            {
                return _spriteStrips.TryGetValue(Key(mobId, action), out var v) ? v : null;
            }
        }

        public void SaveBalloonTiles(string balloonId, BalloonTiles tiles)
        {
            lock (_lock) _balloonTiles[balloonId] = tiles;
        }

        public BalloonTiles? LoadBalloonTiles(string balloonId)
        {
            lock (_lock)
            {
                return _balloonTiles.TryGetValue(balloonId, out var t) ? t : null;
            }
        }

        public void SaveMapHierarchy(string cacheKey, string json)
        {
            lock (_lock) _mapHierarchy[cacheKey] = json;
        }

        public string? LoadMapHierarchy(string cacheKey)
        {
            lock (_lock)
            {
                return _mapHierarchy.TryGetValue(cacheKey, out var json) ? json : null;
            }
        }

        private static string Key(string mobId, string action) => $"{mobId}|{action}";
    }
}
