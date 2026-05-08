package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"

	tea "github.com/charmbracelet/bubbletea"
)

func main() {
	port := flag.Int("port", 9090, "HTTP server port")
	publicDirFlag := flag.String("public", "", "Path to public/ directory (default: ../public relative to binary)")
	flag.Parse()

	// Locate resources relative to the Go binary
	execDir, err := os.Executable()
	if err != nil {
		execDir, _ = os.Getwd()
	}
	baseDir := filepath.Dir(execDir)

	publicDir := *publicDirFlag
	if publicDir == "" {
		publicDir = filepath.Join(baseDir, "..", "public")
	}

	audioPath := filepath.Join(baseDir, "audio", "audio.exe")
	smtcPath := filepath.Join(baseDir, "smtc", "smtc.exe")

	// Verify binaries exist
	if _, err := os.Stat(audioPath); os.IsNotExist(err) {
		fmt.Fprintf(os.Stderr, "Audio binary not found at %s\n", audioPath)
		os.Exit(1)
	}
	if _, err := os.Stat(smtcPath); os.IsNotExist(err) {
		fmt.Fprintf(os.Stderr, "SMTC binary not found at %s\n", smtcPath)
		os.Exit(1)
	}

	m := newModel(audioPath, smtcPath, publicDir, *port)
	p := tea.NewProgram(m)
	if _, err := p.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}
}
