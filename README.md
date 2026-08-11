# GMD Similarity Scanner 0.2.0

Windows Geode mod for comparing a local `.gmd`, `.gmd2`, or `.lvl` against online Geometry Dash levels.

## Two modes

### 1. Recent discovery (default)

Leave **Target Level ID = 0** in the mod settings. The scanner:

1. parses the selected GMD through `hjfod.gmd-api`;
2. reads up to 12 Recent pages;
3. collects server metadata without downloading every level;
4. ranks candidates using `m_objectCount`, `m_levelLength`, and `m_originalLevel` when available;
5. downloads only the 14 strongest metadata candidates, sequentially and throttled;
6. performs the structural comparison and shows the best matches.

This is still a sample of Recent uploads, not a search over the entire GD database.

### 2. Exact Level ID verification

Open this mod's settings in Geode and set **Target Level ID** to a known suspicious online level ID. Then press **GMD Scan** and select the source `.gmd`.

In this mode the scanner skips Recent discovery and directly downloads exactly that Level ID. This is the best mode for testing whether the similarity algorithm recognizes a known copied level.

Set Target Level ID back to `0` to return to discovery mode.

## What the fingerprint ignores

The parser currently compares placed-object structure: object ID, X/Y, rotation, scale, and flips. It intentionally ignores unrelated object properties such as text content, colors, groups, and trigger parameters. The level settings/header record is not treated as a placed object, so changing the background/header alone should not lower the structural score.

## Before publishing

Edit `mod.json`:

- replace `example.gmd-similarity-scanner` with your own unique mod ID;
- replace `YourName` with your developer name.

## GitHub-only build

`.github/workflows/build.yml`:

1. compiles/runs standalone similarity tests on Ubuntu with warnings as errors;
2. builds the Windows `.geode` package with Geode SDK **v5.9.0**;
3. uploads the build artifact.

## Rate limits

The mod does not bypass RobTop rate limits. Full level downloads are sequential with a 3-second pause, and the scan stops after repeated server download failures. Search-page requests are also lightly throttled.

## Current limitations

- Recent discovery is not global; an older copy outside the sampled pages can be missed.
- Metadata shortlist quality depends on the server-provided object count/length/original-level fields.
- Gameplay-vs-decoration semantic classification is not implemented yet.
- Section matching is greedy rather than full sequence alignment.
- Results list Level IDs but does not yet open them directly.
