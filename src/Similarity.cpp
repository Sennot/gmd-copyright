#include "Similarity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace gmdscan {
namespace {

constexpr float kPositionQuantum = 6.f;
constexpr float kRotationQuantum = 15.f;
constexpr float kScaleQuantum = 0.05f;

bool parseInt(std::string_view text, int& out) {
    if (text.empty()) return false;
    std::string tmp(text);
    char* end = nullptr;
    long value = std::strtol(tmp.c_str(), &end, 10);
    if (end == tmp.c_str()) return false;
    out = static_cast<int>(value);
    return true;
}

bool parseFloat(std::string_view text, float& out) {
    if (text.empty()) return false;
    std::string tmp(text);
    char* end = nullptr;
    float value = std::strtof(tmp.c_str(), &end);
    if (end == tmp.c_str() || !std::isfinite(value)) return false;
    out = value;
    return true;
}

int quantize(float value, float quantum) {
    return static_cast<int>(std::lround(value / quantum));
}

float normalizeRotation(float rotation) {
    float r = std::fmod(rotation, 360.f);
    if (r < 0.f) r += 360.f;
    return r;
}

struct ExactToken {
    int id;
    int x;
    int y;
    int rotation;
    int scale;
    bool flipX;
    bool flipY;

    auto operator<=>(ExactToken const&) const = default;
};

struct ShapeToken {
    int x;
    int y;
    int rotation;
    int scale;
    bool flipX;
    bool flipY;

    auto operator<=>(ShapeToken const&) const = default;
};

template <class Token>
double multisetJaccard(std::vector<Token> const& a, std::vector<Token> const& b) {
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;

    std::map<Token, std::size_t> ca;
    std::map<Token, std::size_t> cb;
    for (auto const& item : a) ++ca[item];
    for (auto const& item : b) ++cb[item];

    std::size_t intersection = 0;
    std::size_t uni = 0;

    auto ia = ca.begin();
    auto ib = cb.begin();
    while (ia != ca.end() || ib != cb.end()) {
        if (ib == cb.end() || (ia != ca.end() && ia->first < ib->first)) {
            uni += ia->second;
            ++ia;
        }
        else if (ia == ca.end() || ib->first < ia->first) {
            uni += ib->second;
            ++ib;
        }
        else {
            intersection += std::min(ia->second, ib->second);
            uni += std::max(ia->second, ib->second);
            ++ia;
            ++ib;
        }
    }

    return uni == 0 ? 0.0 : static_cast<double>(intersection) / static_cast<double>(uni);
}

double segmentSimilarity(Fingerprint::Segment const& a, Fingerprint::Segment const& b) {
    if (a.objects.empty() || b.objects.empty()) return 0.0;

    float aMinX = std::numeric_limits<float>::max();
    float aMinY = std::numeric_limits<float>::max();
    float bMinX = std::numeric_limits<float>::max();
    float bMinY = std::numeric_limits<float>::max();
    for (auto const& o : a.objects) {
        aMinX = std::min(aMinX, o.x);
        aMinY = std::min(aMinY, o.y);
    }
    for (auto const& o : b.objects) {
        bMinX = std::min(bMinX, o.x);
        bMinY = std::min(bMinY, o.y);
    }

    std::vector<ExactToken> exactA;
    std::vector<ExactToken> exactB;
    std::vector<ShapeToken> shapeA;
    std::vector<ShapeToken> shapeB;
    exactA.reserve(a.objects.size());
    exactB.reserve(b.objects.size());
    shapeA.reserve(a.objects.size());
    shapeB.reserve(b.objects.size());

    auto append = [](ParsedObject const& o, float minX, float minY,
                     std::vector<ExactToken>& exact,
                     std::vector<ShapeToken>& shape) {
        int qx = quantize(o.x - minX, kPositionQuantum);
        int qy = quantize(o.y - minY, kPositionQuantum);
        int qr = quantize(normalizeRotation(o.rotation), kRotationQuantum);
        int qs = quantize(o.scale, kScaleQuantum);
        exact.push_back({o.id, qx, qy, qr, qs, o.flipX, o.flipY});
        shape.push_back({qx, qy, qr, qs, o.flipX, o.flipY});
    };

    for (auto const& o : a.objects) append(o, aMinX, aMinY, exactA, shapeA);
    for (auto const& o : b.objects) append(o, bMinX, bMinY, exactB, shapeB);

    double exact = multisetJaccard(exactA, exactB);
    double shape = multisetJaccard(shapeA, shapeB);
    double countRatio = static_cast<double>(std::min(a.objects.size(), b.objects.size())) /
                        static_cast<double>(std::max(a.objects.size(), b.objects.size()));

    // Geometry is deliberately weighted slightly more than object IDs. That lets
    // the detector keep a useful score when a copier swaps decoration/object IDs
    // while leaving the layout in the same places.
    return 0.40 * exact + 0.50 * shape + 0.10 * countRatio;
}

} // namespace

std::vector<ParsedObject> parseLevelData(std::string_view data) {
    std::vector<ParsedObject> result;

    std::size_t begin = 0;
    while (begin < data.size()) {
        std::size_t end = data.find(';', begin);
        if (end == std::string_view::npos) end = data.size();
        auto objectText = data.substr(begin, end - begin);
        begin = end + 1;
        if (objectText.empty()) continue;

        ParsedObject object;
        bool hasID = false;
        bool hasX = false;
        bool hasY = false;

        std::vector<std::string_view> tokens;
        std::size_t tBegin = 0;
        while (tBegin <= objectText.size()) {
            std::size_t tEnd = objectText.find(',', tBegin);
            if (tEnd == std::string_view::npos) tEnd = objectText.size();
            tokens.push_back(objectText.substr(tBegin, tEnd - tBegin));
            if (tEnd == objectText.size()) break;
            tBegin = tEnd + 1;
        }

        for (std::size_t i = 0; i + 1 < tokens.size(); i += 2) {
            int key = 0;
            if (!parseInt(tokens[i], key)) continue;
            auto value = tokens[i + 1];

            switch (key) {
                case 1: hasID = parseInt(value, object.id); break;
                case 2: hasX = parseFloat(value, object.x); break;
                case 3: hasY = parseFloat(value, object.y); break;
                case 4: {
                    int v = 0;
                    if (parseInt(value, v)) object.flipX = v != 0;
                    break;
                }
                case 5: {
                    int v = 0;
                    if (parseInt(value, v)) object.flipY = v != 0;
                    break;
                }
                case 6: parseFloat(value, object.rotation); break;
                case 32: parseFloat(value, object.scale); break;
                default: break;
            }
        }

        // The level settings/header record does not have object ID + position,
        // so it naturally falls out here.
        if (hasID && hasX && hasY && object.id > 0) {
            if (!std::isfinite(object.scale) || object.scale <= 0.f) object.scale = 1.f;
            result.push_back(object);
        }
    }

    return result;
}

Fingerprint buildFingerprint(std::vector<ParsedObject> objects, float segmentWidth) {
    Fingerprint fp;
    if (objects.empty()) return fp;
    if (!(segmentWidth > 0.f)) segmentWidth = 300.f;

    std::sort(objects.begin(), objects.end(), [](auto const& a, auto const& b) {
        return std::tie(a.x, a.y, a.id) < std::tie(b.x, b.y, b.id);
    });

    float minX = objects.front().x;
    float maxX = objects.front().x;
    for (auto const& o : objects) {
        minX = std::min(minX, o.x);
        maxX = std::max(maxX, o.x);
    }

    fp.objectCount = objects.size();
    fp.width = std::max(0.f, maxX - minX);

    std::map<int, std::vector<ParsedObject>> bySegment;
    for (auto o : objects) {
        // Normalize horizontal translation before creating sections.
        o.x -= minX;
        int index = static_cast<int>(std::floor(o.x / segmentWidth));
        bySegment[index].push_back(o);
    }

    fp.segments.reserve(bySegment.size());
    for (auto& [index, segmentObjects] : bySegment) {
        Fingerprint::Segment segment;
        segment.index = index;
        segment.objectCount = segmentObjects.size();
        segment.objects = std::move(segmentObjects);
        fp.segments.push_back(std::move(segment));
    }

    return fp;
}

Fingerprint buildFingerprint(std::string_view decompressedLevelData, float segmentWidth) {
    return buildFingerprint(parseLevelData(decompressedLevelData), segmentWidth);
}

SimilarityReport compare(Fingerprint const& source, Fingerprint const& candidate) {
    SimilarityReport report;
    report.sourceObjects = source.objectCount;
    report.candidateObjects = candidate.objectCount;

    if (source.objectCount == 0 || candidate.objectCount == 0 ||
        source.segments.empty() || candidate.segments.empty()) {
        return report;
    }

    struct PairScore {
        std::size_t sourceIndex;
        std::size_t candidateIndex;
        double score;
    };

    std::vector<PairScore> pairs;
    pairs.reserve(source.segments.size() * candidate.segments.size());
    for (std::size_t i = 0; i < source.segments.size(); ++i) {
        for (std::size_t j = 0; j < candidate.segments.size(); ++j) {
            double score = segmentSimilarity(source.segments[i], candidate.segments[j]);
            report.bestSection = std::max(report.bestSection, score);
            pairs.push_back({i, j, score});
        }
    }

    std::sort(pairs.begin(), pairs.end(), [](auto const& a, auto const& b) {
        return a.score > b.score;
    });

    std::vector<bool> sourceUsed(source.segments.size(), false);
    std::vector<bool> candidateUsed(candidate.segments.size(), false);
    double weightedScore = 0.0;
    std::size_t coveredObjects = 0;

    for (auto const& pair : pairs) {
        if (sourceUsed[pair.sourceIndex] || candidateUsed[pair.candidateIndex]) continue;
        sourceUsed[pair.sourceIndex] = true;
        candidateUsed[pair.candidateIndex] = true;

        auto weight = source.segments[pair.sourceIndex].objectCount;
        weightedScore += pair.score * static_cast<double>(weight);
        if (pair.score >= 0.75) coveredObjects += weight;
    }

    report.overall = weightedScore / static_cast<double>(source.objectCount);
    report.coverage = static_cast<double>(coveredObjects) / static_cast<double>(source.objectCount);

    // A whole-level clone ranks mostly by overall similarity. A copied subsection
    // can still surface through bestSection, but without pretending it is a full clone.
    double wholeLevelRank = 0.80 * report.overall + 0.20 * report.coverage;
    double partialRank = 0.55 * report.bestSection + 0.45 * report.coverage;
    report.rankScore = std::max(wholeLevelRank, partialRank);

    return report;
}

} // namespace gmdscan
