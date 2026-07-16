package srvinfo

import (
	"strings"

	srcinfo "github.com/Jguer/go-srcinfo"
)

type Entry struct {
	Name         string
	Base         string
	Version      string
	Description  string
	URL          string
	License      []string
	Depends      []string
	MakeDepends  []string
	CheckDepends []string
	OptDepends   []string
	Provides     []string
	Conflicts    []string
	Replaces     []string
	Groups       []string
	Keywords     []string
	UpdatedAt    int64
}

func Parse(data string) ([]Entry, error) {
	si, err := srcinfo.Parse(data)
	if err != nil {
		return nil, err
	}

	version := si.Version()
	base := Entry{
		Base:         si.Pkgbase,
		Version:      version,
		Description:  si.Pkgdesc,
		URL:          si.URL,
		License:      filterEmpty(si.License),
		Depends:      archToStrings(si.Depends),
		MakeDepends:  archToStrings(si.MakeDepends),
		CheckDepends: archToStrings(si.CheckDepends),
		OptDepends:   archToStrings(si.OptDepends),
		Provides:     archToStrings(si.Provides),
		Conflicts:    archToStrings(si.Conflicts),
		Replaces:     archToStrings(si.Replaces),
		Groups:       filterEmpty(si.Groups),
	}

	if len(si.Packages) == 0 {
		e := base
		e.Name = si.Pkgbase
		if e.Name == "" {
			return nil, nil
		}
		return []Entry{e}, nil
	}

	merged := si.SplitPackages()
	entries := make([]Entry, 0, len(merged))
	for _, pkg := range merged {
		e := base
		e.Name = pkg.Pkgname
		if pkg.Pkgdesc != "" {
			e.Description = pkg.Pkgdesc
		}
		if pkg.URL != "" {
			e.URL = pkg.URL
		}
		if len(pkg.License) > 0 {
			e.License = filterEmpty(pkg.License)
		}
		if len(pkg.Groups) > 0 {
			e.Groups = filterEmpty(pkg.Groups)
		}
		if len(pkg.Depends) > 0 {
			e.Depends = append(e.Depends, archToStrings(pkg.Depends)...)
		}
		e.OptDepends = append(e.OptDepends, archToStrings(pkg.OptDepends)...)
		e.Provides = append(e.Provides, archToStrings(pkg.Provides)...)
		e.Conflicts = append(e.Conflicts, archToStrings(pkg.Conflicts)...)
		e.Replaces = append(e.Replaces, archToStrings(pkg.Replaces)...)
		entries = append(entries, e)
	}
	return entries, nil
}

func archToStrings(av []srcinfo.ArchString) []string {
	if len(av) == 0 {
		return nil
	}
	out := make([]string, 0, len(av))
	for _, a := range av {
		if a.Value != "" && a.Value != srcinfo.EmptyOverride {
			out = append(out, a.Value)
		}
	}
	return out
}

func filterEmpty(ss []string) []string {
	if len(ss) == 0 {
		return nil
	}
	var out []string
	for _, s := range ss {
		s = strings.TrimSpace(s)
		if s != "" && s != srcinfo.EmptyOverride {
			out = append(out, s)
		}
	}
	return out
}
