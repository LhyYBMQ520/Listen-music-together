using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Windows.Media.Control;
using Windows.Storage.Streams;

namespace smtc
{
    public partial class MainWindow : Window
    {
        // SMTC 核心对象
        private GlobalSystemMediaTransportControlsSessionManager _sessionManager;
        private readonly System.Windows.Threading.DispatcherTimer _progressTimer;
        private GlobalSystemMediaTransportControlsSession _currentSession;
        
        // 线程安全与状态
        private readonly object _sessionLock = new object();
        private CancellationTokenSource _cts;
        private TimeSpan _lastPosition;
        private DateTime _lastUpdateTime;
        
        // 会话管理
        private List<GlobalSystemMediaTransportControlsSession> _sessions = new();
        private bool _userSwitchLock; // 手动切换锁定

        // HTTP输出API
        private readonly SmtcHttpServer _httpServer = new(9863);
        private byte[] _thumbnailBytes;

        public MainWindow()
        {
            InitializeComponent();
            // 窗口拖动
            MouseDown += (_, e) =>
            {
                if (e.LeftButton == System.Windows.Input.MouseButtonState.Pressed)
                    DragMove();
            };

            // 关键修复：应用启动时立即初始化UI显示
            ClearUI(); // 显示"无播放信息"和0/0状态

            // 初始化定时器
            _progressTimer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
            _progressTimer.Tick += async (_, _) => await UpdateSystemState();
            _progressTimer.Start();

            // 初始化SMTC
            _ = InitSMTCAsync();

            // 启动HTTP API
            try { _httpServer.Start(); } catch { }
        }

        #region 核心初始化
        private async Task InitSMTCAsync()
        {
            try
            {
                _sessionManager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
                if (_sessionManager == null) throw new Exception("SMTC 管理器初始化失败");
            }
            catch (Exception ex)
            {
                Dispatcher.Invoke(() =>
                {
                    txtAppName.Text = $"错误：{ex.Message}";
                    ClearUI(); // 确保显示0/0状态
                });
            }
        }
        #endregion

        #region 系统状态刷新（核心驱动）
        private async Task UpdateSystemState()
        {
            try
            {
                if (_sessionManager == null) return;

                lock (_sessionLock)
                {
                    // 强制每次刷新会话列表
                    _sessions = _sessionManager.GetSessions()?.ToList() ?? new();
                }

                // 修复：无会话时强制更新UI显示0/0
                if (_sessions.Count == 0)
                {
                    _currentSession = null;
                    ClearUI();
                    return;
                }

                // 强制同步当前活跃会话
                UpdateActiveSession();

                // 更新媒体信息与进度
                await UpdateMediaInfoAsync();
                UpdatePlayProgress();
                PushToHttpServer();
            }
            catch { }
        }

        private void PushToHttpServer()
        {
            try
            {
                if (_currentSession == null) return;

                var playback = _currentSession.GetPlaybackInfo();
                var timeline = _currentSession.GetTimelineProperties();

                var data = new SmtcData
                {
                    SourceApp = GetAppFriendlyName(_currentSession.SourceAppUserModelId),
                    SourceAppId = _currentSession.SourceAppUserModelId ?? "",
                    Title = "Unknown",
                    Artist = "Unknown",
                    PlaybackStatus = playback != null
                        ? playback.PlaybackStatus switch
                        {
                            GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing => PlaybackStatus.Playing,
                            GlobalSystemMediaTransportControlsSessionPlaybackStatus.Paused => PlaybackStatus.Paused,
                            GlobalSystemMediaTransportControlsSessionPlaybackStatus.Stopped => PlaybackStatus.Stopped,
                            GlobalSystemMediaTransportControlsSessionPlaybackStatus.Changing => PlaybackStatus.Changing,
                            GlobalSystemMediaTransportControlsSessionPlaybackStatus.Closed => PlaybackStatus.Closed,
                            _ => PlaybackStatus.Unknown
                        }
                        : PlaybackStatus.Unknown,
                    Timeline = timeline != null
                        ? new SmtcTimeline
                        {
                            PositionSeconds = timeline.Position.TotalSeconds,
                            EndTimeSeconds = timeline.EndTime.TotalSeconds,
                            PositionTicks = timeline.Position.Ticks,
                            EndTimeTicks = timeline.EndTime.Ticks
                        }
                        : null,
                    Thumbnail = _thumbnailBytes,
                    SessionIndex = _sessions.IndexOf(_currentSession) + 1,
                    SessionCount = _sessions.Count
                };

                // 从UI读取最新的Title/Artist（异步更新可能在之后才写入UI）
                Dispatcher.Invoke(() =>
                {
                    if (txtTitle.Text != "Title" && txtTitle.Text != "未知歌曲")
                        data.Title = txtTitle.Text;
                    if (txtArtist.Text != "Artist" && txtArtist.Text != "未知歌手")
                        data.Artist = txtArtist.Text;
                });

                _httpServer.Update(data);
            }
            catch { }
        }

        /// <summary>
        /// 强制获取系统当前活跃播放会话（✅ 已修复核心BUG）
        /// 规则：有播放中 → 跟随；无播放中 → 保持最后状态，不自动切换
        /// </summary>
        private void UpdateActiveSession()
        {
            if (_userSwitchLock || _sessions.Count == 0) return;

            lock (_sessionLock)
            {
                // 查找正在播放的会话
                var playingSession = _sessions.FirstOrDefault(s =>
                {
                    try
                    {
                        var info = s.GetPlaybackInfo();
                        return info?.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;
                    }
                    catch { return false; }
                });

                // ✅ 核心修复：只有找到正在播放的新会话，才切换
                // ❌ 无播放会话时，绝不修改当前会话，保持最后状态
                if (playingSession != null)
                {
                    _currentSession = playingSession;
                }
            }
        }
        #endregion

        #region 媒体信息与进度
        private async Task UpdateMediaInfoAsync()
        {
            try
            {
                if (_currentSession == null) return;

                _cts?.Cancel();
                _cts = new CancellationTokenSource();
                
                var mediaInfo = await _currentSession.TryGetMediaPropertiesAsync().AsTask(_cts.Token);
                if (mediaInfo == null) return;

                // 加载封面
                ImageSource cover = null;
                if (mediaInfo.Thumbnail != null)
                {
                    try
                    {
                        using var stream = await mediaInfo.Thumbnail.OpenReadAsync().AsTask(_cts.Token);
                        using var ms = stream.AsStream();
                        var rawBytes = new byte[ms.Length];
                        ms.Read(rawBytes, 0, rawBytes.Length);
                        _thumbnailBytes = rawBytes;
                        ms.Position = 0;
                        var bitmap = new BitmapImage();
                        bitmap.BeginInit();
                        bitmap.CacheOption = BitmapCacheOption.OnLoad;
                        bitmap.StreamSource = ms;
                        bitmap.EndInit();
                        bitmap.Freeze();
                        cover = new FormatConvertedBitmap(bitmap, PixelFormats.Bgra32, null, 0);
                        cover.Freeze();
                    }
                    catch { _thumbnailBytes = null; }
                }
                else
                {
                    _thumbnailBytes = null;
                }

                // 更新UI
                Dispatcher.Invoke(() =>
                {
                    txtAppName.Text = GetAppFriendlyName(_currentSession.SourceAppUserModelId);
                    txtTitle.Text = mediaInfo.Title ?? "未知歌曲";
                    txtArtist.Text = mediaInfo.Artist ?? "未知歌手";
                    imgCover.Source = cover;
                    txtIndex.Text = $"{_sessions.IndexOf(_currentSession) + 1}/{_sessions.Count}";
                });
            }
            catch (OperationCanceledException) { }
            catch { }
        }

        private void UpdatePlayProgress()
        {
            try
            {
                if (_currentSession == null) return;

                var timeline = _currentSession.GetTimelineProperties();
                var playback = _currentSession.GetPlaybackInfo();
                if (timeline == null || playback == null) return;

                double total = timeline.EndTime.TotalSeconds;
                if (total <= 0)
                {
                    Dispatcher.Invoke(() =>
                    {
                        txtPlayState.Text = GetPlayStatus(playback.PlaybackStatus);
                        txtCurrentTime.Text = "--:--";
                        txtTotalTime.Text = "--:--";
                        progressBar.Value = 0;
                    });
                    return;
                }

                // 计算进度
                if (timeline.Position != _lastPosition)
                {
                    _lastPosition = timeline.Position;
                    _lastUpdateTime = DateTime.Now;
                }

                var nowPos = playback.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing
                    ? _lastPosition + (DateTime.Now - _lastUpdateTime)
                    : _lastPosition;

                nowPos = TimeSpan.FromMilliseconds(Math.Min(nowPos.TotalMilliseconds, timeline.EndTime.TotalMilliseconds));

                // 更新进度UI
                Dispatcher.Invoke(() =>
                {
                    txtPlayState.Text = GetPlayStatus(playback.PlaybackStatus);
                    txtCurrentTime.Text = nowPos.ToString(@"mm\:ss");
                    txtTotalTime.Text = timeline.EndTime.ToString(@"mm\:ss");
                    progressBar.Maximum = total;
                    progressBar.Value = nowPos.TotalSeconds;
                });
            }
            catch { }
        }
        #endregion

        #region 工具方法
        private string GetAppFriendlyName(string appId)
        {
            if (string.IsNullOrEmpty(appId)) return "未知应用";
            return appId.Contains("cloudmusic") ? "网易云音乐" :
                   appId.Contains("qqmusic") ? "QQ音乐" :
                   appId.Contains("foobar") ? "Foobar2000" :
                   Path.GetFileNameWithoutExtension(appId);
        }

        private string GetPlayStatus(GlobalSystemMediaTransportControlsSessionPlaybackStatus status)
        {
            return status switch
            {
                GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing => "正在播放",
                GlobalSystemMediaTransportControlsSessionPlaybackStatus.Paused => "已暂停",
                _ => "未知状态"
            };
        }

        private void ClearUI()
        {
            Dispatcher.Invoke(() =>
            {
                txtAppName.Text = "无播放信息";
                txtTitle.Text = "Title";
                txtArtist.Text = "Artist";
                txtPlayState.Text = "状态";
                imgCover.Source = null;
                progressBar.Value = 0;
                progressBar.Maximum = 1;
                txtCurrentTime.Text = "00:00";
                txtTotalTime.Text = "00:00";
                txtIndex.Text = "0/0";
            });
        }
        #endregion

        #region 手动切换会话
        private void NextButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                lock (_sessionLock)
                {
                    if (_sessions.Count <= 1) return;

                    // 手动切换，锁定3秒
                    _userSwitchLock = true;
                    int currentIdx = _sessions.IndexOf(_currentSession);
                    _currentSession = _sessions[(currentIdx + 1) % _sessions.Count];
                    
                    // 3秒后解除锁定，恢复自动跟随
                    Task.Delay(3000).ContinueWith(_ => _userSwitchLock = false);
                }
            }
            catch { }
        }
        #endregion

        #region 关闭程序
        private void CloseButton_Click(object sender, RoutedEventArgs e)
        {
            _httpServer?.Dispose();
            _progressTimer.Stop();
            _cts?.Cancel();
            Close();
        }
        #endregion
    }
}
