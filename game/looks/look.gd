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

## Index = the value of --look and the View panel's switch.
const NAMES := ["Classic", "Illustration"]
const BLURBS := [
	"The plain working view: flat colours, no detail.",
	"Stylised realism, like a surgical atlas: soft light, the skin's layers in every cut, wrinkles as hatching.",
]

var index := 0
var name := ""
var skin_material: Material        # null = SkinSheet's plain skin
var wall_material: Material
var bed_material: Material
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


## Put this look on the scene.
func apply(sheet: SkinSheet, sun: DirectionalLight3D, env: Environment, overlay: Node) -> void:
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
