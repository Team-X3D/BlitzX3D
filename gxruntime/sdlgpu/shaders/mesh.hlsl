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
			float3 H = normalize(hitDir + V);
			specAcc += lightSpec[li].rgb * (pow(clamp(dot(nW, H), 0.0, 1.0), max(matSpec.w, 1.0)) * atten);
		}
	}
	float3 finalRgb;
	if (flags.y > 0.5) {
		finalRgb = matD;
	} else {
		finalRgb = clamp(matE + ambAcc + difAcc, 0.0, 1.0) + matSpec.rgb * clamp(specAcc, 0.0, 1.0);
	}

	o.color = float4(finalRgb, baseA);
	float3 nV = float3(dot(nW, viewX.xyz), dot(nW, viewY.xyz), dot(nW, viewZ.xyz));
	float2 sph = float2(nV.x * 0.5 + 0.5, -nV.y * 0.5 + 0.5);
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

cbuffer PSParams : register(b0, space3)
{
	float4 psStage1;
	float4 psMat0A;
	float4 psMat0B;
	float4 psMat1A;
	float4 psMat1B;
	float4 psBump;
};

float2 xformUV(float2 uv, float4 A, float4 B)
{
	if (A.w < 0.5)
		return uv;
	return float2(uv.x * A.x + uv.y * A.y + A.z, uv.x * B.x + uv.y * B.y + B.z);
}

float4 PSMain(VSOut i) : SV_Target0
{
	float2 uv = xformUV(i.uv, psMat0A, psMat0B);
	float4 tex = MeshTex.Sample(MeshSamp, uv) * i.color;
	if (i.testParams.x > 0.5 && tex.a < i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

float4 PSMain2Tex(VSOut i) : SV_Target0
{
	float2 uv = xformUV(i.uv, psMat0A, psMat0B);
	float2 uv1base = (psStage1.y > 0.5) ? i.uv1 : i.uv;
	float2 uv1 = xformUV(uv1base, psMat1A, psMat1B);
	float op = psStage1.x;
	if (op > 5.5) {
		float4 bump = MeshTex1.Sample(MeshSamp1, uv1);
		float2 duv = float2(dot(bump.rg - 0.5, psBump.xy), dot(bump.rg - 0.5, psBump.zw));
		float4 tex = MeshTex.Sample(MeshSamp, uv + duv) * i.color;
		if (i.testParams.x > 0.5 && tex.a < i.testParams.y)
			discard;
		tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
		return tex;
	}
	float4 tex = MeshTex.Sample(MeshSamp, uv) * i.color;
	float4 t1 = MeshTex1.Sample(MeshSamp1, uv1);
	if (op < 1.5) {
		tex.rgb = lerp(tex.rgb, t1.rgb, t1.a);
	} else if (op < 2.5) {
		tex.rgb = saturate(tex.rgb * t1.rgb);
	} else if (op < 3.5) {
		tex.rgb = saturate(tex.rgb + t1.rgb);
	} else if (op < 4.5) {
		float d = dot(tex.rgb - 0.5, t1.rgb - 0.5) * 4.0;
		tex.rgb = saturate(d);
	} else {
		tex.rgb = saturate(tex.rgb * t1.rgb * 2.0);
	}
	if (psStage1.w > 0.5)
		tex.a = saturate(tex.a * t1.a);
	if (i.testParams.x > 0.5 && tex.a < i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

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
	float4 tex = MeshTexCube.Sample(MeshSamp, i.refl) * i.color;
	if (i.testParams.x > 0.5 && tex.a < i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

float4 PSMainCubeTex(VSOut i) : SV_Target0
{
	float4 tex = MeshTexCube.Sample(MeshSamp, i.refl) * i.color;
	float2 uv1base = (psStage1.y > 0.5) ? i.uv1 : i.uv;
	float2 uv1 = xformUV(uv1base, psMat1A, psMat1B);
	float4 t1 = MeshTex1.Sample(MeshSamp1, uv1);
	tex = blendStages(tex, t1, psStage1.x, psStage1.w);
	if (i.testParams.x > 0.5 && tex.a < i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

float4 PSMainTexCube(VSOut i) : SV_Target0
{
	float2 uv = xformUV(i.uv, psMat0A, psMat0B);
	float4 tex = MeshTex.Sample(MeshSamp, uv) * i.color;
	float4 t1 = MeshTex1Cube.Sample(MeshSamp1, i.refl1);
	tex = blendStages(tex, t1, psStage1.x, psStage1.w);
	if (i.testParams.x > 0.5 && tex.a < i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

float4 PSMainCubeCube(VSOut i) : SV_Target0
{
	float4 tex = MeshTexCube.Sample(MeshSamp, i.refl) * i.color;
	float4 t1 = MeshTex1Cube.Sample(MeshSamp1, i.refl1);
	tex = blendStages(tex, t1, psStage1.x, psStage1.w);
	if (i.testParams.x > 0.5 && tex.a < i.testParams.y)
		discard;
	tex.rgb = lerp(tex.rgb, i.fogColor.rgb, i.fog);
	return tex;
}

Texture2D ExtraTex : register(t0, space2);
SamplerState ExtraSamp : register(s0, space2);

cbuffer PSExtraParams : register(b0, space3)
{
	float4 psExtra;
	float4 psExtraMatA;
	float4 psExtraMatB;
};

float4 PSMainExtra(VSOut i) : SV_Target0
{
	float2 base = (psExtra.x > 0.5) ? i.uv1 : i.uv;
	float2 uv = xformUV(base, psExtraMatA, psExtraMatB);
	float4 tex = ExtraTex.Sample(ExtraSamp, uv);
	if (i.testParams.x > 0.5 && tex.a < i.testParams.y)
		discard;
	return tex;
}
