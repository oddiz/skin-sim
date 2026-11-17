class_name Look
extends RefCounted
## A visual style for the skin, switched live from the View panel: the
## materials of the skin, the cut walls and the wound bed, the light, the
## background, and how the overlay draws sutures and markings. Nothing here
## changes the mechanics.
##
## The shaders live next to this file (skin_*.gdshader, walls.gdshader,
## bed.gdshader); they read the vertex data SkinSheet provides (see
## skin_common.gdshaderinc).

## Skin types for the Realistic look (Fitzpatrick I-VI: how skin reacts to
## sun, a proxy for its pigment): base tone, melanin colour, freckles.
const SKIN_TYPES := [
	[Color(0.95, 0.78, 0.70), Color(0.66, 0.47, 0.36), 0.35],
	[Color(0.88, 0.68, 0.57), Color(0.58, 0.39, 0.28), 0.25],
	[Color(0.80, 0.60, 0.45), Color(0.50, 0.33, 0.22), 0.08],
	[Color(0.66, 0.46, 0.32), Color(0.40, 0.25, 0.15), 0.0],
	[Color(0.46, 0.30, 0.20), Color(0.29, 0.17, 0.10), 0.0],
	[Color(0.30, 0.19, 0.13), Color(0.18, 0.11, 0.07), 0.0],
]

## Index = the value of --look and the View panel's switch.
const NAMES := ["Classic", "Illustration", "Realistic", "Game"]
const BLURBS := [
	"The plain working view: flat colours, no detail.",
	"Stylised realism, like a surgical atlas: soft light, the skin's layers in every cut, wrinkles as hatching.",
	"As under theatre lights: pores and skin lines, light glowing through the skin, wet cut surfaces, a drape, black nylon sutures.",
	"Bright and bold: cel shading, thick outlines, layer-cake cuts, glowing threads and neon marker, effects on.",
]

var index := 0
var name := ""
var skin_material: Material        # null = SkinSheet's plain skin
var wall_material: Material
var bed_material: Material
var drape_material: Material       # the plane around the skin; null = plain background
## The drape also covers the skin's margin (skin shader's window_half).
var drape_on_skin := false
## Bloom around bright parts.
var glow := false
## The Effects switch (View) is set to this when the look is picked.
var effects_default := false
## Anti-aliasing (smoothing of jagged edges). MSAA renders edges at 2x and
## costs a lot of GPU time on high-resolution screens (it doubled the
## Realistic look's cost at 4K); FXAA smooths the finished image for almost
## nothing. The shaders smooth their own lines, so FXAA is enough there.
var msaa := Viewport.MSAA_2X
var fxaa := false
## How much this look bleeds, 0..1, when Bleeding is on (View panel).
var bleed_scale := 0.0
var relief_mm := 1.5               # how strongly dog-ears and lips are shaded
var wall_width_mm := 0.8           # width of the cut face seen from above
var sun_rotation := Vector3(-1.2217305, 0.3490659, 0.0)
var sun_energy := 1.0
var sun_color := Color.WHITE
var ambient_energy := 0.35
var ambient_color := Color.WHITE
var background := Color(0.071, 0.078, 0.094)
var tonemap := Environment.TONE_MAPPER_LINEAR
## How the overlay draws sutures: "thread" null = coloured by load.
var overlay_style := {}

static var _noise: Texture2D
static var _cells: Texture2D


static func make(i: int) -> Look:
	var l := Look.new()
	l.index = clampi(i, 0, NAMES.size() - 1)
	l.name = NAMES[l.index]
	match l.index:
		1:
			l._illustration()
		2:
			l._realistic()
		3:
			l._game()
	return l


## Smooth noise and cellular noise, tileable, made once by the engine.
static func noise() -> Texture2D:
	if _noise == null:
		var n := FastNoiseLite.new()
		n.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
		n.frequency = 0.02
		n.fractal_octaves = 4
		var t := NoiseTexture2D.new()
		t.width = 256
		t.height = 256
		t.seamless = true
		t.generate_mipmaps = true
		t.noise = n
		_noise = t
	return _noise


static func cells() -> Texture2D:
	if _cells == null:
		var n := FastNoiseLite.new()
		n.noise_type = FastNoiseLite.TYPE_CELLULAR
		n.frequency = 0.03
		n.cellular_return_type = FastNoiseLite.RETURN_DISTANCE
		n.fractal_type = FastNoiseLite.FRACTAL_NONE
		var t := NoiseTexture2D.new()
		t.width = 256
		t.height = 256
		t.seamless = true
		t.generate_mipmaps = true
		t.normalize = true
		t.noise = n
		_cells = t
	return _cells


static func _shader_material(path: String) -> ShaderMaterial:
	var m := ShaderMaterial.new()
	m.shader = load(path)
	m.set_shader_parameter("noise_tex", noise())
	m.set_shader_parameter("cell_tex", cells())
	return m


func _illustration() -> void:
	skin_material = _shader_material("res://looks/skin_illustration.gdshader")
	wall_material = _shader_material("res://looks/walls.gdshader")
	bed_material = _shader_material("res://looks/bed.gdshader")
	relief_mm = 3.0
	wall_width_mm = 0.9
	sun_rotation = Vector3(deg_to_rad(-42.0), deg_to_rad(35.0), 0.0)
	sun_energy = 0.8
	sun_color = Color(1.0, 0.97, 0.92)
	ambient_energy = 0.3
	ambient_color = Color(0.85, 0.88, 1.0)
	background = Color(0.13, 0.125, 0.12)
	tonemap = Environment.TONE_MAPPER_FILMIC
	overlay_style = {"thread_width": 3.0, "outline": true, "knot": true}
	bleed_scale = 0.35


func _realistic() -> void:
	skin_material = _shader_material("res://looks/skin_realistic.gdshader")
	wall_material = _shader_material("res://looks/walls.gdshader")
	wall_material.set_shader_parameter("show_contact", 0.0)
	wall_material.set_shader_parameter("outline", 0.0)
	wall_material.set_shader_parameter("wetness", 0.7)
	wall_material.set_shader_parameter("epidermis", Color(0.62, 0.42, 0.36))
	wall_material.set_shader_parameter("dermis", Color(0.86, 0.55, 0.52))
	wall_material.set_shader_parameter("fat", Color(0.97, 0.80, 0.38))
	wall_material.set_shader_parameter("deep", Color(0.62, 0.12, 0.10))
	wall_material.set_shader_parameter("depth_shade", 0.5)
	bed_material = _shader_material("res://looks/bed.gdshader")
	bed_material.set_shader_parameter("wetness", 0.75)
	bed_material.set_shader_parameter("tissue", Color(0.66, 0.13, 0.11))
	bed_material.set_shader_parameter("tissue_dark", Color(0.36, 0.04, 0.05))
	bed_material.set_shader_parameter("fat_amount", 0.6)
	bed_material.set_shader_parameter("fat_islands", Color(0.95, 0.78, 0.38))
	drape_material = _shader_material("res://looks/drape.gdshader")
	drape_on_skin = true
	msaa = Viewport.MSAA_DISABLED
	fxaa = true
	relief_mm = 2.0
	wall_width_mm = 0.8
	# A theatre light: bright, slightly warm, from high up.
	sun_rotation = Vector3(deg_to_rad(-55.0), deg_to_rad(25.0), 0.0)
	sun_energy = 1.0
	sun_color = Color(1.0, 0.98, 0.95)
	ambient_energy = 0.35
	ambient_color = Color(0.9, 0.93, 1.0)
	background = Color(0.05, 0.12, 0.13)
	tonemap = Environment.TONE_MAPPER_FILMIC
	overlay_style = {"thread_width": 2.0, "thread_color": Color(0.04, 0.04, 0.05), "ink_bleed": true}
	bleed_scale = 1.0



func _game() -> void:
	skin_material = _shader_material("res://looks/skin_game.gdshader")
	wall_material = _shader_material("res://looks/walls.gdshader")
	wall_material.set_shader_parameter("epidermis", Color(0.95, 0.45, 0.42))
	wall_material.set_shader_parameter("dermis", Color(1.0, 0.78, 0.86))
	wall_material.set_shader_parameter("fat", Color(1.0, 0.86, 0.15))
	wall_material.set_shader_parameter("deep", Color(0.95, 0.12, 0.35))
	wall_material.set_shader_parameter("ink", Color(0.22, 0.04, 0.12))
	wall_material.set_shader_parameter("epidermis_end", 0.2)
	wall_material.set_shader_parameter("dermis_end", 0.6)
	wall_material.set_shader_parameter("outline_width", 0.12)
	wall_material.set_shader_parameter("depth_shade", 0.15)
	wall_material.set_shader_parameter("wetness", 0.4)
	bed_material = _shader_material("res://looks/bed.gdshader")
	bed_material.set_shader_parameter("tissue", Color(0.98, 0.16, 0.30))
	bed_material.set_shader_parameter("tissue_dark", Color(0.62, 0.03, 0.18))
	bed_material.set_shader_parameter("fat_amount", 0.12)
	bed_material.set_shader_parameter("wetness", 1.0)
	bed_material.set_shader_parameter("blood_color", Color(0.55, 0.0, 0.12))
	drape_material = _shader_material("res://looks/game_background.gdshader")
	relief_mm = 3.0
	wall_width_mm = 1.0
	sun_rotation = Vector3(deg_to_rad(-50.0), deg_to_rad(30.0), 0.0)
	sun_energy = 1.0
	background = Color(0.03, 0.04, 0.09)
	tonemap = Environment.TONE_MAPPER_FILMIC
	sun_energy = 0.8
	ambient_energy = 0.25
	# (No 3D glow: it bloomed the whole pale skin and costs on phones; the
	# threads' and markers' glow is drawn by the overlay.)
	effects_default = true
	overlay_style = {"thread_width": 3.0, "outline": true, "knot": true, "glow": true, "neon": true}
	bleed_scale = 1.0

## Put this look on the scene.
func apply(sheet: SkinSheet, sun: DirectionalLight3D, env: Environment, overlay: Node,
		drape: MeshInstance3D) -> void:
	sheet.skin_material = skin_material
	sheet.wall_material = wall_material
	sheet.bed_material = bed_material
	sheet.relief_mm = relief_mm
	sheet.wall_width_mm = wall_width_mm
	sun.rotation = sun_rotation
	sun.light_energy = sun_energy
	sun.light_color = sun_color
	env.background_color = background
	env.ambient_light_energy = ambient_energy
	env.ambient_light_color = ambient_color
	env.tonemap_mode = tonemap
	overlay.set("style", overlay_style)
	drape.visible = drape_material != null
	drape.material_override = drape_material
	env.glow_enabled = glow
	var vp := sheet.get_viewport()
	vp.msaa_3d = msaa
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_FXAA if fxaa else Viewport.SCREEN_SPACE_AA_DISABLED
	if glow:
		# Only what is brighter than white (highlights) blooms.
		env.glow_hdr_threshold = 0.95
		env.glow_intensity = 0.6
		env.glow_bloom = 0.0
		env.glow_blend_mode = Environment.GLOW_BLEND_MODE_SCREEN
	if drape_on_skin and skin_material is ShaderMaterial:
		# The opening in the drape: the skin minus a margin the cloth is glued on.
		var half := Vector2(sheet.width, sheet.height) * 0.5
		var margin := clampf(0.12 * minf(sheet.width, sheet.height), 4.0, 15.0)
		skin_material.set_shader_parameter("window_half", half - Vector2(margin, margin))


## Does this look's skin shader draw the lesions (lesions.gdshaderinc)? If
## not (Classic), the overlay draws them as discs.
func draws_lesions() -> bool:
	return skin_material is ShaderMaterial


## The lesions for the skin shader, from SkinSheet.get_lesion_info (rest
## centre, radius, kind). Up to 8 are drawn.
func set_lesions(info: Array) -> void:
	if not skin_material is ShaderMaterial:
		return
	var arr := PackedVector4Array()
	arr.resize(8)
	var n := mini(info.size(), 8)
	for i in n:
		var c: Vector2 = info[i].centre
		arr[i] = Vector4(c.x, c.y, info[i].radius, info[i].kind)
	skin_material.set_shader_parameter("lesions", arr)
	skin_material.set_shader_parameter("lesion_count", n)


## Skin type 0..5 (Fitzpatrick I-VI), for looks that have one (Realistic).
## Blanching and flushing scale with the tone: on dark skin a blanched patch
## goes greyer rather than white, and redness shows less.
func set_skin_type(i: int) -> void:
	if index != 2 or not skin_material is ShaderMaterial:
		return
	var t: Array = SKIN_TYPES[clampi(i, 0, SKIN_TYPES.size() - 1)]
	var tone: Color = t[0]
	var m := skin_material as ShaderMaterial
	m.set_shader_parameter("tone", tone)
	m.set_shader_parameter("melanin", t[1])
	m.set_shader_parameter("freckles", t[2])
	m.set_shader_parameter("haemoglobin", Color(tone.r * 0.98, tone.g * 0.62, tone.b * 0.66))
	m.set_shader_parameter("blanched", tone.lerp(Color(0.97, 0.90, 0.80), 0.25 + 0.35 * tone.get_luminance()))
	m.set_shader_parameter("undermined_tone", tone.lerp(Color(0.62, 0.48, 0.58), 0.5))
	# The epidermis seen in the cut face carries the pigment.
	if wall_material is ShaderMaterial:
		wall_material.set_shader_parameter("epidermis", tone.darkened(0.3))


## Bleeding, every frame: `level` 0..1 how much blood has come out so far,
## `fresh` 1 just cut .. 0 long ago (blood darkens as it clots). Blood only
## pools in the wound bed: none on the skin or the cut faces.
func update_wound(level: float, fresh: float) -> void:
	if bed_material is ShaderMaterial:
		bed_material.set_shader_parameter("fresh", fresh)
		bed_material.set_shader_parameter("blood", 0.9 * level * bleed_scale)
