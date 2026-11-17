// A surgical drape around the skin: a large plane below the sheet (and below
// the wound bed, so it never shows through a wound). The cloth continues
// onto the skin's margin, drawn by the skin shader (skin_realistic.gdshader,
// window_half), so it looks glued to the skin and moves with it. Woven
// cotton drawn procedurally: threads in two directions, soft folds.
shader_type spatial;
render_mode diffuse_lambert;  // as the cloth drawn on the skin

uniform vec3 cloth : source_color = vec3(0.16, 0.40, 0.44);
uniform vec3 cloth_dark : source_color = vec3(0.07, 0.22, 0.25);
uniform sampler2D noise_tex : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform float thread_mm = 0.7;

varying vec2 world_mm;

void vertex() {
	world_mm = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xz;
}

void fragment() {
	vec2 p = world_mm;
	// Folds: broad soft undulations; their slope tilts the normal.
	float e = 2.0;
	float f0 = texture(noise_tex, p / 260.0).r;
	float fx = texture(noise_tex, (p + vec2(e, 0.0)) / 260.0).r;
	float fz = texture(noise_tex, (p + vec2(0.0, e)) / 260.0).r;
	vec2 slope = vec2(fx - f0, fz - f0) / e * 12.0;
	// The weave: threads over and under, faded out when they get too small.
	float px = length(fwidth(p));
	float weave_vis = 1.0 - smoothstep(0.12, 0.35, px / thread_mm);
	vec2 t = p / thread_mm;
	float over = step(0.5, fract(floor(t.x) * 0.5 + floor(t.y) * 0.5));
	float warp = sin(fract(t.x) * 3.14159);
	float weft = sin(fract(t.y) * 3.14159);
	float weave = mix(warp, weft, over);
	vec3 col = mix(cloth_dark, cloth, 0.8 + 0.2 * f0);
	col *= mix(1.0, 0.8 + 0.2 * weave, weave_vis);
	ALBEDO = col;
	NORMAL = normalize(NORMAL + (VIEW_MATRIX * vec4(-slope.x, 0.0, -slope.y, 0.0)).xyz * 0.6);
	ROUGHNESS = 0.95;
	SPECULAR = 0.05;
}
