Texture2D CanvasTex : register(t0, space2);
SamplerState CanvasSamp : register(s0, space2);

struct VSIn
{
	float2 pos : TEXCOORD0;
	float2 uv : TEXCOORD1;
};

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOut VSMain(VSIn i)
{
	VSOut o;
	o.pos = float4(i.pos, 0.0, 1.0);
	o.uv = i.uv;
	return o;
}

float4 PSMain(VSOut i) : SV_Target0
{
	return CanvasTex.Sample(CanvasSamp, i.uv);
}
