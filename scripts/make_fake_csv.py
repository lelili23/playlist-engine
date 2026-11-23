#!/usr/bin/env python3
"""Generate a fake CSV with the same schema and quirks as the Kaggle Spotify Tracks Dataset.

Useful for smoke tests / CI without downloading the real data. Includes the
messy parts the loader must handle: commas and quotes in names, duplicate
track_ids under different genres, and tempo == 0 rows.

    python3 scripts/make_fake_csv.py 20000 data/fake.csv
"""
import csv
import random
import sys

HEADER = ["", "track_id", "artists", "album_name", "track_name", "popularity", "duration_ms",
          "explicit", "danceability", "energy", "key", "loudness", "mode", "speechiness",
          "acousticness", "instrumentalness", "liveness", "valence", "tempo",
          "time_signature", "track_genre"]
GENRES = ["pop", "rock", "jazz", "edm", "acoustic", "hip-hop", "classical", "study"]


def main() -> None:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    out = sys.argv[2] if len(sys.argv) > 2 else "data/fake.csv"
    rng = random.Random(7)
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(HEADER)
        idx = 0
        for i in range(n):
            name = rng.choice([f"Song {i}", f"Song {i}, Part II", f'The "Best" Song {i}'])
            tempo = 0.0 if rng.random() < 0.002 else rng.gauss(120, 28)
            row = [f"t{i:07d}", f"Artist {i % 900};Feat {i % 37}", f"Album {i % 3000}", name,
                   rng.randint(0, 100), rng.randint(90_000, 400_000), rng.random() < 0.1,
                   round(rng.random(), 3), round(rng.random(), 3), rng.randint(-1, 11),
                   round(rng.uniform(-30, 0), 3), rng.randint(0, 1), round(rng.random() * 0.3, 4),
                   round(rng.random(), 4), round(rng.random() ** 4, 4), round(rng.random(), 3),
                   round(rng.random(), 3), round(max(tempo, 0.0), 3), 4]
            for genre in rng.sample(GENRES, 2 if rng.random() < 0.15 else 1):  # duplicates
                w.writerow([idx, *row, genre])
                idx += 1


if __name__ == "__main__":
    main()
