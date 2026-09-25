using System.IO;
using System.Text.Json;

namespace MinipetServer.Device;

/// <summary>JSON 持久化小工具：统一序列化口径 + 原子写（temp + File.Move）。</summary>
public static class StorageUtil
{
    /// <summary>仓库内 JSON 读写的统一选项（camelCase、缩进、忽略 null）。</summary>
    public static readonly JsonSerializerOptions JsonOpts = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
    };

    /// <summary>原子写文本：先写临时文件再 File.Move 覆盖，读者永远不会看到半截文件。</summary>
    public static void AtomicWriteAllText(string path, string contents)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var tmp = path + ".tmp-" + Guid.NewGuid().ToString("N")[..8];
        try
        {
            File.WriteAllText(tmp, contents);
            File.Move(tmp, path, overwrite: true);
        }
        finally
        {
            try { if (File.Exists(tmp)) File.Delete(tmp); } catch { /* best effort */ }
        }
    }

    /// <summary>原子写字节。</summary>
    public static void AtomicWriteAllBytes(string path, byte[] bytes)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var tmp = path + ".tmp-" + Guid.NewGuid().ToString("N")[..8];
        try
        {
            File.WriteAllBytes(tmp, bytes);
            File.Move(tmp, path, overwrite: true);
        }
        finally
        {
            try { if (File.Exists(tmp)) File.Delete(tmp); } catch { /* best effort */ }
        }
    }

    /// <summary>读 JSON 文件；文件不存在返回默认实例，解析失败抛出由调用方兜底。</summary>
    public static T? ReadJson<T>(string path) where T : class
    {
        if (!File.Exists(path)) return null;
        var json = File.ReadAllText(path);
        return JsonSerializer.Deserialize<T>(json, JsonOpts);
    }

    /// <summary>把任意对象序列化为 JsonElement（供自由结构 payload 入队/落盘）。</summary>
    public static JsonElement? ToElement(object? payload)
    {
        if (payload == null) return null;
        return JsonSerializer.SerializeToElement(payload, JsonOpts);
    }

    /// <summary>文件名安全化（thumb 缓存键、队列文件名等）。</summary>
    public static string SafeFileId(string raw)
    {
        var chars = raw.Trim().Replace('\\', '_').Replace('/', '_');
        var sb = new System.Text.StringBuilder(chars.Length);
        foreach (var c in chars)
        {
            sb.Append(char.IsLetterOrDigit(c) || c == '-' || c == '_' || c == '.' ? c : '_');
        }
        return sb.Length == 0 ? "_" : sb.ToString();
    }
}
