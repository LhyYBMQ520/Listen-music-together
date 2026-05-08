using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using ProtoBuf;

namespace smtc;

public class SmtcHttpServer : IDisposable
{
    private TcpListener _listener;
    private CancellationTokenSource _cts;
    private SmtcData _currentData = new();
    private readonly object _lock = new();
    private volatile bool _disposed;

    public int Port { get; }

    public SmtcHttpServer(int port = 9863)
    {
        Port = port;
    }

    public void Start()
    {
        _cts = new CancellationTokenSource();
        _listener = new TcpListener(IPAddress.Loopback, Port);
        _listener.Start();
        _ = RunAsync(_cts.Token);
    }

    public void Update(SmtcData data)
    {
        lock (_lock)
            _currentData = data ?? new SmtcData();
    }

    private async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                var client = await _listener.AcceptTcpClientAsync(ct);
                _ = HandleAsync(client);
            }
            catch (OperationCanceledException) { }
            catch (ObjectDisposedException) { }
            catch
            {
                try { await Task.Delay(100, ct); } catch { }
            }
        }
    }

    private async Task HandleAsync(TcpClient client)
    {
        try
        {
            using (client)
            {
                var ns = client.GetStream();
                ns.ReadTimeout = 3000;
                ns.WriteTimeout = 3000;

                var buffer = new byte[4096];
                var read = await ns.ReadAsync(buffer, 0, buffer.Length);
                if (read == 0) return;
                var request = Encoding.UTF8.GetString(buffer, 0, read);

                // CORS preflight
                if (request.StartsWith("OPTIONS"))
                {
                    var cors = "HTTP/1.1 204 No Content\r\n" +
                               "Access-Control-Allow-Origin: *\r\n" +
                               "Access-Control-Allow-Methods: GET, OPTIONS\r\n" +
                               "Access-Control-Allow-Headers: Content-Type\r\n" +
                               "Connection: close\r\n\r\n";
                    await ns.WriteAsync(Encoding.UTF8.GetBytes(cors));
                    return;
                }

                // Parse path from first line: "GET /json HTTP/1.1"
                var firstLine = request.Split("\r\n").FirstOrDefault() ?? "";
                var parts = firstLine.Split(' ');
                var path = parts.Length >= 2 ? parts[1].TrimStart('/') : "";

                if (path.StartsWith("json"))
                {
                    await WriteJsonResponse(ns);
                }
                else
                {
                    await WriteProtoResponse(ns);
                }
            }
        }
        catch { }
    }

    private async Task WriteJsonResponse(NetworkStream ns)
    {
        SmtcData data;
        lock (_lock)
            data = _currentData;

        // Exclude potentially large thumbnail bytes from JSON output
        var jsonObj = new
        {
            data.SourceApp,
            data.SourceAppId,
            data.Title,
            data.Artist,
            PlaybackStatus = data.PlaybackStatus.ToString(),
            Timeline = data.Timeline != null ? new
            {
                data.Timeline.PositionSeconds,
                data.Timeline.EndTimeSeconds,
                data.Timeline.PositionTicks,
                data.Timeline.EndTimeTicks
            } : null,
            ThumbnailSize = data.Thumbnail?.Length ?? 0,
            data.SessionIndex,
            data.SessionCount
        };

        var jsonBytes = JsonSerializer.SerializeToUtf8Bytes(jsonObj, new JsonSerializerOptions { WriteIndented = true });

        var header = "HTTP/1.1 200 OK\r\n" +
                     "Content-Type: application/json; charset=utf-8\r\n" +
                     $"Content-Length: {jsonBytes.Length}\r\n" +
                     "Access-Control-Allow-Origin: *\r\n" +
                     "Connection: close\r\n\r\n";
        await ns.WriteAsync(Encoding.UTF8.GetBytes(header));
        await ns.WriteAsync(jsonBytes);
    }

    private async Task WriteProtoResponse(NetworkStream ns)
    {
        byte[] protoBytes;
        lock (_lock)
        {
            using var ms = new MemoryStream();
            Serializer.Serialize(ms, _currentData);
            protoBytes = ms.ToArray();
        }

        var header = "HTTP/1.1 200 OK\r\n" +
                     "Content-Type: application/x-protobuf\r\n" +
                     $"Content-Length: {protoBytes.Length}\r\n" +
                     "Access-Control-Allow-Origin: *\r\n" +
                     "Connection: close\r\n\r\n";
        await ns.WriteAsync(Encoding.UTF8.GetBytes(header));
        await ns.WriteAsync(protoBytes);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _cts?.Cancel();
        try { _listener?.Stop(); } catch { }
        _cts?.Dispose();
    }
}
