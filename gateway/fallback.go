package main

import (
	"sort"
	"strings"
	"unicode"
)

// The fallback interpreter: a small hand-written table from mood keywords to
// profiles. Used when no LLM key is configured or the LLM call fails, so
// POST /playlist always works, just less cleverly.

type moodEntry struct {
	keywords []string
	profile  Profile
}

func intp(v int) *int { return &v }

var moodTable = []moodEntry{
	{[]string{"study", "studying", "focus", "concentrat", "reading", "homework", "coding", "exam"},
		Profile{Energy: 0.30, Valence: 0.40, Danceability: 0.45, Acousticness: 0.70, Tempo: 90, Speechiness: 0.04, Instrumentalness: 0.60,
			Genres: []string{"study", "ambient", "piano", "classical", "chill"}}},
	{[]string{"rain", "sad", "cry", "heartbreak", "lonely", "melanchol", "breakup"},
		Profile{Energy: 0.30, Valence: 0.20, Danceability: 0.40, Acousticness: 0.65, Tempo: 85, Speechiness: 0.04, Instrumentalness: 0.10, Mode: intp(0),
			Genres: []string{"sad", "acoustic", "singer-songwriter", "indie", "piano"}}},
	{[]string{"party", "dance", "club", "hype", "celebrat"},
		Profile{Energy: 0.85, Valence: 0.75, Danceability: 0.85, Acousticness: 0.05, Tempo: 124, Speechiness: 0.08, Instrumentalness: 0.05,
			Genres: []string{"dance", "pop", "edm", "house", "party"}}},
	{[]string{"workout", "gym", "run", "lift", "cardio", "pump"},
		Profile{Energy: 0.90, Valence: 0.60, Danceability: 0.70, Acousticness: 0.03, Tempo: 140, Speechiness: 0.10, Instrumentalness: 0.05,
			Genres: []string{"edm", "hip-hop", "hard-rock", "electro", "dubstep"}}},
	{[]string{"chill", "relax", "calm", "lazy", "sunday", "cozy"},
		Profile{Energy: 0.35, Valence: 0.55, Danceability: 0.55, Acousticness: 0.55, Tempo: 95, Speechiness: 0.05, Instrumentalness: 0.15,
			Genres: []string{"chill", "acoustic", "indie-pop", "r-n-b", "jazz"}}},
	{[]string{"sleep", "bedtime", "dream", "lullab", "insomnia"},
		Profile{Energy: 0.15, Valence: 0.30, Danceability: 0.30, Acousticness: 0.85, Tempo: 75, Speechiness: 0.03, Instrumentalness: 0.70,
			Genres: []string{"sleep", "ambient", "piano", "new-age", "classical"}}},
	{[]string{"late night", "midnight", "after hours", "city lights", "neon"},
		Profile{Energy: 0.55, Valence: 0.35, Danceability: 0.60, Acousticness: 0.15, Tempo: 105, Speechiness: 0.05, Instrumentalness: 0.10,
			Genres: []string{"synth-pop", "r-n-b", "trip-hop", "chill", "electronic"}}},
	{[]string{"road trip", "drive", "driving", "summer", "beach", "sunny"},
		Profile{Energy: 0.75, Valence: 0.80, Danceability: 0.65, Acousticness: 0.15, Tempo: 118, Speechiness: 0.05, Instrumentalness: 0.02, Mode: intp(1),
			Genres: []string{"pop", "rock", "indie-pop", "country", "alt-rock"}}},
	{[]string{"happy", "good mood", "upbeat", "joy", "fun"},
		Profile{Energy: 0.75, Valence: 0.85, Danceability: 0.70, Acousticness: 0.15, Tempo: 120, Speechiness: 0.05, Instrumentalness: 0.02, Mode: intp(1),
			Genres: []string{"pop", "happy", "dance", "indie-pop", "funk"}}},
	{[]string{"love", "romant", "date", "valentine"},
		Profile{Energy: 0.40, Valence: 0.55, Danceability: 0.55, Acousticness: 0.50, Tempo: 95, Speechiness: 0.04, Instrumentalness: 0.02,
			Genres: []string{"romance", "r-n-b", "soul", "acoustic", "pop"}}},
	{[]string{"angry", "rage", "mad", "intense"},
		Profile{Energy: 0.95, Valence: 0.25, Danceability: 0.45, Acousticness: 0.02, Tempo: 150, Speechiness: 0.08, Instrumentalness: 0.05, Mode: intp(0),
			Genres: []string{"metal", "hard-rock", "punk", "grunge", "metalcore"}}},
	{[]string{"morning", "coffee", "wake", "breakfast"},
		Profile{Energy: 0.45, Valence: 0.65, Danceability: 0.55, Acousticness: 0.60, Tempo: 100, Speechiness: 0.04, Instrumentalness: 0.10, Mode: intp(1),
			Genres: []string{"acoustic", "folk", "indie", "jazz", "singer-songwriter"}}},
}

// neutral is used when no keyword matches: middle-of-the-road pop.
var neutral = Profile{Energy: 0.6, Valence: 0.55, Danceability: 0.6, Acousticness: 0.3, Tempo: 112, Speechiness: 0.05, Instrumentalness: 0.05}

// matches reports whether keyword kw appears in the idea. A single-word keyword
// must match the START of a word ("rain" matches "rainy", but "work" would no
// longer match inside "homework"); a multi-word keyword is matched as a phrase.
func matches(lower string, words []string, kw string) bool {
	if strings.Contains(kw, " ") {
		return strings.Contains(lower, kw)
	}
	for _, w := range words {
		if strings.HasPrefix(w, kw) {
			return true
		}
	}
	return false
}

// FallbackProfile averages the profiles of every mood whose keyword appears in
// the idea, e.g. "rainy study session" = average of "rain" and "study".
func FallbackProfile(idea string) Profile {
	lower := strings.ToLower(idea)
	words := strings.FieldsFunc(lower, func(r rune) bool { return !unicode.IsLetter(r) && !unicode.IsDigit(r) })
	var matched []Profile
	for _, e := range moodTable {
		for _, kw := range e.keywords {
			if matches(lower, words, kw) {
				matched = append(matched, e.profile)
				break
			}
		}
	}
	if len(matched) == 0 {
		return neutral
	}

	var out Profile
	n := float64(len(matched))
	votes := map[int]int{}
	genreCount := map[string]int{}
	for _, p := range matched {
		out.Energy += p.Energy / n
		out.Valence += p.Valence / n
		out.Danceability += p.Danceability / n
		out.Acousticness += p.Acousticness / n
		out.Tempo += p.Tempo / n
		out.Speechiness += p.Speechiness / n
		out.Instrumentalness += p.Instrumentalness / n
		if p.Mode != nil {
			votes[*p.Mode]++
		}
		for _, g := range p.Genres {
			genreCount[g]++
		}
	}
	if votes[0] != votes[1] { // only set mode when the matched moods agree on balance
		m := 0
		if votes[1] > votes[0] {
			m = 1
		}
		out.Mode = &m
	}
	// Genres that several matched moods share come first.
	for g := range genreCount {
		out.Genres = append(out.Genres, g)
	}
	sort.Slice(out.Genres, func(i, j int) bool {
		a, b := out.Genres[i], out.Genres[j]
		if genreCount[a] != genreCount[b] {
			return genreCount[a] > genreCount[b]
		}
		return a < b
	})
	if len(out.Genres) > 5 {
		out.Genres = out.Genres[:5]
	}
	return out
}
