package mirror

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/willvar/aurhub/internal/memdb"
	"github.com/willvar/aurhub/internal/srvinfo"
)

const GitHubMirrorURL = "https://github.com/archlinux/aur.git"

type Mirror struct {
	dir string
	db  *memdb.DB
}

func Open(dir string, db *memdb.DB) (*Mirror, error) {
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, fmt.Errorf("mkdir mirror dir: %w", err)
	}
	return &Mirror{dir: dir, db: db}, nil
}

func (m *Mirror) RepoPath() string { return filepath.Join(m.dir, "aur.git") }

func (m *Mirror) Init() error {
	repoPath := m.RepoPath()
	if _, err := os.Stat(filepath.Join(repoPath, "HEAD")); err == nil {
		return nil
	}

	fmt.Println("aurhub: cloning AUR GitHub mirror (bare, full)...")
	cmd := exec.Command("git", "clone", "--bare", "--progress",
		GitHubMirrorURL, repoPath)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

type branchInfo struct {
	Name string
	TS   int64
}

func (m *Mirror) changedBranches() ([]branchInfo, error) {
	cmd := exec.Command("git", "-C", m.RepoPath(), "for-each-ref",
		"--format=%(refname:short) %(committerdate:unix)",
		"refs/heads/")
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("git for-each-ref: %w", err)
	}

	var branches []branchInfo
	for _, line := range bytes.Split(out, []byte("\n")) {
		parts := strings.SplitN(string(line), " ", 2)
		if len(parts) != 2 {
			continue
		}
		ts, err := strconv.ParseInt(parts[1], 10, 64)
		if err != nil {
			continue
		}
		branches = append(branches, branchInfo{Name: parts[0], TS: ts})
	}
	return branches, nil
}

func (m *Mirror) Fetch() error {
	cmd := exec.Command("git", "-C", m.RepoPath(), "fetch", "--all")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

func (m *Mirror) readSrcInfo(branch string) (string, error) {
	cmd := exec.Command("git", "-C", m.RepoPath(), "show",
		branch+":.SRCINFO")
	cmd.Stderr = nil
	out, err := cmd.Output()
	if err != nil {
		return "", fmt.Errorf("show %s:.SRCINFO: %w", branch, err)
	}
	return string(out), nil
}

func (m *Mirror) Sync(workers int) (int, int, error) {
	fmt.Println("aurhub: fetching updates...")
	if err := m.Fetch(); err != nil {
		return 0, 0, fmt.Errorf("fetch: %w", err)
	}

	fmt.Println("aurhub: scanning branches...")
	branches, err := m.changedBranches()
	if err != nil {
		return 0, 0, err
	}
	total := len(branches)
	fmt.Printf("aurhub: %d branches total\n", total)

	var (
		updated int64
		skipped int64

		jobs = make(chan branchInfo, 16384)
		wg   sync.WaitGroup
	)

	if workers <= 0 {
		workers = 8
	}

	tStart := time.Now()

	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for br := range jobs {
				cached, ok, err := m.db.GetBranchTS(br.Name)
				if err != nil {
					fmt.Fprintf(os.Stderr, "aurhub: branch %s db error: %v\n", br.Name, err)
					atomic.AddInt64(&skipped, 1)
					continue
				}
				if ok && cached >= br.TS && cached > 0 {
					atomic.AddInt64(&skipped, 1)
					continue
				}

				src, err := m.readSrcInfo(br.Name)
				if err != nil {
					atomic.AddInt64(&skipped, 1)
					continue
				}

				pkgs, err := srvinfo.Parse(src)
				if err != nil {
					fmt.Fprintf(os.Stderr, "\naurhub: parse %s: %v\n", br.Name, err)
					atomic.AddInt64(&skipped, 1)
					continue
				}

				for i := range pkgs {
					pkgs[i].UpdatedAt = br.TS
					if err := m.db.Upsert(&pkgs[i], br.Name, br.TS); err != nil {
						fmt.Fprintf(os.Stderr, "\naurhub: upsert %s: %v\n", br.Name, err)
						atomic.AddInt64(&skipped, 1)
						continue
					}
				}
				atomic.AddInt64(&updated, 1)
			}
		}()
	}

	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			proc := atomic.LoadInt64(&updated) + atomic.LoadInt64(&skipped)
			if proc >= int64(total) {
				return
			}
			elapsed := time.Since(tStart)
			pct := float64(proc) / float64(total) * 100
			speed := float64(proc) / elapsed.Seconds()
			fmt.Fprintf(os.Stderr, "\raurhub: %d/%d (%.0f%%) %.0f pkg/s    ",
				proc, total, pct, speed)
		}
	}()

	for _, br := range branches {
		jobs <- br
	}
	close(jobs)
	wg.Wait()

	fmt.Fprintf(os.Stderr, "\n")

	m.db.Finalize()

	activeBranches := make(map[string]bool, len(branches))
	for _, br := range branches {
		activeBranches[br.Name] = true
	}
	purged := m.db.PurgeStale(activeBranches)
	if purged > 0 {
		fmt.Printf("aurhub: purged %d deleted packages\n", purged)
	}

	return int(updated), int(skipped), nil
}
