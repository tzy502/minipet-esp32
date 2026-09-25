using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace MiniPet.Export;

/// <summary>
/// kind=5 AUDIO_META（BGM 元数据包）—— 算法规格 §七。
///
/// payload：
/// <code>
/// [u32] track_count
/// [track_count × N]:
///     id u32（track key 的 xxhash32，稳定不受目录顺序影响）
///     title 定长 96B UTF-8（约 32 汉字，超长截断）
///     source u8（0=WZ，1=QQ）
///     duration_s u32（秒）
/// </code>
/// 音频本体不打包（流式拉取 E8）；此包仅列表（设备端选择器/控制条显示用）。
/// </summary>
public static class AudioMetaWriter
{
    public const int TitleSize = 96;
    public const int TrackEntrySize = 4 + TitleSize + 1 + 4;

    public sealed class AudioTrackMeta
    {
        public uint Id;
        public string Title = "";
        /// <summary>0=WZ 1=QQ。</summary>
        public byte Source;
        public uint DurationS;
    }

    /// <summary>track key（如 "Bgm00.img/SleepyWood"）→ 稳定 u32 id。</summary>
    public static uint TrackIdForKey(string key) =>
        System.IO.Hashing.XxHash32.HashToUInt32(Encoding.UTF8.GetBytes(key ?? ""));

    public static byte[] Build(IReadOnlyList<AudioTrackMeta> tracks)
    {
        var ms = new MemoryStream(4 + tracks.Count * TrackEntrySize);
        using (var w = new BinaryWriter(ms, Encoding.UTF8, leaveOpen: true))
        {
            w.Write((uint)tracks.Count);
            foreach (var t in tracks)
            {
                w.Write(t.Id);
                LayoutPackWriter.WriteFixedString(w, t.Title, TitleSize);
                w.Write(t.Source);
                w.Write(t.DurationS);
            }
        }
        return ms.ToArray();
    }
}
