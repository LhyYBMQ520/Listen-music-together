(function () {
    const wsUrl = `ws://${location.host}/ws`;
    let ws;
    let player;

    const WAVE_FORMAT_IEEE_FLOAT = 0x0003;
    const WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

    class AudioPlayer {
        constructor() {
            this.ctx = null;
            this.format = null;
            this.chunks = [];
            this.firstTs = null;
            this.playbackEpoch = null;
            this.scheduleTimer = null;
            this.maxBufferSec = 30;
            this.nextPlayTime = 0;  // AudioContext time when the currently-scheduled source ends
            this.started = false;
        }

        init(format) {
            // 先停掉旧的调度循环，避免多个 _scheduleLoop 同时运行
            if (this.scheduleTimer) {
                clearTimeout(this.scheduleTimer);
                this.scheduleTimer = null;
            }

            const formatChanged = !this.ctx || !this.format ||
                this.format.sampleRate !== format.sampleRate ||
                this.format.channels !== format.channels;
            if (formatChanged) {
                if (this.ctx) {
                    this.ctx.close();
                    this.ctx = null;
                }
                this.ctx = new (window.AudioContext || window.webkitAudioContext)({
                    sampleRate: format.sampleRate
                });
            }

            this.format = format;
            this.chunks = [];
            this.firstTs = null;
            this.playbackEpoch = null;
            this.nextPlayTime = 0;
            this.started = false;
            console.log('AudioPlayer init:', format, 'state:', this.ctx.state);
            if (this.ctx.state === 'suspended') {
                updateAudioStatus('waiting');
            } else {
                updateAudioStatus('ready');
            }
        }

        _isFloat() {
            if (!this.format) return false;
            const tag = this.format.formatTag || 0;
            if (tag === WAVE_FORMAT_IEEE_FLOAT || tag === WAVE_FORMAT_EXTENSIBLE) return true;
            // Backward compat: WASAPI loopback always uses IEEE float for 32-bit samples
            if (tag === 0 && this.format.bitsPerSample === 32) return true;
            return false;
        }

        enqueue(arrayBuffer) {
            if (!this.ctx || !this.format) return;

            const view = new DataView(arrayBuffer);
            const ts = view.getUint32(0, true) + view.getUint32(4, true) * 0x100000000;
            const pcmCopy = arrayBuffer.slice(8);

            this.chunks.push({ ts, pcm: pcmCopy, scheduled: false });

            if (this.firstTs === null) {
                this.firstTs = ts;
            }

            this._trim();
            if (this.ctx.state !== 'suspended') {
                this._startScheduling();
            }
        }

        _trim() {
            if (this.chunks.length < 2) return;
            const newestTs = this.chunks[this.chunks.length - 1].ts;
            const cutoff = newestTs - this.maxBufferSec * 1000000;
            while (this.chunks.length > 2 && this.chunks[0].ts < cutoff) {
                this.chunks.shift();
            }
        }

        _startScheduling() {
            if (this.started) return;
            if (this.ctx.state === 'suspended') {
                updateAudioStatus('waiting');
                return;
            }
            this.started = true;
            this.playbackEpoch = this.ctx.currentTime;
            this.nextPlayTime = this.ctx.currentTime;
            updateAudioStatus('playing');
            this._scheduleLoop();
        }

        _scheduleLoop() {
            if (!this.ctx || this.chunks.length === 0) {
                this.scheduleTimer = setTimeout(() => this._scheduleLoop(), 80);
                return;
            }

            if (this.ctx.state === 'suspended') {
                this.scheduleTimer = setTimeout(() => this._scheduleLoop(), 300);
                return;
            }

            const unscheduled = this.chunks.filter(c => !c.scheduled);
            if (unscheduled.length === 0) {
                this.scheduleTimer = setTimeout(() => this._scheduleLoop(), 80);
                return;
            }

            // Schedule chunks up to 600ms ahead of currentTime
            const maxAhead = this.ctx.currentTime + 0.6;

            // Group contiguous chunks and play as single AudioBuffer
            let i = 0;
            while (i < unscheduled.length) {
                const first = unscheduled[i];
                const when = this.playbackEpoch + (first.ts - this.firstTs) / 1_000_000;

                if (when > maxAhead) break;

                // Collect all contiguous chunks (gap tolerance: 1.5x chunk duration)
                const blockAlign = this.format.blockAlign;
                const sampleRate = this.format.sampleRate;
                let totalBytes = first.pcm.byteLength;
                let totalFrames = Math.floor(totalBytes / blockAlign);
                let expectedNextTs = first.ts + (totalFrames / sampleRate) * 1_000_000;
                const gapTolerance = (totalFrames / sampleRate) * 1_500_000; // 1.5x in us

                let end = i + 1;
                while (end < unscheduled.length) {
                    const next = unscheduled[end];
                    if (Math.abs(next.ts - expectedNextTs) < gapTolerance) {
                        totalBytes += next.pcm.byteLength;
                        const nextFrames = Math.floor(next.pcm.byteLength / blockAlign);
                        totalFrames += nextFrames;
                        expectedNextTs = next.ts + (nextFrames / sampleRate) * 1_000_000;
                        end++;
                    } else {
                        break;
                    }
                }

                // Create one AudioBuffer for all merged chunks
                const audioBuffer = this.ctx.createBuffer(this.format.channels, totalFrames, sampleRate);
                const channelData = [];
                for (let c = 0; c < this.format.channels; c++) {
                    channelData.push(audioBuffer.getChannelData(c));
                }

                const isFloat = this._isFloat();
                const bytesPerSample = this.format.bitsPerSample / 8;
                const channels = this.format.channels;
                let frameOffset = 0;

                for (let ci = i; ci < end; ci++) {
                    const chunk = unscheduled[ci];
                    const chunkFrames = Math.floor(chunk.pcm.byteLength / blockAlign);
                    const raw = new DataView(chunk.pcm);
                    let sampleIdx = 0;

                    for (let f = 0; f < chunkFrames; f++) {
                        for (let c = 0; c < channels; c++) {
                            const offset = sampleIdx * bytesPerSample;
                            let sample;
                            if (isFloat && bytesPerSample === 4) {
                                sample = raw.getFloat32(offset, true);
                            } else if (bytesPerSample === 2) {
                                sample = raw.getInt16(offset, true) / 32768;
                            } else if (bytesPerSample === 4) {
                                sample = raw.getInt32(offset, true) / 2147483648;
                            } else {
                                sample = (raw.getUint8(offset) - 128) / 128;
                            }
                            channelData[c][frameOffset + f] = sample;
                            sampleIdx++;
                        }
                    }
                    frameOffset += chunkFrames;
                    chunk.scheduled = true;
                }

                const playAt = Math.max(when, this.nextPlayTime, this.ctx.currentTime);
                const source = this.ctx.createBufferSource();
                source.buffer = audioBuffer;
                source.connect(this.ctx.destination);
                source.start(playAt);
                this.nextPlayTime = playAt + (totalFrames / sampleRate);

                i = end;
            }

            this.scheduleTimer = setTimeout(() => this._scheduleLoop(), 80);
        }

        stop() {
            if (this.scheduleTimer) {
                clearTimeout(this.scheduleTimer);
                this.scheduleTimer = null;
            }
            if (this.ctx) {
                this.ctx.close();
                this.ctx = null;
            }
            this.chunks = [];
            this.started = false;
            this.format = null;
            this.firstTs = null;
            this.playbackEpoch = null;
            this.nextPlayTime = 0;
            updateAudioStatus('idle');
        }
    }

    player = new AudioPlayer();

    function updateAudioStatus(state) {
        const el = document.getElementById('audioStatus');
        if (!el) return;
        switch (state) {
            case 'ready':
                el.textContent = '音频就绪';
                el.className = 'text-xs text-green-500 mt-1';
                break;
            case 'playing':
                el.textContent = '● 正在播放音频';
                el.className = 'text-xs text-green-400 mt-1 animate-pulse';
                break;
            case 'waiting':
                el.textContent = '点击页面以开始播放';
                el.className = 'text-xs text-yellow-400 mt-1 animate-pulse';
                break;
            case 'idle':
                el.textContent = '等待播放';
                el.className = 'text-xs text-gray-500 mt-1';
                break;
        }
    }

    function connect() {
        ws = new WebSocket(wsUrl);
        ws.binaryType = 'blob';

        ws.onopen = () => {
            console.log('WebSocket connected');
        };

        ws.onmessage = (event) => {
            if (event.data instanceof Blob) {
                event.data.arrayBuffer().then(buf => player.enqueue(buf));
                return;
            }

            try {
                const msg = JSON.parse(event.data);
                if (msg.type === 'smtc') {
                    updateSmtcUI(msg.data);
                } else if (msg.type === 'audio-format') {
                    console.log('Audio format:', msg.format);
                    player.init(msg.format);
                } else if (msg.type === 'capture-stopped') {
                    console.log('Capture stopped');
                    player.stop();
                }
            } catch (e) {
                console.error('Failed to parse WebSocket message:', e);
            }
        };

        ws.onclose = () => {
            console.log('WebSocket disconnected, reconnecting in 3s...');
            player.stop();
            setTimeout(connect, 3000);
        };

        ws.onerror = (err) => {
            console.error('WebSocket error:', err);
            ws.close();
        };
    }

    // Resume AudioContext on user interaction (browser autoplay policy)
    document.addEventListener('click', () => {
        if (player.ctx && player.ctx.state === 'suspended') {
            player.ctx.resume().then(() => {
                console.log('AudioContext resumed, starting playback');
                player._startScheduling();
            });
        }
    }, { once: false });

    const statusLabels = {
        'Playing': '正在播放',
        'Paused': '已暂停',
        'Stopped': '已停止',
        'Changing': '切换中',
        'Closed': '已关闭',
        'Unknown': '未知状态'
    };

    let lastCoverKey = '';

    function updateSmtcUI(data) {
        document.getElementById('appName').textContent = data.SourceApp || '无播放信息';
        document.getElementById('sessionIndex').textContent =
            `${data.SessionIndex || 0}/${data.SessionCount || 0}`;
        document.getElementById('title').textContent = data.Title || 'Title';
        document.getElementById('artist').textContent = data.Artist || 'Artist';
        document.getElementById('playState').textContent =
            statusLabels[data.PlaybackStatus] || '状态';

        const coverImg = document.getElementById('cover');
        if (data.ThumbnailSize > 0) {
            const coverKey = `${data.Title}|${data.Artist}|${data.ThumbnailSize}`;
            if (coverKey !== lastCoverKey) {
                lastCoverKey = coverKey;
                coverImg.src = `/api/thumbnail?t=${Date.now()}`;
            }
            coverImg.classList.remove('hidden');
        } else {
            lastCoverKey = '';
            coverImg.classList.add('hidden');
        }

        if (data.Timeline && data.Timeline.EndTimeSeconds > 0) {
            const pct = (data.Timeline.PositionSeconds / data.Timeline.EndTimeSeconds) * 100;
            document.getElementById('progressFill').style.width = `${Math.min(pct, 100)}%`;
            document.getElementById('currentTime').textContent = formatTime(data.Timeline.PositionSeconds);
            document.getElementById('totalTime').textContent = formatTime(data.Timeline.EndTimeSeconds);
        } else {
            document.getElementById('progressFill').style.width = '0%';
            document.getElementById('currentTime').textContent = '00:00';
            document.getElementById('totalTime').textContent = '00:00';
        }
    }

    function formatTime(seconds) {
        if (!seconds || seconds < 0) return '00:00';
        const m = Math.floor(seconds / 60);
        const s = Math.floor(seconds % 60);
        return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
    }

    connect();
})();
