# Changelog

## 0.2.0

- Added exact **Target Level ID** verification mode through Geode mod settings.
- Recent discovery now ranks metadata candidates by object-count similarity instead of mostly by coarse length.
- Uses `m_originalLevel` as a strong ancestry hint when the source GMD retains its online level ID.
- Increased Recent metadata pool while keeping full downloads capped and throttled.
- Added a small delay between Recent page queries.
- Result wording now distinguishes “not found in downloaded shortlist” from “no copy exists”.
- Added a regression test proving background/header and text-content changes are ignored by the structural parser.

## 0.1.1

- Added download throttling and early stop on repeated server failures.
- Added bounded scrollable result popup.
- Reduced random tiny-section false positives.

## 0.1.0

- Initial MVP.
