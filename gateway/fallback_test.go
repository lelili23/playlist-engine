package main

import "testing"

func TestFallbackMatchesKeywords(t *testing.T) {
	study := FallbackProfile("Late night STUDY session")
	party := FallbackProfile("house party")
	if !(study.Energy < party.Energy) {
		t.Errorf("study energy %.2f should be below party %.2f", study.Energy, party.Energy)
	}
	if party.Danceability < 0.7 {
		t.Errorf("party should be danceable: %.2f", party.Danceability)
	}
}

func TestFallbackAveragesSeveralMoods(t *testing.T) {
	rainy := FallbackProfile("rain")
	study := FallbackProfile("study")
	both := FallbackProfile("rainy day study")
	want := (rainy.Energy + study.Energy) / 2
	if diff := both.Energy - want; diff > 1e-9 || diff < -1e-9 {
		t.Errorf("energy %.3f, want average %.3f", both.Energy, want)
	}
	if len(both.Genres) == 0 || len(both.Genres) > 5 {
		t.Errorf("genres = %v", both.Genres)
	}
}

func TestFallbackUnknownIdeaIsNeutral(t *testing.T) {
	if p := FallbackProfile("qwertyuiop"); p.Energy != neutral.Energy || p.Tempo != neutral.Tempo {
		t.Errorf("got %+v", p)
	}
}

func TestFallbackMatchesWordStartsNotSubstrings(t *testing.T) {
	gym := FallbackProfile("gym workout")
	if gym.Energy < 0.85 {
		t.Errorf("workout should be pure high energy (no study mixed in): %.2f", gym.Energy)
	}
	drive := FallbackProfile("late night drive")
	for _, g := range drive.Genres {
		if g == "sleep" || g == "classical" {
			t.Errorf("late night drive should not pull in sleep music: %v", drive.Genres)
		}
	}
	if FallbackProfile("rainy").Valence != FallbackProfile("rain").Valence {
		t.Error("rain should match rainy")
	}
}
