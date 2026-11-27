// The z and w rows share the forward axis in a perspective view-projection.
// The intercept in z = slope*w + intercept determines near-to-far ordering.
cbuffer RefractionCamera : register(b12) {
    row_major float4x4 RefractionViewProjection : packoffset(c8);
};
float ProjectionDepthSign(float fallback) {
    float4 z = RefractionViewProjection[2], w = RefractionViewProjection[3];
    float wSquared = dot(w.xyz, w.xyz);
    if (!(wSquared > 0)) return fallback;
    float slope = dot(z.xyz, w.xyz) / wSquared;
    float3 residual = z.xyz - slope * w.xyz;
    const float tolerance = 32 * 1.192092896e-7;
    if (!(dot(residual, residual) <= tolerance * tolerance * wSquared * (1 + slope * slope))) return fallback;
    float intercept = z.w - slope * w.w;
    if (!(abs(intercept) > tolerance * (abs(z.w) + abs(slope * w.w)))) return fallback;
    return intercept < 0 ? 1 : -1;
}
