package rpc

import (
	"compress/gzip"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"

	"github.com/willvar/aurhub/internal/memdb"
)

type Server struct {
	db  *memdb.DB
	srv *http.Server
}

func NewServer(addr string, db *memdb.DB) *Server {
	s := &Server{db: db}
	mux := http.NewServeMux()
	mux.HandleFunc("/rpc", s.handleRPC)
	mux.HandleFunc("/packages.gz", s.handlePackagesGz)
	mux.HandleFunc("/health", s.handleHealth)

	s.srv = &http.Server{Addr: addr, Handler: withCORS(mux)}
	return s
}

func (s *Server) ListenAndServe() error {
	log.Printf("aurhub: listening on %s", s.srv.Addr)
	return s.srv.ListenAndServe()
}

func withCORS(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, OPTIONS")
		if r.Method == "OPTIONS" {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	last := s.db.LastSync()
	fmt.Fprintf(w, "ok\nlast_sync: %s\n", last.Format("2006-01-02 15:04:05"))
}

func (s *Server) handlePackagesGz(w http.ResponseWriter, r *http.Request) {
	names := s.db.AllPackageNames()

	w.Header().Set("Content-Type", "application/gzip")
	w.Header().Set("Content-Disposition", "attachment; filename=packages.gz")
	gw := gzip.NewWriter(w)
	defer gw.Close()

	for _, name := range names {
		fmt.Fprintln(gw, name)
	}
}

func (s *Server) handleRPC(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()

	v := q.Get("v")
	if v != "" && v != "5" {
		sendError(w, fmt.Sprintf("unsupported RPC version: %s", v))
		return
	}

	rtype := q.Get("type")

	switch rtype {
	case "search":
		s.handleSearch(w, q)
	case "info", "multiinfo":
		s.handleInfo(w, q)
	default:
		sendError(w, fmt.Sprintf("unknown rpc type: %s", rtype))
	}
}

func (s *Server) handleSearch(w http.ResponseWriter, q map[string][]string) {
	by := getVal(q, "by")
	if by == "" {
		by = "name-desc"
	}
	arg := getVal(q, "arg")
	if arg == "" {
		sendError(w, "missing arg")
		return
	}

	result, err := s.db.Search(by, arg)
	if err != nil {
		sendError(w, fmt.Sprintf("search error: %v", err))
		return
	}
	if result.Results == nil {
		result.Results = []memdb.RPCPackage{}
	}
	sendJSON(w, result)
}

func (s *Server) handleInfo(w http.ResponseWriter, q map[string][]string) {
	args, ok := q["arg[]"]
	if !ok {
		args, ok = q["arg"]
	}
	if !ok || len(args) == 0 {
		sendError(w, "missing arg")
		return
	}

	var names []string
	for _, a := range args {
		names = append(names, strings.TrimSpace(a))
	}

	result, err := s.db.Info(names)
	if err != nil {
		sendError(w, fmt.Sprintf("info error: %v", err))
		return
	}
	if result.Results == nil {
		result.Results = []memdb.RPCPackage{}
	}
	sendJSON(w, result)
}

func sendJSON(w http.ResponseWriter, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(v); err != nil {
		log.Printf("rpc: json encode: %v", err)
	}
}

func getVal(q map[string][]string, key string) string {
	if vs, ok := q[key]; ok && len(vs) > 0 {
		return vs[0]
	}
	return ""
}

func sendError(w http.ResponseWriter, msg string) {
	w.Header().Set("Content-Type", "application/json")
	result := memdb.SearchResult{
		Version: 5,
		Type:    "error",
		Error:   msg,
		Results: []memdb.RPCPackage{},
	}
	json.NewEncoder(w).Encode(result)
}
