using System;
using System.IO;
using System.IO.Hashing;

namespace MiniPet.Export;

/// <summary>MPAK 包类型（algorithm-asset-format.md §二）。</summary>
public enum MpakKind : ulong
{
    Parts = 1,     // 部件图包
    Layout = 2,    // 布局表包
    Bgmap = 3,     // 地图背景包
    Font = 4,      // 字体包
    AudioMeta = 5, // BGM 元数据包
    Thumb = 6,     // 缩略图（manifest 选择器展示用 PNG，非固件五类包；设备按 kind 白名单忽略）
}

/// <summary>
/// MPAK 通用信封（算法规格 §二）读写。
///
/// 布局（全部小端、4B 对齐原则）：
/// <code>
/// [16B]  MAGIC "MPAK"(4B) + version u16(=1) + flags u16 + 零填充 8B
/// [ 8B]  kind          u64（MpakKind）
/// [ 8B]  content_hash  u64（xxhash64(payload)，manifest 身份）
/// [ 4B]  payload_len   u32
/// [ 4B]  reserved      u32（0）
/// [ ...]  payload      （按 kind 各自结构）
/// [ 8B]  尾部：crc32c u32 + zero u32
/// </code>
/// crc32c 覆盖「header 全量 + payload」（MAGIC 至 payload 末尾，共 32+payload_len 字节）；
/// crc 字段本身位于尾部、不在覆盖区内（等价于「计算时 crc 字段按 0」）。
///
/// 读取校验顺序（固件同序）：MAGIC → payload_len → crc32c → content_hash。
/// 任一失败按 E11 损坏处理（弃用重拉）。
/// </summary>
public static class Mpak
{
    public const ushort CurrentVersion = 1;
    public const int HeaderSize = 40;   // magic块16(4+2+2+8零填充) + kind 8 + hash 8 + len 4 + reserved 4 —— 与固件 40B 头一致（真机联调定稿 2026-09-26）
    public const int TrailerSize = 8;   // crc32c + zero

    private static ReadOnlySpan<byte> Magic => "MPAK"u8;

    /// <summary>xxhash64(payload) —— 包内容身份（manifest diff 的 key）。</summary>
    public static ulong ContentHash(byte[] payload) => XxHash64.HashToUInt64(payload);

    // ── CRC-32C（Castagnoli，多项式 0x1EDC6F41 反射 0x82F63B78；init/final xor 0xFFFFFFFF）──
    // System.IO.Hashing 9.0 只带 Crc32（ISO），crc32c 在此自实现（表驱动，正确性由回放测试 2 篡改用例守护）。
    private static readonly uint[] Crc32CTable = BuildCrc32CTable();

    private static uint[] BuildCrc32CTable()
    {
        var table = new uint[256];
        for (uint i = 0; i < 256; i++)
        {
            uint c = i;
            for (int k = 0; k < 8; k++)
            {
                c = (c & 1) != 0 ? 0x82F63B78u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        return table;
    }

    /// <summary>增量 CRC-32C（续算：crc 传入上次返回值）。</summary>
    public static uint Crc32C(ReadOnlySpan<byte> data, uint crc = 0xFFFFFFFFu)
    {
        foreach (byte b in data)
        {
            crc = Crc32CTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFu;
    }

    /// <summary>组装完整 MPAK 文件字节（header + payload + 尾部 crc）。</summary>
    public static byte[] Build(MpakKind kind, byte[] payload, ushort flags = 0)
    {
        if (payload == null) throw new ArgumentNullException(nameof(payload));
        ulong hash = ContentHash(payload);
        var ms = new MemoryStream(HeaderSize + payload.Length + TrailerSize);
        using (var w = new BinaryWriter(ms, System.Text.Encoding.UTF8, leaveOpen: true))
        {
            w.Write(Magic);
            w.Write(CurrentVersion);
            w.Write(flags);
            w.Write(0UL);               // magic 块补齐 16B 的零填充（固件按 16B 块游标）
            w.Write((ulong)kind);
            w.Write(hash);
            w.Write((uint)payload.Length);
            w.Write(0u);
            w.Write(payload);
            // crc 覆盖 header+payload：此刻 crc 字段尚未写入，天然满足「crc 字段按 0 计算」
            uint crc = Crc32C(ms.GetBuffer().AsSpan(0, (int)ms.Length));
            w.Write(crc);
            w.Write(0u);
        }
        return ms.ToArray();
    }

    /// <summary>便捷重载：返回 (content_hash, 完整文件字节)。</summary>
    public static (ulong Hash, byte[] File) BuildWithHash(MpakKind kind, byte[] payload, ushort flags = 0)
    {
        var file = Build(kind, payload, flags);
        return (ContentHash(payload), file);
    }

    /// <summary>校验失败原因。</summary>
    public enum ReadError
    {
        None,
        TooShort,        // 连 header+trailer 都不够
        BadMagic,        // MAGIC 不符
        BadVersion,      // version != 1（当前不支持）
        BadLength,       // payload_len 与实际文件长度不符
        BadCrc,          // crc32c 不符（损坏）
        BadHash,         // content_hash 与 payload 实算不符
    }

    /// <summary>解析后的包。</summary>
    public sealed class Packet
    {
        public MpakKind Kind;
        public ushort Flags;
        public ulong ContentHashValue;
        public byte[] Payload = Array.Empty<byte>();
    }

    /// <summary>
    /// 读取并全量校验。校验序：MAGIC → len → crc → hash（规格 §二固件流程）。
    /// </summary>
    public static ReadError TryRead(byte[] file, out Packet? packet)
    {
        packet = null;
        if (file == null || file.Length < HeaderSize + TrailerSize) return ReadError.TooShort;
        if (!file.AsSpan(0, 4).SequenceEqual(Magic)) return ReadError.BadMagic;

        using var r = new BinaryReader(new MemoryStream(file, false));
        r.BaseStream.Position = 4;
        ushort version = r.ReadUInt16();
        if (version != CurrentVersion) return ReadError.BadVersion;
        ushort flags = r.ReadUInt16();
        r.BaseStream.Position += 8;     // 跳过 magic 块 16B 的零填充
        var kind = (MpakKind)r.ReadUInt64();
        ulong hash = r.ReadUInt64();
        uint payloadLen = r.ReadUInt32();
        uint reserved = r.ReadUInt32();

        // len：header(32) + payload_len + trailer(8) 必须精确等于文件长
        if ((long)HeaderSize + payloadLen + TrailerSize != file.Length) return ReadError.BadLength;

        // crc：覆盖 header+payload（crc 字段在尾部，不参与）
        uint crcStored = BitConverter.ToUInt32(file, HeaderSize + (int)payloadLen);
        uint crcActual = Crc32C(file.AsSpan(0, HeaderSize + (int)payloadLen));
        if (crcStored != crcActual) return ReadError.BadCrc;

        var payload = new byte[payloadLen];
        Buffer.BlockCopy(file, HeaderSize, payload, 0, (int)payloadLen);

        // hash：payload xxhash64 必须与 header 声明一致
        if (ContentHash(payload) != hash) return ReadError.BadHash;

        packet = new Packet { Kind = kind, Flags = flags, ContentHashValue = hash, Payload = payload };
        return ReadError.None;
    }

    /// <summary>hash → 文件名（u64 小写 16 位十六进制零填充）。</summary>
    public static string HashFileName(ulong hash) => $"{hash:x16}.mpak";
}
