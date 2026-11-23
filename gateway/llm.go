package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// Interpreter turns a playlist idea into a target Profile.
// An interface so tests (and other LLM providers) can plug in their own.
type Interpreter interface {
	Interpret(ctx context.Context, idea string, genres []string) (Profile, error)
}

// ClaudeInterpreter calls the Anthropic Messages API with only the standard library.
type ClaudeInterpreter struct {
	APIKey  string
	Model   string
	BaseURL string // https://api.anthropic.com in production; a fake server in tests
	Client  *http.Client
}

func NewClaudeInterpreter(apiKey, model string) *ClaudeInterpreter {
	return &ClaudeInterpreter{
		APIKey:  apiKey,
		Model:   model,
		BaseURL: "https://api.anthropic.com",
		Client:  &http.Client{Timeout: 20 * time.Second},
	}
}

const systemPrompt = `You turn a playlist idea into target audio features for a music search engine.
Reply with ONLY one JSON object, no prose, with exactly these keys:
  "energy"           0..1  perceived intensity (0.1 ambient, 0.9 loud and fast)
  "valence"          0..1  musical positivity (0.1 sad or dark, 0.9 happy or euphoric)
  "danceability"     0..1  how suitable for dancing
  "acousticness"     0..1  0 = electronic or produced, 1 = acoustic instruments
  "tempo"            beats per minute, 60..200
  "speechiness"      0..1  0.03 = sung, 0.3+ = rap or spoken word
  "instrumentalness" 0..1  0 = has vocals, 0.8+ = no vocals
  "mode"             1 = major, 0 = minor, null = either
  "genres"           up to 4 genres that fit, chosen ONLY from the allowed list
Think about what the listener is doing (studying, working out, falling asleep) and the
mood they want. Be decisive: avoid putting everything at 0.5.`

// Interpret asks the model for a profile. Any problem (network, bad JSON,
// non-200) returns an error, and the caller falls back to the keyword table.
func (c *ClaudeInterpreter) Interpret(ctx context.Context, idea string, genres []string) (Profile, error) {
	reqBody := map[string]any{
		"model":      c.Model,
		"max_tokens": 400,
		"system":     systemPrompt,
		"messages": []map[string]string{{
			"role":    "user",
			"content": fmt.Sprintf("Allowed genres: %s\n\nPlaylist idea: %q", strings.Join(genres, ", "), idea),
		}},
	}
	payload, err := json.Marshal(reqBody)
	if err != nil {
		return Profile{}, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.BaseURL+"/v1/messages", bytes.NewReader(payload))
	if err != nil {
		return Profile{}, err
	}
	req.Header.Set("content-type", "application/json")
	req.Header.Set("x-api-key", c.APIKey)
	req.Header.Set("anthropic-version", "2023-06-01")

	resp, err := c.Client.Do(req)
	if err != nil {
		return Profile{}, fmt.Errorf("llm request: %w", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return Profile{}, fmt.Errorf("llm read: %w", err)
	}
	if resp.StatusCode != http.StatusOK {
		return Profile{}, fmt.Errorf("llm returned %d: %s", resp.StatusCode, truncate(string(body), 200))
	}

	var parsed struct {
		Content []struct {
			Type string `json:"type"`
			Text string `json:"text"`
		} `json:"content"`
	}
	if err := json.Unmarshal(body, &parsed); err != nil {
		return Profile{}, fmt.Errorf("llm response not JSON: %w", err)
	}
	var text strings.Builder
	for _, block := range parsed.Content {
		if block.Type == "text" {
			text.WriteString(block.Text)
		}
	}
	return parseProfileJSON(text.String())
}

// parseProfileJSON extracts the JSON object from the model's reply. Models
// sometimes wrap it in ```json fences or add a sentence, so take the span from
// the first '{' to the last '}'.
func parseProfileJSON(text string) (Profile, error) {
	start, end := strings.Index(text, "{"), strings.LastIndex(text, "}")
	if start < 0 || end <= start {
		return Profile{}, errors.New("no JSON object in model reply")
	}
	var p Profile
	dec := json.NewDecoder(strings.NewReader(text[start : end+1]))
	if err := dec.Decode(&p); err != nil {
		return Profile{}, fmt.Errorf("model reply is not a valid profile: %w", err)
	}
	if p.Tempo == 0 && p.Energy == 0 && p.Valence == 0 {
		return Profile{}, errors.New("model reply is missing the feature values")
	}
	return p, nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}
