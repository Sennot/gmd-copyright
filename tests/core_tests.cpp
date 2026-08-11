#include "Similarity.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string makeLevel(float dx, float dy, int changedEvery = 0, int objectLimit = 120) {
    std::ostringstream out;
    out << "kA13,0,kA4,1;"; // settings/header-like record
    for (int i = 0; i < objectLimit; ++i) {
        int id = 1 + (i % 8);
        if (changedEvery > 0 && i % changedEvery == 0) id += 1000;
        float x = dx + static_cast<float>(i) * 12.f;
        float y = dy + static_cast<float>((i % 9) * 18);
        float rot = static_cast<float>((i % 4) * 90);
        out << "1," << id
            << ",2," << x
            << ",3," << y
            << ",6," << rot
            << ",32,1;";
    }
    return out.str();
}

bool closeTo(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}

} // namespace

int main() {
    using namespace gmdscan;

    auto source = buildFingerprint(makeLevel(0.f, 0.f));
    assert(source.objectCount == 120);
    assert(!source.segments.empty());

    // Pure translation should not change the structural fingerprint score.
    auto translated = buildFingerprint(makeLevel(5000.f, -700.f));
    auto exactReport = compare(source, translated);
    assert(closeTo(exactReport.overall, 1.0));
    assert(closeTo(exactReport.bestSection, 1.0));
    assert(closeTo(exactReport.coverage, 1.0));

    // Swapping some object IDs while keeping positions should stay meaningfully similar.
    auto changedIDs = buildFingerprint(makeLevel(200.f, 300.f, 3));
    auto changedReport = compare(source, changedIDs);
    assert(changedReport.rankScore > 0.65);
    assert(changedReport.bestSection > 0.65);

    // A shorter level that contains the same opening structure should have a strong
    // section match but lower whole-level coverage.
    auto partial = buildFingerprint(makeLevel(900.f, 100.f, 0, 35));
    auto partialReport = compare(source, partial);
    assert(partialReport.bestSection > 0.90);
    assert(partialReport.overall < exactReport.overall);
    assert(partialReport.coverage < exactReport.coverage);

    // Parser should ignore malformed/header records and require ID + X + Y.
    auto parsed = parseLevelData("kA13,0;1,1,2,30,3,60;1,2,2,10;garbage;");
    assert(parsed.size() == 1);
    assert(parsed[0].id == 1);

    std::cout << "core tests passed\n";
    std::cout << "exact rank=" << exactReport.rankScore
              << " changed rank=" << changedReport.rankScore
              << " partial rank=" << partialReport.rankScore << '\n';
    return 0;
}
