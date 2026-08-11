# GMD Similarity Scanner (MVP)

A Windows Geode mod MVP that lets you select a `.gmd`, `.gmd2`, or `.lvl` file, parses it through `hjfod.gmd-api`, searches a small number of Geometry Dash **Recent** pages, collects candidate metadata, downloads only a small throttled subset, and ranks them by structural similarity.

## Current scope

This is deliberately an MVP, not a global anti-piracy index. It scans only `kPagesToScan` Recent pages and caps the candidate queue. A full-server search needs a separate crawler/index/database because the official game search does not expose a server-side “similar structure” query.

The comparison currently uses object ID, position, rotation, scale, flips, translation normalization, 300-unit sections, multiset Jaccard similarity, and partial-section matching. Partial matches are coverage-weighted so a random match in one tiny section does not inflate the final rank. Color/group/trigger metadata is ignored by the parser for now.

## Before publishing

Edit `mod.json`:

- replace `example.gmd-similarity-scanner` with your own unique mod ID;
- replace `YourName` with your developer name.

## Build only on GitHub

Push the repository as-is. `.github/workflows/build.yml` does two jobs:

1. compiles and runs the standalone similarity tests on Ubuntu;
2. builds the Windows `.geode` package with Geode SDK **v5.9.0** using the official `geode-sdk/build-geode-mod` action.

After a successful run, download the `GMD-Similarity-Scanner-Windows` artifact from the Actions run.

## Local core test (optional)

The comparison engine has no Geode dependency, so it can be checked with:

```bash
g++ -std=c++23 -O2 -Wall -Wextra -Wpedantic -Isrc tests/core_tests.cpp src/Similarity.cpp -o core-tests
./core-tests
```

## How to use in-game

1. Install the built `.geode` file and its dependency `hjfod.gmd-api`.
2. Open Geometry Dash main menu.
3. Click **GMD Scan** at the bottom-right.
4. Pick a `.gmd`, `.gmd2`, or `.lvl` file.
5. The mod queries Recent pages, pre-filters the candidate pool, downloads at most 10 full levels with a 2.5-second delay between downloads, then shows results in a bounded scrollable popup.

The result is a similarity signal, not a verdict that a creator stole a level.

## Tunables

At the top of `src/main.cpp`:

- `kPagesToScan` — number of Recent pages to query;
- `kMaxCollectedCandidates` — cap on metadata candidates collected from Recent pages;
- `kMaxDownloadedCandidates` — hard cap on full level downloads per scan;
- `kDownloadDelay` — pause between full level downloads;
- `kMaxConsecutiveDownloadFailures` — abort threshold when the server starts rejecting downloads;
- `kMaxDisplayedResults` — number of interesting results shown.

In `src/Similarity.cpp`:

- `kPositionQuantum` — tolerance through coordinate quantization;
- `kRotationQuantum`;
- `kScaleQuantum`;
- weights inside `segmentSimilarity` and `compare`.

## Rate limits

The mod deliberately does not attempt to bypass RobTop server rate limits. If the game reports a cooldown, wait for that cooldown to expire before scanning again. Version 0.1.1 reduces the request burst substantially and stops after repeated download failures instead of continuing to retry.

## Known limitations of v0.1.1

- Recent search only; it does not crawl all historical levels.
- It temporarily uses `GameLevelManager` level-search/download delegate slots while the scan is active and restores the previous pointers when done.
- It compares all parsed placed objects; gameplay-vs-decoration semantic weighting is not implemented yet.
- Section matching is greedy rather than a full sequence alignment algorithm.
- The final popup lists IDs but does not yet provide clickable “open level” buttons.
