import upt04_hit_contract;

#ifndef UPT04_CONTINUATION_HIT_WRITE
#define UPT04_CONTINUATION_HIT_WRITE 0
#endif

#if UPT04_CONTINUATION_HIT_WRITE
public [[vk::binding(24, 0)]] RWStructuredBuffer<Upt04HitFacts>
    gUpt04ContinuationHits;
#else
public [[vk::binding(24, 0)]] StructuredBuffer<Upt04HitFacts>
    gUpt04ContinuationHits;
#endif
