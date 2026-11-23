package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestIndexServesTheWebUI(t *testing.T) {
	srv, _ := NewServer(NewEngineClient("http://127.0.0.1:1"), nil)
	rec := httptest.NewRecorder()
	srv.Routes().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/", nil))
	if rec.Code != 200 || !strings.Contains(rec.Body.String(), "<title>Playlist Engine</title>") {
		t.Fatalf("got %d", rec.Code)
	}
	if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "text/html") {
		t.Errorf("content type %q", ct)
	}
	if !strings.Contains(rec.Header().Get("Content-Security-Policy"), "frame-src https://open.spotify.com") {
		t.Error("missing CSP")
	}
	// Unknown paths are still 404, not the UI.
	rec = httptest.NewRecorder()
	srv.Routes().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/nope", nil))
	if rec.Code != http.StatusNotFound {
		t.Errorf("/nope -> %d", rec.Code)
	}
}
