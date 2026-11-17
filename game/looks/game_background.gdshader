// "Game" look background, on the plane below the skin (the drape's place):
// a dark gradient with a faint 10 mm grid (as on a cutting mat) and a
// vignette at the screen's edges.
shader_type spatial;
render_mode unshaded;

uniform vec3 top : source_color = vec3(0.10, 0.08, 0.20);
uniform vec3 bottom : source_color = vec3(0.03, 0.04, 0.09);
uniform vec3 grid_color : source_color = vec3(0.35, 0.55, 1.0);
uniform float grid_mm = 10.0;

varying vec2 world_mm;

void vertex() {
	world_mm = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xz;
}

void fragment() {
	vec3 col = mix(top, bottom, SCREEN_UV.y);
	vec2 g = world_mm / grid_mm;
	vec2 fw = fwidth(g);
	vec2 line = 1.0 - smoothstep(vec2(0.0), fw * 1.2, abs(fract(g - 0.5) - 0.5));
	float grid = max(line.x, line.y) * clamp(1.0 - 3.0 * max(fw.x, fw.y), 0.0, 1.0);
	col = mix(col, grid_color, 0.12 * grid);
	vec2 v = SCREEN_UV - 0.5;
	col *= 1.0 - 0.9 * dot(v, v);
	ALBEDO = col;
}
