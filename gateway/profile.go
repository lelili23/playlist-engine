package main

import (
	"fmt"
	"math"
	"strconv"
)

// Profile is a wished-for sound: what a playlist idea like "rainy day study"
// means in the engine's audio-feature units. It comes from the LLM, or from the
// keyword fallback table when the LLM is unavailable.
type Profile struct {
	Energy           float64  `json:"energy"`           // 0..1 intensity
	Valence          float64  `json:"valence"`          // 0..1 sad -> happy
	Danceability     float64  `json:"danceability"`     // 0..1
	Acousticness     float64  `json:"acousticness"`     // 0..1 electronic -> acoustic
	Tempo            float64  `json:"tempo"`            // BPM, 40..250
	Speechiness      float64  `json:"speechiness"`      // 0..1 sung -> spoken/rap
	Instrumentalness float64  `json:"instrumentalness"` // 0..1 vocals -> no vocals
	Mode             *int     `json:"mode"`             // 1 major, 0 minor, nil = don't care
	Genres           []string `json:"genres"`           // preferred genres from the engine's list
}

// Clean clamps every number into its valid range and keeps only genres the
// engine knows. LLM output is untrusted input: never pass it through unchecked.
func (p *Profile) Clean(knownGenres map[string]bool) error {
	for _, f := range []*float64{&p.Energy, &p.Valence, &p.Danceability, &p.Acousticness,
		&p.Speechiness, &p.Instrumentalness, &p.Tempo} {
		if math.IsNaN(*f) || math.IsInf(*f, 0) {
			return fmt.Errorf("profile contains a non-finite number")
		}
	}
	clamp := func(v, lo, hi float64) float64 { return math.Max(lo, math.Min(hi, v)) }
	p.Energy = clamp(p.Energy, 0, 1)
	p.Valence = clamp(p.Valence, 0, 1)
	p.Danceability = clamp(p.Danceability, 0, 1)
	p.Acousticness = clamp(p.Acousticness, 0, 1)
	p.Speechiness = clamp(p.Speechiness, 0, 1)
	p.Instrumentalness = clamp(p.Instrumentalness, 0, 1)
	p.Tempo = clamp(p.Tempo, 40, 250)
	if p.Mode != nil && *p.Mode != 0 && *p.Mode != 1 {
		p.Mode = nil
	}
	if knownGenres != nil {
		kept := p.Genres[:0]
		for _, g := range p.Genres {
			if knownGenres[g] && len(kept) < 5 {
				kept = append(kept, g)
			}
		}
		p.Genres = kept
	}
	return nil
}

// queryParams renders the profile as the engine's /recommend parameters.
func (p *Profile) queryParams() map[string]string {
	f := func(v float64) string { return strconv.FormatFloat(v, 'f', 3, 64) }
	m := map[string]string{
		"energy": f(p.Energy), "valence": f(p.Valence), "danceability": f(p.Danceability),
		"acousticness": f(p.Acousticness), "tempo": f(p.Tempo), "speechiness": f(p.Speechiness),
		"instrumentalness": f(p.Instrumentalness),
	}
	if p.Mode != nil {
		m["mode"] = strconv.Itoa(*p.Mode)
	}
	return m
}
