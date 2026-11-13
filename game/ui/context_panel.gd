class_name ContextPanel
extends PanelContainer
## The settings and results for the current tool, and nothing else: picking
## the Suture tool shows thread, knot tightness and the selected suture;
## picking Flap shows the flap's type and shape. One "page" per tool, built
## once; switching tools only swaps which page is visible.
##
## Tissue properties live in the Tissue drawer (tissue_drawer.gd), view
## options in the top bar: they apply whatever the tool.

signal tightness_changed(tightness: float)
signal remove_suture_requested
signal adjust_tightness_requested(delta: float)
signal thread_changed(usp_size: int)
signal label_mode_changed(mode: int)
signal tie_key_sutures_requested
signal release_grabs_requested
## A "Clear ..." button: what = "cuts", "sutures", "undermining", "lesions", "markings".
signal clear_requested(what: String)
signal pen_color_changed(index: int)

## Matches main.gd's Tool enum.
const TOOLS := [
	{"name": "Grab", "icon": "grab", "key": "G",
		"how": "Drag the skin with forceps. The pull is spread over a small patch, like forceps jaws."},
	{"name": "Cut", "icon": "cut", "key": "C",
		"how": "Draw an incision; it is made when you lift. Start near an existing cut to join it."},
	{"name": "Suture", "icon": "suture", "key": "S",
		"how": "Press where the needle goes in, drag, lift where it comes out. Tap a suture to select it."},
	{"name": "Undermine", "icon": "undermine", "key": "U",
		"how": "Paint where the skin is freed from the tissue below; it then moves more easily."},
	{"name": "Lesion", "icon": "lesion", "key": "L",
		"how": "Tap to mark a lesion. Excise it with the Excise or Flap tool."},
	{"name": "Excise", "icon": "excise", "key": "E",
		"how": "Drag from one tip of a fusiform excision to the other. Or tap inside an outline you cut, to remove it."},
	{"name": "Flap", "icon": "flap", "key": "F",
		"how": "Drag from a lesion outward, towards where the flap should come from. Release to cut and raise it."},
	{"name": "Mark", "icon": "mark", "key": "M",
		"how": "Draw on the skin with a surgical marker to plan cuts and flaps. Lines move with the skin. Tap a line to erase it."},
]
## USP thread sizes offered (5 = 5-0).
const THREADS := [6, 5, 4, 3]
const TIGHTNESS_STEP := 0.05
const PEN_COLORS := [Color("#7b3fe4"), Color("#1f6feb"), Color("#1a1a1a")]
const PEN_NAMES := ["Violet", "Blue", "Black"]

var sheet: SkinSheet
## Knot tightness for new sutures (and the selected one).
var tightness := 0.0

var _pages: Array[Control] = []
var _rows: Array[ParamRow] = []
var _scroll: ScrollContainer
var _tightness_slider: HSlider
var _tightness_value: Label
var _thread_buttons: Array[Button] = []
var _label_buttons: Array[Button] = []
var _suture_card: PanelContainer
var _suture_title: Label
var _suture_detail: Label
var _load_bar: ProgressBar
var _suture_actions: Array[Button] = []
var _contact_label: Label
var _lesion_labels: Array[Label] = []
var _flap_label: Label
var _flap_rows := {}  # prop -> ParamRow (shown only for the flap types they apply to)
var _pen_buttons: Array[Button] = []


func setup(p_sheet: SkinSheet) -> void:
	sheet = p_sheet
	theme_type_variation = "Card"
	mouse_filter = Control.MOUSE_FILTER_STOP  # clicks here never reach the skin
	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(_scroll)
	var stack := VBoxContainer.new()
	stack.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var pad := MarginContainer.new()  # room for the scrollbar
	pad.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	pad.add_theme_constant_override("margin_right", 10)
	_scroll.add_child(pad)
	pad.add_child(stack)
	for i in TOOLS.size():
		var page := VBoxContainer.new()
		page.add_theme_constant_override("separation", 12)
		page.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_header(page, TOOLS[i])
		stack.add_child(page)
		_pages.append(page)
	_build_grab(_pages[0])
	_build_cut(_pages[1])
	_build_suture(_pages[2])
	_build_undermine(_pages[3])
	_build_lesion(_pages[4])
	_build_excise(_pages[5])
	_build_flap(_pages[6])
	_build_mark(_pages[7])
	show_thread(sheet.suture_thread)
	show_tool(0)


# --- building blocks ------------------------------------------------------

func _header(page: VBoxContainer, t: Dictionary) -> void:
	var row := HBoxContainer.new()
	var icon := TextureRect.new()
	icon.texture = Style.icon(t.icon)
	icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	icon.custom_minimum_size = Vector2(22, 22)
	icon.modulate = Style.ACCENT
	row.add_child(icon)
	row.add_child(Style.label(t.name, "Title"))
	page.add_child(row)
	page.add_child(Style.label(t.how, "Caption", true))


func _section(page: VBoxContainer, title: String) -> void:
	var l := Style.label(title.to_upper(), "SectionTitle")
	page.add_child(l)


func _row(page: VBoxContainer, spec: Dictionary) -> ParamRow:
	var r := ParamRow.make(sheet, spec)
	page.add_child(r)
	_rows.append(r)
	return r


func _button(page: Container, text: String, icon_name: String, on_press: Callable,
		variation := "") -> Button:
	var b := Button.new()
	b.text = text
	if icon_name != "":
		b.icon = Style.icon(icon_name)
		b.expand_icon = true
		b.add_theme_constant_override("icon_max_width", 18)
	b.theme_type_variation = variation
	b.focus_mode = Control.FOCUS_NONE
	b.custom_minimum_size.y = 40
	b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	b.pressed.connect(on_press)
	page.add_child(b)
	return b


func _segment_row(options: Array, on_pick: Callable) -> Array[Button]:
	var buttons: Array[Button] = []
	var group := ButtonGroup.new()
	for i in options.size():
		var b := Button.new()
		b.text = options[i]
		b.theme_type_variation = "SegmentButton"
		b.toggle_mode = true
		b.button_group = group
		b.focus_mode = Control.FOCUS_NONE
		b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		b.custom_minimum_size.y = 36
		b.pressed.connect(func(): on_pick.call(i))
		buttons.append(b)
	return buttons


# --- pages ----------------------------------------------------------------

func _build_grab(page: VBoxContainer) -> void:
	_row(page, {"prop": "grab_radius", "label": "Forceps size", "min": 0.0, "max": 10.0, "step": 0.1,
		"fmt": "%.1f mm", "tip": "Radius of the patch of skin the pull is spread over. Applies to the next grab. 0 = a single point."})
	_row(page, {"prop": "max_pull_force", "label": "Strongest pull", "min": 0.1, "max": 50.0, "step": 0.1,
		"exp": true, "fmt": "%.1f N", "tip": "The forceps never pull harder than this: beyond it the skin lags behind your finger (shown in orange)."})
	_row(page, {"prop": "grab_stiffness", "label": "Grip firmness", "min": 0.1, "max": 100.0, "step": 0.1,
		"exp": true, "fmt": "%.1f N/mm", "tip": "How tightly the skin follows the finger, until the strongest pull is reached."})
	_button(page, "Release all", "close", func(): release_grabs_requested.emit())


func _build_cut(page: VBoxContainer) -> void:
	page.add_child(Style.label("On a computer, Shift + drag cuts with any tool.", "Caption", true))
	_button(page, "Clear incisions", "trash", func(): clear_requested.emit("cuts"))


func _build_suture(page: VBoxContainer) -> void:
	_section(page, "Thread")
	var threads := HBoxContainer.new()
	threads.add_theme_constant_override("separation", 4)
	var names: Array = []
	for t in THREADS:
		names.append("%d-0" % t)
	_thread_buttons = _segment_row(names, func(i: int): thread_changed.emit(THREADS[i]))
	for b in _thread_buttons:
		threads.add_child(b)
	page.add_child(threads)
	var strengths: Array = []
	for t in THREADS:
		strengths.append("%d-0: %.1f N" % [t, SkinSheet.thread_strength(t)])
	page.add_child(Style.label("Breaks at (knot-pull): " + ", ".join(strengths), "Caption", true))

	var head := HBoxContainer.new()
	var name_label := Style.label("Knot tightness")
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(name_label)
	_tightness_value = Style.label("+0%", "Value")
	head.add_child(_tightness_value)
	page.add_child(head)
	_tightness_slider = HSlider.new()
	_tightness_slider.step = 0.01
	_tightness_slider.min_value = -0.5
	_tightness_slider.max_value = 0.6
	_tightness_slider.value = 0.0
	_tightness_slider.custom_minimum_size.y = 28
	_tightness_slider.focus_mode = Control.FOCUS_NONE
	_tightness_slider.value_changed.connect(func(t: float):
		tightness = t
		_tightness_value.text = "%+.0f%%" % (t * 100.0)
		tightness_changed.emit(t))
	page.add_child(_tightness_slider)
	page.add_child(Style.label("0% = the edges just meet. Tighter squeezes the tissue in the loop; below 0 leaves it loose.",
		"Caption", true))
	_row(page, {"prop": "sutures_can_snap", "label": "Threads can snap", "type": "bool",
		"tip": "A suture whose thread is pulled harder than its knot-pull strength breaks. Off: it holds, flashing OVER."})

	# The selected suture.
	_suture_card = PanelContainer.new()
	_suture_card.theme_type_variation = "Inset"
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 6)
	_suture_card.add_child(v)
	_suture_title = Style.label("", "")
	_suture_title.add_theme_font_override("font", Style.FONT_SEMIBOLD)
	v.add_child(_suture_title)
	_load_bar = ProgressBar.new()
	_load_bar.min_value = 0.0
	_load_bar.max_value = 1.0
	_load_bar.show_percentage = false
	_load_bar.custom_minimum_size.y = 8
	_load_bar.add_theme_stylebox_override("background", Style.box(Color("#323a47"), 4, Color(0, 0, 0, 0), 0, 0))
	v.add_child(_load_bar)
	_suture_detail = Style.label("", "Caption", true)
	v.add_child(_suture_detail)
	var actions := HBoxContainer.new()
	actions.add_theme_constant_override("separation", 6)
	_suture_actions = [
		_button(actions, "Loosen", "", func(): adjust_tightness_requested.emit(-TIGHTNESS_STEP)),
		_button(actions, "Tighten", "", func(): adjust_tightness_requested.emit(TIGHTNESS_STEP)),
		_button(actions, "Cut", "scissors", func(): remove_suture_requested.emit()),
	]
	_suture_actions[0].tooltip_text = "Re-tie 5% looser [-]"
	_suture_actions[1].tooltip_text = "Re-tie 5% tighter [+]"
	_suture_actions[2].tooltip_text = "Cut the suture out [Del]; undo ties it again"
	v.add_child(actions)
	page.add_child(_suture_card)

	_section(page, "Labels on the skin")
	var labels := HBoxContainer.new()
	labels.add_theme_constant_override("separation", 4)
	_label_buttons = _segment_row(["Smart", "All", "None"], func(i: int): label_mode_changed.emit(i))
	for b in _label_buttons:
		labels.add_child(b)
	page.add_child(labels)
	page.add_child(Style.label("Smart: the selected suture, any near breaking point, and the one under the pointer. [T]",
		"Caption", true))
	_label_buttons[0].set_pressed_no_signal(true)
	_contact_label = Style.label("", "Caption", true)
	page.add_child(_contact_label)
	_button(page, "Clear sutures", "trash", func(): clear_requested.emit("sutures"))
	show_selected_suture({})


func _build_undermine(page: VBoxContainer) -> void:
	_row(page, {"prop": "undermine_radius", "label": "Brush size", "min": 1.0, "max": 30.0, "step": 0.5,
		"fmt": "%.1f mm", "tip": "Radius of the undermining brush."})
	_row(page, {"prop": "undermined_tethering_pct", "label": "Attachment left", "min": 0.0, "max": 100.0,
		"step": 1.0, "fmt": "%.0f %%", "tip": "How much attachment to the tissue below is left where you undermined, % of normal."})
	_button(page, "Clear undermining", "trash", func(): clear_requested.emit("undermining"))


func _build_lesion(page: VBoxContainer) -> void:
	_row(page, {"prop": "lesion_diameter", "label": "Lesion diameter", "min": 0.5, "max": 40.0, "step": 0.5,
		"fmt": "%.1f mm", "tip": "Size of the next lesion you mark."})
	var l := Style.label("", "Caption", true)
	page.add_child(l)
	_lesion_labels.append(l)
	_button(page, "Clear lesions", "trash", func(): clear_requested.emit("lesions"))


func _build_excise(page: VBoxContainer) -> void:
	_row(page, {"prop": "ellipse_ratio", "label": "Length : width", "min": 1.0, "max": 6.0, "step": 0.1,
		"fmt": "%.1f : 1", "tip": "Shape of the fusiform excision. 3 : 1 is classic; shorter ellipses leave dog-ears."})
	_row(page, {"prop": "tip_angle_deg", "label": "Tip angle", "min": 10.0, "max": 120.0, "step": 1.0,
		"fmt": "%.0f°", "tip": "Angle at each end of the ellipse. About 30° is classic."})
	page.add_child(Style.label("Tip: draw it along the RSTL (the compass at the top right).", "Caption", true))
	var l := Style.label("", "Caption", true)
	page.add_child(l)
	_lesion_labels.append(l)


func _build_flap(page: VBoxContainer) -> void:
	_row(page, {"prop": "flap_type", "label": "Flap", "type": "segment",
		"options": ["Advance", "Rotate", "Rhomboid"],
		"tip": "Advancement slides forward into a square defect. Rotation swings around into a triangular one. Rhomboid (Limberg) jumps across into a 60/120° rhombus."})
	_row(page, {"prop": "flap_margin", "label": "Excision margin", "min": 0.0, "max": 10.0, "step": 0.5,
		"fmt": "%.1f mm", "tip": "Normal skin removed all round the lesion: the defect is the lesion plus this margin."})
	_flap_rows["flap_length_ratio"] = _row(page, {"prop": "flap_length_ratio", "label": "Length : width",
		"min": 0.5, "max": 6.0, "step": 0.1, "fmt": "%.1f : 1",
		"tip": "A longer flap advances with less tension, but a long narrow flap risks the blood supply of its tip."})
	_flap_rows["flap_arc_deg"] = _row(page, {"prop": "flap_arc_deg", "label": "Arc", "min": 30.0, "max": 240.0,
		"step": 1.0, "fmt": "%.0f°", "tip": "Angle swept by the arc around the triangle's apex. Longer arcs spread the tension."})
	_flap_rows["flap_mirror"] = _row(page, {"prop": "flap_mirror", "label": "Mirror (other side)", "type": "bool",
		"tip": "Build the flap on the other side of the drag direction."})
	_row(page, {"prop": "flap_undermine_margin", "label": "Undermine beyond", "min": 0.0, "max": 30.0,
		"step": 0.5, "fmt": "%.1f mm", "tip": "The flap and defect are undermined, plus this much skin around them."})
	var tie := _button(page, "Tie key sutures", "suture", func(): tie_key_sutures_requested.emit(), "PrimaryButton")
	tie.tooltip_text = "Ties the last flap's key sutures (thread and tightness from the Suture tool): they pull the flap into place."
	_flap_label = Style.label("", "Caption", true)
	page.add_child(_flap_label)


func _build_mark(page: VBoxContainer) -> void:
	_section(page, "Marker")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	_pen_buttons = _segment_row(PEN_NAMES, func(i: int): pen_color_changed.emit(i))
	for i in _pen_buttons.size():
		_pen_buttons[i].icon = Style.icon("mark")
		_pen_buttons[i].expand_icon = true
		_pen_buttons[i].add_theme_constant_override("icon_max_width", 14)
		_pen_buttons[i].add_theme_color_override("icon_normal_color", PEN_COLORS[i].lightened(0.3))
		_pen_buttons[i].add_theme_color_override("icon_pressed_color", PEN_COLORS[i].lightened(0.3))
		row.add_child(_pen_buttons[i])
	_pen_buttons[0].set_pressed_no_signal(true)
	page.add_child(row)
	page.add_child(Style.label("Markings are for planning only: they don't change the skin.", "Caption", true))
	_button(page, "Clear markings", "trash", func(): clear_requested.emit("markings"))


# --- updates from main.gd -------------------------------------------------

func show_tool(tool: int) -> void:
	for i in _pages.size():
		_pages[i].visible = i == tool
	_scroll.scroll_vertical = 0


## Pull every setting from the sheet (after a preset or keyboard change).
func refresh() -> void:
	for r in _rows:
		r.refresh()


func _process(_delta: float) -> void:
	if sheet == null:
		return
	# Only the shape setting that applies to the chosen flap type.
	_flap_rows["flap_length_ratio"].visible = sheet.flap_type == 0
	_flap_rows["flap_arc_deg"].visible = sheet.flap_type == 1
	_flap_rows["flap_mirror"].visible = sheet.flap_type != 0


func show_suture_tightness(t: float) -> void:
	tightness = t
	_tightness_slider.set_value_no_signal(t)
	_tightness_value.text = "%+.0f%%" % (t * 100.0)


## Set the tightness for the next suture (and the selected one), as the slider would.
func set_tightness(t: float) -> void:
	_tightness_slider.value = t


func show_thread(usp_size: int) -> void:
	var i := THREADS.find(usp_size)
	for k in _thread_buttons.size():
		_thread_buttons[k].set_pressed_no_signal(k == i)


func show_label_mode(mode: int) -> void:
	for k in _label_buttons.size():
		_label_buttons[k].set_pressed_no_signal(k == mode)


func show_selected_suture(info: Dictionary) -> void:
	for b in _suture_actions:
		b.disabled = info.is_empty()
	if info.is_empty():
		_suture_title.text = "No suture selected"
		_suture_detail.text = "Tap a suture to select it, then loosen, tighten or cut it."
		_load_bar.visible = false
		return
	_load_bar.visible = true
	var load: float = info.load if info.strength_n > 0.0 else 0.0
	_load_bar.value = clampf(load, 0.0, 1.0)
	var c := Style.OK if load < 0.5 else (Style.WARN if load < 0.9 else Style.DANGER)
	_load_bar.add_theme_stylebox_override("fill", Style.box(c, 4, Color(0, 0, 0, 0), 0, 0))
	if info.slack:
		_suture_title.text = "Suture #%d · %d-0 · slack" % [info.id, info.thread]
	else:
		_suture_title.text = "Suture #%d · %d-0 · %.0f%% of breaking" % [info.id, info.thread, load * 100.0]
	_suture_detail.text = "Closing force %.2f N (thread %.2f N each way). Loop stress %+.0f kPa. Tied at %+.0f%%." % [
		info.force, info.tension_n, info.loop_stress_kpa, info.tightness * 100.0]


func show_contact(info: Dictionary) -> void:
	if not sheet.lip_contact:
		_contact_label.text = "Lip contact is off (Tissue → Advanced): wound lips can overlap."
	elif info.pairs == 0:
		_contact_label.text = ""
	else:
		_contact_label.text = "Lips pressed together (blue): up to %.0f kPa, %.2f N in total." % [
			info.max_pressure_kpa, info.force_n]


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
	for label in _lesion_labels:
		label.text = "\n".join(lines)
		label.visible = not lines.is_empty()


func show_flaps(info: Array) -> void:
	var lines: Array[String] = []
	var i := 0
	for f in info:
		i += 1
		var kind: String = ["Advancement", "Rotation", "Rhomboid"][f.type]
		var shape := ""
		if f.type == 0:
			shape = ", %.1f : 1" % f.ratio
		elif f.type == 1:
			shape = ", arc %.1f × width" % f.ratio
		# Base stress: a rough sign of how hard the closure pulls on the
		# pedicle; not a model of its blood supply.
		lines.append("Flap %d: %s, %.0f mm defect%s. Base stress %+.0f kPa." % [
			i, kind, f.diameter_mm, shape, f.base_stress_kpa])
	_flap_label.text = "\n".join(lines)
	_flap_label.visible = not lines.is_empty()
