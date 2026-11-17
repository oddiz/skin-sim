// "Realistic" look: skin as it looks under theatre lights. Everything is
// computed from the per-vertex data (skin_common.gdshaderinc) and two noise
// textures made at start-up, so it runs on phones (Compatibility renderer:
// no built-in subsurface scattering, so it is faked in light()).
//
//  - colour: a base tone varied by melanin (brown) and blood (red) on
//    several scales, plus small freckles; all fixed to the tissue (rest mm)
//  - pores and fine skin lines as bumps in the normal (fade out when zoomed
//    out so they never shimmer)
//  - dog-ears: real ridges along the wrinkle crests
//  - blanching: stretched skin pales towards yellow-white, squeezed flushes
//  - along the lips: a wet, darker rim
//  - the drape: woven cloth glued over the skin's margin, outside the
//    fenestration; drawn on the skin's rest coordinates so it moves with it
//  - light: wrapped diffuse with a red glow at the light/shadow line (light
//    travelling under the surface), and a soft two-lobe sheen
shader_type spatial;

#include "res://looks/skin_common.gdshaderinc"

uniform vec3 tone : source_color = vec3(0.86, 0.66, 0.55);
uniform vec3 melanin : source_color = vec3(0.55, 0.36, 0.25);
uniform vec3 haemoglobin : source_color = vec3(0.85, 0.42, 0.40);
uniform vec3 blanched : source_color = vec3(0.96, 0.88, 0.76);
uniform vec3 undermined_tone : source_color = vec3(0.70, 0.52, 0.58);
uniform vec3 rim_color : source_color = vec3(0.55, 0.16, 0.14);
uniform vec3 scatter_color : source_color = vec3(0.85, 0.20, 0.10);
uniform sampler2D noise_tex : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform sampler2D cell_tex : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform float pore_mm = 0.45;       // spacing of pores
uniform float pore_depth = 0.5;
uniform float line_depth = 0.12;    // fine skin lines (the "crosshatch" of the surface)
uniform float wrinkle_depth = 0.9;
uniform float wrinkle_period = 1.6; // mm between dog-ear wrinkles
uniform float freckles = 0.25;
uniform float sheen = 0.35;
// The drape (Look sets these; window_half.x <= 0: no drape).
uniform vec2 window_half = vec2(0.0);  // half size of the opening, rest mm
uniform vec3 cloth : source_color = vec3(0.16, 0.40, 0.44);
uniform vec3 cloth_dark : source_color = vec3(0.07, 0.22, 0.25);
uniform float thread_mm = 0.7;

varying float wet;
varying float on_cloth;

void vertex() {
	READ_SKIN_DATA
}

// A bump height field on rest mm, and its slope, for the normal.
float bumps(vec2 p, float detail) {
	float pores = texture(cell_tex, p / (pore_mm * 8.0)).r;            // cellular: dips at the cell centres
	float h = -pore_depth * (1.0 - smoothstep(0.0, 0.16, pores));  // small round pits
	// Fine lines: two families crossing at about 70 degrees, wavy.
	float wob = texture(noise_tex, p / 6.0).r * 4.0;
	float l1 = abs(fract(dot(p, vec2(0.82, 0.57)) / 0.9 + wob) - 0.5);
	float l2 = abs(fract(dot(p, vec2(-0.34, 0.94)) / 1.1 - wob) - 0.5);
	h -= line_depth * (1.0 - smoothstep(0.0, 0.12, min(l1, l2)));
	return h * detail;
}

void fragment() {
	vec2 p = rest_mm;
	// How much fine detail survives at this zoom (pixels per mm).
	float mm_per_px = length(fwidth(p));
	float detail = 1.0 - smoothstep(0.06, 0.18, mm_per_px);

	// --- colour ---
	float big = texture(noise_tex, p / 60.0).r;
	float mid = texture(noise_tex, p / 14.0 + 0.37).r;
	float small = texture(noise_tex, p / 3.5 + 0.71).r;
	vec3 col = tone;
	col = mix(col, melanin, 0.25 * smoothstep(0.35, 0.8, big * 0.6 + small * 0.4));
	col = mix(col, haemoglobin, 0.18 * smoothstep(0.45, 0.85, mid));
	float fr = texture(cell_tex, p / 9.0 + 0.5).r;
	col = mix(col, melanin * 0.85, freckles * (1.0 - smoothstep(0.03, 0.09, fr)) * smoothstep(0.45, 0.7, big));
	col = mix(col, undermined_tone, 0.3 * undermined);
	float b = blanching();
	col = b > 0.0 ? mix(col, blanched, 0.8 * b) : mix(col, haemoglobin * 0.92, -0.9 * b);
	// Darker in the folds of the dog-ears.
	float ph = wrinkle_phase(wrinkle_period);
	float amt = wrinkle_amount();
	float fold = 0.5 + 0.5 * cos(6.2831853 * ph);
	col *= 1.0 - 0.18 * amt * fold;

	float pit = 1.0 - smoothstep(0.0, 0.16, texture(cell_tex, p / (pore_mm * 8.0)).r);
	col *= 1.0 - 0.12 * pit * detail;

	// --- the lips: shadow, a wet dark-red rim ---
	col *= mix(0.72, 1.0, smoothstep(0.0, 1.5, lip_mm));
	float rim = 1.0 - smoothstep(0.08, 0.35, lip_mm);
	col = mix(col, rim_color, 0.7 * rim);
	float w = rim;

	wet = w;

	// --- the drape, glued on outside the window ---
	float cloth_amt = 0.0;
	vec2 cloth_slope = vec2(0.0);
	if (window_half.x > 0.0) {
		vec2 dw = abs(p) - window_half;
		float out_mm = max(dw.x, dw.y);           // > 0: under the drape
		float aa = mm_per_px;
		cloth_amt = smoothstep(-aa, aa, out_mm);
		// The skin just inside the edge: a soft shadow cast by the cloth's thickness.
		col *= mix(0.72, 1.0, smoothstep(0.0, 1.5, -out_mm));
		vec2 t = p / thread_mm;
		float over = step(0.5, fract(floor(t.x) * 0.5 + floor(t.y) * 0.5));
		float weave = mix(sin(fract(t.x) * 3.14159), sin(fract(t.y) * 3.14159), over);
		float weave_vis = 1.0 - smoothstep(0.12, 0.35, mm_per_px / thread_mm);
		// Same folds and shading as the drape plane beyond the skin
		// (drape.gdshader; rest x/y = world x/-z), so the two meet seamlessly.
		vec2 q = vec2(p.x, -p.y);
		float f0 = texture(noise_tex, q / 260.0).r;
		cloth_slope = vec2(texture(noise_tex, (q + vec2(2.0, 0.0)) / 260.0).r - f0,
				texture(noise_tex, (q + vec2(0.0, 2.0)) / 260.0).r - f0) / 2.0 * 12.0;
		vec3 cc = mix(cloth_dark, cloth, 0.8 + 0.2 * f0);
		cc *= mix(1.0, 0.8 + 0.2 * weave, weave_vis);
		// The adhesive edge: a slightly paler, raised hem 1.5 mm wide.
		cc *= 1.0 + 0.12 * (1.0 - smoothstep(0.8, 1.5, out_mm));
		col = mix(col, cc, cloth_amt);
	}
	on_cloth = cloth_amt;

	// --- normal: pores, fine lines, wrinkles ---
	float e = 0.05;
	float h0 = bumps(p, detail);
	vec2 grad = vec2(bumps(p + vec2(e, 0.0), detail) - h0, bumps(p + vec2(0.0, e), detail) - h0) / e;
	// Wrinkle ridges across the squeeze direction (only where it puckers).
	vec2 sd = squeeze_dir();
	grad += sd * (-sin(6.2831853 * ph) * 6.2831853 / wrinkle_period) * wrinkle_depth * amt * 0.25;
	grad *= (1.0 - w) * (1.0 - on_cloth);  // wet tissue fills the pores; cloth hides them
	// Rest x/y is world x/-z (SkinSheet); tilt the normal against the slope.
	vec3 tilt = (VIEW_MATRIX * vec4(-grad.x, 0.0, grad.y, 0.0)).xyz;
	NORMAL = normalize(NORMAL + 0.12 * tilt);
	NORMAL = normalize(NORMAL + on_cloth * 0.6 * (VIEW_MATRIX * vec4(-cloth_slope.x, 0.0, -cloth_slope.y, 0.0)).xyz);

	ALBEDO = col;
	ROUGHNESS = mix(mix(0.55, 0.12, w), 0.95, on_cloth);
	SPECULAR = mix(mix(0.35, 0.8, w), 0.05, on_cloth);
}

void light() {
	float ndl = dot(NORMAL, LIGHT);
	// Light enters, scatters under the skin and leaves a little further on:
	// shadows soften, and the light/shadow line glows red.
	float wrap = clamp((ndl + 0.45) / 1.45, 0.0, 1.0);
	float direct = clamp(ndl, 0.0, 1.0);
	vec3 sss = scatter_color * clamp(wrap - direct, 0.0, 1.0) * 0.9 * (1.0 - on_cloth);
	wrap = mix(wrap, direct, on_cloth);  // cloth: plain matte light
	// (The engine multiplies DIFFUSE_LIGHT by ALBEDO afterwards; LIGHT_COLOR
	// includes a factor PI.)
	DIFFUSE_LIGHT += LIGHT_COLOR * ATTENUATION * (vec3(wrap) + sss) / PI;
	// Sheen: a broad soft lobe (the skin's oil layer) and a sharp one where wet.
	vec3 hv = normalize(LIGHT + VIEW);
	float ndh = clamp(dot(NORMAL, hv), 0.0, 1.0);
	float wet_spec = pow(ndh, mix(30.0, 400.0, wet)) * mix(0.0, 1.6, wet);
	float soft = pow(ndh, 14.0) * sheen * 0.25;
	SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * (soft + wet_spec) * direct * (1.0 - on_cloth) / PI;
}
