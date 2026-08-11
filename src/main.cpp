#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/cocos.hpp>
#include <hjfod.gmd-api/include/GMD.hpp>

#include "Similarity.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr int kPagesToScan = 5;
constexpr std::size_t kMaxCandidates = 75;
constexpr std::size_t kMaxDisplayedResults = 8;
constexpr std::size_t kMinimumSourceObjects = 8;

static auto GMD_PICK_OPTIONS = file::FilePickOptions {
    std::nullopt,
    {
        {
            "Geometry Dash level files",
            { "*.gmd", "*.gmd2", "*.lvl" }
        }
    }
};

std::string unpackLevelString(GJGameLevel* level) {
    if (!level) return {};

    std::string encoded(level->m_levelString);
    if (encoded.empty()) return {};

    // GMD-API may already hand us the raw semicolon-separated object data.
    if (encoded.find(';') != std::string::npos && encoded.find(',') != std::string::npos) {
        return encoded;
    }

    auto unpacked = cocos2d::ZipUtils::decompressString(level->m_levelString, false, 0);
    return std::string(unpacked);
}

struct Candidate {
    int id = 0;
    std::string name;
};

struct ScanResult {
    int id = 0;
    std::string name;
    gmdscan::SimilarityReport similarity;
};

class ScannerController : public LevelManagerDelegate, public LevelDownloadDelegate {
public:
    static ScannerController& get() {
        static ScannerController instance;
        return instance;
    }

    bool isRunning() const {
        return m_running;
    }

    void start(GJGameLevel* source) {
        if (m_running) {
            FLAlertLayer::create(
                "GMD Similarity Scanner",
                "A scan is already running.",
                "OK"
            )->show();
            return;
        }

        auto raw = unpackLevelString(source);
        if (raw.empty()) {
            FLAlertLayer::create(
                "Cannot scan",
                "The selected GMD did not contain readable level data.",
                "OK"
            )->show();
            return;
        }

        m_source = gmdscan::buildFingerprint(raw);
        if (m_source.objectCount < kMinimumSourceObjects) {
            FLAlertLayer::create(
                "Cannot scan",
                fmt::format(
                    "Only {} objects were parsed. At least {} are required for a useful comparison.",
                    m_source.objectCount,
                    kMinimumSourceObjects
                ),
                "OK"
            )->show();
            m_source = {};
            return;
        }

        m_sourceLevelID = source ? static_cast<int>(source->m_levelID) : 0;
        m_candidates.clear();
        m_results.clear();
        m_seenIDs.clear();
        m_currentPage = 0;
        m_downloadIndex = 0;
        m_failedDownloads = 0;

        m_manager = GameLevelManager::get();
        if (!m_manager) {
            FLAlertLayer::create("Cannot scan", "GameLevelManager is unavailable.", "OK")->show();
            resetState();
            return;
        }

        // The game uses raw delegate pointers. Preserve anything that was there and
        // restore it as soon as this finite scan finishes.
        m_previousLevelManagerDelegate = m_manager->m_levelManagerDelegate;
        m_previousLevelDownloadDelegate = m_manager->m_levelDownloadDelegate;
        m_manager->m_levelManagerDelegate = this;
        m_manager->m_levelDownloadDelegate = this;
        m_running = true;

        Notification::create(
            fmt::format("Scanning {} recent pages...", kPagesToScan),
            NotificationIcon::Loading,
            2.f
        )->show();

        requestCurrentPage();
    }

    void loadLevelsFinished(cocos2d::CCArray* levels, char const*) override {
        handleLevelsPage(levels);
    }

    void loadLevelsFailed(char const*) override {
        handleLevelsPageFailure();
    }

    void loadLevelsFinished(cocos2d::CCArray* levels, char const*, int) override {
        handleLevelsPage(levels);
    }

    void loadLevelsFailed(char const*, int) override {
        handleLevelsPageFailure();
    }

    void setupPageInfo(gd::string, char const*) override {}

    void levelDownloadFinished(GJGameLevel* level) override {
        if (!m_running) return;

        if (level) {
            auto raw = unpackLevelString(level);
            if (!raw.empty()) {
                auto fingerprint = gmdscan::buildFingerprint(raw);
                if (fingerprint.objectCount > 0) {
                    ScanResult result;
                    result.id = static_cast<int>(level->m_levelID);
                    result.name = std::string(level->m_levelName);
                    result.similarity = gmdscan::compare(m_source, fingerprint);
                    m_results.push_back(std::move(result));
                }
            }
        }

        downloadNextCandidate();
    }

    void levelDownloadFailed(int) override {
        if (!m_running) return;
        ++m_failedDownloads;
        downloadNextCandidate();
    }

private:
    void requestCurrentPage() {
        if (!m_running || !m_manager) return;

        if (m_currentPage >= kPagesToScan || m_candidates.size() >= kMaxCandidates) {
            beginDownloads();
            return;
        }

        auto* base = GJSearchObject::create(SearchType::Recent);
        if (!base) {
            failAndFinish("Could not create a Recent search object.");
            return;
        }

        auto* page = base->getPageObject(m_currentPage);
        if (!page) {
            failAndFinish("Could not create the requested Recent page.");
            return;
        }

        m_manager->getOnlineLevels(page);
    }

    void handleLevelsPage(cocos2d::CCArray* levels) {
        if (!m_running) return;

        if (levels) {
            for (auto* level : CCArrayExt<GJGameLevel*>(levels)) {
                if (!level) continue;
                int id = static_cast<int>(level->m_levelID);
                if (id <= 0 || id == m_sourceLevelID || m_seenIDs.contains(id)) continue;

                m_seenIDs.insert(id);
                m_candidates.push_back({ id, std::string(level->m_levelName) });
                if (m_candidates.size() >= kMaxCandidates) break;
            }
        }

        ++m_currentPage;
        requestCurrentPage();
    }

    void handleLevelsPageFailure() {
        if (!m_running) return;

        // A single bad page should not discard already collected candidates.
        ++m_currentPage;
        requestCurrentPage();
    }

    void beginDownloads() {
        if (!m_running) return;

        if (m_candidates.empty()) {
            failAndFinish("No candidate levels were returned by the Recent search.");
            return;
        }

        Notification::create(
            fmt::format("Comparing {} candidate levels...", m_candidates.size()),
            NotificationIcon::Loading,
            2.f
        )->show();

        m_downloadIndex = 0;
        downloadNextCandidate();
    }

    void downloadNextCandidate() {
        if (!m_running || !m_manager) return;

        if (m_downloadIndex >= m_candidates.size()) {
            finishAndShowResults();
            return;
        }

        auto const& candidate = m_candidates[m_downloadIndex++];
        // Sequential downloads are intentional: this avoids hammering the GD
        // servers with a burst of simultaneous level-download requests.
        m_manager->downloadLevel(candidate.id, false, 0);
    }

    void restoreDelegates() {
        if (!m_manager) return;
        if (m_manager->m_levelManagerDelegate == this) {
            m_manager->m_levelManagerDelegate = m_previousLevelManagerDelegate;
        }
        if (m_manager->m_levelDownloadDelegate == this) {
            m_manager->m_levelDownloadDelegate = m_previousLevelDownloadDelegate;
        }
    }

    void finishAndShowResults() {
        std::sort(m_results.begin(), m_results.end(), [](auto const& a, auto const& b) {
            return a.similarity.rankScore > b.similarity.rankScore;
        });

        auto compared = m_results.size();
        auto failures = m_failedDownloads;
        restoreDelegates();
        m_running = false;

        std::string body = fmt::format(
            "Parsed <cy>{}</c> source objects. Compared <cy>{}</c> online levels",
            m_source.objectCount,
            compared
        );
        if (failures > 0) {
            body += fmt::format(" ({} downloads failed)", failures);
        }
        body += ".\n\n";

        if (m_results.empty()) {
            body += "No downloadable candidates could be compared.";
        }
        else {
            body += "<cg>Top structural matches</c>\n";
            std::size_t count = std::min(kMaxDisplayedResults, m_results.size());
            for (std::size_t i = 0; i < count; ++i) {
                auto const& result = m_results[i];
                body += fmt::format(
                    "\n<cy>{:.1f}%</c>  {}  <co>ID {}</c>\n"
                    "overall {:.1f}% | best section {:.1f}% | coverage {:.1f}%",
                    result.similarity.rankScore * 100.0,
                    result.name.empty() ? "(unnamed)" : result.name,
                    result.id,
                    result.similarity.overall * 100.0,
                    result.similarity.bestSection * 100.0,
                    result.similarity.coverage * 100.0
                );
            }

            body += "\n\n<cl>This MVP scans only a limited number of Recent pages, not the whole GD database.</c>";
        }

        FLAlertLayer::create("GMD Similarity Scanner", body, "OK")->show();
        resetState(false);
    }

    void failAndFinish(std::string const& message) {
        restoreDelegates();
        m_running = false;
        FLAlertLayer::create("Scan stopped", message, "OK")->show();
        resetState(false);
    }

    void resetState(bool restore = true) {
        if (restore) restoreDelegates();
        m_running = false;
        m_manager = nullptr;
        m_previousLevelManagerDelegate = nullptr;
        m_previousLevelDownloadDelegate = nullptr;
        m_source = {};
        m_sourceLevelID = 0;
        m_candidates.clear();
        m_results.clear();
        m_seenIDs.clear();
        m_currentPage = 0;
        m_downloadIndex = 0;
        m_failedDownloads = 0;
    }

    bool m_running = false;
    GameLevelManager* m_manager = nullptr;
    LevelManagerDelegate* m_previousLevelManagerDelegate = nullptr;
    LevelDownloadDelegate* m_previousLevelDownloadDelegate = nullptr;

    gmdscan::Fingerprint m_source;
    int m_sourceLevelID = 0;
    int m_currentPage = 0;
    std::size_t m_downloadIndex = 0;
    std::size_t m_failedDownloads = 0;

    std::vector<Candidate> m_candidates;
    std::vector<ScanResult> m_results;
    std::unordered_set<int> m_seenIDs;
};

} // namespace

struct $modify(GMDScannerMenuLayer, MenuLayer) {
    struct Fields {
        async::TaskHolder<file::PickResult> picker;
    };

    bool init() {
        if (!MenuLayer::init()) return false;

        auto* menu = cocos2d::CCMenu::create();
        menu->setPosition({0.f, 0.f});

        auto* sprite = ButtonSprite::create("GMD Scan");
        auto* button = CCMenuItemSpriteExtra::create(
            sprite,
            this,
            menu_selector(GMDScannerMenuLayer::onGMDScan)
        );

        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        button->setPosition({winSize.width - 67.f, 32.f});
        menu->addChild(button);
        this->addChild(menu, 100);
        return true;
    }

    void onGMDScan(cocos2d::CCObject*) {
        if (ScannerController::get().isRunning()) {
            FLAlertLayer::create(
                "GMD Similarity Scanner",
                "A scan is already running.",
                "OK"
            )->show();
            return;
        }

        m_fields->picker.spawn(
            file::pick(file::PickMode::OpenFile, GMD_PICK_OPTIONS),
            [](file::PickResult result) {
                if (!result.isOk()) {
                    FLAlertLayer::create("File picker error", result.unwrapErr(), "OK")->show();
                    return;
                }

                auto path = std::move(result).unwrap();
                if (!path) return;

                auto imported = gmd::importGmdAsLevel(*path);
                if (!imported) {
                    FLAlertLayer::create("GMD import error", imported.unwrapErr(), "OK")->show();
                    return;
                }

                ScannerController::get().start(*imported);
            }
        );
    }
};
