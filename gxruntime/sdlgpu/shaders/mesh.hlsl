cbuffer VSParams : register(b0, space1)
{
	float4x4 mvp;
	float4x4 world;
	float4 ambient;
	float4 matDiffuse;
	float4 matAmbient;
	float4 matEmissive;
	float4 matSpec;
	float4 matSrc;
	float4 fogColor;
	float4 fogParams;
	float4 eyePos;
	float4 flags;
	int lightCount;
	float3 lightPad;
	float4 lightPos[8];
	float4 lightColor[8];
	float4 lightSpec[8];
	float4 lightAmb[8];
	float4 lightAtten[8];
	float4 lightSpotDir[8];
	float4 lightSpotPrm[8];
	float4 texGen;
	float4 viewX;
	float4 viewY;
	float4 viewZ;
	float4 cubeParams;
};

StructuredBuffer<float4> g_bones : register(t0, space0);

struct VSIn
{
	float3 pos : TEXCOORD0;
	float3 normal : TEXCOORD1;
	float4 color : TEXCOORD2;
	float2 uv : TEXCOORD3;
	float2 uv1 : TEXCOORD4;
};

struct VSInSkin
{
	float3 pos : TEXCOORD0;
	float3 normal : TEXCOORD1;
	float4 color : TEXCOORD2;
	float2 uv : TEXCOORD3;
	float2 uv1 : TEXCOORD4;
	float4 blendIdx : TEXCOORD5;
	float4 blendWgt : TEXCOORD6;
};

struct VSOut
{
	float4 pos : SV_Position;
	float4 color : COLOR0;
	nointerpolation float4 colorFlat : COLOR1;
	float3 spec : TEXCOORD7;
	float2 uv : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float fog : TEXCOORD2;
	float4 fogColor : TEXCOORD3;
	float2 testParams : TEXCOORD4;
	float3 refl : TEXCOORD5;
	float3 refl1 : TEXCOORD6;
};

float3 cubeVector(float packed, float3 nW, float3 V, float4 worldPos)
{
	int code = (int)(packed + 0.5);
	int mode = code % 8;
	int space = code / 8;
	float3 nCam = float3(dot(nW, viewX.xyz), dot(nW, viewY.xyz), dot(nW, viewZ.xyz));
	float3 vCam = float3(dot(-V, viewX.xyz), dot(-V, viewY.xyz), dot(-V, viewZ.xyz));
	float3 r;
	if (mode == 2) {
		r = nCam;
	} else if (mode == 3) {
		float3 d = worldPos.xyz - eyePos.xyz;
		r = float3(dot(d, viewX.xyz), dot(d, viewY.xyz), dot(d, viewZ.xyz));
	} else {
		r = reflect(vCam, nCam);
	}
	if (space == 0) r = r.x * viewX.xyz + r.y * viewY.xyz + r.z * viewZ.xyz;
	return r;
}

VSOut shadeMesh(float3 lPos, float3 lNrm, float4 vcol, float2 uv, float2 uv1)
{
	VSOut o;
	o.pos = mul(mvp, float4(lPos, 1.0));

	float4 vcolBgra = vcol.bgra;
	float3 matD = (matSrc.x > 0.5) ? vcolBgra.rgb : matDiffuse.rgb;
	float3 matA = (matSrc.y > 0.5) ? vcolBgra.rgb : matAmbient.rgb;
	float3 matE = (matSrc.z > 0.5) ? vcolBgra.rgb : matEmissive.rgb;
	float baseA = vcolBgra.a * matDiffuse.a;

	float4 worldPos = mul(world, float4(lPos, 1.0));
	float3 nW = normalize(mul(world, float4(lNrm, 0.0)).xyz);
	float3 V = normalize(eyePos.xyz - worldPos.xyz);
	float3 Vinf = -viewZ.xyz;

	float3 difAcc = float3(0.0, 0.0, 0.0);
	float3 ambAcc = matA * ambient.rgb;
	float3 specAcc = float3(0.0, 0.0, 0.0);
	bool doSpec = (matSpec.x + matSpec.y + matSpec.z) > 0.0 && matSpec.w > 0.0;
	for (uint li = 0; li < 8; li++) {
		if ((int)li >= lightCount)
			break;
		float w = lightPos[li].w;
		float3 hitDir;
		float atten = 1.0;
		if (w > 2.5) {
			hitDir = normalize(lightPos[li].xyz);
		} else {
			float3 delta = lightPos[li].xyz - worldPos.xyz;
			float dist = length(delta);
			hitDir = delta / max(dist, 1e-6);
			atten = 1.0 / max(lightAtten[li].x + dist * (lightAtten[li].y + dist * lightAtten[li].z), 1e-6);
			if (dist > lightAtten[li].w)
				atten = 0.0;
			if (w > 1.5) {
				float rho = dot(-hitDir, normalize(lightSpotDir[li].xyz));
				float s = pow(saturate((rho - lightSpotPrm[li].y) / (lightSpotPrm[li].x - lightSpotPrm[li].y)), lightSpotDir[li].w);
				if (rho <= lightSpotPrm[li].y)
					s = 0.0;
				if (rho > lightSpotPrm[li].x)
					s = 1.0;
				atten *= s;
			}
		}
		float ndl = clamp(dot(nW, hitDir), 0.0, 1.0);
		ambAcc += matA * lightAmb[li].rgb * atten;
		difAcc += matD * lightColor[li].rgb * (ndl * atten);
		if (doSpec && ndl > 0.0) {
			float3 H = normalize(hitDir + Vinf);
			specAcc += lightSpec[li].rgb * (pow(clamp(dot(nW, H), 0.0, 1.0), max(matSpec.w, 1.0)) * atten);
		}
	}
	float3 finalRgb;
	if (flags.y > 0.5) {
		finalRgb = matD;
	} else {
		finalRgb = clamp(matE + ambAcc + difAcc, 0.0, 1.0);
	}

	o.color = float4(finalRgb, baseA);
	o.colorFlat = o.color;
	o.spec = matSpec.rgb * clamp(specAcc, 0.0, 1.0);
	float3 nV = float3(dot(nW, viewX.xyz), dot(nW, viewY.xyz), dot(nW, viewZ.xyz));
	float3 vDir = float3(dot(worldPos.xyz - eyePos.xyz, viewX.xyz), dot(worldPos.xyz - eyePos.xyz, viewY.xyz), dot(worldPos.xyz - eyePos.xyz, viewZ.xyz));
	float3 rfl = reflect(normalize(vDir), nV);
	float sphM = length(rfl + float3(0.0, 0.0, 1.0)) * 2.0;
	float2 sph = rfl.xy / sphM + 0.5;
	float2 baseUv0 = (texGen.z > 0.5) ? uv1 : uv;
	o.uv = (texGen.x > 0.5) ? sph : baseUv0;
	o.uv1 = (texGen.y > 0.5) ? sph : uv1;

	float dist = distance(worldPos.xyz, eyePos.xyz);
	float f = 0.0;
	if (fogParams.w > 0.5 && fogParams.w < 1.5) {
		float span = max(fogParams.y - fogParams.x, 1e-6);
		f = 1.0 - saturate((fogParams.y - dist) / span);
	} else if (fogParams.w > 1.5 && fogParams.w < 2.5) {
		f = 1.0 - exp(-fogParams.z * dist);
	} else if (fogParams.w > 2.5) {
		float d = fogParams.z * dist;
		f = 1.0 - exp(-d * d);
	}
	o.fog = saturate(f);
	o.fogColor = fogColor;
	o.testParams = float2(flags.z, flags.w);
	o.refl = cubeVector(cubeParams.z, nW, V, worldPos);
	o.refl1 = cubeVector(cubeParams.w, nW, V, worldPos);
	return o;
}

VSOut VSMain(VSIn i)
{
	return shadeMesh(i.pos, i.normal, i.color, i.uv, i.uv1);
}

void skinBone(float3 pos, float3 nrm, int b, out float3 wp, out float3 wn)
{
	float4 c0 = g_bones[b * 3 + 0];
	float4 c1 = g_bones[b * 3 + 1];
	float4 c2 = g_bones[b * 3 + 2];
	wp = float3(dot(pos, c0.xyz), dot(pos, c1.xyz), dot(pos, c2.xyz)) + float3(c0.w, c1.w, c2.w);
	wn = float3(dot(nrm, c0.xyz), dot(nrm, c1.xyz), dot(nrm, c2.xyz));
}

VSOut VSMainSkinned(VSInSkin i)
{
	float3 sp = float3(0.0, 0.0, 0.0);
	float3 sn = float3(0.0, 0.0, 0.0);
	float tw = 0.0;
	for (uint k = 0; k < 4; ++k) {
		float w = i.blendWgt[k];
		int b = clamp((int)(i.blendIdx[k] + 0.5), 0, 63);
		float3 wp, wn;
		skinBone(i.pos, i.normal, b, wp, wn);
		sp += wp * w;
		sn += wn * w;
		tw += w;
	}
	if (tw <= 1e-5) {
		skinBone(i.pos, i.normal, 0, sp, sn);
	}
	sn = normalize(sn);
	return shadeMesh(sp, sn, i.color, i.uv, i.uv1);
}

Texture2D MeshTex : register(t0, space2);
SamplerState MeshSamp : register(s0, space2);
Texture2D MeshTex1 : register(t1, space2);
SamplerState MeshSamp1 : register(s1, space2);

Texture2D StageTex0 : register(t0, space2);
Texture2D StageTex1 : register(t1, space2);
Texture2D StageTex2 : register(t2, space2);
Texture2D StageTex3 : register(t3, space2);
Texture2D StageTex4 : register(t4, space2);
Texture2D StageTex5 : register(t5, space2);
Texture2D StageTex6 : register(t6, space2);
Texture2D StageTex7 : register(t7, space2);
SamplerState StageSamp0 : register(s0, space2);
SamplerState StageSamp1 : register(s1, space2);
SamplerState StageSamp2 : register(s2, space2);
SamplerState StageSamp3 : register(s3, space2);
SamplerState StageSamp4 : register(s4, space2);
SamplerState StageSamp5 : register(s5, space2);
SamplerState StageSamp6 : register(s6, space2);
SamplerState StageSamp7 : register(s7, space2);

cbuffer PSParams : register(b0, space3)
{
	float4 psStage1;
	float4 psMat0A;
	float4 psMat0B;
	float4 psMat1A;
	float4 psMat1B;
	float4 psBump;
	float4 psFlat;
	float4 psStage[8];
	float4 psMatA[8];
	float4 psMatB[8];
	float4 psBumpEnv[8];
};

float2 xformUV(float2 uv, float4 A, float4 B)
{
	if (A.w < 0.5)
		return uv;
	return float2(uv.x * A.x + uv.y * A.y + A.z, uv.x * B.x + uv.y * B.y + B.z);
}

float4 shadeColor(VSOut i)
{
	return (psFlat.x > 0.5) ? i.colorFlat : i.color;
}

void applyStage(float4 t, float4 st, float4 bump, inout float3 current, inout float alpha, inout float2 bumpOfs)
{
	int op = (int)(st.x + 0.5);
	if (op == 6) {
		bumpOfs = float2(dot(t.rg - 0.5, bump.xy), dot(t.rg - 0.5, bump.zw));
		return;
	}
	if (op == 1)      current = lerp(current, t.rgb, t.a);
	else if (op == 2) current = saturate(current * t.rgb);
	else if (op == 3) current = saturate(current + t.rgb);
	else if (op == 4) current = saturate(dot(current - 0.5, t.rgb - 0.5) * 4.0);
	else if (op == 5) current = saturate(current * t.rgb * 2.0);
	if (st.z > 0.5) alpha = saturate(alpha * t.a);
}

#define MULTI_STAGE(N, TEX, SAMP) \
	if (psStage[N].x > 0.5) { \
		float2 mbase = (psStage[N].y > 0.5) ? i.uv1 : i.uv; \
		float2 muv = xformUV(mbase, psMatA[N], psMatB[N]) + bumpOfs; \
		applyStage(TEX.Sample(SAMP, muv), psStage[N], psBumpEnv[N], current, alpha, bumpOfs); \
	}

#ifndef NSTAGES
#define NSTAGES 8
#endif

float4 PSMainMulti(VSOut i) : SV_Target0
{
	float4 base = shadeColor(i);
	float3 current = base.rgb;
	float alpha = base.a;
	float2 bumpOfs = float2(0.0, 0.0);
#if NSTAGES > 0
	MULTI_STAGE(0, StageTex0, StageSamp0)
#endif
#if NSTAGES > 1
	MULTI_STAGE(1, StageTex1, StageSamp1)
#endif
#if NSTAGES > 2
	MULTI_STAGE(2, StageTex2, StageSamp2)
#endif
#if NSTAGES > 3
	MULTI_STAGE(3, StageTex3, StageSamp3)
#endif
#if NSTAGES > 4
	MULTI_STAGE(4, StageTex4, StageSamp4)
#endif
#if NSTAGES > 5
	MULTI_STAGE(5, StageTex5, StageSamp5)
#endif
#if NSTAGES > 6
	MULTI_STAGE(6, StageTex6, StageSamp6)
#endif
#if NSTAGES > 7
	MULTI_STAGE(7, StageTex7, StageSamp7)
#endif
	current += i.spec;
	if (i.testParams.x > 0.5 && alpha <= i.testParams.y)
		discard;
	current = lerp(current, i.fogColor.rgb, i.fog);
	return float4(current, alpha);
}

#undef MULTI_STAGE

TextureCube MeshTexCube : register(t0, space2);
TextureCube MeshTex1Cube : register(t1, space2);

float4 blendStages(float4 tex, float4 t1, float op, float alphaFlag)
{
	if (op > 5.5) return tex;
	if (op < 1.5)      tex.rgb = lerp(tex.rgb, t1.rgb, t1.a);
	else if (op < 2.5) tex.rgb = saturate(tex.rgb * t1.rgb);
	else if (op < 3.5) tex.rgb = saturate(tex.rgb + t1.rgb);
	else if (op < 4.5) { float d = dot(tex.rgb - 0.5, t1.rgb - 0.5) * 4.0; tex.rgb = saturate(d); }
	else               tex.rgb = saturate(tex.rgb * t1.rgb * 2.0);
	if (alphaFlag > 0.5) tex.a = saturate(tex.a * t1.a);
	return tex;
}

float4 PSMainCube(VSOut i) : SV_Target0
{
	float4 tex = MeshTexCube.Sample(MeshSamp, i.refl) * shadeColor(i);
	tex.rgb += i.spec;
	if (i.testParams.x > 0.5 && tex.a <= i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

float4 PSMainCubeTex(VSOut i) : SV_Target0
{
	float4 tex = MeshTexCube.Sample(MeshSamp, i.refl) * shadeColor(i);
	float2 uv1base = (psStage1.y > 0.5) ? i.uv1 : i.uv;
	float2 uv1 = xformUV(uv1base, psMat1A, psMat1B);
	float4 t1 = MeshTex1.Sample(MeshSamp1, uv1);
	tex = blendStages(tex, t1, psStage1.x, psStage1.w);
	tex.rgb += i.spec;
	if (i.testParams.x > 0.5 && tex.a <= i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

float4 PSMainTexCube(VSOut i) : SV_Target0
{
	float2 uv = xformUV(i.uv, psMat0A, psMat0B);
	float4 tex = MeshTex.Sample(MeshSamp, uv) * shadeColor(i);
	float4 t1 = MeshTex1Cube.Sample(MeshSamp1, i.refl1);
	tex = blendStages(tex, t1, psStage1.x, psStage1.w);
	tex.rgb += i.spec;
	if (i.testParams.x > 0.5 && tex.a <= i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

float4 PSMainCubeCube(VSOut i) : SV_Target0
{
	float4 tex = MeshTexCube.Sample(MeshSamp, i.refl) * shadeColor(i);
	float4 t1 = MeshTex1Cube.Sample(MeshSamp1, i.refl1);
	tex = blendStages(tex, t1, psStage1.x, psStage1.w);
	tex.rgb += i.spec;
	if (i.testParams.x > 0.5 && tex.a <= i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

