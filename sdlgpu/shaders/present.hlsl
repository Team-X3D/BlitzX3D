Texture2D GammaSrc : register(t0, space2);
SamplerState GammaSrcSamp : register(s0, space2);
Texture2D GammaLut : register(t1, space2);
SamplerState GammaLutSamp : register(s1, space2);

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

float3 gammaMap(float3 c)
{
	const float offset = 0.5 / 256.0;
	const float scale = 255.0 / 256.0;
	float r = GammaLut.Sample(GammaLutSamp, float2(c.r * scale + offset, 0.5)).r;
	float g = GammaLut.Sample(GammaLutSamp, float2(c.g * scale + offset, 0.5)).g;
	float b = GammaLut.Sample(GammaLutSamp, float2(c.b * scale + offset, 0.5)).b;
	return float3(r, g, b);
}

float4 PSMain(VSOut i) : SV_Target0
{
	float4 c = GammaSrc.Sample(GammaSrcSamp, i.uv);
	c.rgb = gammaMap(c.rgb);
	return c;
}
