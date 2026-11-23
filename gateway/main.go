// Command gateway is the public API for the Playlist Engine.
//
//	go run . -engine http://localhost:8080 -addr :9090
//
// Set ANTHROPIC_API_KEY to turn playlist ideas into profiles with Claude;
// without it, a keyword table is used. LLM_MODEL overrides the model.
package main

import (
	"context"
	"errors"
	"flag"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	addr := flag.String("addr", ":9090", "address to listen on")
	engineURL := flag.String("engine", "http://localhost:8080", "C++ engine base URL")
	flag.Parse()

	var llm Interpreter
	if key := os.Getenv("ANTHROPIC_API_KEY"); key != "" {
		model := os.Getenv("LLM_MODEL")
		if model == "" {
			model = "claude-haiku-5-5"
		}
		llm = NewClaudeInterpreter(key, model)
		slog.Info("LLM enabled", "model", model)
	} else {
		slog.Info("no ANTHROPIC_API_KEY: ideas will use the keyword fallback table")
	}

	srv, err := NewServer(NewEngineClient(*engineURL), llm)
	if err != nil {
		slog.Error("bad engine URL", "err", err)
		os.Exit(1)
	}
	httpServer := &http.Server{
		Addr:              *addr,
		Handler:           srv.Routes(),
		ReadHeaderTimeout: 5 * time.Second, // don't let slow clients hold connections open
		WriteTimeout:      30 * time.Second,
	}

	// Ctrl+C: stop accepting, let in-flight requests finish (up to 10 s), exit.
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = httpServer.Shutdown(shutdownCtx)
	}()

	slog.Info("gateway listening", "addr", *addr, "engine", *engineURL)
	if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		slog.Error("server failed", "err", err)
		os.Exit(1)
	}
	slog.Info("shut down cleanly")
}
