# Changelog

## 0.1.1

- Cap full online level downloads at 10 per scan.
- Add a 2.5 second delay between level downloads.
- Stop after two consecutive download failures instead of continuing into a possible server rate limit.
- Keep scanning 5 Recent metadata pages, then prefer candidates in the same coarse level-length category.
- Use a bounded scrollable results popup so the result list stays on-screen.
- Show only structurally interesting results (>= 40% rank); otherwise show a single highest weak score.
- Reduce false-positive partial-match rank when coverage is near zero.

## 0.1.0

- Initial MVP.
