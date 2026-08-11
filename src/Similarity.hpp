#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gmdscan {

struct ParsedObject {
    int id = 0;
    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f;
    float scale = 1.f;
    bool flipX = false;
    bool flipY = false;
};

struct Fingerprint {
    struct Segment {
        int index = 0;
        std::size_t objectCount = 0;
        std::vector<ParsedObject> objects;
    };

    std::size_t objectCount = 0;
    float width = 0.f;
    std::vector<Segment> segments;
};

struct SimilarityReport {
    double overall = 0.0;
    double bestSection = 0.0;
    double coverage = 0.0;
    double rankScore = 0.0;
    std::size_t sourceObjects = 0;
    std::size_t candidateObjects = 0;
};

// Parses the decompressed Geometry Dash level string (the semicolon-separated
// object format stored inside GJGameLevel::m_levelString after decompression).
std::vector<ParsedObject> parseLevelData(std::string_view data);

Fingerprint buildFingerprint(std::vector<ParsedObject> objects, float segmentWidth = 300.f);
Fingerprint buildFingerprint(std::string_view decompressedLevelData, float segmentWidth = 300.f);
SimilarityReport compare(Fingerprint const& source, Fingerprint const& candidate);

} // namespace gmdscan
