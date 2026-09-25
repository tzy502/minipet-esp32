using System;

namespace System.IO.Hashing;

/// <summary>
/// Crc32C 兼容垫片：System.IO.Hashing 9.0.0 只提供 Crc32（ISO-HDLC），无 Castagnoli 变体；
/// 算法规格 §二要求 crc32c，表驱动实现在 <see cref="MiniPet.Export.Mpak.Crc32C"/>。
/// 方法名沿用 System.IO.Hashing 的 HashToUInt64 命名族（返回 uint，Castagnoli 32 位），
/// 供回放测试「修复 crc 后验 content_hash」用例直接调用。
/// 注意：若未来包版本内建 Crc32C，此类型会与之冲突——届时删除本垫片改用官方实现即可。
/// </summary>
public static class Crc32C
{
    /// <summary>CRC-32C（Castagnoli），一次性哈希（等价 MiniPet.Export.Mpak.Crc32C）。</summary>
    public static uint HashToUInt64(ReadOnlySpan<byte> data) => MiniPet.Export.Mpak.Crc32C(data);
}
