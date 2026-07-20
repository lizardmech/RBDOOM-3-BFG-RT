#ifndef RB_PATH_TRACE_LIQUID_POOL_MODIFIER_HLSLI
#define RB_PATH_TRACE_LIQUID_POOL_MODIFIER_HLSLI

// Pure liquid-film reduction and effective-material collapse. Callers own all
// traversal, resource access, texture decoding, and receiver reconstruction.

static const float LIQUID_POOL_T_MIN = 1.0 / 1024.0;
static const float LIQUID_POOL_D_MAX = 8.0;
static const float LIQUID_POOL_DEFAULT_COAT_ROUGHNESS = 0.0;
static const float LIQUID_POOL_DEFAULT_DIELECTRIC_IOR = 1.5;
static const float LIQUID_POOL_DEFAULT_SINGLE_SCATTER_STRENGTH = 0.30;
static const float LIQUID_POOL_CLEARCOAT_FULL_COVERAGE = 0.35;
static const float LIQUID_POOL_RECEIVER_OFFSET_MIN = -0.05;
static const float LIQUID_POOL_RECEIVER_OFFSET_MAX = 1.85;
static const float LIQUID_POOL_RECEIVER_PLANE_DOT_MIN = 0.95;

struct LiquidPoolTransportCandidate
{
    uint materialIndex;
    float2 barycentrics;
    float hitT;
    uint instanceId;
    uint primitiveIndex;
    uint routeEvidence;
    uint valid;
};

// Five full words form the authoritative occurrence key. Barycentrics are
// finite, saturated, canonical positive floats stored without packing or loss.
struct LiquidPoolContributorKey
{
    uint instanceId;
    uint primitiveIndex;
    uint materialIndex;
    uint barycentricXBits;
    uint barycentricYBits;
};

struct LiquidPoolReceiverEvidence
{
    float3 cardPosition;
    float3 cardPlaneNormal;
    float3 receiverPosition;
    float3 receiverGeometryNormal;
    float3 rayDirection;
    uint domainAccepted;
    uint identityAccepted;
    uint receiverOpaque;
    uint receiverPathTransmission;
};

struct LiquidPoolReducerCandidate
{
    float coverage;
    float height;
    float3 decalRgb;
    float3 referenceTransmittance;
    float opticalDepthScale;
    float coatRoughness;
    float dielectricIor;
    float authoredNormalStrength;
    LiquidPoolContributorKey key;
    uint diagnosticHash;
    uint valid;
};

struct LiquidPoolResolvedFilm
{
    float coverage;
    float height;
    float3 decalRgb;
    float3 referenceTransmittance;
    float opticalDepthScale;
    float coatRoughness;
    float dielectricIor;
    float authoredNormalStrength;
    LiquidPoolContributorKey winnerKey;
    uint diagnosticHash;
    uint valid;
    uint overflowed;
};

struct LiquidPoolEffectiveReceiverMaterial
{
    float3 albedo;
    float3 transmittance;
    float3 specularF0;
    float roughness;
    uint applied;
};

bool LiquidPoolFinite(float value)
{
    return isfinite(value);
}

bool LiquidPoolFinite2(float2 value)
{
    return all(isfinite(value));
}

bool LiquidPoolFinite3(float3 value)
{
    return all(isfinite(value));
}

float LiquidPoolSanitizeParameter(float value, float defaultValue, float minimumValue, float maximumValue)
{
    return LiquidPoolFinite(value) ? clamp(value, minimumValue, maximumValue) : defaultValue;
}

float LiquidPoolCanonicalUnit(float value)
{
    const float result = saturate(value);
    return result == 0.0 ? 0.0 : result;
}

LiquidPoolContributorKey LiquidPoolMakeContributorKey(
    uint instanceId,
    uint primitiveIndex,
    uint materialIndex,
    float2 barycentrics)
{
    const float2 canonicalBarycentrics = saturate(barycentrics);
    LiquidPoolContributorKey key;
    key.instanceId = instanceId;
    key.primitiveIndex = primitiveIndex;
    key.materialIndex = materialIndex;
    key.barycentricXBits = asuint(canonicalBarycentrics.x == 0.0 ? 0.0 : canonicalBarycentrics.x);
    key.barycentricYBits = asuint(canonicalBarycentrics.y == 0.0 ? 0.0 : canonicalBarycentrics.y);
    return key;
}

bool LiquidPoolContributorKeyLess(LiquidPoolContributorKey lhs, LiquidPoolContributorKey rhs)
{
    if (lhs.instanceId != rhs.instanceId) return lhs.instanceId < rhs.instanceId;
    if (lhs.primitiveIndex != rhs.primitiveIndex) return lhs.primitiveIndex < rhs.primitiveIndex;
    if (lhs.materialIndex != rhs.materialIndex) return lhs.materialIndex < rhs.materialIndex;
    if (lhs.barycentricXBits != rhs.barycentricXBits) return lhs.barycentricXBits < rhs.barycentricXBits;
    return lhs.barycentricYBits < rhs.barycentricYBits;
}

bool LiquidPoolContributorKeyEqual(LiquidPoolContributorKey lhs, LiquidPoolContributorKey rhs)
{
    return lhs.instanceId == rhs.instanceId &&
        lhs.primitiveIndex == rhs.primitiveIndex &&
        lhs.materialIndex == rhs.materialIndex &&
        lhs.barycentricXBits == rhs.barycentricXBits &&
        lhs.barycentricYBits == rhs.barycentricYBits;
}

float3 LiquidPoolFaceForwardNormal(float3 normal, float3 rayDirection)
{
    const float3 normalized = normal * rsqrt(max(dot(normal, normal), 1.0e-20));
    return dot(normalized, rayDirection) > 0.0 ? -normalized : normalized;
}

bool LiquidPoolAcceptsReceiver(LiquidPoolReceiverEvidence evidence)
{
    if (evidence.domainAccepted == 0u || evidence.identityAccepted == 0u ||
        evidence.receiverOpaque == 0u || evidence.receiverPathTransmission != 0u ||
        !LiquidPoolFinite3(evidence.cardPosition) ||
        !LiquidPoolFinite3(evidence.cardPlaneNormal) ||
        !LiquidPoolFinite3(evidence.receiverPosition) ||
        !LiquidPoolFinite3(evidence.receiverGeometryNormal) ||
        !LiquidPoolFinite3(evidence.rayDirection))
    {
        return false;
    }

    const float cardNormalLengthSquared = dot(evidence.cardPlaneNormal, evidence.cardPlaneNormal);
    const float receiverNormalLengthSquared = dot(evidence.receiverGeometryNormal, evidence.receiverGeometryNormal);
    const float rayLengthSquared = dot(evidence.rayDirection, evidence.rayDirection);
    if (cardNormalLengthSquared <= 1.0e-20 || receiverNormalLengthSquared <= 1.0e-20 || rayLengthSquared <= 1.0e-20)
    {
        return false;
    }

    const float3 rayDirection = evidence.rayDirection * rsqrt(rayLengthSquared);
    const float3 cardNormal = LiquidPoolFaceForwardNormal(evidence.cardPlaneNormal, rayDirection);
    const float3 receiverNormal = LiquidPoolFaceForwardNormal(evidence.receiverGeometryNormal, rayDirection);
    const float signedOffset = dot(evidence.cardPosition - evidence.receiverPosition, receiverNormal);
    return signedOffset >= LIQUID_POOL_RECEIVER_OFFSET_MIN &&
        signedOffset <= LIQUID_POOL_RECEIVER_OFFSET_MAX &&
        dot(cardNormal, receiverNormal) >= LIQUID_POOL_RECEIVER_PLANE_DOT_MIN;
}

LiquidPoolReducerCandidate LiquidPoolCanonicalCandidate(LiquidPoolReducerCandidate candidate)
{
    if (candidate.valid == 0u ||
        !LiquidPoolFinite(candidate.coverage) ||
        !LiquidPoolFinite(candidate.height) ||
        !LiquidPoolFinite3(candidate.decalRgb))
    {
        candidate.valid = 0u;
        return candidate;
    }

    candidate.coverage = LiquidPoolCanonicalUnit(candidate.coverage);
    candidate.height = LiquidPoolCanonicalUnit(candidate.height);
    candidate.decalRgb = max(candidate.decalRgb, 0.0);
    candidate.referenceTransmittance.x = LiquidPoolSanitizeParameter(candidate.referenceTransmittance.x, 1.0, LIQUID_POOL_T_MIN, 1.0);
    candidate.referenceTransmittance.y = LiquidPoolSanitizeParameter(candidate.referenceTransmittance.y, 1.0, LIQUID_POOL_T_MIN, 1.0);
    candidate.referenceTransmittance.z = LiquidPoolSanitizeParameter(candidate.referenceTransmittance.z, 1.0, LIQUID_POOL_T_MIN, 1.0);
    candidate.opticalDepthScale = LiquidPoolSanitizeParameter(candidate.opticalDepthScale, 1.0, 0.0, LIQUID_POOL_D_MAX);
    candidate.coatRoughness = LiquidPoolSanitizeParameter(candidate.coatRoughness, LIQUID_POOL_DEFAULT_COAT_ROUGHNESS, 0.0, 1.0);
    candidate.dielectricIor = LiquidPoolSanitizeParameter(candidate.dielectricIor, LIQUID_POOL_DEFAULT_DIELECTRIC_IOR, 1.0, 2.5);
    candidate.authoredNormalStrength = LiquidPoolSanitizeParameter(candidate.authoredNormalStrength, 0.0, 0.0, 1.0);
    candidate.valid = candidate.coverage > 0.0 ? 1u : 0u;
    return candidate;
}

LiquidPoolResolvedFilm LiquidPoolResolvedFilmIdentity()
{
    LiquidPoolResolvedFilm result = (LiquidPoolResolvedFilm)0;
    result.referenceTransmittance = float3(1.0, 1.0, 1.0);
    result.opticalDepthScale = 1.0;
    result.coatRoughness = LIQUID_POOL_DEFAULT_COAT_ROUGHNESS;
    result.dielectricIor = LIQUID_POOL_DEFAULT_DIELECTRIC_IOR;
    return result;
}

bool LiquidPoolCandidatePayloadLess(LiquidPoolReducerCandidate lhs, LiquidPoolReducerCandidate rhs)
{
    const uint lhsWords[10] = {
        asuint(lhs.decalRgb.x), asuint(lhs.decalRgb.y), asuint(lhs.decalRgb.z),
        asuint(lhs.referenceTransmittance.x), asuint(lhs.referenceTransmittance.y), asuint(lhs.referenceTransmittance.z),
        asuint(lhs.opticalDepthScale), asuint(lhs.coatRoughness), asuint(lhs.dielectricIor), asuint(lhs.authoredNormalStrength)
    };
    const uint rhsWords[10] = {
        asuint(rhs.decalRgb.x), asuint(rhs.decalRgb.y), asuint(rhs.decalRgb.z),
        asuint(rhs.referenceTransmittance.x), asuint(rhs.referenceTransmittance.y), asuint(rhs.referenceTransmittance.z),
        asuint(rhs.opticalDepthScale), asuint(rhs.coatRoughness), asuint(rhs.dielectricIor), asuint(rhs.authoredNormalStrength)
    };
    [unroll]
    for (uint index = 0u; index < 10u; ++index)
    {
        if (lhsWords[index] != rhsWords[index])
        {
            return lhsWords[index] < rhsWords[index];
        }
    }
    return false;
}

bool LiquidPoolCandidateWins(LiquidPoolReducerCandidate candidate, LiquidPoolReducerCandidate incumbent)
{
    if (candidate.height != incumbent.height) return candidate.height > incumbent.height;
    if (candidate.coverage != incumbent.coverage) return candidate.coverage > incumbent.coverage;
    if (!LiquidPoolContributorKeyEqual(candidate.key, incumbent.key))
    {
        return LiquidPoolContributorKeyLess(candidate.key, incumbent.key);
    }
    // A complete occurrence key should imply identical contributor data. Keep
    // malformed collisions deterministic without consulting a lossy hash.
    return LiquidPoolCandidatePayloadLess(candidate, incumbent);
}

LiquidPoolReducerCandidate LiquidPoolResolvedWinnerCandidate(LiquidPoolResolvedFilm film)
{
    LiquidPoolReducerCandidate candidate;
    candidate.coverage = film.coverage;
    candidate.height = film.height;
    candidate.decalRgb = film.decalRgb;
    candidate.referenceTransmittance = film.referenceTransmittance;
    candidate.opticalDepthScale = film.opticalDepthScale;
    candidate.coatRoughness = film.coatRoughness;
    candidate.dielectricIor = film.dielectricIor;
    candidate.authoredNormalStrength = film.authoredNormalStrength;
    candidate.key = film.winnerKey;
    candidate.diagnosticHash = film.diagnosticHash;
    candidate.valid = film.valid;
    return candidate;
}

LiquidPoolResolvedFilm LiquidPoolReduceCandidate(LiquidPoolResolvedFilm film, LiquidPoolReducerCandidate inputCandidate)
{
    const LiquidPoolReducerCandidate candidate = LiquidPoolCanonicalCandidate(inputCandidate);
    if (candidate.valid == 0u)
    {
        return film;
    }

    if (film.valid == 0u)
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
        film.valid = 1u;
        return film;
    }

    const float maxCoverage = max(film.coverage, candidate.coverage);
    const float maxHeight = max(film.height, candidate.height);
    const LiquidPoolReducerCandidate incumbent = LiquidPoolResolvedWinnerCandidate(film);
    if (LiquidPoolCandidateWins(candidate, incumbent))
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
    film.valid = 1u;
    return film;
}

float LiquidPoolDielectricF0(float dielectricIor)
{
    const float ior = LiquidPoolSanitizeParameter(dielectricIor, LIQUID_POOL_DEFAULT_DIELECTRIC_IOR, 1.0, 2.5);
    const float ratio = (ior - 1.0) / max(ior + 1.0, 1.0e-6);
    return saturate(ratio * ratio);
}

LiquidPoolEffectiveReceiverMaterial LiquidPoolApplyResolvedFilm(
    float3 receiverAlbedo,
    float3 receiverSpecularF0,
    float receiverRoughness,
    LiquidPoolResolvedFilm film,
    uint alreadyApplied)
{
    LiquidPoolEffectiveReceiverMaterial result;
    result.albedo = receiverAlbedo;
    result.transmittance = float3(1.0, 1.0, 1.0);
    result.specularF0 = receiverSpecularF0;
    result.roughness = receiverRoughness;
    result.applied = 0u;
    if (alreadyApplied != 0u || film.valid == 0u || film.coverage <= 0.0)
    {
        return result;
    }

    const float coverage = LiquidPoolCanonicalUnit(film.coverage);
    const float height = LiquidPoolCanonicalUnit(film.height);
    const float3 referenceTransmittance = float3(
        LiquidPoolSanitizeParameter(film.referenceTransmittance.x, 1.0, LIQUID_POOL_T_MIN, 1.0),
        LiquidPoolSanitizeParameter(film.referenceTransmittance.y, 1.0, LIQUID_POOL_T_MIN, 1.0),
        LiquidPoolSanitizeParameter(film.referenceTransmittance.z, 1.0, LIQUID_POOL_T_MIN, 1.0));
    const float3 decalRgb = LiquidPoolFinite3(film.decalRgb) ? max(film.decalRgb, 0.0) : float3(1.0, 1.0, 1.0);
    const float3 transmittanceUnit = clamp(referenceTransmittance * decalRgb, LIQUID_POOL_T_MIN, 1.0);
    const float opticalDepth = clamp(
        LiquidPoolSanitizeParameter(film.opticalDepthScale, 1.0, 0.0, LIQUID_POOL_D_MAX) * height,
        0.0,
        LIQUID_POOL_D_MAX);
    const float3 transmittance = exp(log(transmittanceUnit) * opticalDepth);
    const float coatF0 = LiquidPoolDielectricF0(film.dielectricIor);
    // The legacy filter decal only attenuates its destination.  Reusing that
    // rule literally on a dim path-traced receiver collapses saturated blood
    // toward black.  A shallow pigmented liquid also returns a small amount of
    // selectively scattered light, so recover the least-absorbed hue and add a
    // bounded single-scatter term.  Neutral attenuation remains absorption-only.
    const float minimumTransmittance = min(transmittance.x, min(transmittance.y, transmittance.z));
    const float3 selectiveTransmittance = max(transmittance - minimumTransmittance, 0.0);
    const float selectiveMaximum = max(selectiveTransmittance.x, max(selectiveTransmittance.y, selectiveTransmittance.z));
    const float3 pigmentTint = selectiveMaximum > 1.0e-5
        ? selectiveTransmittance / selectiveMaximum
        : float3(0.0, 0.0, 0.0);
    const float singleScatterAmount =
        (1.0 - minimumTransmittance) * LIQUID_POOL_DEFAULT_SINGLE_SCATTER_STRENGTH;
    const float3 coatedAlbedo = saturate(receiverAlbedo * transmittance + pigmentTint * singleScatterAmount);
    const float3 coatedF0 = saturate(coatF0 + ((1.0 - coatF0) * (1.0 - coatF0)) * receiverSpecularF0);
    const float coatRoughness = LiquidPoolSanitizeParameter(
        film.coatRoughness,
        LIQUID_POOL_DEFAULT_COAT_ROUGHNESS,
        0.0,
        1.0);
    // Pigment opacity and liquid-interface presence are not the same signal.
    // Keep the authored footprint for albedo, but let the clear interface reach
    // full strength through most of the visible pool.  smoothstep preserves a
    // soft zero-slope transition at the transparent card boundary.
    const float clearcoatCoverage = smoothstep(
        0.0,
        LIQUID_POOL_CLEARCOAT_FULL_COVERAGE,
        coverage);

    result.albedo = lerp(receiverAlbedo, coatedAlbedo, coverage);
    result.transmittance = lerp(float3(1.0, 1.0, 1.0), transmittance, coverage);
    result.specularF0 = lerp(receiverSpecularF0, coatedF0, clearcoatCoverage);
    result.roughness = lerp(receiverRoughness, coatRoughness, clearcoatCoverage);
    result.applied = 1u;
    return result;
}

#endif // RB_PATH_TRACE_LIQUID_POOL_MODIFIER_HLSLI
