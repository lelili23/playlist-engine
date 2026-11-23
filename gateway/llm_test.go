package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// fakeClaude stands in for the real API so tests run offline and free.
func fakeClaude(t *testing.T, status int, replyText string) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/messages" || r.Header.Get("x-api-key") != "test-key" ||
			r.Header.Get("anthropic-version") == "" {
			t.Errorf("unexpected request: %s %v", r.URL.Path, r.Header)
		}
		body, _ := io.ReadAll(r.Body)
		if !strings.Contains(string(body), "rainy day study") || !strings.Contains(string(body), "Allowed genres: chill, study") {
			t.Errorf("prompt missing idea or genres: %s", body)
		}
		w.WriteHeader(status)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"content": []map[string]string{{"type": "text", "text": replyText}},
		})
	}))
}

func newTestInterpreter(url string) *ClaudeInterpreter {
	c := NewClaudeInterpreter("test-key", "test-model")
	c.BaseURL = url
	return c
}

func TestClaudeInterpreterParsesFencedJSON(t *testing.T) {
	reply := "Here you go:\n```json\n{\"energy\":0.25,\"valence\":0.3,\"danceability\":0.4,\"acousticness\":0.8," +
		"\"tempo\":82,\"speechiness\":0.03,\"instrumentalness\":0.6,\"mode\":0,\"genres\":[\"study\",\"chill\"]}\n```"
	srv := fakeClaude(t, http.StatusOK, reply)
	defer srv.Close()

	p, err := newTestInterpreter(srv.URL).Interpret(context.Background(), "rainy day study", []string{"chill", "study"})
	if err != nil {
		t.Fatal(err)
	}
	if p.Energy != 0.25 || p.Tempo != 82 || p.Mode == nil || *p.Mode != 0 || len(p.Genres) != 2 {
		t.Errorf("parsed %+v", p)
	}
}

func TestClaudeInterpreterErrors(t *testing.T) {
	for name, tc := range map[string]struct {
		status int
		reply  string
	}{
		"http error":   {http.StatusTooManyRequests, "{}"},
		"no json":      {http.StatusOK, "I can't help with that."},
		"empty values": {http.StatusOK, "{\"genres\":[]}"},
	} {
		t.Run(name, func(t *testing.T) {
			srv := fakeClaude(t, tc.status, tc.reply)
			defer srv.Close()
			if _, err := newTestInterpreter(srv.URL).Interpret(context.Background(), "rainy day study", []string{"chill", "study"}); err == nil {
				t.Error("expected an error")
			}
		})
	}
}
