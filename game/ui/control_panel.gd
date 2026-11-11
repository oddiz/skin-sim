class_name ControlPanel
extends PanelContainer
## Side panel with live sliders for every simulation parameter, plus presets.
##
## The panel is built in code from the PARAMS table below rather than laid out
## by hand in the editor: adding a parameter is then one line in the table.
## Each entry names a SkinSheet property; the slider writes it directly and
## the solver relaxes to the new equilibrium on the next frame.
##
## Presets are JSON files (see res://presets/README.md for the format):
##   res://presets/*.json   built-in, shipped with the app (read-only)
##   user://presets/*.json  saved by the user ("user://" is the app's private
##                          data folder: %APPDATA%/Godot/app_userdata/skin-sim
##                          on Windows, the app sandbox on iOS)

## Emitted when the user presses a button that changes the scene.
signal reset_requested
## Emitted when the user picks a tool (main.gd's Tool enum: 0 = grab, 1 = cut).
signal tool_selected(tool: int)

const PANEL_WIDTH := 300
const BUILTIN_PRESET_DIR := "res://presets"
const USER_PRESET_DIR := "user://presets"
const PRESET_FORMAT_VERSION := 1

## Which SkinSheet properties a preset stores: the tissue, not the user's
## grab settings (those are personal preference, not anatomy).
const PRESET_KEYS := [
	"shear_modulus_kpa", "stiffening", "thickness", "tether_stiffness_kpa_per_mm", "pin_boundary",
]

## One row per control. "exp" = logarithmic slider, for values spanning decades.
const PARAMS := [
	{"section": "Tissue"},
	{"prop": "shear_modulus_kpa", "label": "Shear modulus μ", "min": 1.0, "max": 1000.0,
		"step": 0.5, "exp": true, "fmt": "%.0f kPa",
		"tip": "Small-strain stiffness of the dermis."},
	{"prop": "stiffening", "label": "Stiffening b", "min": 0.0, "max": 30.0, "step": 0.1,
		"fmt": "%.1f",
		"tip": "How early the skin 'locks' as collagen straightens. 0 = no lock (Neo-Hookean)."},
	{"prop": "thickness", "label": "Dermis thickness", "min": 0.2, "max": 6.0, "step": 0.05,
		"fmt": "%.2f mm", "tip": "Membrane forces scale with modulus x thickness."},
	{"prop": "tether_stiffness_kpa_per_mm", "label": "Tethering", "min": 0.0, "max": 20.0,
		"step": 0.01, "fmt": "%.2f kPa/mm",
		"tip": "Attachment to the underlying tissue. Low = mobile, undermined skin."},
	{"prop": "pin_boundary", "label": "Pin outer edge", "type": "bool",
		"tip": "Hold the sheet's outer edge fixed (skin continues beyond it)."},
	{"section": "Grab (forceps)"},
	{"prop": "grab_radius", "label": "Grab radius", "min": 0.0, "max": 10.0, "step": 0.1,
		"fmt": "%.1f mm", "tip": "Area the pull is spread over. Applies to the next grab."},
	{"prop": "max_pull_force", "label": "Max pull force", "min": 0.1, "max": 50.0, "step": 0.1,
		"exp": true, "fmt": "%.1f N", "tip": "Beyond this the skin lags behind the finger."},
	{"prop": "grab_stiffness", "label": "Grab stiffness", "min": 0.1, "max": 100.0, "step": 0.1,
		"exp": true, "fmt": "%.1f N/mm", "tip": "How tightly the skin follows the finger."},
	{"section": "Solver"},
	{"prop": "iterations_per_frame", "label": "Iterations / frame", "min": 1.0, "max": 30.0,
		"step": 1.0, "fmt": "%d", "tip": "More = faster settling, more CPU per frame."},
]

var sheet: SkinSheet

var _sliders := {}        # property name -> HSlider
var _value_labels := {}   # property name -> Label showing the value
var _checkboxes := {}     # property name -> CheckBox
var _presets: Array[Dictionary] = []
var _preset_list: OptionButton
var _preset_name: LineEdit
var _status: Label
var _tool_buttons: Array[Button] = []


## Called by main.gd once the sheet exists.
func setup(p_sheet: SkinSheet) -> void:
	sheet = p_sheet
	_build()
	_reload_presets()
	refresh()


func _ready() -> void:
	# Pin to the right edge, full height. Anchors are fractions of the parent
	# (here the window): left/right anchor 1 = right edge; offsets are pixels.
	set_anchors_and_offsets_preset(Control.PRESET_RIGHT_WIDE)
	offset_left = -PANEL_WIDTH
	mouse_filter = Control.MOUSE_FILTER_STOP  # clicks here never reach the skin


func _build() -> void:
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(scroll)
	# Padding inside the panel; the extra right margin keeps text clear of
	# the vertical scrollbar.
	var margin := MarginContainer.new()
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for side in ["left", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 10)
	margin.add_theme_constant_override("margin_right", 18)
	scroll.add_child(margin)
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 6)
	margin.add_child(box)

	_build_tools(box)
	_build_presets(box)
	for spec in PARAMS:
		if spec.has("section"):
			box.add_child(_section_label(spec.section))
		elif spec.get("type", "") == "bool":
			box.add_child(_make_checkbox(spec))
		else:
			box.add_child(_make_slider(spec))

	box.add_child(_section_label("Scene"))
	var row := HBoxContainer.new()
	var reset := Button.new()
	reset.text = "Reset skin"
	reset.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	reset.pressed.connect(func(): reset_requested.emit())
	row.add_child(reset)
	var release := Button.new()
	release.text = "Release grabs"
	release.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	release.pressed.connect(func(): sheet.release_all_grabs())
	row.add_child(release)
	box.add_child(row)


## Tool choice (grab / cut) and incision management, at the top of the panel.
func _build_tools(box: VBoxContainer) -> void:
	box.add_child(_section_label("Tool"))
	var row := HBoxContainer.new()
	# A ButtonGroup makes toggle buttons mutually exclusive, like radio buttons.
	var group := ButtonGroup.new()
	for i in 2:
		var b := Button.new()
		b.text = ["Grab (forceps)", "Cut (scalpel)"][i]
		b.toggle_mode = true
		b.button_group = group
		b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		b.custom_minimum_size.y = 32  # comfortable touch target
		b.pressed.connect(func(): tool_selected.emit(i))
		row.add_child(b)
		_tool_buttons.append(b)
	_tool_buttons[0].button_pressed = true
	box.add_child(row)

	var cuts := HBoxContainer.new()
	var undo := Button.new()
	undo.text = "Undo cut"
	undo.tooltip_text = "Remove the most recent incision [Z]."
	undo.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	undo.pressed.connect(func(): sheet.undo_cut())
	cuts.add_child(undo)
	var clear := Button.new()
	clear.text = "Clear cuts"
	clear.tooltip_text = "Remove all incisions (the deformation is kept)."
	clear.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	clear.pressed.connect(func(): sheet.clear_cuts())
	cuts.add_child(clear)
	box.add_child(cuts)


## Show which tool is active (when it was changed by keyboard, not by a button).
func show_tool(tool: int) -> void:
	_tool_buttons[tool].set_pressed_no_signal(true)


func _section_label(text: String) -> Label:
	var l := Label.new()
	l.text = text.to_upper()
	l.add_theme_color_override("font_color", Color(0.95, 0.75, 0.6))
	return l


func _make_slider(spec: Dictionary) -> Control:
	var v := VBoxContainer.new()
	v.tooltip_text = spec.get("tip", "")
	var header := HBoxContainer.new()
	var name_label := Label.new()
	name_label.text = spec.label
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(name_label)
	var value_label := Label.new()
	header.add_child(value_label)
	v.add_child(header)

	var slider := HSlider.new()
	slider.min_value = spec.min
	slider.max_value = spec.max
	slider.step = spec.step
	slider.exp_edit = spec.get("exp", false)  # logarithmic: equal slider travel per decade
	slider.custom_minimum_size.y = 24  # comfortable touch target
	var prop: String = spec.prop
	# A lambda (an inline, unnamed function) that remembers ("captures") `prop`
	# and `spec`, so each slider writes its own property.
	var on_change := func(value: float) -> void:
		sheet.set(prop, value)
		_update_value_label(spec, value)
	slider.value_changed.connect(on_change)
	v.add_child(slider)

	_sliders[prop] = slider
	_value_labels[prop] = value_label
	return v


func _make_checkbox(spec: Dictionary) -> Control:
	var cb := CheckBox.new()
	cb.text = spec.label
	cb.tooltip_text = spec.get("tip", "")
	var prop: String = spec.prop
	cb.toggled.connect(func(on: bool): sheet.set(prop, on))
	_checkboxes[prop] = cb
	return cb


func _update_value_label(spec: Dictionary, value: float) -> void:
	var fmt: String = spec.fmt
	_value_labels[spec.prop].text = fmt % (int(value) if fmt.contains("%d") else value)


## Pull current values from the sheet into the controls, without triggering
## their change signals (which would write the same values back).
func refresh() -> void:
	for spec in PARAMS:
		if not spec.has("prop"):
			continue
		var value = sheet.get(spec.prop)
		if spec.get("type", "") == "bool":
			_checkboxes[spec.prop].set_pressed_no_signal(value)
		else:
			_sliders[spec.prop].set_value_no_signal(value)
			_update_value_label(spec, value)


# ---------------------------------------------------------------------------
# Presets
# ---------------------------------------------------------------------------

func _build_presets(box: VBoxContainer) -> void:
	box.add_child(_section_label("Preset"))
	_preset_list = OptionButton.new()
	_preset_list.item_selected.connect(_on_preset_selected)
	box.add_child(_preset_list)

	var save_row := HBoxContainer.new()
	_preset_name = LineEdit.new()
	_preset_name.placeholder_text = "name for current settings"
	_preset_name.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	save_row.add_child(_preset_name)
	var save := Button.new()
	save.text = "Save"
	save.pressed.connect(_save_preset)
	save_row.add_child(save)
	box.add_child(save_row)

	_status = Label.new()
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.add_theme_font_size_override("font_size", 12)
	_status.add_theme_color_override("font_color", Color(0.75, 0.75, 0.75))
	box.add_child(_status)


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

	_preset_list.clear()
	_preset_list.add_item("(choose a preset)")
	for p in _presets:
		_preset_list.add_item(p.name + ("  [saved]" if p.user else ""))


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


func _on_preset_selected(index: int) -> void:
	if index == 0:
		return
	var p: Dictionary = _presets[index - 1]
	for key in p.params:
		sheet.set(key, p.params[key])
	refresh()
	_status.text = p.description


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
	_reload_presets()
	_status.text = "Saved to " + ProjectSettings.globalize_path(path)
