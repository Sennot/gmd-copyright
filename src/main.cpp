#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/cocos.hpp>
#include <hjfod.gmd-api/include/GMD.hpp>

#include "Similarity.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {

// Searching pages is relatively cheap; downloading every full level is not.
// We collect a wider pool, then download only a small subset to avoid hammering
// RobTop's servers and triggering the game's rate limiter.
constexpr int kPagesToScan = 5;
constexpr std::size_t kMaxCollectedCandidates = 60;
constexpr std::size_t kMaxDownloadedCandidates = 10;
constexpr std::size_t kMaxDisplayedResults = 5;
constexpr std::size_t kMinimumSourceObjects = 8;
constexpr int kMaxConsecutiveDownloadFailures = 2;
constexpr double kMinimumInterestingScore = 0.40;
constexpr auto kDownloadDelay = std::chrono::milliseconds(2500);

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
    int length = 0;
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
        m_sourceLength = source ? source->m_levelLength : 0;
        m_candidates.clear();
        m_results.clear();
        m_seenIDs.clear();
        m_currentPage = 0;
        m_downloadIndex = 0;
        m_failedDownloads = 0;
        m_consecutiveDownloadFailures = 0;
        m_stoppedForServerFailures = false;
        m_lastDownloadError = 0;

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

        m_consecutiveDownloadFailures = 0;

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

        scheduleNextCandidate();
    }

    void levelDownloadFailed(int errorCode) override {
        if (!m_running) return;

        ++m_failedDownloads;
        ++m_consecutiveDownloadFailures;
        m_lastDownloadError = errorCode;

        log::warn(
            "Level download failed with code {} ({} consecutive failures)",
            errorCode,
            m_consecutiveDownloadFailures
        );

        // If the server starts rejecting downloads, stop instead of continuing to
        // fire requests into a rate limit. We still show any results gathered so far.
        if (m_consecutiveDownloadFailures >= kMaxConsecutiveDownloadFailures) {
            m_stoppedForServerFailures = true;
            finishAndShowResults();
            return;
        }

        scheduleNextCandidate();
    }

private:
    void requestCurrentPage() {
        if (!m_running || !m_manager) return;

        if (m_currentPage >= kPagesToScan || m_candidates.size() >= kMaxCollectedCandidates) {
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
                m_candidates.push_back({ id, level->m_levelLength, std::string(level->m_levelName) });
                if (m_candidates.size() >= kMaxCollectedCandidates) break;
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

    void selectDownloadCandidates() {
        if (m_candidates.size() <= kMaxDownloadedCandidates) return;

        // Prefer candidates with the same coarse GD length category when GMD
        // metadata contains it. This pre-filter costs no extra network requests.
        if (m_sourceLength > 0) {
            std::stable_sort(m_candidates.begin(), m_candidates.end(), [this](auto const& a, auto const& b) {
                return std::abs(a.length - m_sourceLength) < std::abs(b.length - m_sourceLength);
            });
        }

        m_candidates.resize(kMaxDownloadedCandidates);
    }

    void beginDownloads() {
        if (!m_running) return;

        if (m_candidates.empty()) {
            failAndFinish("No candidate levels were returned by the Recent search.");
            return;
        }

        auto collected = m_candidates.size();
        selectDownloadCandidates();

        Notification::create(
            fmt::format(
                "Found {} candidates; downloading only {} to avoid rate limits...",
                collected,
                m_candidates.size()
            ),
            NotificationIcon::Loading,
            3.f
        )->show();

        m_downloadIndex = 0;
        downloadNextCandidate();
    }

    void scheduleNextCandidate() {
        if (!m_running) return;

        if (m_downloadIndex >= m_candidates.size()) {
            finishAndShowResults();
            return;
        }

        // Do the wait off the GD thread, then return to the main thread before
        // touching GameLevelManager again.
        std::thread([] {
            std::this_thread::sleep_for(kDownloadDelay);
            geode::queueInMainThread([] {
                ScannerController::get().downloadNextCandidate();
            });
        }).detach();
    }

    void downloadNextCandidate() {
        if (!m_running || !m_manager) return;

        if (m_downloadIndex >= m_candidates.size()) {
            finishAndShowResults();
            return;
        }

        auto const& candidate = m_candidates[m_downloadIndex++];
        log::debug(
            "Downloading comparison candidate {}/{}: {} ({})",
            m_downloadIndex,
            m_candidates.size(),
            candidate.id,
            candidate.name
        );

        // Downloads are intentionally sequential and throttled by
        // scheduleNextCandidate().
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

    void showScrollableResults(std::string const& body) {
        // The long FLAlertLayer overload provides a bounded scrolling text area.
        // This prevents the result list from growing beyond the screen.
        auto* alert = FLAlertLayer::create(
            nullptr,
            "GMD Similarity Scanner",
            body,
            "OK",
            nullptr,
            470.f,
            true,
            260.f,
            0.62f
        );
        if (alert) {
            alert->show();
        }
    }

    void finishAndShowResults() {
        std::sort(m_results.begin(), m_results.end(), [](auto const& a, auto const& b) {
            return a.similarity.rankScore > b.similarity.rankScore;
        });

        auto compared = m_results.size();
        auto attempted = m_downloadIndex;
        auto failures = m_failedDownloads;
        auto stoppedForFailures = m_stoppedForServerFailures;
        auto lastError = m_lastDownloadError;
        auto sourceObjects = m_source.objectCount;

        restoreDelegates();
        m_running = false;

        std::string body = fmt::format(
            "Source: <cy>{}</c> objects\n"
            "Downloaded: <cy>{}</c>/<cy>{}</c>",
            sourceObjects,
            compared,
            attempted
        );

        if (failures > 0) {
            body += fmt::format("  <cr>({} failed)</c>", failures);
        }
        body += "\n\n";

        if (stoppedForFailures) {
            body += fmt::format(
                "<cr>Stopped early after repeated server download failures</c> "
                "(last code {}). The scanner will not keep retrying into a possible rate limit.\n\n",
                lastError
            );
        }

        if (m_results.empty()) {
            body += "No downloadable candidates could be compared.";
        }
        else {
            std::vector<ScanResult const*> interesting;
            interesting.reserve(m_results.size());
            for (auto const& result : m_results) {
                if (result.similarity.rankScore >= kMinimumInterestingScore) {
                    interesting.push_back(&result);
                }
            }

            if (interesting.empty()) {
                auto const& best = m_results.front();
                body += fmt::format(
                    "<cg>No suspicious structural match found.</c>\n\n"
                    "Highest weak score: <cy>{:.1f}%</c>\n"
                    "{}  <co>ID {}</c>\n"
                    "overall {:.1f}% | section {:.1f}% | coverage {:.1f}%",
                    best.similarity.rankScore * 100.0,
                    best.name.empty() ? "(unnamed)" : best.name,
                    best.id,
                    best.similarity.overall * 100.0,
                    best.similarity.bestSection * 100.0,
                    best.similarity.coverage * 100.0
                );
            }
            else {
                body += "<cg>Possible structural matches</c>\n";
                std::size_t count = std::min(kMaxDisplayedResults, interesting.size());
                for (std::size_t i = 0; i < count; ++i) {
                    auto const& result = *interesting[i];
                    body += fmt::format(
                        "\n<cy>{}. {:.1f}%</c>  {}\n"
                        "<co>ID {}</c> | overall {:.1f}% | section {:.1f}% | coverage {:.1f}%\n",
                        i + 1,
                        result.similarity.rankScore * 100.0,
                        result.name.empty() ? "(unnamed)" : result.name,
                        result.id,
                        result.similarity.overall * 100.0,
                        result.similarity.bestSection * 100.0,
                        result.similarity.coverage * 100.0
                    );
                }
            }
        }

        body += "\n\n<cl>MVP: scans a small Recent sample, not the entire GD database.</c>";

        showScrollableResults(body);
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
        m_sourceLength = 0;
        m_candidates.clear();
        m_results.clear();
        m_seenIDs.clear();
        m_currentPage = 0;
        m_downloadIndex = 0;
        m_failedDownloads = 0;
        m_consecutiveDownloadFailures = 0;
        m_stoppedForServerFailures = false;
        m_lastDownloadError = 0;
    }

    bool m_running = false;
    GameLevelManager* m_manager = nullptr;
    LevelManagerDelegate* m_previousLevelManagerDelegate = nullptr;
    LevelDownloadDelegate* m_previousLevelDownloadDelegate = nullptr;

    gmdscan::Fingerprint m_source;
    int m_sourceLevelID = 0;
    int m_sourceLength = 0;
    int m_currentPage = 0;
    std::size_t m_downloadIndex = 0;
    std::size_t m_failedDownloads = 0;
    int m_consecutiveDownloadFailures = 0;
    bool m_stoppedForServerFailures = false;
    int m_lastDownloadError = 0;

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
