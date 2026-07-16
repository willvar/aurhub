package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/willvar/aurhub/internal/memdb"
	"github.com/willvar/aurhub/internal/mirror"
	"github.com/willvar/aurhub/internal/rpc"
)

var (
	syncInterval = 30 * time.Minute
)

func dataDir() string {
	xdg := os.Getenv("XDG_DATA_HOME")
	if xdg == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			home = "/tmp"
		}
		xdg = filepath.Join(home, ".local", "share")
	}
	return filepath.Join(xdg, "aurhub")
}

func pidFile() string  { return filepath.Join(dataDir(), "aurhub.pid") }
func dbFile() string   { return filepath.Join(dataDir(), "aurhub.gob") }
func mirrorDir() string { return filepath.Join(dataDir(), "mirror") }

func usage() {
	fmt.Fprint(os.Stderr, `aurhub — AUR fallback mirror, backed by GitHub

Usage:
  aurhub serve [-a addr]   Start server (auto-init + periodic sync)
  aurhub stop              Stop running server
  aurhub restart [-a addr] Restart server
  aurhub status            Show run status

Examples:
  aurhub serve -a :9090
  aurhub status
`)
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(1)
	}

	switch os.Args[1] {
	case "serve":
		doServe()
	case "stop":
		doStop()
	case "restart":
		doRestart()
	case "status":
		doStatus()
	default:
		usage()
	}
}

func doServe() {
	fs := flag.NewFlagSet("serve", flag.ExitOnError)
	addr := fs.String("a", ":8080", "listen address")
	fs.Parse(os.Args[2:])

	os.MkdirAll(dataDir(), 0755)

	if pid, running := readPID(); running {
		fmt.Fprintf(os.Stderr, "aurhub: already running (pid %d)\n", pid)
		os.Exit(1)
	}

	db, err := memdb.Open(dbFile())
	if err != nil {
		fmt.Fprintf(os.Stderr, "aurhub: db open: %v\n", err)
		os.Exit(1)
	}

	m, err := mirror.Open(mirrorDir(), db)
	if err != nil {
		fmt.Fprintf(os.Stderr, "aurhub: mirror open: %v\n", err)
		os.Exit(1)
	}

	if err := m.Init(); err != nil {
		fmt.Fprintf(os.Stderr, "aurhub: mirror init: %v\n", err)
		os.Exit(1)
	}

	needSave := false
	if len(db.Entries) == 0 {
		t0 := time.Now()
		fmt.Println("aurhub: first run, building index...")
		if _, _, err := m.Sync(12); err != nil {
			fmt.Fprintf(os.Stderr, "aurhub: initial sync: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("aurhub: initial index built in %v\n", time.Since(t0).Round(time.Second))
		needSave = true
	}

	if needSave {
		if err := db.Save(dbFile()); err != nil {
			fmt.Fprintf(os.Stderr, "aurhub: save: %v\n", err)
			os.Exit(1)
		}
	}

	if err := writePID(); err != nil {
		fmt.Fprintf(os.Stderr, "aurhub: cannot write pid file: %v\n", err)
		os.Exit(1)
	}
	defer os.Remove(pidFile())

	stopSync := make(chan struct{})
	go autoSync(db, stopSync)

	srv := rpc.NewServer(*addr, db)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		fmt.Fprintln(os.Stderr, "\naurhub: shutting down...")
		close(stopSync)
		os.Remove(pidFile())
		os.Exit(0)
	}()

	if err := srv.ListenAndServe(); err != nil {
		fmt.Fprintf(os.Stderr, "aurhub: %v\n", err)
		close(stopSync)
		os.Remove(pidFile())
		os.Exit(1)
	}
}

func autoSync(db *memdb.DB, stop <-chan struct{}) {
	time.Sleep(10 * time.Second)

	runSync := func() {
		m, err := mirror.Open(mirrorDir(), db)
		if err != nil {
			fmt.Fprintf(os.Stderr, "aurhub: auto-sync: %v\n", err)
			return
		}
		updated, _, err := m.Sync(12)
		if err != nil {
			fmt.Fprintf(os.Stderr, "aurhub: auto-sync: %v\n", err)
			return
		}
		if updated > 0 {
			fmt.Printf("aurhub: auto-sync — %d updated\n", updated)
		}
		if err := db.Save(dbFile()); err != nil {
			fmt.Fprintf(os.Stderr, "aurhub: auto-sync save: %v\n", err)
		}
	}

	ticker := time.NewTicker(syncInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			runSync()
		case <-stop:
			return
		}
	}
}

func doStop() {
	pid, running := readPID()
	if !running {
		fmt.Println("aurhub: not running")
		os.Exit(0)
	}

	proc, err := os.FindProcess(pid)
	if err != nil {
		fmt.Println("aurhub: not running")
		os.Remove(pidFile())
		os.Exit(0)
	}

	fmt.Printf("aurhub: stopping (pid %d)...\n", pid)
	proc.Signal(syscall.SIGTERM)

	for i := 0; i < 50; i++ {
		time.Sleep(100 * time.Millisecond)
		if _, running := readPID(); !running {
			fmt.Println("aurhub: stopped")
			os.Exit(0)
		}
	}

	proc.Signal(syscall.SIGKILL)
	os.Remove(pidFile())
	fmt.Println("aurhub: killed")
}

func doRestart() {
	fs := flag.NewFlagSet("restart", flag.ExitOnError)
	addr := fs.String("a", ":8080", "listen address")
	fs.Parse(os.Args[2:])

	pid, running := readPID()
	if running {
		proc, _ := os.FindProcess(pid)
		proc.Signal(syscall.SIGTERM)
		for i := 0; i < 50; i++ {
			time.Sleep(100 * time.Millisecond)
			if _, running := readPID(); !running {
				break
			}
		}
	}

	os.Remove(pidFile())

	exe, err := os.Executable()
	if err != nil {
		fmt.Fprintf(os.Stderr, "aurhub: cannot find executable: %v\n", err)
		os.Exit(1)
	}

	args := []string{"serve"}
	if *addr != ":8080" {
		args = append(args, "-a", *addr)
	}

	cmd := exec.Command(exe, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Start()

	fmt.Printf("aurhub: restarted (pid %d)\n", cmd.Process.Pid)
}

func doStatus() {
	pid, running := readPID()
	if !running {
		fmt.Println("aurhub: not running")
		os.Exit(0)
	}

	db, err := memdb.Open(dbFile())
	if err != nil {
		fmt.Printf("aurhub: running (pid %d)\naurhub: cannot read db: %v\n", pid, err)
		os.Exit(0)
	}

	fmt.Printf("aurhub: running (pid %d)\n", pid)
	fmt.Printf("aurhub: last sync:  %s\n", db.LastSync().Format("2006-01-02 15:04:05"))
	fmt.Printf("aurhub: packages:   %d\n", len(db.Entries))
	fmt.Printf("aurhub: branches:   %d\n", len(db.Branches))
	fmt.Printf("aurhub: data dir:   %s\n", dataDir())
}

func writePID() error {
	return os.WriteFile(pidFile(), []byte(fmt.Sprintf("%d\n", os.Getpid())), 0644)
}

func readPID() (int, bool) {
	data, err := os.ReadFile(pidFile())
	if err != nil {
		return 0, false
	}
	pid, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil {
		return 0, false
	}
	proc, err := os.FindProcess(pid)
	if err != nil {
		return 0, false
	}
	if err := proc.Signal(syscall.Signal(0)); err != nil {
		return 0, false
	}
	return pid, true
}
