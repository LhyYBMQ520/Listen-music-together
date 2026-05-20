package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
)

// AudioFormat describes the PCM audio format received from the audio module.
type AudioFormat struct {
	SampleRate    uint32 `json:"sampleRate"`
	Channels      uint16 `json:"channels"`
	BitsPerSample uint16 `json:"bitsPerSample"`
	BlockAlign    uint16 `json:"blockAlign"`
	FormatTag     uint16 `json:"formatTag"`
}

// AudioSession represents a capturable audio session.
type AudioSession struct {
	Index       int    `json:"index"`
	PID         int    `json:"pid"`
	ProcessName string `json:"processName"`
	State       string `json:"state"`
	Volume      int    `json:"volume"`
	Muted       bool   `json:"muted"`
}

// Frame type constants for binary framing protocol.
const (
	FrameText        byte = 0x01
	FrameAudioChunk  byte = 0x02
	FrameAudioFormat byte = 0x03
	FrameShutdown    byte = 0xFF
)

// maxFramePayload limits the payload size per frame to prevent memory exhaustion.
const maxFramePayload = 16 * 1024 * 1024 // 16 MiB

// readFrame reads a single framed message from r.
// Wire format: [type:1][length:3 big-endian][payload:N].
func readFrame(r io.Reader) (byte, []byte, error) {
	var header [4]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return 0, nil, err
	}
	typ := header[0]
	length := uint32(header[1])<<16 | uint32(header[2])<<8 | uint32(header[3])
	if length > maxFramePayload {
		return 0, nil, fmt.Errorf("frame payload too large: %d", length)
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	return typ, payload, nil
}

// AudioManager manages the audio.exe slave process.
type AudioManager struct {
	cmd               *exec.Cmd
	stdin             io.WriteCloser
	stdout            io.ReadCloser
	hub               *Hub
	cancel            chan struct{}
	stopCaptureFlag   atomic.Bool
	listingInProgress atomic.Bool
	formatInfo        *AudioFormat
	running           atomic.Bool
	capturing         atomic.Bool
	capturedBytes     atomic.Uint64
	mu                sync.Mutex // protects stdin writes
	readMu            sync.Mutex // serializes stdout reads
}

// Exited returns true if the audio process has exited.
func (am *AudioManager) Exited() bool {
	return !am.running.Load()
}

// IsCapturing returns true if audio capture is active.
func (am *AudioManager) IsCapturing() bool {
	return am.capturing.Load()
}

// GetCapturedBytes returns the number of bytes captured so far.
func (am *AudioManager) GetCapturedBytes() uint64 {
	return am.capturedBytes.Load()
}

// NewAudioManager creates a new AudioManager.
func NewAudioManager(hub *Hub) *AudioManager {
	return &AudioManager{
		hub:    hub,
		cancel: make(chan struct{}),
	}
}

// Start launches the audio.exe process (auto-detects slave mode when stdin is a pipe).
func (am *AudioManager) Start(audioPath string) error {
	am.cmd = exec.Command(audioPath)
	am.cmd.Stderr = os.Stderr

	var err error
	am.stdin, err = am.cmd.StdinPipe()
	if err != nil {
		return fmt.Errorf("failed to create stdin pipe: %w", err)
	}

	am.stdout, err = am.cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("failed to create stdout pipe: %w", err)
	}

	if err := am.cmd.Start(); err != nil {
		return fmt.Errorf("failed to start audio process: %w", err)
	}

	am.running.Store(true)
	tuiLog("Audio process started (PID=%d)", am.cmd.Process.Pid)
	return nil
}

// readSessionsLocked reads session lines until READY. Caller must hold readMu.
func (am *AudioManager) readSessionsLocked() ([]AudioSession, error) {
	am.listingInProgress.Store(true)
	defer am.listingInProgress.Store(false)

	var sessions []AudioSession

	for {
		typ, payload, err := readFrame(am.stdout)
		if err != nil {
			if am.cmd != nil && am.cmd.ProcessState != nil {
				return nil, fmt.Errorf("audio process exited with code %d", am.cmd.ProcessState.ExitCode())
			}
			return nil, fmt.Errorf("failed to read from audio process: %w", err)
		}

		if typ == FrameShutdown {
			return nil, fmt.Errorf("audio process shut down")
		}

		if typ != FrameText {
			tuiLog("[protocol] unexpected frame type 0x%02X during session listing", typ)
			continue
		}

		line := string(payload)
		tuiLog("[protocol] read: %s", line)

		if strings.HasPrefix(line, "ERROR|") {
			return nil, fmt.Errorf("audio process error: %s", strings.TrimPrefix(line, "ERROR|"))
		}

		if line == "READY" {
			break
		}

		if strings.HasPrefix(line, "SESSION|") {
			parts := strings.SplitN(line, "|", 7)
			if len(parts) >= 7 {
				idx, _ := strconv.Atoi(parts[1])
				pid, _ := strconv.Atoi(parts[2])
				vol, _ := strconv.Atoi(parts[5])
				muted := parts[6] == "1"
				sessions = append(sessions, AudioSession{
					Index:       idx,
					PID:         pid,
					ProcessName: parts[3],
					State:       parts[4],
					Volume:      vol,
					Muted:       muted,
				})
			}
		}
	}

	return sessions, nil
}

// ListSessions reads the initial session list from stdout. Called once after Start().
func (am *AudioManager) ListSessions() ([]AudioSession, error) {
	am.readMu.Lock()
	defer am.readMu.Unlock()

	return am.readSessionsLocked()
}

// RefreshAndListSessions sends REFRESH, then reads the session list. Safe any time except during capture.
func (am *AudioManager) RefreshAndListSessions() ([]AudioSession, error) {
	am.mu.Lock()
	if am.stdin != nil {
		n, err := fmt.Fprintf(am.stdin, "REFRESH\n")
		tuiLog("[protocol] sent REFRESH (n=%d, err=%v)", n, err)
	}
	am.mu.Unlock()

	am.readMu.Lock()
	defer am.readMu.Unlock()

	tuiLog("[protocol] start reading session list...")
	sessions, err := am.readSessionsLocked()
	tuiLog("[protocol] done reading: %d sessions, err=%v", len(sessions), err)
	return sessions, err
}

// SelectSession sends the selected session index to the audio process.
func (am *AudioManager) SelectSession(index int) error {
	if am.listingInProgress.Load() {
		return fmt.Errorf("session list refresh in progress, please retry")
	}

	am.stopCaptureFlag.Store(false)

	am.mu.Lock()
	defer am.mu.Unlock()

	n, err := fmt.Fprintf(am.stdin, "%d\n", index)
	tuiLog("[protocol] sent index %d (n=%d, err=%v)", index, n, err)
	if err != nil {
		return fmt.Errorf("failed to send session selection: %w", err)
	}
	return nil
}

// ReadAudioAndBroadcast reads framed messages from audio stdout, dispatching by type.
// Blocks until StopCapture is called or the process shuts down.
func (am *AudioManager) ReadAudioAndBroadcast() {
	am.readMu.Lock()
	defer am.readMu.Unlock()

	defer func() {
		am.capturing.Store(false)
		am.capturedBytes.Store(0)
		am.hub.ClearAudioFormatJSON()
		am.hub.BroadcastText([]byte(`{"type":"capture-stopped"}`))
	}()

	for {
		if am.stopCaptureFlag.Load() {
			return
		}

		typ, payload, err := readFrame(am.stdout)
		if err != nil {
			if err == io.EOF {
				tuiLog("Audio stdout closed")
			} else {
				tuiLog("Audio read error: %v", err)
			}
			return
		}

		switch typ {
		case FrameAudioFormat:
			if len(payload) < 10 {
				tuiLog("Invalid AUDIO_FORMAT frame: %d bytes", len(payload))
				return
			}
			sampleRate := binary.LittleEndian.Uint32(payload[0:4])
			channels := binary.LittleEndian.Uint16(payload[4:6])
			bitsPerSample := binary.LittleEndian.Uint16(payload[6:8])
			blockAlign := binary.LittleEndian.Uint16(payload[8:10])
			var formatTag uint16
			if len(payload) >= 12 {
				formatTag = binary.LittleEndian.Uint16(payload[10:12])
			}

			am.formatInfo = &AudioFormat{
				SampleRate:    sampleRate,
				Channels:      channels,
				BitsPerSample: bitsPerSample,
				BlockAlign:    blockAlign,
				FormatTag:     formatTag,
			}

			formatMsg := map[string]interface{}{
				"type":   "audio-format",
				"format": am.formatInfo,
			}
			if formatJSON, err := json.Marshal(formatMsg); err == nil {
				am.hub.BroadcastText(formatJSON)
				am.hub.SetAudioFormatJSON(formatJSON)
			}

			tuiLog("Audio format: %d Hz, %d ch, %d bits, align=%d",
				sampleRate, channels, bitsPerSample, blockAlign)
			am.capturing.Store(true)

		case FrameAudioChunk:
			if len(payload) > 0 {
				am.capturedBytes.Add(uint64(len(payload)))
				am.hub.BroadcastBinary(payload)
			}

		case FrameText:
			line := string(payload)
			tuiLog("[protocol] capture-text: %s", line)
			if strings.HasPrefix(line, "STOPPED|") {
				return
			}
			if strings.HasPrefix(line, "ERROR|") {
				tuiLog("Audio process error during capture: %s", strings.TrimPrefix(line, "ERROR|"))
				return
			}

		case FrameShutdown:
			tuiLog("Audio process sent SHUTDOWN frame")
			return

		default:
			tuiLog("Unknown frame type: 0x%02X", typ)
		}
	}
}

// StopCapture sends the STOP command to the audio process and signals ReadAudioAndBroadcast to exit.
func (am *AudioManager) StopCapture() {
	am.stopCaptureFlag.Store(true)
	am.hub.ClearAudioFormatJSON()

	am.mu.Lock()
	defer am.mu.Unlock()

	if am.stdin != nil {
		n, err := fmt.Fprintf(am.stdin, "STOP\n")
		tuiLog("[protocol] sent STOP (n=%d, err=%v)", n, err)
	}
}

// Shutdown gracefully stops the audio process.
func (am *AudioManager) Shutdown() {
	if !am.running.Load() {
		return
	}

	close(am.cancel)

	if am.stdin != nil {
		fmt.Fprintf(am.stdin, "EXIT\n")
	}

	if am.cmd != nil && am.cmd.Process != nil {
		am.cmd.Process.Kill()
		am.cmd.Wait()
		tuiLog("Audio process stopped")
	}

	am.running.Store(false)
}

// SMTCManager manages the smtc.exe process.
type SMTCManager struct {
	cmd *exec.Cmd
}

// Start launches the smtc.exe process.
func (sm *SMTCManager) Start(smtcPath string) error {
	sm.cmd = exec.Command(smtcPath)

	if err := sm.cmd.Start(); err != nil {
		return fmt.Errorf("failed to start smtc process: %w", err)
	}

	tuiLog("SMTC process started (PID=%d)", sm.cmd.Process.Pid)
	return nil
}

// Shutdown gracefully stops the SMTC process.
func (sm *SMTCManager) Shutdown() {
	if sm.cmd != nil && sm.cmd.Process != nil {
		sm.cmd.Process.Kill()
		sm.cmd.Wait()
		tuiLog("SMTC process stopped")
	}
}
