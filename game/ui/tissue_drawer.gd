class_name TissueDrawer
extends PanelContainer
## The skin's properties: body-site presets first, then the few settings
## that change how the skin behaves in plain words (tension lines, how
## stretched and mobile it is), and every raw model parameter under
## "Advanced" for research use.
##
## Presets are JSON files (format in res://presets/README.md):
##   res://presets/*.json   built in, shipped with the app (read-only)
##   user://presets/*.json  saved by the user ("user://" is the app's private
##                          data folder: %APPDATA%/Godot/app_userdata/skin-sim
##                          on Windows, browser storage on the web)

signal closed
signal preset_applied(name: String)

const BUILTIN_PRESET_DIR := "res://presets"
const USER_PRESET_DIR := "user://presets"
const PRESET_FORMAT_VERSION := 1
## Which SkinSheet properties a preset stores: the tissue, not tool settings.
const PRESET_KEYS := [
	"shear_modulus_kpa", "stiffening", "thickness", "tether_stiffness_kpa_per_mm", "pin_boundary",
	"fibre_angle_deg", "fibre_stiffness_kpa", "fibre_nonlinearity", "fibre_dispersion",
	"prestretch_along_pct", "prestretch_across_pct", "undermined_tethering_pct", "wrinkling_pct",
]

const BASIC := [
	{"prop": "fibre_angle_deg", "label": "Tension lines (RSTL) direction", "min": -90.0, "max": 90.0, "step": 1.0,
		"fmt": "%.0f°", "tip": "Direction of the relaxed skin tension lines (0° = horizontal). Incisions along them gape least and heal best."},
	{"prop": "prestretch_along_pct", "label": "Natural tension along the lines", "min": 0.0, "max": 30.0, "step": 0.5,
		"fmt": "%.1f %%", "tip": "Living skin is stretched beyond its relaxed size. This makes incisions gape and excised skin shrink."},
	{"prop": "prestretch_across_pct", "label": "Natural tension across the lines", "min": 0.0, "max": 30.0, "step": 0.5,
		"fmt": "%.1f %%", "tip": "Usually less than along the lines."},
	{"prop": "tether_stiffness_kpa_per_mm", "label": "Attachment to the tissue below", "min": 0.0, "max": 20.0,
		"step": 0.01, "fmt": "%.2f kPa/mm", "tip": "Low = mobile skin that slides easily over the tissue below (like the back of the hand); high = tethered (like the nose tip)."},
	{"prop": "thickness", "label": "Dermis thickness", "min": 0.2, "max": 6.0, "step": 0.05, "fmt": "%.2f mm",
		"tip": "Thicker dermis carries more force: the skin feels stiffer."},
	{"prop": "wrinkling_pct", "label": "Buckles into wrinkles", "min": 0.0, "max": 99.0, "step": 1.0,
		"fmt": "%.0f %%", "tip": "Squeezed skin buckles out of the plane (dog-ears) instead of pushing back like a solid block. 0 = flat sheet."},
]
const ADVANCED := [
	{"section": "Material model"},
	{"prop": "shear_modulus_kpa", "label": "Shear modulus μ", "min": 1.0, "max": 1000.0, "step": 0.5, "exp": true,
		"fmt": "%.0f kPa", "tip": "Small-strain stiffness of the dermis (Demiray matrix)."},
	{"prop": "stiffening", "label": "Strain stiffening b", "min": 0.0, "max": 30.0, "step": 0.1, "fmt": "%.1f",
		"tip": "How early the skin 'locks' as its collagen straightens. 0 = Neo-Hookean (no lock)."},
	{"prop": "fibre_stiffness_kpa", "label": "Fibre stiffness k1", "min": 0.0, "max": 500.0, "step": 0.5,
		"fmt": "%.0f kPa", "tip": "Gasser-Ogden-Holzapfel fibres along the RSTL: how strongly they resist stretch. 0 = no fibres."},
	{"prop": "fibre_nonlinearity", "label": "Fibre stiffening k2", "min": 0.0, "max": 30.0, "step": 0.1,
		"fmt": "%.1f", "tip": "How steeply the fibres stiffen as they straighten."},
	{"prop": "fibre_dispersion", "label": "Fibre dispersion κ", "min": 0.0, "max": 0.333, "step": 0.005,
		"fmt": "%.3f", "tip": "0 = all fibres along the RSTL; 0.333 = fibres in every direction (isotropic)."},
	{"prop": "pin_boundary", "label": "Pin the outer edge", "type": "bool",
		"tip": "Hold the sheet's outer edge fixed: the skin continues beyond it."},
	{"section": "Sutures and wound lips"},
	{"prop": "suture_stiffness", "label": "Suture stiffness", "min": 0.1, "max": 200.0, "step": 0.1, "exp": true,
		"fmt": "%.1f N/mm", "tip": "Stiffness of a thread plus the tissue it bites."},
	{"prop": "suture_strip_width", "label": "Bite strip width", "min": 0.1, "max": 6.0, "step": 0.1,
		"fmt": "%.1f mm", "tip": "Width of the strip of dermis each bite pulls on (needle hole to wound edge)."},
	{"prop": "lip_contact", "label": "Wound lips touch", "type": "bool",
		"tip": "The lips of incisions press on each other instead of passing through (pressed lips turn blue)."},
	{"section": "Solver"},
	{"prop": "iterations_per_frame", "label": "Iterations per frame", "min": 1.0, "max": 30.0, "step": 1.0,
		"fmt": "%d", "tip": "More = faster settling, more CPU per frame."},
]

var sheet: SkinSheet
var _rows: Array[ParamRow] = []
var _presets: Array[Dictionary] = []
var _preset_box: VBoxContainer
var _preset_buttons: Array[Button] = []
var _preset_name: LineEdit
var _status: Label
var _advanced: VBoxContainer
var current_preset := "Default skin"


func setup(p_sheet: SkinSheet) -> void:
	sheet = p_sheet
	theme_type_variation = "Card"
	mouse_filter = Control.MOUSE_FILTER_STOP
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(scroll)
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 12)
	var pad := MarginContainer.new()  # room for the scrollbar
	pad.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	pad.add_theme_constant_override("margin_right", 10)
	scroll.add_child(pad)
	pad.add_child(box)

	var head := HBoxContainer.new()
	var title := Style.label("Tissue", "Title")
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(title)
	var close := Style.icon_button("close", "", "Close [Esc]")
	close.pressed.connect(func(): closed.emit())
	head.add_child(close)
	box.add_child(head)
	box.add_child(Style.label("Illustrative values, not calibrated against measurements.", "Caption", true))

	box.add_child(Style.label("BODY SITE", "SectionTitle"))
	_preset_box = VBoxContainer.new()
	_preset_box.add_theme_constant_override("separation", 6)
	box.add_child(_preset_box)

	box.add_child(Style.label("HOW THIS SKIN BEHAVES", "SectionTitle"))
	for spec in BASIC:
		_add_row(box, spec)

	var adv := Button.new()
	adv.text = "Advanced (model parameters)"
	adv.theme_type_variation = "FlatButton"
	adv.icon = Style.icon("chevron_down")
	adv.expand_icon = true
	adv.add_theme_constant_override("icon_max_width", 16)
	adv.alignment = HORIZONTAL_ALIGNMENT_LEFT
	adv.toggle_mode = true
	adv.focus_mode = Control.FOCUS_NONE
	box.add_child(adv)
	_advanced = VBoxContainer.new()
	_advanced.add_theme_constant_override("separation", 12)
	_advanced.visible = false
	adv.toggled.connect(func(on: bool):
		_advanced.visible = on
		adv.icon = Style.icon("chevron_up" if on else "chevron_down"))
	box.add_child(_advanced)
	for spec in ADVANCED:
		if spec.has("section"):
			_advanced.add_child(Style.label(spec.section.to_upper(), "SectionTitle"))
		else:
			_add_row(_advanced, spec)

	box.add_child(Style.label("SAVE THESE SETTINGS", "SectionTitle"))
	var save_row := HBoxContainer.new()
	_preset_name = LineEdit.new()
	_preset_name.placeholder_text = "Name, e.g. Forearm"
	_preset_name.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_preset_name.custom_minimum_size.y = 40
	save_row.add_child(_preset_name)
	var save := Style.icon_button("save", "Save", "Save as a preset", "")
	save.custom_minimum_size = Vector2(90, 40)
	save.pressed.connect(_save_preset)
	save_row.add_child(save)
	box.add_child(save_row)
	_status = Style.label("", "Caption", true)
	box.add_child(_status)
	_reload_presets()


func _add_row(parent: VBoxContainer, spec: Dictionary) -> void:
	var r := ParamRow.make(sheet, spec)
	parent.add_child(r)
	_rows.append(r)


func refresh() -> void:
	for r in _rows:
		r.refresh()


# --- presets --------------------------------------------------------------

func _reload_presets() -> void:
	_presets.clear()
	for dir in [BUILTIN_PRESET_DIR, USER_PRESET_DIR]:
		if not DirAccess.dir_exists_absolute(dir):
			continue
		var files := DirAccess.get_files_at(dir)
		files.sort()
		for file in files:
			if file.get_extension() != "json":
				continue
			var preset := _load_preset_file(dir.path_join(file))
			if not preset.is_empty():
				preset["user"] = dir == USER_PRESET_DIR
				_presets.append(preset)
	for b in _preset_buttons:
		b.queue_free()
	_preset_buttons.clear()
	var group := ButtonGroup.new()
	for i in _presets.size():
		var p := _presets[i]
		# A card-like button: the site's name, and its description below.
		var b := Button.new()
		b.theme_type_variation = "SegmentButton"
		b.toggle_mode = true
		b.button_group = group
		b.focus_mode = Control.FOCUS_NONE
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		b.text = p.name + ("  · saved" if p.user else "")
		b.tooltip_text = p.description
		b.custom_minimum_size.y = 40
		b.set_pressed_no_signal(p.name == current_preset)
		b.pressed.connect(func(): _apply_preset(i))
		_preset_box.add_child(b)
		_preset_buttons.append(b)


## Read and validate one preset file. Returns {} if it is unusable, so a
## broken file can never crash the app or inject unexpected properties.
func _load_preset_file(path: String) -> Dictionary:
	var data = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(data) != TYPE_DICTIONARY or not data.has("params") or typeof(data.params) != TYPE_DICTIONARY:
		push_warning("Ignoring malformed preset: " + path)
		return {}
	var clean := {}
	for key in data.params:
		if key in PRESET_KEYS:
			clean[key] = data.params[key]
		else:
			push_warning("Preset %s: unknown parameter '%s' ignored" % [path, key])
	return {
		"name": str(data.get("name", path.get_file().get_basename())),
		"description": str(data.get("description", "")),
		"params": clean,
	}


func _apply_preset(i: int) -> void:
	var p: Dictionary = _presets[i]
	for key in p.params:
		sheet.set(key, p.params[key])
	current_preset = p.name
	refresh()
	_status.text = p.description
	preset_applied.emit(p.name)


## Apply a preset by name (scenarios, command line). False if not found.
func apply_preset_named(preset_name: String) -> bool:
	for i in _presets.size():
		if _presets[i].name == preset_name:
			_preset_buttons[i].set_pressed_no_signal(true)
			_apply_preset(i)
			return true
	return false


func _save_preset() -> void:
	var preset_name := _preset_name.text.strip_edges()
	if preset_name.is_empty():
		_status.text = "Type a name first."
		return
	# Keep file names portable: letters, digits, dash and underscore only.
	var file_name := ""
	for ch in preset_name.to_lower():
		file_name += ch if (ch >= "a" and ch <= "z") or (ch >= "0" and ch <= "9") or ch in "-_" else "_"
	var params := {}
	for key in PRESET_KEYS:
		params[key] = sheet.get(key)
	var data := {
		"format_version": PRESET_FORMAT_VERSION,
		"name": preset_name,
		"description": "Saved " + Time.get_datetime_string_from_system(false, true),
		"params": params,
	}
	DirAccess.make_dir_recursive_absolute(USER_PRESET_DIR)
	var path := USER_PRESET_DIR.path_join(file_name + ".json")
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		_status.text = "Could not save: error %d" % FileAccess.get_open_error()
		return
	f.store_string(JSON.stringify(data, "  "))
	f.close()
	current_preset = preset_name
	_reload_presets()
	_status.text = "Saved."
	preset_applied.emit(preset_name)
