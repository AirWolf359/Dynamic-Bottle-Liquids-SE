// Ordered subpixel coverage for passes that sample the native scene depth.
cbuffer Coverage : register(b10) { float opacity; };
void main(float4 position : SV_Position) {
    uint2 p = uint2(position.xy);
    uint rank = 0;
    [unroll] for (uint bit = 0; bit < 4; ++bit) {
        uint2 b = (p >> bit) & 1;
        rank = rank * 4 + ((b.x ^ b.y) * 2 + b.y);
    }
    clip(opacity - (rank + 0.5) / 256.0);
}
