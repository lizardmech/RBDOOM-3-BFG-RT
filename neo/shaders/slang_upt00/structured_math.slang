import upt00_contracts;

[[vk::binding(0, 0)]] StructuredBuffer<Upt00StructuredMathInput> gInput;
[[vk::binding(1, 0)]] RWStructuredBuffer<Upt00StructuredMathOutput> gOutput;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    const Upt00StructuredMathInput input = gInput[index];

    Upt00StructuredMathOutput output;
    output.words.x = Upt00Hash(input.words.x ^ index);
    output.words.y = Upt00Hash(input.words.y + input.words.x);
    output.words.z = input.words.z ^ 0x9e3779b9u;
    output.words.w = input.words.w + index * 17u;
    output.values = input.values * 2.0f + float4(1.0f, -2.0f, 0.5f, 4.0f);
    gOutput[index] = output;
}
