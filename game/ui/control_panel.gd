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
## Emitted when the user picks a tool (main.gd's Tool enum: 0 grab, 1 cut, 2 suture, 3 undermine,
## 4 lesion, 5 excise).
signal tool_selected(tool: int)
## Undo the last cut or suture (main.gd keeps the history).
signal undo_requested
## Knot tightness slider moved (applies to the selected suture, and to new ones).
signal tightness_changed(tightness: float)
signal remove_suture_requested
## Loosen (< 0) or tighten (> 0) the selected suture by this much tightness.
signal adjust_tightness_requested(delta: float)
## Thread size picked (USP: 5 = 5-0); applies to new sutures and the selected one.
signal thread_changed(usp_size: int)
## Which suture labels to draw: 0 smart, 1 all, 2 none.
signal label_mode_changed(mode: int)
signal cuts_cleared
signal lesions_cleared
signal undermining_cleared
signal sutures_cleared

const PANEL_WIDTH := 300
const BUILTIN_PRESET_DIR := "res://presets"
const USER_PRESET_DIR := "user://presets"
const PRESET_FORMAT_VERSION := 1

## Which SkinSheet properties a preset stores: the tissue, not the user's
## grab settings (those are personal preference, not anatomy).
const PRESET_KEYS := [
	"shear_modulus_kpa", "stiffening", "thickness", "tether_stiffness_kpa_per_mm", "pin_boundary",
	"fibre_angle_deg", "fibre_stiffness_kpa", "fibre_nonlinearity", "fibre_dispersion",
	"prestretch_along_pct", "prestretch_across_pct", "undermined_tethering_pct", "wrinkling_pct",
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
	{"prop": "wrinkling_pct", "label": "Wrinkling", "min": 0.0, "max": 99.0, "step": 1.0,
		"fmt": "%.0f %%",
		"tip": "How much of its resistance to squeezing the skin loses by buckling into wrinkles (dog-ears). 0 = flat sheet that pushes back like a solid block. Tension is not affected."},
	{"section": "Collagen fibres (RSTL)"},
	{"prop": "fibre_angle_deg", "label": "RSTL direction", "min": -90.0, "max": 90.0, "step": 1.0,
		"fmt": "%.0f deg", "tip": "Direction of the relaxed skin tension lines (0 = horizontal)."},
	{"prop": "fibre_stiffness_kpa", "label": "Fibre stiffness k1", "min": 0.0, "max": 500.0,
		"step": 0.5, "fmt": "%.0f kPa", "tip": "How strongly the aligned collagen resists stretch. 0 = no fibres."},
	{"prop": "fibre_nonlinearity", "label": "Fibre stiffening k2", "min": 0.0, "max": 30.0,
		"step": 0.1, "fmt": "%.1f", "tip": "How steeply the fibres stiffen as they straighten."},
	{"prop": "fibre_dispersion", "label": "Fibre dispersion", "min": 0.0, "max": 0.333,
		"step": 0.005, "fmt": "%.3f",
		"tip": "0 = all fibres along the RSTL; 0.333 = fibres in every direction (isotropic)."},
	{"section": "In-vivo prestretch"},
	{"prop": "prestretch_along_pct", "label": "Along RSTL", "min": 0.0, "max": 30.0, "step": 0.5,
		"fmt": "%.1f %%", "tip": "How much living skin is stretched beyond its unloaded size. Makes incisions gape."},
	{"prop": "prestretch_across_pct", "label": "Across RSTL", "min": 0.0, "max": 30.0, "step": 0.5,
		"fmt": "%.1f %%", "tip": "Usually less than along the RSTL."},
	{"section": "Excision"},
	{"prop": "lesion_diameter", "label": "Lesion diameter", "min": 0.5, "max": 40.0, "step": 0.5,
		"fmt": "%.1f mm", "tip": "Size of the next lesion marked with the Lesion tool."},
	{"prop": "ellipse_ratio", "label": "Ellipse length : width", "min": 1.0, "max": 6.0, "step": 0.1,
		"fmt": "%.1f : 1", "tip": "Fusiform excision shape. 3 : 1 is classic; shorter leaves dog-ears."},
	{"prop": "tip_angle_deg", "label": "Tip angle", "min": 10.0, "max": 120.0, "step": 1.0,
		"fmt": "%.0f deg", "tip": "Angle at each end of the fusiform excision. About 30 deg is classic."},
	{"section": "Undermining"},
	{"prop": "undermine_radius", "label": "Brush radius", "min": 1.0, "max": 30.0, "step": 0.5,
		"fmt": "%.1f mm", "tip": "Size of the undermining brush."},
	{"prop": "undermined_tethering_pct", "label": "Tethering left", "min": 0.0, "max": 100.0,
		"step": 1.0, "fmt": "%.0f %%", "tip": "Attachment left in undermined skin, % of normal."},
	{"section": "Grab (forceps)"},
	{"prop": "grab_radius", "label": "Grab radius", "min": 0.0, "max": 10.0, "step": 0.1,
		"fmt": "%.1f mm", "tip": "Area the pull is spread over. Applies to the next grab."},
	{"prop": "max_pull_force", "label": "Max pull force", "min": 0.1, "max": 50.0, "step": 0.1,
		"exp": true, "fmt": "%.1f N", "tip": "Beyond this the skin lags behind the finger."},
	{"prop": "grab_stiffness", "label": "Grab stiffness", "min": 0.1, "max": 100.0, "step": 0.1,
		"exp": true, "fmt": "%.1f N/mm", "tip": "How tightly the skin follows the finger."},
	{"section": "Sutures"},
	{"prop": "suture_stiffness", "label": "Suture stiffness", "min": 0.1, "max": 200.0,
		"step": 0.1, "exp": true, "fmt": "%.1f N/mm",
		"tip": "How firmly a suture holds the edges together. The force it carries is the closure tension."},
	{"prop": "suture_strip_width", "label": "Bite strip width", "min": 0.1, "max": 6.0, "step": 0.1,
		"fmt": "%.1f mm",
		"tip": "Width of the strip of dermis each bite pulls on (needle hole to wound edge)."},
	{"prop": "sutures_can_snap", "label": "Sutures can snap", "type": "bool",
		"tip": "A suture whose thread is pulled harder than its knot-pull strength breaks. Off: it holds, drawn red with its overload shown."},
	{"prop": "lip_contact", "label": "Lip contact", "type": "bool",
		"tip": "The lips of incisions press on each other instead of passing through. Lips pressed together turn blue."},
	{"section": "Heatmap"},
	{"prop": "heatmap", "label": "Show", "type": "enum",
		"options": ["Off (skin)", "Max principal stress", "Von Mises stress", "Area change",
			"Min principal stress", "Excess skin (wrinkles)"],
		"tip": "Colour the skin by a result. Max principal: red = tension, blue = compression. Min principal: blue where the skin is squeezed in some direction. Excess skin: how much extra skin is bunched into wrinkles, where real skin puckers (dog-ears). [H]"},
	{"prop": "heatmap_range_kpa", "label": "Stress scale", "min": 1.0, "max": 2000.0, "step": 1.0,
		"exp": true, "fmt": "%.0f kPa", "tip": "Stress at the end of the colour scale."},
	{"section": "Solver"},
	{"prop": "iterations_per_frame", "label": "Iterations / frame", "min": 1.0, "max": 30.0,
		"step": 1.0, "fmt": "%d", "tip": "More = faster settling, more CPU per frame."},
]

var sheet: SkinSheet

var _sliders := {}        # property name -> HSlider
var _value_labels := {}   # property name -> Label showing the value
var _checkboxes := {}     # property name -> CheckBox
var _enums := {}          # property name -> OptionButton
var _presets: Array[Dictionary] = []
var _preset_list: OptionButton
var _preset_name: LineEdit
var _status: Label
var _tool_buttons: Array[Button] = []
## Knot tightness for new sutures: 0 = edges just meet, +0.2 = tied 20%
## shorter (squeezing the tissue in the loop), negative = loose.
var tightness := 0.0
var _tightness_slider: HSlider
var _tightness_label: Label
var _suture_label: Label
var _contact_label: Label
var _lesion_label: Label
var _remove_suture: Button
var _loosen: Button
var _tighten: Button
var _thread: OptionButton
var _label_mode: OptionButton
## USP sizes offered, finest first (7 = 7-0).
const THREADS := [7, 6, 5, 4, 3, 2]
## One press of Loosen / Tighten changes the knot tightness by this much.
const TIGHTNESS_STEP := 0.05


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
		elif spec.get("type", "") == "enum":
			box.add_child(_make_enum(spec))
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
	for i in 6:
		if i == 3:  # second row
			box.add_child(row)
			row = HBoxContainer.new()
		var b := Button.new()
		b.text = ["Grab", "Cut", "Suture", "Undermine", "Lesion", "Excise"][i]
		b.add_theme_font_size_override("font_size", 13)
		b.tooltip_text = ["Forceps: drag the skin [G]", "Scalpel: draw an incision [C]",
			"Drag from needle entry to exit; tap a suture to select it [S]",
			"Brush: free the skin from the tissue below [U]",
			"Tap to mark a lesion [L]",
			"Drag tip to tip: fusiform excision. Tap inside a closed outline: remove it [E]"][i]
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
	undo.text = "Undo"
	undo.tooltip_text = "Undo the last incision or suture [Z]."
	undo.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	undo.pressed.connect(func(): undo_requested.emit())
	cuts.add_child(undo)
	var clear := Button.new()
	clear.text = "Clear cuts"
	clear.tooltip_text = "Remove all incisions (the deformation and sutures are kept)."
	clear.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	clear.pressed.connect(func():
		sheet.clear_cuts()
		cuts_cleared.emit())
	cuts.add_child(clear)
	var clear_sutures := Button.new()
	clear_sutures.text = "Clear sutures"
	clear_sutures.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	clear_sutures.pressed.connect(func():
		sheet.clear_sutures()
		sutures_cleared.emit())
	cuts.add_child(clear_sutures)
	box.add_child(cuts)
	var undermine_row := HBoxContainer.new()
	var clear_undermining := Button.new()
	clear_undermining.text = "Clear undermining"
	clear_undermining.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	clear_undermining.pressed.connect(func():
		sheet.clear_undermining()
		undermining_cleared.emit())
	undermine_row.add_child(clear_undermining)
	var clear_lesions := Button.new()
	clear_lesions.text = "Clear lesions"
	clear_lesions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	clear_lesions.pressed.connect(func():
		sheet.clear_lesions()
		lesions_cleared.emit())
	undermine_row.add_child(clear_lesions)
	box.add_child(undermine_row)
	_lesion_label = Label.new()
	_lesion_label.add_theme_font_size_override("font_size", 12)
	_lesion_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_lesion_label)
	_build_knot(box)


## Suture controls: thread size, knot tightness, the selected suture
## (loosen / tighten / cut) and which labels to draw.
## Knot tightness: how short the thread is tied, relative to the length at
## which the wound edges just meet (the two bites added together).
func _build_knot(box: VBoxContainer) -> void:
	box.add_child(_section_label("Suture"))
	var thread_row := HBoxContainer.new()
	thread_row.tooltip_text = "Thread size (USP) for new sutures, and for the selected one. " 		+ "Finer threads break at lower tension (minimum knot-pull strength, USP Class I)."
	var thread_label := Label.new()
	thread_label.text = "Thread"
	thread_row.add_child(thread_label)
	_thread = OptionButton.new()
	_thread.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for size in THREADS:
		_thread.add_item("%d-0  (breaks at %.1f N)" % [size, SkinSheet.thread_strength(size)])
	_thread.select(THREADS.find(sheet.suture_thread))
	_thread.item_selected.connect(func(i: int): thread_changed.emit(THREADS[i]))
	thread_row.add_child(_thread)
	box.add_child(thread_row)

	var v := VBoxContainer.new()
	v.tooltip_text = "How tight the knot is tied. 0% = the edges just meet; " \
		+ "more = the loop is shorter than the two bites, squeezing the tissue; " \
		+ "less than 0 = loose, the wound stays partly open."
	var header := HBoxContainer.new()
	var name_label := Label.new()
	name_label.text = "Knot tightness"
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(name_label)
	_tightness_label = Label.new()
	header.add_child(_tightness_label)
	v.add_child(header)
	_tightness_slider = HSlider.new()
	# Step first: a Range snaps its value to min + n * step, and with the
	# default step of 1 setting min = -0.5 would snap the value 0 to 0.5.
	_tightness_slider.step = 0.01
	_tightness_slider.min_value = -0.5
	_tightness_slider.max_value = 0.6
	_tightness_slider.value = 0.0
	_tightness_slider.custom_minimum_size.y = 24
	_tightness_slider.value_changed.connect(func(t: float):
		tightness = t
		_tightness_label.text = "%+.0f%%" % (t * 100.0)
		tightness_changed.emit(t))
	v.add_child(_tightness_slider)
	box.add_child(v)
	_tightness_label.text = "+0%"

	_suture_label = Label.new()
	_suture_label.add_theme_font_size_override("font_size", 12)
	_suture_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_suture_label)
	var row := HBoxContainer.new()
	_loosen = Button.new()
	_loosen.text = "Loosen"
	_loosen.tooltip_text = "Re-tie the selected suture %d%% looser [-]." % int(TIGHTNESS_STEP * 100)
	_loosen.pressed.connect(func(): adjust_tightness_requested.emit(-TIGHTNESS_STEP))
	_tighten = Button.new()
	_tighten.text = "Tighten"
	_tighten.tooltip_text = "Re-tie the selected suture %d%% tighter [+]." % int(TIGHTNESS_STEP * 100)
	_tighten.pressed.connect(func(): adjust_tightness_requested.emit(TIGHTNESS_STEP))
	_remove_suture = Button.new()
	_remove_suture.text = "Cut"
	_remove_suture.tooltip_text = "Cut the selected suture out [Del]. Undo ties it again."
	_remove_suture.pressed.connect(func(): remove_suture_requested.emit())
	for b in [_loosen, _tighten, _remove_suture]:
		b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		b.custom_minimum_size.y = 30
		row.add_child(b)
	box.add_child(row)
	var labels_row := HBoxContainer.new()
	labels_row.tooltip_text = "Text on the sutures. Smart: only the selected suture, ones near " 		+ "breaking point, and the one under the mouse. Colour always shows the load [T]."
	var labels_label := Label.new()
	labels_label.text = "Labels"
	labels_row.add_child(labels_label)
	_label_mode = OptionButton.new()
	_label_mode.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for o in ["Smart", "All", "None"]:
		_label_mode.add_item(o)
	_label_mode.item_selected.connect(func(i: int): label_mode_changed.emit(i))
	labels_row.add_child(_label_mode)
	box.add_child(labels_row)
	_contact_label = Label.new()
	_contact_label.add_theme_font_size_override("font_size", 12)
	box.add_child(_contact_label)
	show_selected_suture({})


## Move the slider to a selected suture's tightness, without re-tying it.
func show_suture_tightness(t: float) -> void:
	tightness = t
	_tightness_slider.set_value_no_signal(t)
	_tightness_label.text = "%+.0f%%" % (t * 100.0)


## Set the tightness for the next suture (and the selected one), as the slider would.
func set_tightness(t: float) -> void:
	_tightness_slider.value = t


## Show the label mode (when it was changed by keyboard).
func show_label_mode(mode: int) -> void:
	_label_mode.select(mode)


## Show a thread size in the picker without emitting thread_changed.
func show_thread(usp_size: int) -> void:
	var i := THREADS.find(usp_size)
	if i != -1:
		_thread.select(i)


## Called every frame by main.gd with the selected suture's info ({} = none).
func show_selected_suture(info: Dictionary) -> void:
	for b in [_loosen, _tighten, _remove_suture]:
		b.disabled = info.is_empty()
	if info.is_empty():
		_suture_label.text = "No suture selected (tap one with the Suture tool)."
		return
	var text := "Suture #%d, %d-0: " % [info.id, info.thread]
	if info.slack:
		text += "slack"
	else:
		# Closing force = what pulls the edges together; the thread carries
		# half of it (the loop crosses the wound twice).
		text += "closing %.2f N (thread %.2f N, %.0f%% of breaking)" % [
			info.force, info.tension_n, info.load * 100.0]
	text += "
Loop stress %+.0f kPa" % info.loop_stress_kpa
	_suture_label.text = text


## Called every frame by main.gd with sheet.get_lesion_info().
func show_lesions(info: Array) -> void:
	var lines: Array[String] = []
	var i := 0
	for l in info:
		i += 1
		if not l.excised:
			lines.append("Lesion %d: in place." % i)
		elif l.complete:
			lines.append("Lesion %d: excised, margin %.1f mm." % [i, l.margin_mm])
		else:
			lines.append("Lesion %d: INCOMPLETE excision." % i)
	_lesion_label.text = "\n".join(lines)
	_lesion_label.visible = not lines.is_empty()


## Called every frame by main.gd with sheet.get_contact_info().
func show_contact(info: Dictionary) -> void:
	if not sheet.lip_contact:
		_contact_label.text = "Lip contact off: lips can overlap."
	elif info.pairs == 0:
		_contact_label.text = "Lips: not pressed together."
	else:
		_contact_label.text = "Lips pressed: max %.0f kPa, %.2f N" % [info.max_pressure_kpa, info.force_n]


## Show which tool is active (when it was changed by keyboard, not by a button).
func show_tool(tool: int) -> void:
	# set_pressed_no_signal bypasses the ButtonGroup, so release the others too.
	for i in _tool_buttons.size():
		_tool_buttons[i].set_pressed_no_signal(i == tool)


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
	slider.step = spec.step  # before min/max: see the note in _build_knot
	slider.min_value = spec.min
	slider.max_value = spec.max
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


func _make_enum(spec: Dictionary) -> Control:
	var h := HBoxContainer.new()
	h.tooltip_text = spec.get("tip", "")
	var l := Label.new()
	l.text = spec.label
	h.add_child(l)
	var ob := OptionButton.new()
	ob.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for o in spec.options:
		ob.add_item(o)
	var prop: String = spec.prop
	ob.item_selected.connect(func(i: int): sheet.set(prop, i))
	h.add_child(ob)
	_enums[prop] = ob
	return h


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
		elif spec.get("type", "") == "enum":
			_enums[spec.prop].select(value)  # select() does not emit item_selected
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
