// Skin lesions drawn by the skin shaders, in rest (tissue) coordinates: they
// stretch with the skin, get its lighting, and vanish exactly where they are
// excised (no skin, nothing drawn). Set by Look.set_lesions from
// SkinSheet.get_lesion_info: lesions[i] = (centre x, centre y, radius, kind),
// rest mm; kind 0 nodular BCC, 1 SCC, 2 melanoma.
//
// Everything is procedural (hashed value noise, no textures), so each lesion
// looks different (its seed comes from its centre) and the same every frame.
// The outline stays inside the lesion's radius, so the margin the simulator
// measures is the margin you see.
//
// Teaching appearance, simplified:
//  - nodular BCC: pearly, translucent dome with a rolled edge, fine branching
//    vessels (telangiectasia) on its surface, a small central ulcer when larger
//  - SCC: keratin crust (yellow-white to brown, cracked) on a pink, raised,
//    inflamed base with a red halo
//  - melanoma: asymmetric, irregular and partly blurred border, variegated
//    light / dark brown, black and blue-grey, a pale area of regression

uniform vec4 lesions[8];
uniform int lesion_count = 0;

float lz_hash(vec2 p) {
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

// Value noise, 0..1, smooth.
float lz_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(lz_hash(i), lz_hash(i + vec2(1.0, 0.0)), u.x),
			mix(lz_hash(i + vec2(0.0, 1.0)), lz_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float lz_fbm(vec2 p) {
	return 0.55 * lz_noise(p) + 0.3 * lz_noise(p * 2.03 + 17.1) + 0.15 * lz_noise(p * 4.1 + 31.7);
}

// Edge radius (in units of the lesion's radius) in the direction of q.
// Periodic round the circle: noise sampled on a circle, not on the angle.
float lz_border(vec2 q, float seed, float amount, float scale) {
	vec2 u = normalize(q + vec2(1e-5)) * scale + seed;
	return 1.0 - amount * lz_fbm(u);
}

// Height of lesion i at q (lesion units: q = (p - centre) / radius), in
// units of its radius. Used for the shading (normals) of the realistic look,
// three times per pixel: kept cheap (one noise for the edge, not three).
float lz_height(int kind, vec2 q, float seed) {
	float d = length(q);
	if (kind == 0) {  // BCC: dome + rolled edge, a dip for the ulcer
		float rb = 1.0 - 0.08 * lz_noise(normalize(q + vec2(1e-5)) * 2.0 + seed);
		float x = d / rb;
		float h = 0.22 * sqrt(max(0.0, 1.0 - x * x));
		h += 0.1 * exp(-pow((x - 0.8) / 0.12, 2.0));
		h -= 0.12 * (1.0 - smoothstep(0.15, 0.32, d + 0.08 * lz_noise(q * 5.0 + seed)));
		return h;
	} else if (kind == 1) {  // SCC: raised base, rough crust on top
		float rb = 1.0 - 0.14 * lz_noise(normalize(q + vec2(1e-5)) * 2.0 + seed);
		float x = d / rb;
		float h = 0.2 * (1.0 - smoothstep(0.55, 1.0, x));
		float crust = 1.0 - smoothstep(0.55, 0.72, x + 0.12 * lz_noise(q * 4.0 + seed));
		h += crust * (0.06 + 0.05 * lz_noise(q * 14.0 + seed));
		return h;
	}
	// Melanoma: nearly flat, a slight plaque
	return 0.04 * (1.0 - smoothstep(0.5, 0.9, d));
}

// The lesion's colour at q and how much it covers (alpha), plus how glossy
// it is (0..1). `skin`: the skin colour underneath (the BCC is translucent).
vec4 lz_color(int kind, vec2 q, float seed, vec3 skin, float aa, out float gloss) {
	float d = length(q);
	gloss = 0.0;
	if (kind == 0) {
		// --- nodular BCC ---
		float rb = lz_border(q, seed, 0.08, 2.0);
		float x = d / rb;
		float cover = 1.0 - smoothstep(rb - aa - 0.03, rb, d);
		// Pearly and translucent: the skin's own pink shows through, paler
		// and a little waxy.
		vec3 pearl = mix(skin, vec3(0.93, 0.76, 0.74), 0.55);
		vec3 col = mix(pearl, pearl * vec3(1.0, 0.92, 0.93), smoothstep(0.0, 0.7, x));
		// The rolled edge: a pale pearly ring, a faint shadow just inside it.
		float ridge = exp(-pow((x - 0.82) / 0.09, 2.0));
		col = mix(col, vec3(0.97, 0.88, 0.86), 0.45 * ridge);
		col *= 1.0 - 0.08 * exp(-pow((x - 0.62) / 0.1, 2.0));
		// Telangiectasia: long thin branching vessels running over the dome
		// (the curves where a warped noise crosses its midpoint).
		vec2 wq = q * 1.8 + 0.6 * vec2(lz_noise(q * 2.0 + seed), lz_noise(q * 2.0 + seed + 5.0)) + seed;
		float v1 = abs(lz_noise(wq) - 0.5);
		float v2 = abs(lz_noise(wq * 2.3 + 11.0) - 0.5);
		float vessel = max(1.0 - smoothstep(0.008, 0.03 + aa * 0.5, v1),
				0.7 * (1.0 - smoothstep(0.006, 0.02 + aa * 0.5, v2)));
		vessel *= smoothstep(0.25, 0.45, x) * (1.0 - smoothstep(0.85, 0.97, x));
		col = mix(col, vec3(0.8, 0.22, 0.26), 0.7 * vessel);
		// Central ulcer with a dark crust (a "rodent ulcer").
		float ulcer = 1.0 - smoothstep(0.17, 0.27, d + 0.08 * lz_noise(q * 5.0 + seed));
		vec3 crust = mix(vec3(0.45, 0.14, 0.1), vec3(0.2, 0.08, 0.06), lz_noise(q * 18.0 + seed));
		col = mix(col, crust, 0.9 * ulcer);
		gloss = 0.5 * (1.0 - ulcer) + 0.3 * ridge;
		return vec4(col, cover);
	} else if (kind == 1) {
		// --- SCC ---
		float rb = lz_border(q, seed, 0.14, 2.0);
		float x = d / rb;
		// An inflamed pink halo just outside the edge, fading out.
		float halo = (1.0 - smoothstep(1.0, 1.4, x)) * smoothstep(0.95, 1.05, x);
		float cover = 1.0 - smoothstep(rb - aa - 0.14, rb, d);
		vec3 base = mix(skin * vec3(1.0, 0.8, 0.78), vec3(0.78, 0.42, 0.4), 0.3 + 0.35 * lz_noise(q * 3.0 + seed));
		// The raised base slopes down to the skin: a little darker there.
		vec3 col = base * (1.0 - 0.12 * smoothstep(0.7, 1.0, x));
		// Keratin crust: thick, layered, yellow-tan to brown, with flakes.
		float crust = 1.0 - smoothstep(0.5, 0.72, x + 0.15 * lz_noise(q * 4.0 + seed));
		float tone = lz_fbm(q * 3.0 + seed);
		vec3 keratin = mix(vec3(0.86, 0.77, 0.58), vec3(0.52, 0.37, 0.23), smoothstep(0.3, 0.8, tone));
		keratin *= 0.85 + 0.3 * lz_noise(q * 22.0 + seed);  // fine scale
		// A few fissures (only where a second noise allows: not a jigsaw).
		float fissure = (1.0 - smoothstep(0.01, 0.035 + aa, abs(lz_noise(q * 4.5 + seed * 1.3) - 0.5)))
				* smoothstep(0.55, 0.7, lz_noise(q * 2.0 + seed + 13.0));
		keratin = mix(keratin, vec3(0.32, 0.2, 0.14), 0.55 * fissure);
		// Pale flakes lifting at the crust's edge.
		float flake = smoothstep(0.75, 0.9, lz_noise(q * 12.0 + seed + 3.0)) * smoothstep(0.35, 0.65, x);
		keratin = mix(keratin, vec3(0.95, 0.92, 0.84), 0.6 * flake);
		col = mix(col, keratin, crust);
		gloss = 0.3 * (1.0 - crust);  // the crust is dry and dull
		// The halo is outside the lesion proper: a partial cover of a pink tint.
		vec3 halo_col = skin * vec3(1.0, 0.8, 0.78);
		return vec4(mix(halo_col, col, cover), max(cover, 0.5 * halo));
	}
	// --- melanoma ---
	// Asymmetric: shifted a little, irregular border, blurred in places
	// (the edge width itself varies round the lesion).
	vec2 w = q + 0.12 * vec2(lz_noise(q * 1.3 + seed) - 0.5, lz_noise(q * 1.3 + seed + 9.0) - 0.5);
	float dw = length(w);
	float rb = lz_border(w, seed, 0.3, 1.6);
	float blur = 0.02 + 0.12 * lz_noise(normalize(w + vec2(1e-5)) * 2.5 + seed + 3.0);
	float cover = 1.0 - smoothstep(rb - aa - blur, rb, dw);
	float t = lz_fbm(q * 2.4 + seed);
	vec3 col = mix(vec3(0.62, 0.42, 0.28), vec3(0.34, 0.2, 0.12), smoothstep(0.3, 0.65, t));
	col = mix(col, vec3(0.13, 0.09, 0.09), smoothstep(0.62, 0.8, t));                     // black
	float blue = smoothstep(0.7, 0.82, lz_fbm(q * 1.8 + seed + 41.0)) * smoothstep(0.2, 0.5, dw);
	col = mix(col, vec3(0.34, 0.37, 0.46), 0.6 * blue);                                   // blue-grey veil
	float regress = smoothstep(0.72, 0.82, lz_fbm(q * 2.2 + seed + 77.0)) * smoothstep(0.15, 0.4, dw);
	col = mix(col, vec3(0.84, 0.72, 0.68), 0.6 * regress);                                // regression
	// Dark dots and globules, scattered unevenly.
	vec2 g = q * 7.0 + seed;
	vec2 cell = floor(g);
	vec2 at = cell + 0.25 + 0.5 * vec2(lz_hash(cell), lz_hash(cell + 7.7));
	float size = 0.06 + 0.14 * lz_hash(cell + 9.1);
	float glob = (1.0 - smoothstep(size, size + 0.06 + aa * 7.0, length(g - at))) * step(0.72, lz_hash(cell + 3.3));
	glob *= smoothstep(0.35, 0.65, t);  // mostly where the pigment is dark
	col = mix(col, vec3(0.16, 0.1, 0.08), 0.65 * glob * (1.0 - regress));
	// Lighter towards the blurred edge, where pigment thins out.
	col = mix(col, mix(skin, vec3(0.6, 0.42, 0.3), 0.55), smoothstep(rb - blur - 0.1, rb, dw) * 0.5);
	gloss = 0.05;
	return vec4(col, cover);
}

// All lesions at rest point p (mm): colour mixed over `col`; returns how much
// of the pixel is lesion. `grad`: added slope of the lesion's surface (per
// mm) for the normal; `gloss`: shininess there. aa_mm: one pixel in mm.
float lesions_apply(vec2 p, float aa_mm, inout vec3 col, inout vec2 grad, out float gloss) {
	gloss = 0.0;
	float total = 0.0;
	for (int i = 0; i < 8; i++) {
		if (i >= lesion_count) {
			break;
		}
		vec4 L = lesions[i];
		float r = max(L.z, 0.1);
		vec2 q = (p - L.xy) / r;
		if (dot(q, q) > 2.2) {
			continue;  // beyond any lesion's reach (SCC halo: 1.45 r)
		}
		int kind = int(L.w + 0.5);
		float seed = fract(sin(dot(L.xy, vec2(12.9898, 78.233))) * 43758.5453) * 97.0;
		float g;
		vec4 c = lz_color(kind, q, seed, col, aa_mm / r, g);
		col = mix(col, c.rgb, c.a);
		gloss = max(gloss, g * c.a);
		total = max(total, c.a);
		// Slope by finite differences (the height is smooth), in lesion
		// units -> per mm: the height is in units of r, so it cancels.
		if (c.a < 0.01 || kind == 2) {
			continue;  // no relief to speak of (outside; melanoma is flat)
		}
		float e = 0.02;
		float h0 = lz_height(kind, q, seed);
		vec2 s = vec2(lz_height(kind, q + vec2(e, 0.0), seed) - h0, lz_height(kind, q + vec2(0.0, e), seed) - h0) / e;
		grad += s * c.a;
	}
	return total;
}
