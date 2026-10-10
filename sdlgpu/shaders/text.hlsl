Texture2D TextTex : register(t0, space2);
SamplerState TextSamp : register(s0, space2);

struct VSIn
{
	float2 pos : TEXCOORD0;
	float2 uv : TEXCOORD1;
	float4 color : TEXCOORD2;
};

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

VSOut VSMain(VSIn i)
{
	VSOut o;
	o.pos = float4(i.pos, 0.0, 1.0);
	o.uv = i.uv;
	o.color = i.color;
	return o;
}

float4 PSMain(VSOut i) : SV_Target0
{
	float4 t = TextTex.Sample(TextSamp, i.uv);
	return float4(t.rgb * i.color.rgb, t.a * i.color.a);
}
