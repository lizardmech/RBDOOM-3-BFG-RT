import upt04_hit_contract;

[shader("miss")]
void main(inout Upt04HitFacts payload)
{
    payload.status = kUpt04HitMiss;
}
