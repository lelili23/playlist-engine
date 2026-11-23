package main

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
	"sync"
	"time"
)

// Server is the public-facing API. It owns everything that isn't number
// crunching: understanding the idea (LLM or fallback), validation, caching,
// and composing the final playlist. The C++ engine does the search.
type Server struct {
	engine *EngineClient
	llm    Interpreter // nil = no API key: always use the fallback table
	cache  *profileCache
	proxy  *httputil.ReverseProxy

	genresMu sync.Mutex
	genres   []string // fetched from the engine once, then reused
}

func NewServer(engine *EngineClient, llm Interpreter) (*Server, error) {
	target, err := url.Parse(engine.BaseURL)
	if err != nil {
		return nil, err
	}
	return &Server{
		engine: engine,
		llm:    llm,
		cache:  newProfileCache(500),
		proxy:  httputil.NewSingleHostReverseProxy(target),
	}, nil
}

// extraRoutes lets optional features in other files add their own routes
// (each appends in an init function). The API works without any of them; the
// web UI in web.go is one.
var extraRoutes []func(s *Server, mux *http.ServeMux)

// Routes uses Go 1.22's method-aware patterns ("POST /playlist").
func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()
	for _, register := range extraRoutes {
		register(s, mux)
	}
	mux.HandleFunc("POST /playlist", s.handlePlaylist)
	mux.HandleFunc("GET /health", s.handleHealth)
	// Read-only engine endpoints are passed straight through, so clients only
	// need to know one address.
	mux.Handle("GET /search", s.proxy)
	mux.Handle("GET /similar", s.proxy)
	return logRequests(mux)
}

// ---- POST /playlist ----

type playlistRequest struct {
	Idea   string   `json:"idea"`
	Seeds  []string `json:"seeds"`
	Length int      `json:"length"`
}

type Track struct {
	Position int  `json:"position"`
	Seed     bool `json:"seed"`
	Song
}

type playlistResponse struct {
	Idea           string   `json:"idea,omitempty"`
	Profile        *Profile `json:"profile,omitempty"`
	ProfileSource  string   `json:"profile_source"` // "llm", "fallback", "cache:llm", "seeds-only"
	FallbackReason string   `json:"fallback_reason,omitempty"`
	Tracks         []Track  `json:"tracks"`
	TookMs         float64  `json:"took_ms"`
}

func (s *Server) handlePlaylist(w http.ResponseWriter, r *http.Request) {
	start := time.Now()
	var req playlistRequest
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64<<10))
	dec.DisallowUnknownFields() // a typo like "lenght" is an error, not silently ignored
	if err := dec.Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "body must be JSON like {\"idea\": \"rainy day study\", \"length\": 20}")
		return
	}
	req.Idea = strings.TrimSpace(req.Idea)
	switch {
	case req.Idea == "" && len(req.Seeds) == 0:
		writeError(w, http.StatusBadRequest, "give an idea, seed song ids, or both")
		return
	case len(req.Idea) > 200:
		writeError(w, http.StatusBadRequest, "idea must be at most 200 characters")
		return
	case len(req.Seeds) > 5:
		writeError(w, http.StatusBadRequest, "at most 5 seed songs")
		return
	}
	if req.Length == 0 {
		req.Length = 20
	}
	if req.Length < 5 || req.Length > 100 {
		writeError(w, http.StatusBadRequest, "length must be 5-100")
		return
	}

	resp := playlistResponse{Idea: req.Idea, ProfileSource: "seeds-only"}
	var profile *Profile
	if req.Idea != "" {
		p, source, reason := s.profileFor(r.Context(), req.Idea)
		profile, resp.Profile, resp.ProfileSource, resp.FallbackReason = &p, &p, source, reason
	}

	k := max(req.Length-len(req.Seeds), 1)
	rec, err := s.engine.Recommend(r.Context(), profile, req.Seeds, k)
	if err != nil {
		var ee *EngineError
		if errors.As(err, &ee) && (ee.Status == http.StatusBadRequest || ee.Status == http.StatusNotFound) {
			writeError(w, ee.Status, ee.Message) // e.g. unknown seed id: the client's mistake
			return
		}
		slog.Error("engine call failed", "err", err)
		writeError(w, http.StatusBadGateway, "music engine unavailable")
		return
	}

	// Playlist = the seed songs first, then the recommendations.
	for _, sd := range rec.Seeds {
		resp.Tracks = append(resp.Tracks, Track{Position: len(resp.Tracks) + 1, Seed: true, Song: sd})
	}
	for _, res := range rec.Results {
		resp.Tracks = append(resp.Tracks, Track{Position: len(resp.Tracks) + 1, Song: res.Song})
	}
	resp.TookMs = float64(time.Since(start).Microseconds()) / 1000
	writeJSON(w, http.StatusOK, resp)
}

// profileFor turns an idea into a profile: cache, then LLM, then fallback table.
// Returns the profile, where it came from, and why we fell back (if we did).
func (s *Server) profileFor(ctx context.Context, idea string) (Profile, string, string) {
	key := strings.ToLower(idea)
	if p, source, ok := s.cache.get(key); ok {
		return p, "cache:" + source, ""
	}
	genres := s.knownGenres(ctx)
	var known map[string]bool // nil = genre list unavailable: keep whatever genres we get
	if len(genres) > 0 {
		known = make(map[string]bool, len(genres))
		for _, g := range genres {
			known[g] = true
		}
	}

	reason := "no LLM API key configured"
	if s.llm != nil {
		ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
		defer cancel()
		p, err := s.llm.Interpret(ctx, idea, genres)
		if err == nil {
			err = p.Clean(known)
		}
		if err == nil {
			s.cache.put(key, p, "llm")
			return p, "llm", ""
		}
		slog.Warn("LLM failed, using fallback table", "err", err)
		reason = "LLM error: " + truncate(err.Error(), 120)
	}
	p := FallbackProfile(idea)
	_ = p.Clean(known)           // table values are already in range; this drops unknown genres
	return p, "fallback", reason // not cached: a later LLM success should replace it
}

func (s *Server) knownGenres(ctx context.Context) []string {
	s.genresMu.Lock()
	defer s.genresMu.Unlock()
	if s.genres == nil {
		g, err := s.engine.Genres(ctx)
		if err != nil {
			slog.Warn("could not fetch genres from engine", "err", err)
			return nil // try again on the next request
		}
		s.genres = g
	}
	return s.genres
}

// ---- GET /health ----

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	engine := "ok"
	var out map[string]any
	if err := s.engine.get(r.Context(), "/health", nil, &out); err != nil {
		engine = "down: " + err.Error()
	}
	llm := "off (keyword fallback table)"
	if c, ok := s.llm.(*ClaudeInterpreter); ok {
		llm = c.Model
	}
	status := http.StatusOK
	if engine != "ok" {
		status = http.StatusServiceUnavailable
	}
	writeJSON(w, status, map[string]string{"gateway": "ok", "engine": engine, "llm": llm})
}

// ---- helpers ----

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (r *statusRecorder) WriteHeader(code int) {
	r.status = code
	r.ResponseWriter.WriteHeader(code)
}

func logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rec, r)
		slog.Info("request", "method", r.Method, "path", r.URL.Path, "status", rec.status,
			"ms", float64(time.Since(start).Microseconds())/1000)
	})
}

// ---- cache ----

// profileCache remembers LLM answers so repeated ideas don't cost another call.
// Every request runs on its own goroutine, so access goes through a mutex.
type profileCache struct {
	mu  sync.Mutex
	max int
	m   map[string]cachedProfile
}

type cachedProfile struct {
	p      Profile
	source string
}

func newProfileCache(max int) *profileCache {
	return &profileCache{max: max, m: make(map[string]cachedProfile)}
}

func (c *profileCache) get(key string) (Profile, string, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.m[key]
	return e.p, e.source, ok
}

func (c *profileCache) put(key string, p Profile, source string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.m) >= c.max {
		clear(c.m) // simplest bounded policy; an LRU would keep hot entries
	}
	c.m[key] = cachedProfile{p, source}
}
