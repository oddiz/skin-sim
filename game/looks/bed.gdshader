// The wound bed: the tissue under the skin, seen where a wound is open. A
// flat plane under the whole sheet (only visible through wounds), coloured
// from world-space noise.
shader_type spatial;

uniform vec3 tissue : source_color = vec3(0.72, 0.17, 0.15);
uniform vec3 tissue_dark : source_color = vec3(0.45, 0.06, 0.07);
uniform vec3 fat_islands : source_color = vec3(0.93, 0.78, 0.45);
uniform float fat_amount = 0.15;   // share of yellow fat showing through
uniform float wetness = 0.2;
uniform float blood = 0.0;         // 0 none .. 1 pooled blood
uniform vec3 blood_color : source_color = vec3(0.38, 0.02, 0.03);
uniform sampler2D noise_tex : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform sampler2D cell_tex : hint_default_white, filter_linear_mipmap, repeat_enable;

varying vec2 world_mm;

void vertex() {
	world_mm = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xz;
}

void fragment() {
	float n = texture(noise_tex, world_mm / 14.0).r;
	float cells = texture(cell_tex, world_mm / 7.0).r;
	vec3 col = mix(tissue_dark, tissue, smoothstep(0.2, 0.8, n));
	col = mix(col, fat_islands, fat_amount * smoothstep(0.55, 0.75, cells));
	col = mix(col, blood_color, blood * smoothstep(0.25, 0.6, texture(noise_tex, world_mm / 22.0).r + 0.3));
	ALBEDO = col;
	ROUGHNESS = mix(0.85, 0.15, max(wetness, blood));
	SPECULAR = mix(0.15, 0.7, max(wetness, blood));
}
