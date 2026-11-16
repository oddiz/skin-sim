// Shared by the skin shaders of every look. The vertex data comes from
// SkinSheet (extension/src/skin_sheet.cpp, set_skin_material):
//   UV    rest (tissue) position, mm: detail drawn on it moves with the skin
//   UV2   x = area change J (1 = unchanged, > 1 stretched), y = distance to
//         the nearest incision lip, mm
//   COLOR r = undermined (0..1), g = excess skin (0..1 = 0..30% puckered),
//         b, a = compression direction as (cos 2a, sin 2a), mapped to 0..1
//   NORMAL already tilted by the relief (dog-ears rise, lips round off).

varying vec2 rest_mm;
varying float area_change;
varying float lip_mm;
varying float undermined;
varying float excess;
varying vec2 squeeze_dir2;  // doubled-angle direction, -1..1

// Call in vertex(): varyings can only be written there.
#define READ_SKIN_DATA rest_mm = UV; area_change = UV2.x; lip_mm = UV2.y; undermined = COLOR.r; excess = COLOR.g; squeeze_dir2 = COLOR.ba * 2.0 - 1.0;

// The direction the skin is squeezed in (unit, in the sheet's x/y). Wrinkles
// run at right angles to it: their crests follow the tension.
vec2 squeeze_dir() {
	float a = 0.5 * atan(squeeze_dir2.y, squeeze_dir2.x);
	return vec2(cos(a), sin(a));
}

// Wrinkle phase across the crests (one wrinkle per `period` mm).
float wrinkle_phase(float period) {
	return dot(rest_mm, squeeze_dir()) / period;
}

// How much this spot wrinkles (0 smooth .. 1 strongly puckered).
float wrinkle_amount() {
	return smoothstep(0.12, 0.55, excess);
}

// Blanching: skin pulled tight empties its capillaries and pales; squeezed
// skin flushes. t > 0 paler, t < 0 redder.
float blanching() {
	return clamp((area_change - 1.0) * 3.0, -0.6, 0.8);
}
