package main

import (
	_ "embed"
	"net/http"
)

// The web UI is one self-contained HTML file compiled into the binary with
// go:embed, so `go run .` serves it with nothing else to install or deploy.
//
//go:embed web/index.html
var indexHTML []byte

func init() {
	extraRoutes = append(extraRoutes, func(s *Server, mux *http.ServeMux) {
		mux.HandleFunc("GET /{$}", s.handleIndex) // the web UI at exactly "/"
		mux.HandleFunc("GET /favicon.ico", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusNoContent) // no icon; stops browsers logging a 404
		})
	})
}

func (s *Server) handleIndex(w http.ResponseWriter, _ *http.Request) {
	h := w.Header()
	h.Set("Content-Type", "text/html; charset=utf-8")
	// Content Security Policy: the page may only talk to this server and embed
	// Spotify's player. Anything else (an injected script, a stray iframe) is blocked.
	h.Set("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; "+
		"style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; "+
		"frame-src https://open.spotify.com; connect-src 'self'")
	h.Set("X-Content-Type-Options", "nosniff")
	_, _ = w.Write(indexHTML)
}
