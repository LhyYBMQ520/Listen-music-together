package main

import (
	"fmt"
	"strings"
	"sync"
	"time"

	"github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

// --- Messages ---

type refreshTickMsg time.Time

type logMsg string

type sessionsMsg struct {
	sessions []AudioSession
	err      error
}

type captureStatusMsg struct {
	bytes   uint64
	running bool
}

type serverReadyMsg struct {
	port int
}

type audioExitedMsg struct{}

// --- Logger for TUI ---

var (
	logLines    []string
	logMaxLines = 200
	logMu       sync.Mutex
)

func tuiLog(format string, args ...interface{}) {
	logMu.Lock()
	defer logMu.Unlock()
	line := fmt.Sprintf("[%s] %s", time.Now().Format("15:04:05"), fmt.Sprintf(format, args...))
	logLines = append(logLines, line)
	if len(logLines) > logMaxLines {
		logLines = logLines[len(logLines)-logMaxLines:]
	}
}

func getLogs() []string {
	logMu.Lock()
	defer logMu.Unlock()
	c := make([]string, len(logLines))
	copy(c, logLines)
	return c
}

// --- Styles ---

var (
	styleLeft  = lipgloss.NewStyle().Width(46).Padding(0, 1)
	styleRight = lipgloss.NewStyle().Width(40).Padding(0, 1)
	styleTitle = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("6")).MarginBottom(1)
	styleLog   = lipgloss.NewStyle().Foreground(lipgloss.Color("8")).MaxHeight(20)
	styleHelp  = lipgloss.NewStyle().Foreground(lipgloss.Color("8")).MarginTop(1)

	styleListHeader = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("4"))
	styleActive     = lipgloss.NewStyle().Foreground(lipgloss.Color("2")).Bold(true)
	styleInactive   = lipgloss.NewStyle().Foreground(lipgloss.Color("8"))
	styleCapture    = lipgloss.NewStyle().Foreground(lipgloss.Color("3")).Bold(true)
	styleError      = lipgloss.NewStyle().Foreground(lipgloss.Color("1"))
	stylePlaying    = lipgloss.NewStyle().Foreground(lipgloss.Color("2"))
)

// --- Model ---

type model struct {
	sessions      []AudioSession
	isCapturing   bool
	captureTarget string
	captureBytes  uint64
	captureErr    string

	hub          *Hub
	audioManager *AudioManager
	smtcManager  *SMTCManager
	serverPort   int
	serverReady  bool
	audioExited  bool
	audioPath    string
	smtcPath     string
	publicDir    string

	inputBuf  string
	termWidth int
	leftWidth int
	quitting  bool
	err       error
}

const defaultTermWidth = 100

func newModel(audioPath, smtcPath, publicDir string, port int) model {
	return model{
		audioPath:  audioPath,
		smtcPath:   smtcPath,
		publicDir:  publicDir,
		serverPort: port,
		termWidth:  defaultTermWidth,
		leftWidth:  55,
	}
}

func (m model) Init() tea.Cmd {
	return tea.Batch(
		startSubsystems(m.audioPath, m.smtcPath, m.publicDir, m.serverPort),
		refreshTick(),
	)
}

// --- Subsystem startup ---

func startSubsystems(audioPath, smtcPath, publicDir string, port int) tea.Cmd {
	return func() tea.Msg {
		hub := NewHub()
		go hub.Run()

		// Start SMTC
		smtc := &SMTCManager{}
		if err := smtc.Start(smtcPath); err != nil {
			tuiLog("SMTC start error: %v", err)
		} else {
			tuiLog("SMTC started (PID=%d)", smtc.cmd.Process.Pid)
		}

		// Start audio
		audio := NewAudioManager(hub)
		if err := audio.Start(audioPath); err != nil {
			tuiLog("Audio start error: %v", err)
		} else {
			tuiLog("Audio started (PID=%d)", audio.cmd.Process.Pid)
		}


		// Start HTTP server
		go func() {
			tuiLog("HTTP server starting on :%d", port)
			if err := StartServer(hub, publicDir, port); err != nil {
				tuiLog("HTTP server error: %v", err)
			}
		}()

		// Start SMTC polling
		go PollSmtcAndBroadcast(hub)

		return subsystemReadyMsg{
			hub:          hub,
			audioManager: audio,
			smtcManager:  smtc,
			port:         port,
		}
	}
}

type subsystemReadyMsg struct {
	hub          *Hub
	audioManager *AudioManager
	smtcManager  *SMTCManager
	port         int
}

// --- Commands ---

func refreshTick() tea.Cmd {
	return tea.Tick(3*time.Second, func(t time.Time) tea.Msg {
		return refreshTickMsg(t)
	})
}

// initialFetchSessions reads the initial session list without sending REFRESH.
// Used for the first fetch after audio process starts.
func initialFetchSessions(audio *AudioManager) tea.Cmd {
	return func() tea.Msg {
		if audio == nil || audio.Exited() {
			return sessionsMsg{err: fmt.Errorf("audio process not running")}
		}
		sessions, err := audio.ListSessions()
		return sessionsMsg{sessions: sessions, err: err}
	}
}

func fetchSessions(audio *AudioManager) tea.Cmd {
	return func() tea.Msg {
		if audio == nil || audio.Exited() {
			return sessionsMsg{err: fmt.Errorf("audio process not running")}
		}
		sessions, err := audio.RefreshAndListSessions()
		return sessionsMsg{sessions: sessions, err: err}
	}
}

func capturePing(audio *AudioManager) tea.Cmd {
	return tea.Tick(300*time.Millisecond, func(t time.Time) tea.Msg {
		bytes := audio.GetCapturedBytes()
		if !audio.IsCapturing() {
			return captureStatusMsg{running: false, bytes: bytes}
		}
		return captureStatusMsg{running: true, bytes: bytes}
	})
}

// --- Update ---

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.KeyMsg:
		switch msg.String() {
		case "ctrl+c", "esc", "q":
			m.quitting = true
			m.shutdown()
			return m, tea.Quit

		case "enter":
			if m.isCapturing {
				// Stop capture — captureStatusMsg will trigger the session refresh.
				m.audioManager.StopCapture()
				m.isCapturing = false
				m.captureTarget = ""
				m.captureBytes = 0
				m.inputBuf = ""
				tuiLog("Capture stopped")
				return m, nil
			} else if m.inputBuf != "" {
				return m, m.selectSession()
			}

		case "backspace":
			if !m.isCapturing && len(m.inputBuf) > 0 {
				m.inputBuf = m.inputBuf[:len(m.inputBuf)-1]
			}

		default:
			if !m.isCapturing && len(msg.String()) == 1 {
				ch := msg.String()[0]
				if ch >= '0' && ch <= '9' {
					m.inputBuf += msg.String()
				}
			}
		}

	case tea.WindowSizeMsg:
		m.termWidth = msg.Width
		m.leftWidth = msg.Width * 55 / 100
		if m.leftWidth > 55 {
			m.leftWidth = 55
		}
		styleLeft = styleLeft.Width(m.leftWidth)
		styleRight = styleRight.Width(msg.Width - m.leftWidth - 4)
		logMaxLines = msg.Height - 6

	case subsystemReadyMsg:
		m.hub = msg.hub
		m.audioManager = msg.audioManager
		m.smtcManager = msg.smtcManager
		m.serverPort = msg.port
		m.serverReady = true
		return m, initialFetchSessions(m.audioManager)

	case refreshTickMsg:
		if !m.isCapturing && m.audioManager != nil && !m.audioExited {
			return m, tea.Batch(refreshTick(), fetchSessions(m.audioManager))
		}
		return m, refreshTick()

	case sessionsMsg:
		if msg.err != nil {
			tuiLog("Session refresh error: %v", msg.err)
			if strings.Contains(msg.err.Error(), "EOF") || strings.Contains(msg.err.Error(), "exited") {
				m.audioExited = true
				tuiLog("Audio process has exited")
			}
		} else {
			m.sessions = msg.sessions
		}
		return m, nil

	case captureStatusMsg:
		m.captureBytes = msg.bytes
		if !msg.running {
			m.isCapturing = false
			m.captureTarget = ""
			m.captureBytes = 0
			m.inputBuf = ""
			tuiLog("Capture ended, total bytes: %d", msg.bytes)
			return m, tea.Batch(refreshTick(), fetchSessions(m.audioManager))
		}
		return m, capturePing(m.audioManager)

	case logMsg:
		tuiLog("%s", string(msg))
		return m, nil
	}

	return m, nil
}

func (m *model) selectSession() tea.Cmd {
	var idx int
	if _, err := fmt.Sscanf(m.inputBuf, "%d", &idx); err != nil || idx < 1 || idx > len(m.sessions) {
		tuiLog("Invalid session index: %s", m.inputBuf)
		m.inputBuf = ""
		return nil
	}

	selected := m.sessions[idx-1]
	if selected.PID == 0 {
		tuiLog("Cannot capture system audio session")
		m.inputBuf = ""
		return nil
	}

	if err := m.audioManager.SelectSession(idx); err != nil {
		tuiLog("Failed to select session: %v", err)
		m.inputBuf = ""
		return nil
	}

	m.isCapturing = true
	m.captureTarget = fmt.Sprintf("%s (PID=%d)", selected.ProcessName, selected.PID)
	m.captureBytes = 0
	m.inputBuf = ""
	tuiLog("Capture started: %s", m.captureTarget)

	m.audioManager.capturing.Store(true)
	go m.audioManager.ReadAudioAndBroadcast()
	return capturePing(m.audioManager)
}

func (m *model) shutdown() {
	if m.audioManager != nil {
		m.audioManager.Shutdown()
	}
	if m.smtcManager != nil {
		m.smtcManager.Shutdown()
	}
}

// --- View ---

func (m model) View() string {
	if m.quitting {
		return "Shutting down...\n"
	}

	left := m.renderLeft()
	right := m.renderRight()

	content := lipgloss.JoinHorizontal(lipgloss.Top, left, right)

	help := styleHelp.Render("Enter: select/stop capture | Backspace: delete digit | Ctrl+C/ESC/q: quit all")
	if m.isCapturing {
		help = styleHelp.Render("Enter: stop capture | Ctrl+C/ESC/q: quit all")
	}

	return lipgloss.JoinVertical(lipgloss.Left,
		styleTitle.Render("Listen Music Together"),
		content,
		help,
	)
}

func (m model) renderLeft() string {
	var sb strings.Builder

	sb.WriteString(styleListHeader.Render("Audio Sessions"))
	sb.WriteString("  ")
	if !m.isCapturing {
		sb.WriteString(styleInactive.Render("(auto-refresh 3s)"))
	}
	sb.WriteString("\n")

	if m.audioExited {
		sb.WriteString(styleError.Render("\nAudio process has exited.\n"))
	} else if !m.serverReady {
		sb.WriteString(styleInactive.Render("\nStarting subsystems...\n"))
	} else if m.isCapturing {
		sb.WriteString(styleCapture.Render(fmt.Sprintf("\n● Capturing: %s\n", m.captureTarget)))
		sb.WriteString(fmt.Sprintf("  Bytes: %d\n\n", m.captureBytes))
		sb.WriteString(styleInactive.Render("  Press Enter to stop capture.\n"))
	} else if len(m.sessions) == 0 {
		sb.WriteString(styleInactive.Render("\nNo audio sessions found.\n"))
	} else {
		sb.WriteString(fmt.Sprintf("%s\n", strings.Repeat("─", m.leftWidth-2)))
		for i, s := range m.sessions {
			prefix := "  "
			highlight := styleInactive
			if s.State == "Active" {
				highlight = styleActive
			}
			sb.WriteString(prefix)
			idxStr := fmt.Sprintf("[%d]", s.Index)
			muted := ""
			if s.Muted {
				muted = " (Muted)"
			}
			line := fmt.Sprintf("%-4s %-18s PID=%-6d %s%s",
				idxStr, truncate(s.ProcessName, 18), s.PID, s.State, muted)
			_ = i
			sb.WriteString(highlight.Render(line))
			sb.WriteString("\n")
		}
		sb.WriteString(fmt.Sprintf("%s\n", strings.Repeat("─", m.leftWidth-2)))
	}

	if !m.isCapturing && m.serverReady && len(m.sessions) > 0 {
		sb.WriteString(fmt.Sprintf("\nSelect session: %s▊\n", m.inputBuf))
	}

	sb.WriteString(fmt.Sprintf("\nHTTP: :%d  WS: /ws", m.serverPort))

	return styleLeft.Render(sb.String())
}

func (m model) renderRight() string {
	var sb strings.Builder
	sb.WriteString(styleListHeader.Render("Server Logs"))
	sb.WriteString("\n")
	sb.WriteString(strings.Repeat("─", 38))
	sb.WriteString("\n")

	logs := getLogs()
	start := 0
	if len(logs) > 22 {
		start = len(logs) - 22
	}
	for _, l := range logs[start:] {
		sb.WriteString(styleLog.Render(l))
		sb.WriteString("\n")
	}

	return styleRight.Render(sb.String())
}

func truncate(s string, max int) string {
	runes := []rune(s)
	if len(runes) <= max {
		return s
	}
	return string(runes[:max-1]) + "…"
}
