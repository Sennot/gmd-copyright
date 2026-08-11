# Bindings/API check for Geode 5.9.0 / GD 2.2081

Checked on 2026-08-11.

The integration source intentionally sticks to symbols verified in the current Geode/GD 2.2081 bindings and current GMD-API source:

- `SearchType::Recent`
- `GJSearchObject::create(SearchType)`
- `GJSearchObject::getPageObject(int)`
- `GameLevelManager::get()` / `getOnlineLevels(GJSearchObject*)`
- `GameLevelManager::downloadLevel(int, bool, int)`
- `GameLevelManager::m_levelManagerDelegate`
- `GameLevelManager::m_levelDownloadDelegate`
- both `LevelManagerDelegate::loadLevelsFinished/Failed` overload pairs
- `LevelDownloadDelegate::levelDownloadFinished/Failed`
- `GJGameLevel::m_levelID`, `m_levelName`, `m_levelString`
- `SeedValueRSV::operator int()`
- `cocos2d::ZipUtils::decompressString(...)`
- `gmd::importGmdAsLevel(path)` from `hjfod.gmd-api` 1.5.0
- Geode v5 async file picker + `async::TaskHolder` usage matching current GDShare patterns

## Sandbox checks performed

Both standalone builds below passed with warnings promoted to errors:

```text
g++ 14.2 / C++23 / -Wall -Wextra -Wpedantic -Werror: PASS
clang++ 17 / C++23 / -Wall -Wextra -Wpedantic -Werror: PASS
mod.json JSON parse: PASS
GitHub Actions YAML parse: PASS
```

Core test output:

```text
core tests passed
exact rank=1 changed rank=0.898462 partial rank=0.64375
```

## Limitation of the sandbox check

A true Geode link/package build could not be completed in this sandbox because the Geode SDK/game import libraries are not installed in the runtime and ordinary outbound git cloning is unavailable. The source-level API/binding surface was checked against the current upstream Geode/GD bindings, and the included GitHub Actions workflow performs the authoritative Windows Geode 5.9.0 integration build.
