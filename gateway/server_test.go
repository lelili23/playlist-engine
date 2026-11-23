package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
)

// fakeEngine mimics the C++ engine's /genres and /recommend and records the last query.
type fakeEngine struct {
	srv       *httptest.Server
	lastQuery atomic.Value // url.Values
}

func newFakeEngine(t *testing.T) *fakeEngine {
	f := &fakeEngine{}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/genres":
			writeJSON(w, 200, map[string]any{"genres": []string{"chill", "pop", "study"}})
		case "/recommend":
			q := r.URL.Query()
			f.lastQuery.Store(q)
			if strings.Contains(q.Get("seeds"), "missing") {
				writeJSON(w, 404, map[string]string{"error": "unknown seed song id: missing"})
				return
			}
			resp := map[string]any{"seeds": []Song{}, "results": []map[string]any{}}
			if q.Get("seeds") != "" {
				resp["seeds"] = []Song{{ID: q.Get("seeds"), Name: "Seed"}}
			}
			var results []map[string]any
			for i := range 3 {
				results = append(results, map[string]any{"rank": i + 1, "song": Song{ID: "r" + string(rune('a'+i)), Name: "Rec"}})
			}
			resp["results"] = results
			writeJSON(w, 200, resp)
		case "/health":
			writeJSON(w, 200, map[string]string{"status": "ok"})
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(f.srv.Close)
	return f
}

type stubLLM struct {
	p     Profile
	err   error
	calls atomic.Int32
}

func (s *stubLLM) Interpret(context.Context, string, []string) (Profile, error) {
	s.calls.Add(1)
	return s.p, s.err
}

func post(t *testing.T, h http.Handler, body string) (*httptest.ResponseRecorder, playlistResponse) {
	t.Helper()
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/playlist", bytes.NewBufferString(body)))
	var out playlistResponse
	_ = json.Unmarshal(rec.Body.Bytes(), &out)
	return rec, out
}

func TestPlaylistUsesLLMAndCachesIt(t *testing.T) {
	eng := newFakeEngine(t)
	llm := &stubLLM{p: Profile{Energy: 0.2, Valence: 0.3, Tempo: 80, Genres: []string{"study", "nope"}}}
	srv, _ := NewServer(NewEngineClient(eng.srv.URL), llm)
	h := srv.Routes()

	rec, out := post(t, h, `{"idea":"rainy day study","length":5}`)
	if rec.Code != 200 || out.ProfileSource != "llm" || len(out.Tracks) != 3 {
		t.Fatalf("status %d source %q tracks %d: %s", rec.Code, out.ProfileSource, len(out.Tracks), rec.Body)
	}
	q := eng.lastQuery.Load().(url.Values)
	if q.Get("energy") != "0.200" || q.Get("genres") != "study" || q.Get("k") != "5" {
		t.Errorf("engine query = %v (unknown genre should be filtered)", q)
	}

	_, out = post(t, h, `{"idea":"Rainy Day Study","length":5}`)
	if out.ProfileSource != "cache:llm" || llm.calls.Load() != 1 {
		t.Errorf("second call: source %q, LLM calls %d", out.ProfileSource, llm.calls.Load())
	}
}

func TestPlaylistFallsBackWhenLLMFails(t *testing.T) {
	eng := newFakeEngine(t)
	srv, _ := NewServer(NewEngineClient(eng.srv.URL), &stubLLM{err: errors.New("timeout")})
	rec, out := post(t, srv.Routes(), `{"idea":"workout","length":5}`)
	if rec.Code != 200 || out.ProfileSource != "fallback" || !strings.Contains(out.FallbackReason, "timeout") {
		t.Fatalf("got %d %q %q", rec.Code, out.ProfileSource, out.FallbackReason)
	}
}

func TestPlaylistWithoutLLMUsesTable(t *testing.T) {
	eng := newFakeEngine(t)
	srv, _ := NewServer(NewEngineClient(eng.srv.URL), nil)
	_, out := post(t, srv.Routes(), `{"idea":"party"}`)
	if out.ProfileSource != "fallback" || out.Profile == nil || out.Profile.Danceability < 0.7 {
		t.Fatalf("got %+v", out)
	}
	if k := eng.lastQuery.Load().(url.Values).Get("k"); k != "20" {
		t.Errorf("default length should ask for 20, got %s", k)
	}
}

func TestPlaylistSeedsComeFirst(t *testing.T) {
	eng := newFakeEngine(t)
	srv, _ := NewServer(NewEngineClient(eng.srv.URL), nil)
	_, out := post(t, srv.Routes(), `{"seeds":["s1"],"length":5}`)
	if out.ProfileSource != "seeds-only" || len(out.Tracks) == 0 || !out.Tracks[0].Seed || out.Tracks[0].ID != "s1" {
		t.Fatalf("got %+v", out)
	}
	if k := eng.lastQuery.Load().(url.Values).Get("k"); k != "4" {
		t.Errorf("k = %s, want length minus seeds = 4", k)
	}
}

func TestPlaylistValidation(t *testing.T) {
	eng := newFakeEngine(t)
	srv, _ := NewServer(NewEngineClient(eng.srv.URL), nil)
	h := srv.Routes()
	for body, want := range map[string]int{
		`not json`:                            400,
		`{}`:                                  400,
		`{"idea":"x","length":1}`:             400,
		`{"idea":"x","lenght":10}`:            400, // typo'd field is rejected
		`{"seeds":["a","b","c","d","e","f"]}`: 400,
		`{"seeds":["missing"]}`:               404, // engine's 404 passes through
	} {
		if rec, _ := post(t, h, body); rec.Code != want {
			t.Errorf("%s -> %d, want %d (%s)", body, rec.Code, want, rec.Body)
		}
	}
}

func TestEngineDownIs502(t *testing.T) {
	srv, _ := NewServer(NewEngineClient("http://127.0.0.1:1"), nil)
	if rec, _ := post(t, srv.Routes(), `{"idea":"party"}`); rec.Code != http.StatusBadGateway {
		t.Errorf("got %d", rec.Code)
	}
}

func TestOnlyPostOnPlaylist(t *testing.T) {
	srv, _ := NewServer(NewEngineClient("http://127.0.0.1:1"), nil)
	rec := httptest.NewRecorder()
	srv.Routes().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/playlist", nil))
	if rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("got %d", rec.Code)
	}
}
