package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// Song mirrors the C++ engine's JSON for one song.
type Song struct {
	ID         string   `json:"id"`
	Name       string   `json:"name"`
	Artists    string   `json:"artists"`
	Genres     []string `json:"genres"`
	Popularity int      `json:"popularity"`
}

type RecommendResponse struct {
	Seeds   []Song `json:"seeds"`
	Results []struct {
		Rank     int     `json:"rank"`
		Score    float64 `json:"score"`
		Distance float64 `json:"distance"`
		Song     Song    `json:"song"`
	} `json:"results"`
}

// EngineError carries the engine's status code so we can pass 400/404 through.
type EngineError struct {
	Status  int
	Message string
}

func (e *EngineError) Error() string { return fmt.Sprintf("engine %d: %s", e.Status, e.Message) }

// EngineClient talks to the C++ engine over HTTP/JSON.
type EngineClient struct {
	BaseURL string
	Client  *http.Client
}

func NewEngineClient(baseURL string) *EngineClient {
	return &EngineClient{BaseURL: strings.TrimRight(baseURL, "/"), Client: &http.Client{Timeout: 5 * time.Second}}
}

func (e *EngineClient) get(ctx context.Context, path string, params url.Values, out any) error {
	u := e.BaseURL + path
	if len(params) > 0 {
		u += "?" + params.Encode()
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return err
	}
	resp, err := e.Client.Do(req)
	if err != nil {
		return fmt.Errorf("engine unreachable: %w", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return err
	}
	if resp.StatusCode != http.StatusOK {
		var msg struct {
			Error string `json:"error"`
		}
		_ = json.Unmarshal(body, &msg)
		return &EngineError{Status: resp.StatusCode, Message: msg.Error}
	}
	return json.Unmarshal(body, out)
}

func (e *EngineClient) Genres(ctx context.Context) ([]string, error) {
	var out struct {
		Genres []string `json:"genres"`
	}
	err := e.get(ctx, "/genres", nil, &out)
	return out.Genres, err
}

// Recommend asks the engine for k songs near the profile and/or seeds.
func (e *EngineClient) Recommend(ctx context.Context, p *Profile, seeds []string, k int) (*RecommendResponse, error) {
	params := url.Values{}
	if p != nil {
		for key, v := range p.queryParams() {
			params.Set(key, v)
		}
		if len(p.Genres) > 0 {
			params.Set("genres", strings.Join(p.Genres, ","))
		}
	}
	if len(seeds) > 0 {
		params.Set("seeds", strings.Join(seeds, ","))
	}
	params.Set("k", strconv.Itoa(k))
	var out RecommendResponse
	if err := e.get(ctx, "/recommend", params, &out); err != nil {
		return nil, err
	}
	return &out, nil
}
