package main

import (
	"math"
	"testing"
)

func TestCleanClampsAndFiltersGenres(t *testing.T) {
	bad := 3
	p := Profile{Energy: 1.7, Valence: -0.2, Tempo: 400, Mode: &bad,
		Genres: []string{"pop", "made-up", "jazz"}}
	if err := p.Clean(map[string]bool{"pop": true, "jazz": true}); err != nil {
		t.Fatal(err)
	}
	if p.Energy != 1 || p.Valence != 0 || p.Tempo != 250 {
		t.Errorf("not clamped: %+v", p)
	}
	if p.Mode != nil {
		t.Errorf("invalid mode should become nil, got %d", *p.Mode)
	}
	if len(p.Genres) != 2 || p.Genres[0] != "pop" || p.Genres[1] != "jazz" {
		t.Errorf("genres = %v", p.Genres)
	}
}

func TestCleanRejectsNaN(t *testing.T) {
	p := Profile{Energy: math.NaN(), Tempo: 100}
	if p.Clean(nil) == nil {
		t.Error("expected an error for NaN")
	}
}

func TestCleanWithUnknownGenreListKeepsGenres(t *testing.T) {
	p := Profile{Tempo: 100, Genres: []string{"pop"}}
	_ = p.Clean(nil)
	if len(p.Genres) != 1 {
		t.Errorf("genres dropped: %v", p.Genres)
	}
}
