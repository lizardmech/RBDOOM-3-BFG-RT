#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace {

constexpr float kTMin = 1.0f / 1024.0f;
constexpr float kDMax = 8.0f;
constexpr float kDefaultRoughness = 0.18f;
constexpr float kDefaultIor = 1.5f;
constexpr float kOffsetMin = -0.05f;
constexpr float kOffsetMax = 1.85f;
constexpr float kPlaneDotMin = 0.95f;

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Key
{
    uint32_t instanceId = 0;
    uint32_t primitiveIndex = 0;
    uint32_t materialIndex = 0;
    uint32_t barycentricXBits = 0;
    uint32_t barycentricYBits = 0;
};

struct Candidate
{
    float coverage = 0.0f;
    float height = 1.0f;
    Vec3 decalRgb = { 1.0f, 1.0f, 1.0f };
    Vec3 referenceTransmittance = { 1.0f, 1.0f, 1.0f };
    float opticalDepthScale = 1.0f;
    float coatRoughness = kDefaultRoughness;
    float dielectricIor = kDefaultIor;
    float authoredNormalStrength = 0.0f;
    Key key;
    uint32_t diagnosticHash = 0;
    bool valid = false;
};

struct Film
{
    float coverage = 0.0f;
    float height = 0.0f;
    Vec3 decalRgb = {};
    Vec3 referenceTransmittance = { 1.0f, 1.0f, 1.0f };
    float opticalDepthScale = 1.0f;
    float coatRoughness = kDefaultRoughness;
    float dielectricIor = kDefaultIor;
    float authoredNormalStrength = 0.0f;
    Key winnerKey;
    uint32_t diagnosticHash = 0;
    bool valid = false;
    bool overflowed = false;
};

struct EffectiveMaterial
{
    Vec3 albedo;
    Vec3 transmittance = { 1.0f, 1.0f, 1.0f };
    Vec3 specularF0;
    float roughness = 0.0f;
    bool applied = false;
};

struct ReceiverEvidence
{
    Vec3 cardPosition;
    Vec3 cardPlaneNormal;
    Vec3 receiverPosition;
    Vec3 receiverGeometryNormal;
    Vec3 rayDirection;
    bool domainAccepted = false;
    bool identityAccepted = false;
    bool receiverOpaque = false;
    bool receiverPathTransmission = false;
};

int g_failures = 0;

void Check(bool condition, const char* name)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << "\n";
    if (!condition)
    {
        ++g_failures;
    }
}

uint32_t FloatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float Clamp(float value, float minimumValue, float maximumValue)
{
    return std::min(std::max(value, minimumValue), maximumValue);
}

float Sanitize(float value, float defaultValue, float minimumValue, float maximumValue)
{
    return std::isfinite(value) ? Clamp(value, minimumValue, maximumValue) : defaultValue;
}

float CanonicalUnit(float value)
{
    const float result = Clamp(value, 0.0f, 1.0f);
    return result == 0.0f ? 0.0f : result;
}

Vec3 Add(Vec3 lhs, Vec3 rhs) { return { lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z }; }
Vec3 Sub(Vec3 lhs, Vec3 rhs) { return { lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z }; }
Vec3 Mul(Vec3 lhs, Vec3 rhs) { return { lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z }; }
Vec3 Mul(Vec3 value, float scalar) { return { value.x * scalar, value.y * scalar, value.z * scalar }; }
float Dot(Vec3 lhs, Vec3 rhs) { return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z; }
bool Finite(Vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }

Vec3 Normalize(Vec3 value)
{
    const float lengthSquared = Dot(value, value);
    return lengthSquared > 1.0e-20f ? Mul(value, 1.0f / std::sqrt(lengthSquared)) : Vec3{};
}

Vec3 FaceForward(Vec3 normal, Vec3 rayDirection)
{
    Vec3 result = Normalize(normal);
    return Dot(result, rayDirection) > 0.0f ? Mul(result, -1.0f) : result;
}

Vec3 Lerp(Vec3 lhs, Vec3 rhs, float amount)
{
    return Add(Mul(lhs, 1.0f - amount), Mul(rhs, amount));
}

bool Near(float lhs, float rhs, float tolerance = 1.0e-6f)
{
    return std::fabs(lhs - rhs) <= tolerance;
}

bool Near(Vec3 lhs, Vec3 rhs, float tolerance = 1.0e-6f)
{
    return Near(lhs.x, rhs.x, tolerance) && Near(lhs.y, rhs.y, tolerance) && Near(lhs.z, rhs.z, tolerance);
}

Key MakeKey(uint32_t instanceId, uint32_t primitiveIndex, uint32_t materialIndex, float barycentricX, float barycentricY)
{
    const float x = CanonicalUnit(barycentricX);
    const float y = CanonicalUnit(barycentricY);
    return { instanceId, primitiveIndex, materialIndex, FloatBits(x), FloatBits(y) };
}

auto KeyTuple(const Key& key)
{
    return std::tie(key.instanceId, key.primitiveIndex, key.materialIndex, key.barycentricXBits, key.barycentricYBits);
}

bool KeyEqual(const Key& lhs, const Key& rhs)
{
    return KeyTuple(lhs) == KeyTuple(rhs);
}

bool KeyLess(const Key& lhs, const Key& rhs)
{
    return KeyTuple(lhs) < KeyTuple(rhs);
}

std::array<uint32_t, 10> PayloadWords(const Candidate& candidate)
{
    return {
        FloatBits(candidate.decalRgb.x), FloatBits(candidate.decalRgb.y), FloatBits(candidate.decalRgb.z),
        FloatBits(candidate.referenceTransmittance.x), FloatBits(candidate.referenceTransmittance.y), FloatBits(candidate.referenceTransmittance.z),
        FloatBits(candidate.opticalDepthScale), FloatBits(candidate.coatRoughness), FloatBits(candidate.dielectricIor), FloatBits(candidate.authoredNormalStrength)
    };
}

Candidate CanonicalCandidate(Candidate candidate)
{
    if (!candidate.valid || !std::isfinite(candidate.coverage) || !std::isfinite(candidate.height) || !Finite(candidate.decalRgb))
    {
        candidate.valid = false;
        return candidate;
    }
    candidate.coverage = CanonicalUnit(candidate.coverage);
    candidate.height = CanonicalUnit(candidate.height);
    candidate.decalRgb = { std::max(candidate.decalRgb.x, 0.0f), std::max(candidate.decalRgb.y, 0.0f), std::max(candidate.decalRgb.z, 0.0f) };
    candidate.referenceTransmittance = {
        Sanitize(candidate.referenceTransmittance.x, 1.0f, kTMin, 1.0f),
        Sanitize(candidate.referenceTransmittance.y, 1.0f, kTMin, 1.0f),
        Sanitize(candidate.referenceTransmittance.z, 1.0f, kTMin, 1.0f)
    };
    candidate.opticalDepthScale = Sanitize(candidate.opticalDepthScale, 1.0f, 0.0f, kDMax);
    candidate.coatRoughness = Sanitize(candidate.coatRoughness, kDefaultRoughness, 0.02f, 1.0f);
    candidate.dielectricIor = Sanitize(candidate.dielectricIor, kDefaultIor, 1.0f, 2.5f);
    candidate.authoredNormalStrength = Sanitize(candidate.authoredNormalStrength, 0.0f, 0.0f, 1.0f);
    candidate.valid = candidate.coverage > 0.0f;
    return candidate;
}

Candidate WinnerCandidate(const Film& film)
{
    Candidate result;
    result.coverage = film.coverage;
    result.height = film.height;
    result.decalRgb = film.decalRgb;
    result.referenceTransmittance = film.referenceTransmittance;
    result.opticalDepthScale = film.opticalDepthScale;
    result.coatRoughness = film.coatRoughness;
    result.dielectricIor = film.dielectricIor;
    result.authoredNormalStrength = film.authoredNormalStrength;
    result.key = film.winnerKey;
    result.diagnosticHash = film.diagnosticHash;
    result.valid = film.valid;
    return result;
}

bool CandidateWins(const Candidate& candidate, const Candidate& incumbent)
{
    if (candidate.height != incumbent.height) return candidate.height > incumbent.height;
    if (candidate.coverage != incumbent.coverage) return candidate.coverage > incumbent.coverage;
    if (!KeyEqual(candidate.key, incumbent.key)) return KeyLess(candidate.key, incumbent.key);
    return PayloadWords(candidate) < PayloadWords(incumbent);
}

Film Reduce(Film film, Candidate input)
{
    const Candidate candidate = CanonicalCandidate(input);
    if (!candidate.valid)
    {
        return film;
    }
    if (!film.valid)
    {
        film.coverage = candidate.coverage;
        film.height = candidate.height;
        film.decalRgb = candidate.decalRgb;
        film.referenceTransmittance = candidate.referenceTransmittance;
        film.opticalDepthScale = candidate.opticalDepthScale;
        film.coatRoughness = candidate.coatRoughness;
        film.dielectricIor = candidate.dielectricIor;
        film.authoredNormalStrength = candidate.authoredNormalStrength;
        film.winnerKey = candidate.key;
        film.diagnosticHash = candidate.diagnosticHash;
        film.valid = true;
        return film;
    }
    const float maxCoverage = std::max(film.coverage, candidate.coverage);
    const float maxHeight = std::max(film.height, candidate.height);
    if (CandidateWins(candidate, WinnerCandidate(film)))
    {
        film.decalRgb = candidate.decalRgb;
        film.referenceTransmittance = candidate.referenceTransmittance;
        film.opticalDepthScale = candidate.opticalDepthScale;
        film.coatRoughness = candidate.coatRoughness;
        film.dielectricIor = candidate.dielectricIor;
        film.authoredNormalStrength = candidate.authoredNormalStrength;
        film.winnerKey = candidate.key;
        film.diagnosticHash = candidate.diagnosticHash;
    }
    film.coverage = maxCoverage;
    film.height = maxHeight;
    film.valid = true;
    return film;
}

Film Fold(const std::vector<Candidate>& candidates)
{
    Film film;
    for (const Candidate& candidate : candidates)
    {
        film = Reduce(film, candidate);
    }
    return film;
}

bool FilmEqual(const Film& lhs, const Film& rhs)
{
    return FloatBits(lhs.coverage) == FloatBits(rhs.coverage) &&
        FloatBits(lhs.height) == FloatBits(rhs.height) &&
        Near(lhs.decalRgb, rhs.decalRgb, 0.0f) &&
        Near(lhs.referenceTransmittance, rhs.referenceTransmittance, 0.0f) &&
        FloatBits(lhs.opticalDepthScale) == FloatBits(rhs.opticalDepthScale) &&
        FloatBits(lhs.coatRoughness) == FloatBits(rhs.coatRoughness) &&
        FloatBits(lhs.dielectricIor) == FloatBits(rhs.dielectricIor) &&
        FloatBits(lhs.authoredNormalStrength) == FloatBits(rhs.authoredNormalStrength) &&
        KeyEqual(lhs.winnerKey, rhs.winnerKey) && lhs.valid == rhs.valid && lhs.overflowed == rhs.overflowed;
}

float DielectricF0(float dielectricIor)
{
    const float ior = Sanitize(dielectricIor, kDefaultIor, 1.0f, 2.5f);
    const float ratio = (ior - 1.0f) / std::max(ior + 1.0f, 1.0e-6f);
    return Clamp(ratio * ratio, 0.0f, 1.0f);
}

EffectiveMaterial Apply(Vec3 receiverAlbedo, Vec3 receiverF0, float receiverRoughness, const Film& film, bool alreadyApplied)
{
    EffectiveMaterial result{ receiverAlbedo, { 1.0f, 1.0f, 1.0f }, receiverF0, receiverRoughness, false };
    if (alreadyApplied || !film.valid || film.coverage <= 0.0f)
    {
        return result;
    }
    const float coverage = CanonicalUnit(film.coverage);
    const float height = CanonicalUnit(film.height);
    const Vec3 reference = {
        Sanitize(film.referenceTransmittance.x, 1.0f, kTMin, 1.0f),
        Sanitize(film.referenceTransmittance.y, 1.0f, kTMin, 1.0f),
        Sanitize(film.referenceTransmittance.z, 1.0f, kTMin, 1.0f)
    };
    const Vec3 decal = Finite(film.decalRgb)
        ? Vec3{ std::max(film.decalRgb.x, 0.0f), std::max(film.decalRgb.y, 0.0f), std::max(film.decalRgb.z, 0.0f) }
        : Vec3{ 1.0f, 1.0f, 1.0f };
    const Vec3 unit = {
        Clamp(reference.x * decal.x, kTMin, 1.0f),
        Clamp(reference.y * decal.y, kTMin, 1.0f),
        Clamp(reference.z * decal.z, kTMin, 1.0f)
    };
    const float depth = Clamp(Sanitize(film.opticalDepthScale, 1.0f, 0.0f, kDMax) * height, 0.0f, kDMax);
    const Vec3 transmittance = { std::exp(std::log(unit.x) * depth), std::exp(std::log(unit.y) * depth), std::exp(std::log(unit.z) * depth) };
    const float coatF0 = DielectricF0(film.dielectricIor);
    const Vec3 coatedAlbedo = Mul(receiverAlbedo, transmittance);
    const Vec3 coatedF0 = {
        Clamp(coatF0 + (1.0f - coatF0) * (1.0f - coatF0) * receiverF0.x, 0.0f, 1.0f),
        Clamp(coatF0 + (1.0f - coatF0) * (1.0f - coatF0) * receiverF0.y, 0.0f, 1.0f),
        Clamp(coatF0 + (1.0f - coatF0) * (1.0f - coatF0) * receiverF0.z, 0.0f, 1.0f)
    };
    const float coatRoughness = Sanitize(film.coatRoughness, kDefaultRoughness, 0.02f, 1.0f);
    result.albedo = Lerp(receiverAlbedo, coatedAlbedo, coverage);
    result.transmittance = Lerp({ 1.0f, 1.0f, 1.0f }, transmittance, coverage);
    result.specularF0 = Lerp(receiverF0, coatedF0, coverage);
    result.roughness = receiverRoughness * (1.0f - coverage) + coatRoughness * coverage;
    result.applied = true;
    return result;
}

bool AcceptsReceiver(const ReceiverEvidence& evidence)
{
    if (!evidence.domainAccepted || !evidence.identityAccepted || !evidence.receiverOpaque || evidence.receiverPathTransmission ||
        !Finite(evidence.cardPosition) || !Finite(evidence.cardPlaneNormal) || !Finite(evidence.receiverPosition) ||
        !Finite(evidence.receiverGeometryNormal) || !Finite(evidence.rayDirection) ||
        Dot(evidence.cardPlaneNormal, evidence.cardPlaneNormal) <= 1.0e-20f ||
        Dot(evidence.receiverGeometryNormal, evidence.receiverGeometryNormal) <= 1.0e-20f ||
        Dot(evidence.rayDirection, evidence.rayDirection) <= 1.0e-20f)
    {
        return false;
    }
    const Vec3 ray = Normalize(evidence.rayDirection);
    const Vec3 cardNormal = FaceForward(evidence.cardPlaneNormal, ray);
    const Vec3 receiverNormal = FaceForward(evidence.receiverGeometryNormal, ray);
    const float signedOffset = Dot(Sub(evidence.cardPosition, evidence.receiverPosition), receiverNormal);
    return signedOffset >= kOffsetMin && signedOffset <= kOffsetMax && Dot(cardNormal, receiverNormal) >= kPlaneDotMin;
}

Candidate MakeCandidate(float coverage, Vec3 color, Key key, float height = 1.0f)
{
    Candidate candidate;
    candidate.coverage = coverage;
    candidate.height = height;
    candidate.decalRgb = color;
    candidate.key = key;
    candidate.valid = true;
    return candidate;
}

void PrintVector(const char* name, Vec3 value)
{
    std::cout << std::fixed << std::setprecision(6) << name << "=(" << value.x << "," << value.y << "," << value.z << ")\n";
}

void TestReceiverPredicate()
{
    ReceiverEvidence evidence;
    evidence.cardPosition = { 0.0f, 0.0f, 0.15f };
    evidence.cardPlaneNormal = { 0.0f, 0.0f, 1.0f };
    evidence.receiverPosition = { 0.0f, 0.0f, 0.0f };
    evidence.receiverGeometryNormal = { 0.0f, 0.0f, 1.0f };
    evidence.rayDirection = { 0.0f, 0.0f, -1.0f };
    evidence.domainAccepted = evidence.identityAccepted = evidence.receiverOpaque = true;
    Check(AcceptsReceiver(evidence), "receiver predicate accepts aligned opaque receiver");
    evidence.cardPosition.z = kOffsetMin;
    Check(AcceptsReceiver(evidence), "receiver predicate includes minimum offset boundary");
    evidence.cardPosition.z = kOffsetMax;
    Check(AcceptsReceiver(evidence), "receiver predicate includes maximum offset boundary");
    evidence.cardPosition.z = kOffsetMax + 0.001f;
    Check(!AcceptsReceiver(evidence), "receiver predicate rejects out-of-envelope card");
    evidence.cardPosition.z = 0.15f;
    evidence.cardPlaneNormal = { 1.0f, 0.0f, 0.0f };
    Check(!AcceptsReceiver(evidence), "receiver predicate rejects perpendicular plane");
    evidence.cardPlaneNormal = { 0.0f, 0.0f, 1.0f };
    evidence.receiverPathTransmission = true;
    Check(!AcceptsReceiver(evidence), "receiver predicate rejects PATH_TRANSMISSION glass");
    evidence.receiverPathTransmission = false;
    evidence.identityAccepted = false;
    Check(!AcceptsReceiver(evidence), "receiver predicate rejects unaccepted identity evidence");
}

void TestReductionAlgebra()
{
    const Candidate a = MakeCandidate(0.6f, { 0.5f, 0.25f, 1.0f }, MakeKey(2, 3, 4, 0.25f, 0.5f));
    const Candidate b = MakeCandidate(0.8f, { 0.8f, 0.2f, 0.1f }, MakeKey(3, 2, 4, 0.5f, 0.25f));
    const Candidate c = MakeCandidate(0.7f, { 0.1f, 0.9f, 0.2f }, MakeKey(1, 8, 2, 0.1f, 0.2f));
    Check(FilmEqual(Reduce(Film{}, a), Fold({ a })), "identity + A = A");
    Check(FilmEqual(Fold({ a, a }), Fold({ a })), "A + A = A");
    Check(FilmEqual(Fold({ a, b }), Fold({ b, a })), "reduce(A,B) = reduce(B,A)");
    const Film left = Reduce(Fold({ a, b }), c);
    const Film right = Reduce(Fold({ a }), WinnerCandidate(Fold({ b, c })));
    Check(FilmEqual(left, right), "grouping does not change resolved result");
    for (int count : { 1, 2, 4, 32 })
    {
        Check(FilmEqual(Fold(std::vector<Candidate>(count, a)), Fold({ a })), ("identical candidate count " + std::to_string(count)).c_str());
    }
    const Film partial = Fold({ MakeCandidate(0.4f, { 1, 0, 0 }, MakeKey(4, 1, 1, 0.1f, 0.1f)), MakeCandidate(0.7f, { 0, 1, 0 }, MakeKey(5, 1, 1, 0.2f, 0.2f)) });
    Check(Near(partial.coverage, 0.7f), "partial overlap uses max coverage");
    Check(!Near(partial.coverage, 0.82f), "partial overlap is not alpha-over");

    Candidate tieA = MakeCandidate(0.75f, { 1, 0, 0 }, MakeKey(9, 4, 3, 0.25f, 0.25f));
    Candidate tieB = MakeCandidate(0.75f, { 0, 1, 0 }, MakeKey(2, 8, 7, 0.25f, 0.25f));
    tieA.diagnosticHash = tieB.diagnosticHash = 0x12345678u;
    const Film tieForward = Fold({ tieA, tieB });
    const Film tieReverse = Fold({ tieB, tieA });
    Check(FilmEqual(tieForward, tieReverse), "stable key winner independent of traversal order");
    Check(KeyEqual(tieForward.winnerKey, tieB.key), "smallest identity tuple wins diagnostic-hash collision");
    Check(Near(tieForward.decalRgb, tieB.decalRgb), "selected tint is one whole source tint");

    Candidate occurrenceA = MakeCandidate(0.5f, { 1, 0, 0 }, MakeKey(1, 2, 3, 0.25f, 0.5f));
    Candidate occurrenceB = MakeCandidate(0.5f, { 0, 0, 1 }, MakeKey(1, 2, 3, 0.5f, 0.5f));
    const Film occurrence = Fold({ occurrenceB, occurrenceA });
    Check(KeyEqual(occurrence.winnerKey, occurrenceA.key), "full barycentric occurrence key resolves same uint3 collision");

    Candidate malformedA = occurrenceA;
    Candidate malformedB = occurrenceA;
    malformedB.decalRgb = { 0, 1, 0 };
    Check(FilmEqual(Fold({ malformedA, malformedB }), Fold({ malformedB, malformedA })), "same-key malformed payload collision remains deterministic");
}

void TestOpticalGoldens()
{
    Check(Near(DielectricF0(1.0f), 0.0f), "IOR 1 gives F0 0");
    Check(Near(DielectricF0(1.5f), 0.04f), "default IOR 1.5 gives F0 0.04");

    Candidate neutral = MakeCandidate(1.0f, { 1, 1, 1 }, MakeKey(1, 1, 1, 0.2f, 0.3f));
    neutral.referenceTransmittance = { 1, 1, 1 };
    for (float depth : { 0.0f, 1.0f, 8.0f })
    {
        neutral.height = 1.0f;
        neutral.opticalDepthScale = depth;
        Check(Near(Apply({ 1, 1, 1 }, { 0.04f, 0.04f, 0.04f }, 0.6f, Fold({ neutral }), false).transmittance, { 1, 1, 1 }), "neutral Tref remains neutral");
    }

    Candidate optical = MakeCandidate(1.0f, { 1, 1, 1 }, MakeKey(1, 1, 2, 0.2f, 0.3f));
    optical.referenceTransmittance = { 0.5f, 0.25f, 1.0f };
    optical.opticalDepthScale = 0.0f;
    Check(Near(Apply({ 1, 1, 1 }, { 0, 0, 0 }, 0.6f, Fold({ optical }), false).transmittance, { 1, 1, 1 }), "d=0 gives unit transmittance");
    optical.opticalDepthScale = 1.0f;
    Check(Near(Apply({ 1, 1, 1 }, { 0, 0, 0 }, 0.6f, Fold({ optical }), false).transmittance, { 0.5f, 0.25f, 1.0f }), "d=1 gives Tref");
    optical.opticalDepthScale = 2.0f;
    Check(Near(Apply({ 1, 1, 1 }, { 0, 0, 0 }, 0.6f, Fold({ optical }), false).transmittance, { 0.25f, 0.0625f, 1.0f }), "d=2 gives squared Tref");

    optical.opticalDepthScale = 1.0f;
    optical.coatRoughness = 0.12f;
    optical.dielectricIor = 1.5f;
    const EffectiveMaterial full = Apply({ 0.8f, 0.6f, 0.4f }, { 0.04f, 0.04f, 0.04f }, 0.6f, Fold({ optical }), false);
    PrintVector("golden.full.albedo", full.albedo);
    PrintVector("golden.full.f0", full.specularF0);
    Check(Near(full.albedo, { 0.4f, 0.15f, 0.4f }), "coverage 1 effective albedo golden");
    Check(Near(full.specularF0, { 0.076864f, 0.076864f, 0.076864f }), "coverage 1 effective F0 golden");
    Check(Near(full.roughness, 0.12f), "coverage 1 roughness golden");

    optical.coverage = 0.5f;
    const EffectiveMaterial half = Apply({ 0.8f, 0.6f, 0.4f }, { 0.04f, 0.04f, 0.04f }, 0.6f, Fold({ optical }), false);
    PrintVector("golden.half.albedo", half.albedo);
    PrintVector("golden.half.f0", half.specularF0);
    Check(Near(half.albedo, { 0.6f, 0.375f, 0.4f }), "coverage 0.5 effective albedo golden");
    Check(Near(half.specularF0, { 0.058432f, 0.058432f, 0.058432f }), "coverage 0.5 effective F0 golden");
    Check(Near(half.roughness, 0.36f), "coverage 0.5 roughness golden");
    Check(Near(kDefaultRoughness, 0.18f), "default coat roughness exactly 0.18");
}

void TestInvalidAndApplyOnce()
{
    Candidate invalid = MakeCandidate(0.5f, { 1, 1, 1 }, MakeKey(1, 1, 1, 0, 0));
    invalid.valid = false;
    Check(!Fold({ invalid }).valid, "explicit invalid candidate is identity");
    invalid.valid = true;
    invalid.coverage = 0.0f;
    Check(!Fold({ invalid }).valid, "zero alpha is identity");
    invalid.coverage = std::numeric_limits<float>::quiet_NaN();
    Check(!Fold({ invalid }).valid, "NaN coverage is identity");
    invalid.coverage = 0.5f;
    invalid.height = std::numeric_limits<float>::infinity();
    Check(!Fold({ invalid }).valid, "infinite height is identity");

    Candidate clamped = MakeCandidate(2.0f, { 1, 1, 1 }, MakeKey(1, 1, 2, 0, 0), -1.0f);
    const Film clampedFilm = Fold({ clamped });
    Check(Near(clampedFilm.coverage, 1.0f) && Near(clampedFilm.height, 0.0f), "finite out-of-range coverage/height clamp");

    Candidate params = MakeCandidate(1.0f, { 1, 1, 1 }, MakeKey(1, 1, 3, 0, 0));
    params.referenceTransmittance = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f };
    params.opticalDepthScale = std::numeric_limits<float>::quiet_NaN();
    params.coatRoughness = std::numeric_limits<float>::infinity();
    params.dielectricIor = -1.0f;
    params.authoredNormalStrength = std::numeric_limits<float>::quiet_NaN();
    const Film sanitized = Fold({ params });
    Check(Near(sanitized.referenceTransmittance, { 1.0f, 1.0f, kTMin }), "nonfinite/negative Tref uses defaults and clamps");
    Check(Near(sanitized.opticalDepthScale, 1.0f) && Near(sanitized.coatRoughness, 0.18f) && Near(sanitized.dielectricIor, 1.0f), "nonfinite scalar parameters sanitize deterministically");

    Candidate darkest = MakeCandidate(1.0f, { kTMin, kTMin, kTMin }, MakeKey(1, 1, 4, 0, 0));
    darkest.referenceTransmittance = { kTMin, kTMin, kTMin };
    darkest.opticalDepthScale = kDMax;
    const EffectiveMaterial dark = Apply({ 0.8f, 0.6f, 0.4f }, { 0.04f, 0.04f, 0.04f }, 0.6f, Fold({ darkest }), false);
    Check(dark.applied && Finite(dark.albedo) && Finite(dark.specularF0) && std::isfinite(dark.roughness), "darkest allowed T/d remains finite and applied");

    Candidate onceCandidate = MakeCandidate(1.0f, { 0.5f, 0.5f, 0.5f }, MakeKey(2, 2, 2, 0.2f, 0.2f));
    const Film onceFilm = Fold({ onceCandidate });
    const EffectiveMaterial once = Apply({ 0.8f, 0.6f, 0.4f }, { 0.04f, 0.04f, 0.04f }, 0.6f, onceFilm, false);
    const EffectiveMaterial blocked = Apply(once.albedo, once.specularF0, once.roughness, onceFilm, true);
    Check(once.applied && !blocked.applied && Near(blocked.albedo, once.albedo) && Near(blocked.specularF0, once.specularF0) && Near(blocked.roughness, once.roughness), "applied-state guard prevents double application");

    struct ReceiverOwnedState { uint32_t materialId; uint32_t materialIndex; uint32_t instanceId; uint32_t primitiveIndex; Vec3 worldPosition; Vec3 geometryNormal; };
    const ReceiverOwnedState before{ 11, 12, 13, 14, { 1, 2, 3 }, { 0, 0, 1 } };
    ReceiverOwnedState after = before;
    (void)Apply({ 0.8f, 0.6f, 0.4f }, { 0.04f, 0.04f, 0.04f }, 0.6f, onceFilm, false);
    Check(std::memcmp(&before, &after, sizeof(before)) == 0, "application leaves receiver-owned fields bit-identical");
}

} // namespace

int main()
{
    TestReceiverPredicate();
    TestReductionAlgebra();
    TestOpticalGoldens();
    TestInvalidAndApplyOnce();
    if (g_failures != 0)
    {
        std::cout << "PathTrace liquid-pool reducer harness failed: " << g_failures << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "PathTrace liquid-pool reducer harness passed\n";
    return EXIT_SUCCESS;
}
