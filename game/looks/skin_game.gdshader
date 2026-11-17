// "Game" look: saturated colours, cel shading (light in three flat bands),
// a thick outline at every incision. Dog-ears show through the shading
// bands and the squeezed skin's flush. (Stripes along the wrinkle crests were
// tried: the squeeze direction changes from triangle to triangle, so they
// broke into chevrons.)
shader_type spatial;

#include "res://looks/skin_common.gdshaderinc"

uniform vec3 tone : source_color = vec3(0.98, 0.62, 0.47);
uniform vec3 shade : source_color = vec3(0.62, 0.36, 0.52);   // cool purple shadows
uniform vec3 stretched : source_color = vec3(1.0, 0.86, 0.55);
uniform vec3 squeezed : source_color = vec3(0.95, 0.30, 0.42);
uniform vec3 undermined_tone : source_color = vec3(0.80, 0.62, 0.95);
uniform vec3 outline : source_color = vec3(0.22, 0.04, 0.12);
uniform sampler2D noise_tex : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform float outline_px = 3.0;   // outline width on screen

void vertex() {
	READ_SKIN_DATA
}

void fragment() {
	vec3 col = tone;
	// A few big soft patches so it is not a flat plastic colour.
	col *= 0.96 + 0.08 * texture(noise_tex, rest_mm / 70.0).r;
	col = mix(col, undermined_tone, 0.35 * undermined);
	float b = blanching();
	col = b > 0.0 ? mix(col, stretched, b) : mix(col, squeezed, -b);
	// A cartoon outline along every lip, the same width on screen at any zoom
	// (but at most 0.5 mm, so it never covers a closed wound when zoomed out).
	float px = length(fwidth(rest_mm));
	float wid = min(outline_px * px, 0.5);
	ALBEDO = mix(col, outline, 1.0 - smoothstep(wid - px, wid + px, lip_mm));
	ROUGHNESS = 0.6;
	SPECULAR = 0.3;
}

void light() {
	float ndl = dot(NORMAL, LIGHT);
	// Three flat bands. The steps are soft enough not to show the triangles
	// the relief is computed on.
	float w = fwidth(ndl) + 0.05;
	float band = 0.45 + 0.30 * smoothstep(0.05 - w, 0.05 + w, ndl) + 0.25 * smoothstep(0.55 - w, 0.55 + w, ndl);
	vec3 lit = mix(shade, vec3(1.0), band) * band;
	DIFFUSE_LIGHT += LIGHT_COLOR * ATTENUATION * lit / PI;
	// A small hard highlight.
	float ndh = dot(NORMAL, normalize(LIGHT + VIEW));
	SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * smoothstep(0.97, 0.995, ndh) * 0.12 / PI;
}
