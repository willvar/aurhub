package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"sync"

	srcinfo "github.com/Jguer/go-srcinfo"
	"github.com/willvar/aurhub/internal/memdb"
)

type branch struct {
	ref  string
	name string
}

type rpcPackage struct {
	Name         string
	PackageBase  string
	Version      string
	Description  string
	URL          string
	Depends      []string
	MakeDepends  []string
	CheckDepends []string
	OptDepends   []string
	Provides     []string
	Conflicts    []string
	Replaces     []string
	Groups       []string
	License      []string
}

func loadActual(path string) (map[string]rpcPackage, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	result := make(map[string]rpcPackage)
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 64*1024), 4*1024*1024)
	for scanner.Scan() {
		var pkg rpcPackage
		if err := json.Unmarshal(scanner.Bytes(), &pkg); err != nil {
			return nil, err
		}
		if _, exists := result[pkg.Name]; exists {
			return nil, fmt.Errorf("duplicate package in JSONL: %s", pkg.Name)
		}
		result[pkg.Name] = pkg
	}
	return result, scanner.Err()
}

func loadFatal(path string) (map[string]bool, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	result := make(map[string]bool)
	scanner := bufio.NewScanner(f)
	first := true
	for scanner.Scan() {
		if first {
			first = false
			continue
		}
		parts := strings.Split(scanner.Text(), "\t")
		if len(parts) >= 3 && parts[0] == "fatal" {
			result[parts[2]] = true
		}
	}
	return result, scanner.Err()
}

func listBranches(repo string) ([]branch, error) {
	cmd := exec.Command(
		"git", "--git-dir="+repo, "for-each-ref", "--sort=refname",
		"--format=%(refname)%09%(refname:short)", "refs/heads",
	)
	out, err := cmd.Output()
	if err != nil {
		return nil, err
	}

	lines := strings.Split(strings.TrimSpace(string(out)), "\n")
	result := make([]branch, 0, len(lines))
	for _, line := range lines {
		parts := strings.Split(line, "\t")
		if len(parts) != 2 {
			return nil, fmt.Errorf("bad branch line %q", line)
		}
		result = append(result, branch{ref: parts[0], name: parts[1]})
	}
	return result, nil
}

func flat(values []srcinfo.ArchString) []string {
	result := make([]string, 0, len(values))
	for _, value := range values {
		if value.Value != "" && value.Value != srcinfo.EmptyOverride {
			result = append(result, value.Value)
		}
	}
	return result
}

func plain(values []string) []string {
	result := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value != "" && value != srcinfo.EmptyOverride {
			result = append(result, value)
		}
	}
	return result
}

func scalar(value string) string {
	if value == srcinfo.EmptyOverride {
		return ""
	}
	return value
}

func version(info *srcinfo.Srcinfo) string {
	result := scalar(info.Pkgver) + "-" + scalar(info.Pkgrel)
	if epoch := scalar(info.Epoch); epoch != "" && epoch != "0" {
		result = epoch + ":" + result
	}
	return result
}

func packageFrom(info *srcinfo.Srcinfo, pkg *srcinfo.Package) rpcPackage {
	return rpcPackage{
		Name:         pkg.Pkgname,
		PackageBase:  info.Pkgbase,
		Version:      version(info),
		Description:  scalar(pkg.Pkgdesc),
		URL:          scalar(pkg.URL),
		Depends:      flat(pkg.Depends),
		MakeDepends:  flat(info.MakeDepends),
		CheckDepends: flat(info.CheckDepends),
		OptDepends:   flat(pkg.OptDepends),
		Provides:     flat(pkg.Provides),
		Conflicts:    flat(pkg.Conflicts),
		Replaces:     flat(pkg.Replaces),
		Groups:       plain(pkg.Groups),
		License:      plain(pkg.License),
	}
}

func slicesEqual(left, right []string) bool {
	if len(left) != len(right) {
		return false
	}
	for i := range left {
		if left[i] != right[i] {
			return false
		}
	}
	return true
}

func equal(left, right rpcPackage) (bool, string) {
	switch {
	case left.Name != right.Name:
		return false, "Name"
	case left.PackageBase != right.PackageBase:
		return false, "PackageBase"
	case left.Version != right.Version:
		return false, "Version"
	case left.Description != right.Description:
		return false, "Description"
	case left.URL != right.URL:
		return false, "URL"
	case !slicesEqual(left.Depends, right.Depends):
		return false, "Depends"
	case !slicesEqual(left.MakeDepends, right.MakeDepends):
		return false, "MakeDepends"
	case !slicesEqual(left.CheckDepends, right.CheckDepends):
		return false, "CheckDepends"
	case !slicesEqual(left.OptDepends, right.OptDepends):
		return false, "OptDepends"
	case !slicesEqual(left.Provides, right.Provides):
		return false, "Provides"
	case !slicesEqual(left.Conflicts, right.Conflicts):
		return false, "Conflicts"
	case !slicesEqual(left.Replaces, right.Replaces):
		return false, "Replaces"
	case !slicesEqual(left.Groups, right.Groups):
		return false, "Groups"
	case !slicesEqual(left.License, right.License):
		return false, "License"
	default:
		return true, ""
	}
}

func buildExpected(
	repo string,
	branches []branch,
	fatal map[string]bool,
) (map[string]rpcPackage, int, error) {
	cmd := exec.Command("git", "--git-dir="+repo, "cat-file", "--batch")
	in, err := cmd.StdinPipe()
	if err != nil {
		return nil, 0, err
	}
	out, err := cmd.StdoutPipe()
	if err != nil {
		return nil, 0, err
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		return nil, 0, err
	}

	var writeErr error
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		writer := bufio.NewWriterSize(in, 1<<20)
		for _, item := range branches {
			if _, err := fmt.Fprintf(writer, "%s:.SRCINFO\n", item.ref); err != nil {
				writeErr = err
				break
			}
		}
		if err := writer.Flush(); err != nil && writeErr == nil {
			writeErr = err
		}
		if err := in.Close(); err != nil && writeErr == nil {
			writeErr = err
		}
	}()

	reader := bufio.NewReaderSize(out, 1<<20)
	expected := make(map[string]rpcPackage)
	parseErrors := 0
	for _, item := range branches {
		header, err := reader.ReadString('\n')
		if err != nil {
			return nil, 0, err
		}
		header = strings.TrimSpace(header)
		if strings.HasSuffix(header, " missing") {
			continue
		}
		parts := strings.Fields(header)
		if len(parts) != 3 {
			return nil, 0, fmt.Errorf("bad cat-file header %q", header)
		}
		size, err := strconv.Atoi(parts[2])
		if err != nil {
			return nil, 0, err
		}
		data := make([]byte, size)
		if _, err := io.ReadFull(reader, data); err != nil {
			return nil, 0, err
		}
		if separator, err := reader.ReadByte(); err != nil || separator != '\n' {
			return nil, 0, errors.New("missing cat-file separator")
		}
		if fatal[item.name] {
			continue
		}

		info, err := srcinfo.Parse(string(data))
		if err != nil {
			parseErrors++
			continue
		}
		for _, pkg := range info.SplitPackages() {
			expected[pkg.Pkgname] = packageFrom(info, pkg)
		}
	}

	wg.Wait()
	if writeErr != nil {
		return nil, 0, writeErr
	}
	if err := cmd.Wait(); err != nil {
		return nil, 0, fmt.Errorf("git cat-file: %w: %s", err, stderr.String())
	}
	return expected, parseErrors, nil
}

func compareFields(actual, expected map[string]rpcPackage) (int, int, map[string]int, []string) {
	missing := 0
	mismatches := 0
	fields := make(map[string]int)
	samples := make([]string, 0, 20)
	for name, wanted := range expected {
		got, ok := actual[name]
		if !ok {
			missing++
			if len(samples) < cap(samples) {
				samples = append(samples, name+":missing")
			}
			continue
		}
		if ok, field := equal(got, wanted); !ok {
			mismatches++
			fields[field]++
			if len(samples) < cap(samples) {
				samples = append(samples, name+":"+field)
			}
		}
	}
	return missing, mismatches, fields, samples
}

func compareGoSet(actual map[string]rpcPackage, path string) error {
	db, err := memdb.Open(path)
	if err != nil {
		return err
	}
	goNames := db.AllPackageNames()
	goSet := make(map[string]bool, len(goNames))
	for _, name := range goNames {
		goSet[name] = true
	}

	common := 0
	cppOnly := 0
	for name := range actual {
		if goSet[name] {
			common++
		} else {
			cppOnly++
		}
	}
	goOnly := 0
	for _, name := range goNames {
		if _, ok := actual[name]; !ok {
			goOnly++
		}
	}
	fmt.Printf(
		"go_index=%d common=%d cpp_only=%d go_only=%d\n",
		len(goNames), common, cppOnly, goOnly,
	)
	return nil
}

func usage() {
	fmt.Fprintf(
		os.Stderr,
		"usage: %s REPO DIAGNOSTICS CPP_JSONL [GO_GOB]\n",
		os.Args[0],
	)
	os.Exit(2)
}

func main() {
	if len(os.Args) != 4 && len(os.Args) != 5 {
		usage()
	}
	actual, err := loadActual(os.Args[3])
	if err != nil {
		panic(err)
	}
	fatal, err := loadFatal(os.Args[2])
	if err != nil {
		panic(err)
	}
	branches, err := listBranches(os.Args[1])
	if err != nil {
		panic(err)
	}
	expected, parseErrors, err := buildExpected(os.Args[1], branches, fatal)
	if err != nil {
		panic(err)
	}

	missing, mismatches, fields, samples := compareFields(actual, expected)
	extras := len(actual) - (len(expected) - missing)
	fmt.Printf(
		"actual=%d expected=%d matched=%d missing=%d mismatches=%d extras=%d go_parse_errors=%d fatal_branches=%d\n",
		len(actual), len(expected), len(expected)-missing-mismatches,
		missing, mismatches, extras, parseErrors, len(fatal),
	)
	keys := make([]string, 0, len(fields))
	for key := range fields {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		fmt.Printf("mismatch_%s=%d\n", key, fields[key])
	}
	for _, sample := range samples {
		fmt.Printf("sample=%s\n", sample)
	}
	if len(os.Args) == 5 {
		if err := compareGoSet(actual, os.Args[4]); err != nil {
			panic(err)
		}
	}
	if missing != 0 || mismatches != 0 {
		os.Exit(1)
	}
}
