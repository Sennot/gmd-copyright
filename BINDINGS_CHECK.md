# Geode / GD bindings check for v0.2.0

Target: Geode SDK 5.9.0, Geometry Dash 2.2081 (Windows).

Checked against the current Geode 2.2081 bindings:

- `GameLevelManager::get()`
- `GameLevelManager::getOnlineLevels(GJSearchObject*)`
- `GameLevelManager::downloadLevel(int, bool, int)`
- `GameLevelManager::m_levelManagerDelegate`
- `GameLevelManager::m_levelDownloadDelegate`
- `GJSearchObject::create(SearchType)`
- `GJSearchObject::getPageObject(int)`
- `SearchType::Recent`
- `GJGameLevel::m_levelID`
- `GJGameLevel::m_levelName`
- `GJGameLevel::m_levelString`
- `GJGameLevel::m_levelLength`
- `GJGameLevel::m_objectCount`
- `GJGameLevel::m_originalLevel`
- both `LevelManagerDelegate::loadLevelsFinished` / `loadLevelsFailed` overload families
- `LevelDownloadDelegate::levelDownloadFinished` / `levelDownloadFailed`

The mod also uses Geode settings via `Mod::get()->getSettingValue<int64_t>()` for the optional Target Level ID.

Standalone comparison core was compiled and tested in the sandbox using GCC 14 and Clang 17 with C++23 and `-Werror`.

A full Geode link/package build still requires the GitHub Actions Geode toolchain because the sandbox does not contain the installed Geometry Dash/Geode SDK import environment.
