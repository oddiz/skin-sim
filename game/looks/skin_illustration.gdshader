// "Illustration" look: stylised realism, like a surgical atlas. Matte warm
// skin, soft wrapped light in two gentle steps, faint mottling, wrinkles as
// hatching, a fine ink line along every incision.
shader_type spatial;

#include "res://looks/skin_common.gdshaderinc"
#include "res://looks/lesions.gdshaderinc"

uniform vec3 tone : source_color = vec3(0.90, 0.72, 0.61);
uniform vec3 shade_tint : source_color = vec3(0.74, 0.50, 0.50);
uniform vec3 undermined_tone : source_color = vec3(0.70, 0.56, 0.64);
uniform vec3 ink : source_color = vec3(0.30, 0.10, 0.09);
uniform sampler2D noise_tex : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform float mottle = 0.07;
uniform float hatch_period = 1.4;  // mm between wrinkle lines

void vertex() {
	READ_SKIN_DATA
}

void fragment() {
	// Gentle mottling on two scales, fixed to the tissue.
	float n = texture(noise_tex, rest_mm / 45.0).r * 0.6 + texture(noise_tex, rest_mm / 9.0).r * 0.4;
	vec3 col = tone * (1.0 + mottle * (n - 0.5) * 2.0);
	col = mix(col, undermined_tone, 0.4 * undermined);
	float b = blanching();
	col = b > 0.0 ? mix(col, col * 1.06 + vec3(0.05, 0.05, 0.04), b) : mix(col, col * vec3(1.0, 0.82, 0.82), -b);
	// Wrinkles: hatching across the squeeze direction, where skin puckers.
	float ph = wrinkle_phase(hatch_period);
	float d = abs(fract(ph) - 0.5);
	float w = fwidth(ph);
	float hatch = 1.0 - smoothstep(0.06, 0.06 + 1.5 * w, d);
	hatch *= clamp(1.5 - 3.0 * w, 0.0, 1.0);  // fade where lines would crowd
	// Broken into short strokes, as drawn by hand: along each crest a dash,
	// offset from crest to crest by the noise.
	vec2 sd = squeeze_dir();
	float along = dot(rest_mm, vec2(-sd.y, sd.x)) / (2.5 * hatch_period);
	float jitter = texture(noise_tex, rest_mm / 3.0).r;
	hatch *= step(fract(along + floor(ph) * 0.37 + jitter), 0.6);
	col = mix(col, ink, 0.35 * hatch * wrinkle_amount());
	// Lesions, as painted in an atlas: their colours softened towards the
	// skin, and a fine ink line round the edge.
	vec2 lg = vec2(0.0);
	float lgloss;
	vec3 under = col;
	float lesion = lesions_apply(rest_mm, length(fwidth(rest_mm)), col, lg, lgloss);
	col = mix(col, under, 0.15 * lesion);
	// (The edge band only above 55%: an SCC's halo, at most 50%, gets none.)
	col = mix(col, ink, 0.5 * smoothstep(0.55, 0.75, lesion) * (1.0 - smoothstep(0.85, 1.0, lesion)));
	// Along the lips: a little shadow, then a fine ink line.
	col *= mix(0.8, 1.0, smoothstep(0.0, 1.8, lip_mm));
	float edge = 1.0 - smoothstep(0.12, 0.3, lip_mm);
	ALBEDO = mix(col, ink, 0.85 * edge);
	ROUGHNESS = 0.9;
	SPECULAR = 0.1;
}

void light() {
	// Wrapped diffuse (light reaches a little past the terminator, as in
	// soft tissue), eased into two soft steps, shadows tinted warm.
	float wrap = clamp((dot(NORMAL, LIGHT) + 0.6) / 1.6, 0.0, 1.0);
	float steps = mix(wrap, 0.55 + 0.45 * smoothstep(0.5, 0.62, wrap), 0.6);
	// (LIGHT_COLOR includes a factor PI, for physically based models.)
	DIFFUSE_LIGHT += LIGHT_COLOR * ATTENUATION * mix(shade_tint, vec3(1.0), steps) * steps / PI;
}
