#pragma once

// A8-S1 identity-journal transition helpers used by production and the
// lifecycle harness. These helpers change diagnostic transport bookkeeping
// only; they never choose a renderer key or product.

#include <cstddef>
#include <cstdint>

constexpr std::size_t PT_A8_S1_IDENTITY_JOURNAL_COMPACT_THRESHOLD = 16384;

template <typename Journal>
inline void PtA8S1ResetIdentityPublication(
    Journal& journal,
    std::size_t& publishedRecordCount,
    std::uint64_t& publicationGeneration,
    std::uint64_t& publicationSequence)
{
    journal.clear();
    publishedRecordCount = 0;
    ++publicationGeneration;
    if (publicationGeneration == 0)
    {
        publicationGeneration = 1;
    }
    publicationSequence = 0;
}

template <typename Record, typename AppendFn>
inline std::uint64_t PtA8S1AppendUpsertAndStore(
    Record& record,
    AppendFn append)
{
    const std::uint64_t sequence = append(record);
    record.lastUpsertSequence = sequence;
    return sequence;
}

template <
    typename Journal,
    typename SurvivorRange,
    typename IsValidFn,
    typename AppendFn>
inline bool PtA8S1MaybeCompactIdentityJournal(
    Journal& journal,
    std::size_t& publishedRecordCount,
    std::uint64_t& publicationGeneration,
    std::uint64_t& publicationSequence,
    SurvivorRange& survivors,
    IsValidFn isValid,
    AppendFn append,
    bool rewriteSurvivorSequences = true)
{
    if (journal.size() < PT_A8_S1_IDENTITY_JOURNAL_COMPACT_THRESHOLD ||
        publishedRecordCount != journal.size())
    {
        return false;
    }

    PtA8S1ResetIdentityPublication(
        journal,
        publishedRecordCount,
        publicationGeneration,
        publicationSequence);
    for (auto& survivor : survivors)
    {
        if (!isValid(survivor))
        {
            continue;
        }
        const std::uint64_t replaySequence = append(survivor);
        if (rewriteSurvivorSequences)
        {
            survivor.lastUpsertSequence = replaySequence;
        }
    }
    return true;
}

