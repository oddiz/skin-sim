// The cut face of an incision lip, drawn as a band sloping into the wound
// (see SkinSheet, "Cut walls"). UV.x: position along the lip (mm), UV.y:
// depth, 0 at the skin surface to 1 at the bottom. COLOR.a: lip contact
// pressure. The layers top to bottom: epidermis, dermis, subcutaneous fat.
shader_type spatial;
render_mode cull_disabled;  // (faces are wound towards the camera; this is a safety net)

uniform vec3 epidermis : source_color = vec3(0.55, 0.33, 0.27);
uniform vec3 dermis : source_color = vec3(0.94, 0.76, 0.74);
uniform vec3 fat : source_color = vec3(0.99, 0.86, 0.45);
uniform vec3 deep : source_color = vec3(0.78, 0.30, 0.25);
uniform vec3 ink : source_color = vec3(0.30, 0.10, 0.09);
uniform vec3 contact_color : source_color = vec3(0.25, 0.50, 1.0);
uniform float epidermis_end = 0.08;
uniform float dermis_end = 0.55;
uniform float show_contact = 1.0;
uniform float lobule_mm = 0.9;   // fat lobule size
uniform float wetness = 0.0;     // 0 matte .. 1 glistening
uniform float depth_shade = 0.35;
uniform float outline = 1.0;
uniform float outline_width = 0.05;  // share of the band's depth
uniform sampler2D cell_tex : hint_default_white, filter_linear_mipmap, repeat_enable;

varying float depth;
varying float along;
varying float pressure;

void vertex() {
	depth = UV.y;
	along = UV.x;
	pressure = COLOR.a;
}

void fragment() {
	float aa = fwidth(depth) * 1.5;
	vec3 col = mix(epidermis, dermis, smoothstep(epidermis_end - aa, epidermis_end + aa, depth));
	// Fat: lobules from a cellular noise, a little paler at their edges.
	float cells = texture(cell_tex, vec2(along, depth * 2.0) / (lobule_mm * 8.0)).r;
	vec3 fat_col = fat * mix(0.82, 1.05, cells);
	col = mix(col, fat_col, smoothstep(dermis_end - aa, dermis_end + aa, depth));
	col = mix(col, deep, smoothstep(0.85, 1.0, depth) * 0.6);
	col *= 1.0 - depth_shade * depth;
	col = mix(col, contact_color, show_contact * pressure * 0.8);
	// A fine ink line at the skin surface.
	col = mix(col, ink, outline * (1.0 - smoothstep(outline_width, outline_width + aa, depth)));
	ALBEDO = col;
	ROUGHNESS = mix(0.9, 0.15, wetness);
	SPECULAR = mix(0.1, 0.7, wetness);
}
