package memdb

import (
	"encoding/gob"
	"fmt"
	"os"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/willvar/aurhub/internal/srvinfo"
)

func init() {
	gob.Register(&DB{})
}

type 	dbEntry struct {
	Entry      srvinfo.Entry
	SearchText string
}

type DB struct {
	Entries     []dbEntry
	ByName      map[string]int
	ByFirstChar [256][]int
	Names       []string
	Branches    map[string]int64
	UpdatedAt   time.Time
	mu          sync.RWMutex
}

func Open(path string) (*DB, error) {
	d := &DB{
		ByName:   make(map[string]int),
		Branches: make(map[string]int64),
	}

	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return d, nil
		}
		return nil, fmt.Errorf("open memdb: %w", err)
	}
	defer f.Close()

	if err := gob.NewDecoder(f).Decode(d); err != nil {
		return nil, fmt.Errorf("decode memdb: %w", err)
	}
	if d.ByName == nil {
		d.ByName = make(map[string]int)
	}
	if d.Branches == nil {
		d.Branches = make(map[string]int64)
	}

	return d, nil
}

func (d *DB) Save(path string) error {
	d.mu.RLock()
	defer d.mu.RUnlock()

	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return gob.NewEncoder(f).Encode(d)
}

func (d *DB) GetBranchTS(branch string) (int64, bool, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()
	ts, ok := d.Branches[branch]
	return ts, ok, nil
}

func (d *DB) Upsert(entry *srvinfo.Entry, branch string, commitTS int64) error {
	d.mu.Lock()
	defer d.mu.Unlock()

	de := dbEntry{
		Entry:      *entry,
		SearchText: strings.ToLower(entry.Name + " " + entry.Description),
	}

	if idx, ok := d.ByName[entry.Name]; ok {
		d.Entries[idx] = de
	} else {
		d.ByName[entry.Name] = len(d.Entries)
		d.Entries = append(d.Entries, de)
	}

	d.Branches[branch] = commitTS
	return nil
}

func (d *DB) Finalize() {
	d.mu.Lock()
	defer d.mu.Unlock()

	for i := range d.Entries {
		ch := d.Entries[i].SearchText[0]
		d.ByFirstChar[ch] = append(d.ByFirstChar[ch], i)
	}

	d.Names = make([]string, 0, len(d.ByName))
	for name := range d.ByName {
		d.Names = append(d.Names, name)
	}
	sort.Strings(d.Names)
	d.UpdatedAt = time.Now()
}

type RPCPackage struct {
	Name          string   `json:"Name"`
	PackageBase   string   `json:"PackageBase"`
	Version       string   `json:"Version"`
	Description   string   `json:"Description"`
	URL           string   `json:"URL"`
	NumVotes      int      `json:"NumVotes"`
	Popularity    float64  `json:"Popularity"`
	OutOfDate     *int64   `json:"OutOfDate"`
	Maintainer    string   `json:"Maintainer"`
	FirstSubmitted int64   `json:"FirstSubmitted"`
	LastModified  int64    `json:"LastModified"`
	URLPath       string   `json:"URLPath"`
	Depends       []string `json:"Depends"`
	MakeDepends   []string `json:"MakeDepends"`
	CheckDepends  []string `json:"CheckDepends"`
	OptDepends    []string `json:"OptDepends"`
	Provides      []string `json:"Provides"`
	Conflicts     []string `json:"Conflicts"`
	License       []string `json:"License"`
	Keywords      []string `json:"Keywords"`
}

type SearchResult struct {
	Version     int          `json:"version"`
	Type        string       `json:"type"`
	ResultCount int          `json:"resultcount"`
	Results     []RPCPackage `json:"results"`
	Error       string       `json:"error,omitempty"`
}

func toRPC(e *srvinfo.Entry) RPCPackage {
	r := RPCPackage{
		Name:           e.Name,
		PackageBase:    e.Base,
		Version:        e.Version,
		Description:    e.Description,
		URL:            e.URL,
		Maintainer:     "aurhub",
		License:        e.License,
		Depends:        e.Depends,
		MakeDepends:    e.MakeDepends,
		CheckDepends:   e.CheckDepends,
		OptDepends:     e.OptDepends,
		Provides:       e.Provides,
		Conflicts:      e.Conflicts,
		Keywords:       e.Keywords,
		URLPath:        "/cgit/aur.git/snapshot/" + e.Name + ".tar.gz",
		FirstSubmitted: e.UpdatedAt,
		LastModified:   e.UpdatedAt,
	}
	if r.License == nil {
		r.License = []string{}
	}
	if r.Depends == nil {
		r.Depends = []string{}
	}
	if r.MakeDepends == nil {
		r.MakeDepends = []string{}
	}
	if r.CheckDepends == nil {
		r.CheckDepends = []string{}
	}
	if r.OptDepends == nil {
		r.OptDepends = []string{}
	}
	if r.Provides == nil {
		r.Provides = []string{}
	}
	if r.Conflicts == nil {
		r.Conflicts = []string{}
	}
	if r.Keywords == nil {
		r.Keywords = []string{}
	}
	return r
}

func (d *DB) Search(by, arg string) (*SearchResult, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()

	var results []RPCPackage
	switch by {
	case "name":
		if idx, ok := d.ByName[arg]; ok {
			results = append(results, toRPC(&d.Entries[idx].Entry))
		}
	default:
		argLower := strings.ToLower(arg)
		if argLower != "" {
			if ids := d.ByFirstChar[argLower[0]]; len(ids) > 0 {
				for _, id := range ids {
					if strings.Contains(d.Entries[id].SearchText, argLower) {
						results = append(results, toRPC(&d.Entries[id].Entry))
						if len(results) >= 500 {
							break
						}
					}
				}
			}
		}
	}

	return &SearchResult{
		Version:     5,
		Type:        "search",
		ResultCount: len(results),
		Results:     results,
	}, nil
}

func (d *DB) Info(names []string) (*SearchResult, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()

	var results []RPCPackage
	for _, name := range names {
		if idx, ok := d.ByName[name]; ok {
			results = append(results, toRPC(&d.Entries[idx].Entry))
		}
	}

	return &SearchResult{
		Version:     5,
		Type:        "multiinfo",
		ResultCount: len(results),
		Results:     results,
	}, nil
}

func (d *DB) AllPackageNames() []string {
	d.mu.RLock()
	defer d.mu.RUnlock()
	return d.Names
}

func (d *DB) LastSync() time.Time {
	d.mu.RLock()
	defer d.mu.RUnlock()
	return d.UpdatedAt
}

func (d *DB) PurgeStale(activeBranches map[string]bool) int {
	d.mu.Lock()
	defer d.mu.Unlock()

	var stale []string
	for branch := range d.Branches {
		if !activeBranches[branch] {
			stale = append(stale, branch)
		}
	}
	for _, branch := range stale {
		delete(d.Branches, branch)
	}
	return len(stale)
}
