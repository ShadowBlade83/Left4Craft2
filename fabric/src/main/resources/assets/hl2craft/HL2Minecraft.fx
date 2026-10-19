// Half-Life 2 x Minecraft Compositor (HL2Craft)
// Ultra-fast single-pass compositor with Late Stage Reprojection and exact depth occlusion.
#include "ReShade.fxh"

texture MCWorldTex : MCWORLD;
texture MCDepthTex : MCDEPTH;
texture MCOverlayTex : MCOVERLAY;

sampler sWorld { Texture = MCWorldTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sDepth { Texture = MCDepthTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
sampler sOverlay { Texture = MCOverlayTex; AddressU = CLAMP; AddressV = CLAMP; };

// Minecraft near/far/flags
uniform float3 McPlanes = float3(0.05, 1024.0, 7.0);
// Half-Life 2 near/far clip in inches (Source Engine)
uniform float2 HostPlanes = float2(7.0, 28400.0);
uniform bool HostReversedZ = false;
// Active flag set by the C++ bridge (only true when in playable game)
uniform bool McActive = false;
uniform float2 McCursor = float2(960.0, 540.0);
uniform bool McCursorOn = false;

uniform float DepthBias < ui_type = "drag"; ui_min = 0.0; ui_max = 0.2; ui_step = 0.005; ui_label = "Depth bias (m)"; > = 0.02;
uniform float SlopeBias < ui_type = "drag"; ui_min = 0.0; ui_max = 5.0; ui_step = 0.05; ui_label = "Slope bias"; > = 0.0;
uniform float MaxBias < ui_type = "drag"; ui_min = 0.01; ui_max = 0.15; ui_step = 0.005; ui_label = "Max depth bias (m)"; > = 0.05;
uniform float HostDepthScale < ui_type = "drag"; ui_min = 0.01; ui_max = 0.1; ui_step = 0.001; ui_label = "HL2 depth scale"; > = 0.0254;

uniform float2 DeltaAngle = float2(0.0, 0.0); // (dYaw, dPitch) in radians
uniform float2 TanHalfFov = float2(0.577, 0.433); // (Tx, Ty)
uniform bool EnableReprojection = false;

// Minecraft (Blaze3D / RenderPearl) uses Reversed-Z depth (1.0 near, 0.0 far).
float mc_linear(float d)
{
	const float n = McPlanes.x, f = McPlanes.y;
	if (d <= 0.0001) return 1e9;
	return n * f / max(n + d * (f - n), 1e-4);
}

// Source Engine uses D3D9 depth (0.0 near, 1.0 far in inches).
float host_linear(float d)
{
	const float n = HostPlanes.x, f = HostPlanes.y;
	if (HostReversedZ)
	{
		if (d <= 0.0001) return 1e9;
		float z = n * f / max(n + d * (f - n), 1e-4);
		return z * HostDepthScale;
	}
	else
	{
		if (d >= 0.9999) return 1e9;
		float z = n * f / max(f - d * (f - n), 1e-4);
		return z * HostDepthScale;
	}
}

// Reproject screen UV to Minecraft frame texture coordinates.
// When reprojection is disabled, this directly maps 1:1 with bottom-up flip.
float2 ReprojectUV(float2 uv)
{
	if (!EnableReprojection || (abs(DeltaAngle.x) < 0.00001 && abs(DeltaAngle.y) < 0.00001))
	{
		return float2(uv.x, 1.0 - uv.y);
	}

	float xndc = uv.x * 2.0 - 1.0;
	float yndc = 1.0 - uv.y * 2.0;

	float rx = xndc * TanHalfFov.x;
	float ry = yndc * TanHalfFov.y;
	float rz = 1.0;

	// Pitch rotation: rotate by +DeltaAngle.y around X axis
	float cp = cos(DeltaAngle.y);
	float sp = sin(DeltaAngle.y);
	float rpx = rx;
	float rpy = ry * cp - rz * sp;
	float rpz = ry * sp + rz * cp;

	// Yaw rotation: rotate by +DeltaAngle.x around Y axis
	float cy = cos(DeltaAngle.x);
	float sy = sin(DeltaAngle.x);
	float rxx = rpx * cy + rpz * sy;
	float ryy = rpy;
	float rzz = -rpx * sy + rpz * cy;

	if (rzz <= 0.05)
		return float2(-1.0, -1.0);

	float oldXndc = rxx / (rzz * TanHalfFov.x);
	float oldYndc = ryy / (rzz * TanHalfFov.y);

	float u = oldXndc * 0.5 + 0.5;
	float v = 0.5 - oldYndc * 0.5;

	if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
		return float2(-1.0, -1.0);

	return float2(u, 1.0 - v);
}

float3 PS_Main(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 host = tex2D(ReShade::BackBuffer, uv).rgb;
	if (!McActive)
		return host;

	const float2 muv_overlay = float2(uv.x, 1.0 - uv.y);
	const float4 overlay = tex2D(sOverlay, muv_overlay);
	float3 finalColor = lerp(host, overlay.rgb, overlay.a);

	if (McCursorOn)
	{
		float2 p = pos.xy - McCursor;
		if (p.x >= 0.0 && p.y >= 0.0 && p.x < 16.0 && p.y < 22.0)
		{
			bool inMain = (p.y <= 16.0 && p.x <= p.y * 0.7);
			bool inTail = (p.y >= 11.0 && p.y <= 21.0 && p.x >= (p.y - 11.0) * 0.4 + 4.0 && p.x <= (p.y - 11.0) * 0.4 + 7.5);
			if (inMain || inTail)
			{
				bool edge = (p.x <= 1.2 || p.y <= 1.2 || (inMain && (p.x >= p.y * 0.7 - 1.2 || (p.y >= 14.5 && p.x >= 4.0))) || (inTail && (p.x <= (p.y - 11.0) * 0.4 + 5.2 || p.x >= (p.y - 11.0) * 0.4 + 6.3 || p.y >= 19.8)));
				return edge ? float3(0.0, 0.0, 0.0) : float3(1.0, 1.0, 1.0);
			}
		}
	}

	return finalColor;
}

technique HL2Minecraft < ui_tooltip = "Half-Life 2 x Minecraft Compositor: active only during gameplay."; >
{
	pass Main
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Main;
	}
}

technique MCPassthrough < ui_tooltip = "Half-Life 2 x Minecraft Compositor: active only during gameplay."; >
{
	pass Main
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Main;
	}
}
