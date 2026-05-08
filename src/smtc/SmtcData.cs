using ProtoBuf;

namespace smtc;

[ProtoContract]
public class SmtcData
{
    [ProtoMember(1)] public string SourceApp { get; set; }
    [ProtoMember(2)] public string SourceAppId { get; set; }
    [ProtoMember(3)] public string Title { get; set; }
    [ProtoMember(4)] public string Artist { get; set; }
    [ProtoMember(5)] public PlaybackStatus PlaybackStatus { get; set; }
    [ProtoMember(6)] public SmtcTimeline Timeline { get; set; }
    [ProtoMember(7)] public byte[] Thumbnail { get; set; }
    [ProtoMember(8)] public int SessionIndex { get; set; }
    [ProtoMember(9)] public int SessionCount { get; set; }
}

[ProtoContract]
public enum PlaybackStatus
{
    [ProtoEnum] Unknown = 0,
    [ProtoEnum] Playing = 1,
    [ProtoEnum] Paused = 2,
    [ProtoEnum] Stopped = 3,
    [ProtoEnum] Changing = 4,
    [ProtoEnum] Closed = 5,
}

[ProtoContract]
public class SmtcTimeline
{
    [ProtoMember(1)] public double PositionSeconds { get; set; }
    [ProtoMember(2)] public double EndTimeSeconds { get; set; }
    [ProtoMember(3)] public long PositionTicks { get; set; }
    [ProtoMember(4)] public long EndTimeTicks { get; set; }
}
