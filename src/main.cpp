#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/cocos.hpp>
#include <hjfod.gmd-api/include/GMD.hpp>

#include "Similarity.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {

// Discovery mode is still only a Recent sample, but unlike v0.1.1 we use the
// metadata already present in search results to pick the most plausible clones.
constexpr int kPagesToScan = 12;
constexpr std::size_t kMaxCollectedCandidates = 120;
constexpr std::size_t kMaxDownloadedCandidates = 14;
constexpr std::size_t kMaxDisplayedResults = 6;
constexpr std::size_t kMinimumSourceObjects = 8;
constexpr int kMaxConsecutiveDownloadFailures = 2;
constexpr double kMinimumInterestingScore = 0.42;
constexpr auto kDownloadDelay = std::chrono::milliseconds(3000);
constexpr auto kSearchPageDelay = std::chrono::milliseconds(350);

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

    // GMD-API may already hand us raw semicolon-separated object data.
    if (encoded.find(';') != std::string::npos && encoded.find(',') != std::string::npos) {
        return encoded;
    }

    auto unpacked = cocos2d::ZipUtils::decompressString(level->m_levelString, false, 0);
    return std::string(unpacked);
}

struct Candidate {
    int id = 0;
    int length = 0;
    int objectCount = 0;
    int originalLevel = 0;
    std::string name;
    double metadataScore = 0.0;
};

struct ScanResult {
    int id = 0;
    std::string name;
    int metadataObjects = 0;
    int originalLevel = 0;
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
            FLAlertLayer::create("GMD Similarity Scanner", "A scan is already running.", "OK")->show();
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
        m_targetLevelID = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-level-id"));
        m_targetedMode = m_targetLevelID > 0;

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

        m_previousLevelManagerDelegate = m_manager->m_levelManagerDelegate;
        m_previousLevelDownloadDelegate = m_manager->m_levelDownloadDelegate;
        m_manager->m_levelManagerDelegate = this;
        m_manager->m_levelDownloadDelegate = this;
        m_running = true;

        if (m_targetedMode) {
            m_candidates.push_back({m_targetLevelID, 0, 0, 0, "(target level)", 1.0});
            Notification::create(
                fmt::format("Comparing directly with Level ID {}...", m_targetLevelID),
                NotificationIcon::Loading,
                2.f
            )->show();
            beginDownloads();
        }
        else {
            Notification::create(
                fmt::format("Scanning {} Recent pages for metadata...", kPagesToScan),
                NotificationIcon::Loading,
                2.f
            )->show();
            requestCurrentPage();
        }
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
                    result.metadataObjects = static_cast<int>(level->m_objectCount);
                    result.originalLevel = static_cast<int>(level->m_originalLevel);
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

        if (m_consecutiveDownloadFailures >= kMaxConsecutiveDownloadFailures) {
            m_stoppedForServerFailures = true;
            finishAndShowResults();
            return;
        }

        scheduleNextCandidate();
    }

private:
    double metadataScoreFor(Candidate const& candidate) const {
        double score = 0.0;

        // If GD preserved copy ancestry, this is the strongest cheap hint available.
        if (m_sourceLevelID > 0 && candidate.originalLevel == m_sourceLevelID) {
            score += 10.0;
        }

        // A clone with text/background edits usually keeps nearly the same object count.
        if (candidate.objectCount > 0 && m_source.objectCount > 0) {
            double src = static_cast<double>(m_source.objectCount);
            double cand = static_cast<double>(candidate.objectCount);
            double relativeDiff = std::abs(src - cand) / std::max(src, cand);
            score += 4.0 * std::max(0.0, 1.0 - relativeDiff * 4.0);
        }

        // Length is only a coarse category, so give it much less weight than count.
        if (m_sourceLength > 0 && candidate.length > 0) {
            int diff = std::abs(candidate.length - m_sourceLength);
            score += std::max(0.0, 1.0 - static_cast<double>(diff) * 0.25);
        }

        return score;
    }

    void requestCurrentPage() {
        if (!m_running || !m_manager || m_targetedMode) return;

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

    void scheduleNextSearchPage() {
        if (!m_running || m_targetedMode) return;
        std::thread([] {
            std::this_thread::sleep_for(kSearchPageDelay);
            geode::queueInMainThread([] {
                ScannerController::get().requestCurrentPage();
            });
        }).detach();
    }

    void handleLevelsPage(cocos2d::CCArray* levels) {
        if (!m_running || m_targetedMode) return;

        if (levels) {
            for (auto* level : CCArrayExt<GJGameLevel*>(levels)) {
                if (!level) continue;

                int id = static_cast<int>(level->m_levelID);
                if (id <= 0 || id == m_sourceLevelID || m_seenIDs.contains(id)) continue;

                Candidate candidate;
                candidate.id = id;
                candidate.length = level->m_levelLength;
                candidate.objectCount = static_cast<int>(level->m_objectCount);
                candidate.originalLevel = static_cast<int>(level->m_originalLevel);
                candidate.name = std::string(level->m_levelName);
                candidate.metadataScore = metadataScoreFor(candidate);

                m_seenIDs.insert(id);
                m_candidates.push_back(std::move(candidate));
                if (m_candidates.size() >= kMaxCollectedCandidates) break;
            }
        }

        ++m_currentPage;
        if (m_currentPage >= kPagesToScan || m_candidates.size() >= kMaxCollectedCandidates) {
            beginDownloads();
        }
        else {
            scheduleNextSearchPage();
        }
    }

    void handleLevelsPageFailure() {
        if (!m_running || m_targetedMode) return;

        ++m_currentPage;
        if (m_currentPage >= kPagesToScan) beginDownloads();
        else scheduleNextSearchPage();
    }

    void selectDownloadCandidates() {
        if (m_targetedMode) return;

        std::stable_sort(m_candidates.begin(), m_candidates.end(), [](auto const& a, auto const& b) {
            if (a.metadataScore != b.metadataScore) return a.metadataScore > b.metadataScore;
            return a.id > b.id; // Slight preference for newer uploads on equal metadata.
        });

        if (m_candidates.size() > kMaxDownloadedCandidates) {
            m_candidates.resize(kMaxDownloadedCandidates);
        }

        for (auto const& candidate : m_candidates) {
            log::debug(
                "Selected candidate {} '{}' objects={} length={} original={} metadata={:.3f}",
                candidate.id,
                candidate.name,
                candidate.objectCount,
                candidate.length,
                candidate.originalLevel,
                candidate.metadataScore
            );
        }
    }

    void beginDownloads() {
        if (!m_running) return;

        if (m_candidates.empty()) {
            failAndFinish("No candidate levels were returned by the Recent search.");
            return;
        }

        auto collected = m_candidates.size();
        selectDownloadCandidates();

        if (!m_targetedMode) {
            Notification::create(
                fmt::format(
                    "{} metadata candidates; comparing best {}...",
                    collected,
                    m_candidates.size()
                ),
                NotificationIcon::Loading,
                3.f
            )->show();
        }

        m_downloadIndex = 0;
        downloadNextCandidate();
    }

    void scheduleNextCandidate() {
        if (!m_running) return;

        if (m_downloadIndex >= m_candidates.size()) {
            finishAndShowResults();
            return;
        }

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
        if (alert) alert->show();
    }

    void appendResultLine(std::string& body, ScanResult const& result, std::size_t number = 0) const {
        if (number > 0) {
            body += fmt::format("\n<cy>{}. {:.1f}%</c>  {}\n", number, result.similarity.rankScore * 100.0,
                                result.name.empty() ? "(unnamed)" : result.name);
        }
        else {
            body += fmt::format("\n<cy>{:.1f}%</c>  {}\n", result.similarity.rankScore * 100.0,
                                result.name.empty() ? "(unnamed)" : result.name);
        }

        body += fmt::format(
            "<co>ID {}</c> | overall {:.1f}% | section {:.1f}% | coverage {:.1f}%\n"
            "objects {} vs {}",
            result.id,
            result.similarity.overall * 100.0,
            result.similarity.bestSection * 100.0,
            result.similarity.coverage * 100.0,
            result.similarity.sourceObjects,
            result.similarity.candidateObjects
        );

        if (m_sourceLevelID > 0 && result.originalLevel == m_sourceLevelID) {
            body += " | <cr>GD originalLevel points to source</c>";
        }
        body += "\n";
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
        auto targeted = m_targetedMode;
        auto targetID = m_targetLevelID;

        restoreDelegates();
        m_running = false;

        std::string body = fmt::format("Source: <cy>{}</c> parsed objects\n", sourceObjects);
        if (targeted) {
            body += fmt::format("Mode: <cg>exact Level ID {}</c>\n", targetID);
        }
        else {
            body += fmt::format(
                "Mode: Recent discovery | metadata pool <= {}\n",
                kMaxCollectedCandidates
            );
        }
        body += fmt::format("Downloaded: <cy>{}</c>/<cy>{}</c>", compared, attempted);
        if (failures > 0) body += fmt::format("  <cr>({} failed)</c>", failures);
        body += "\n\n";

        if (stoppedForFailures) {
            body += fmt::format(
                "<cr>Stopped after repeated server failures</c> (last code {}).\n\n",
                lastError
            );
        }

        if (m_results.empty()) {
            body += "No downloadable candidate could be compared.";
        }
        else if (targeted) {
            auto const& result = m_results.front();
            if (result.similarity.rankScore >= 0.75) {
                body += "<cr>Strong structural match</c>\n";
            }
            else if (result.similarity.rankScore >= kMinimumInterestingScore) {
                body += "<cy>Moderate structural match</c>\n";
            }
            else {
                body += "<cg>Low structural similarity</c>\n";
            }
            appendResultLine(body, result);
            body += "\n<cl>Exact-ID mode verifies the analyzer itself and does not depend on Recent discovery.</c>";
        }
        else {
            std::vector<ScanResult const*> interesting;
            interesting.reserve(m_results.size());
            for (auto const& result : m_results) {
                bool ancestryMatch = m_sourceLevelID > 0 && result.originalLevel == m_sourceLevelID;
                if (result.similarity.rankScore >= kMinimumInterestingScore || ancestryMatch) {
                    interesting.push_back(&result);
                }
            }

            if (interesting.empty()) {
                auto const& best = m_results.front();
                body += "<cg>No suspicious match among the downloaded shortlist.</c>\n";
                appendResultLine(body, best);
                body += "\n<cl>This does NOT mean no copy exists: discovery only sampled Recent levels.</c>";
            }
            else {
                body += "<cr>Possible structural matches</c>\n";
                std::size_t count = std::min(kMaxDisplayedResults, interesting.size());
                for (std::size_t i = 0; i < count; ++i) {
                    appendResultLine(body, *interesting[i], i + 1);
                }
            }
        }

        if (!targeted) {
            body += "\n<cl>Tip: set Target Level ID in mod settings to verify a known suspicious upload directly.</c>";
        }

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
        m_targetLevelID = 0;
        m_targetedMode = false;
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
    int m_targetLevelID = 0;
    bool m_targetedMode = false;
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
            FLAlertLayer::create("GMD Similarity Scanner", "A scan is already running.", "OK")->show();
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
