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
signal hold_all_changed(on: bool)
## A "Clear ..." button: what = "cuts", "sutures", "undermining", "lesions", "markings".
signal clear_requested(what: String)
signal pen_color_changed(index: int)
signal line_style_changed(index: int)
signal snap_changed(on: bool)
signal margin_changed(mm: float)
signal margin_style_changed(index: int)

## Matches main.gd's Tool enum (the numbers are used by --tool: only add at
## the end). "plan" tools are in the Plan mode's bar, the others in Operate.
const TOOLS := [
	{"name": "Grab", "icon": "grab", "key": "G", "plan": false,
		"how": "Drag the skin with forceps. The pull is spread over a small patch, like forceps jaws."},
	{"name": "Cut", "icon": "cut", "key": "C", "plan": false,
		"how": "Draw an incision; it is made when you lift. Start near an existing cut to join it."},
	{"name": "Suture", "icon": "suture", "key": "S", "plan": false,
		"how": "Press where the needle goes in, drag, lift where it comes out. Tap a suture to select it."},
	{"name": "Undermine", "icon": "undermine", "key": "U", "plan": false,
		"how": "Paint where the skin is freed from the tissue below; it then moves more easily."},
	{"name": "Lesion", "icon": "lesion", "key": "L", "plan": true,
		"how": "Tap to mark a lesion. Excise it with the Excise or Flap tool (Operate)."},
	{"name": "Excise", "icon": "excise", "key": "E", "plan": false,
		"how": "Drag from one tip of a fusiform excision to the other. Or tap inside an outline you cut, to remove it."},
	{"name": "Flap", "icon": "flap", "key": "F", "plan": false,
		"how": "Drag from a lesion outward, towards where the flap should come from. Release to cut and raise it."},
	{"name": "Pen", "icon": "mark", "key": "M", "plan": true,
		"how": "Draw freehand on the skin with a surgical marker. Markings belong to the skin: they move and stretch with it."},
	{"name": "Line", "icon": "line", "key": "I", "plan": true,
		"how": "Drag to draw a straight line. With Snap, it clicks to 15° steps."},
	{"name": "Rectangle", "icon": "rect", "key": "B", "plan": true,
		"how": "Drag from one corner to the opposite one."},
	{"name": "Circle", "icon": "circle", "key": "O", "plan": true,
		"how": "Drag from the centre outward."},
	{"name": "Ruler", "icon": "ruler", "key": "D", "plan": true,
		"how": "Drag to measure a distance; the length shows while you drag. To keep the labels (the length as drawn, and now once the skin has moved): View → Measurements."},
	{"name": "Angle", "icon": "angle", "key": "A", "plan": true,
		"how": "Drag the first arm out from the corner, then drag again for the second arm; the angle shows while you drag. To keep the labels (as drawn, and now): View → Measurements."},
	{"name": "Erase", "icon": "eraser", "key": "", "plan": true,
		"how": "Tap a marking, or rub across several, to erase them. Undo brings them back."},
	{"name": "Margin", "icon": "margin", "key": "N", "plan": true,
		"how": "Tap a lesion to draw its excision margin around it, or drag outward from it for any margin."},
]
## Margin tool: quick choices, mm.
const MARGINS := [2.0, 4.0, 5.0, 10.0, 20.0]
## The drawing tools of the Plan mode (they share colour, line style, snap).
const DRAW_TOOLS := [7, 8, 9, 10, 11, 12]
const LINE_STYLES := ["Solid", "Dashed", "Dotted"]
## Lesion types (SkinSheet.lesion_kind; drawn by looks/lesions.gdshaderinc).
const LESION_TYPES := [
	["BCC", "Nodular basal cell carcinoma: a pearly, translucent dome with a rolled edge and fine vessels (telangiectasia); larger ones ulcerate in the middle."],
	["SCC", "Squamous cell carcinoma: a keratin crust (yellow-white to brown, cracked) on a raised, inflamed pink base."],
	["Melanoma", "Melanoma: asymmetric, with an irregular, partly blurred border and several colours: brown, black, blue-grey, pale regression."],
]
## USP thread sizes offered (5 = 5-0).
const THREADS := [6, 5, 4, 3]
const TIGHTNESS_STEP := 0.05
const PEN_COLORS := [Color("#7b3fe4"), Color("#1f6feb"), Color("#1a1a1a")]
const PEN_NAMES := ["Violet", "Blue", "Black"]

var sheet: SkinSheet
## Phone layout: grab bar at the top of the sheet (app_shell.gd drives it).
var handle: Button
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
var _hooks_label: Label
var _hooks_shown := -1
var _lesion_labels: Array[Label] = []
var _flap_label: Label
var _flap_rows := {}  # prop -> ParamRow (shape rows shown only for the flaps they apply to)
var _flap_picker: OptionButton
var _flap_short: Label
var _flap_card: PanelContainer
var _flap_text := {}  # "moves", "uses", "design", "watch" -> Label
## Flap tool: draw the design with the marker instead of cutting it.
var flap_plan_only := false
var _pen_buttons: Array[Button] = []    # every drawing page's colour buttons, in turn
var _style_buttons: Array[Button] = []  # ...line style buttons
var _snap_switches: Array[CheckButton] = []
var _color_shown := 0
var _style_shown := 0
var _snap_shown := true


func setup(p_sheet: SkinSheet) -> void:
	sheet = p_sheet
	theme_type_variation = "Card"
	mouse_filter = Control.MOUSE_FILTER_STOP  # clicks here never reach the skin
	var outer := VBoxContainer.new()
	outer.add_theme_constant_override("separation", 4)
	add_child(outer)
	# On a phone the panel is a sheet pulled up from the bottom: this handle
	# (tap or drag) resizes it. Hidden on wide screens.
	handle = Button.new()
	handle.theme_type_variation = "FlatButton"
	handle.focus_mode = Control.FOCUS_NONE
	handle.icon = Style.icon("chevron_up")
	handle.expand_icon = true
	handle.icon_alignment = HORIZONTAL_ALIGNMENT_CENTER
	handle.add_theme_constant_override("icon_max_width", 18)
	handle.custom_minimum_size.y = 24
	handle.tooltip_text = "Show more / less"
	handle.visible = false
	outer.add_child(handle)
	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	outer.add_child(_scroll)
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
	for t in DRAW_TOOLS:
		_build_draw(_pages[t])
	_build_erase(_pages[13])
	_build_margin(_pages[14])
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
	page.add_child(Style.label("SKIN HOOKS", "SectionTitle"))
	page.add_child(Style.label("To hold the skin where it is, keep your finger still for a moment while grabbing: the ring fills and a hook is set. It stays when you let go, also while you suture. Drag a hook to move it; tap it to let go.",
		"Caption", true))
	var hold := HBoxContainer.new()
	var hl := Style.label("Every grab holds")
	hl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	hold.add_child(hl)
	var hold_switch := CheckButton.new()
	hold_switch.focus_mode = Control.FOCUS_NONE
	hold_switch.tooltip_text = "Each grab becomes a skin hook when you let go, without waiting."
	hold_switch.toggled.connect(func(on: bool): hold_all_changed.emit(on))
	hold.add_child(hold_switch)
	page.add_child(hold)
	_hooks_label = Style.label("", "Caption", true)
	page.add_child(_hooks_label)
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
	_section(page, "Type")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	var looks := Style.label(LESION_TYPES[0][1], "Caption", true)
	var kinds := _segment_row(LESION_TYPES.map(func(t): return t[0]), func(i: int):
		sheet.lesion_kind = i
		looks.text = LESION_TYPES[i][1])
	for b in kinds:
		row.add_child(b)
	kinds[clampi(sheet.lesion_kind, 0, kinds.size() - 1)].set_pressed_no_signal(true)
	page.add_child(row)
	page.add_child(looks)
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
	# The picker: every flap, grouped by how the skin moves.
	_flap_picker = OptionButton.new()
	_flap_picker.focus_mode = Control.FOCUS_NONE
	_flap_picker.custom_minimum_size.y = Style.TOUCH
	_flap_picker.fit_to_longest_item = false
	for fam in FlapCatalog.FAMILIES.size():
		_flap_picker.add_separator(FlapCatalog.FAMILIES[fam].to_upper())
		for f in FlapCatalog.FLAPS:
			if f.family == fam:
				_flap_picker.add_item(f.name, f.type)
	_flap_picker.item_selected.connect(func(index: int): _pick_flap(_flap_picker.get_item_id(index)))
	page.add_child(_flap_picker)
	_flap_short = Style.label("", "Caption", true)
	page.add_child(_flap_short)
	# The teaching card, folded away until asked for.
	var learn := _fold_button(page, "About this flap", "info")
	_flap_card = PanelContainer.new()
	_flap_card.theme_type_variation = "Inset"
	_flap_card.visible = false
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 4)
	_flap_card.add_child(v)
	var titles := {"moves": "HOW IT MOVES", "uses": "USED FOR", "design": "HOW TO DRAW IT", "watch": "WATCH FOR"}
	for key in titles:
		v.add_child(Style.label(titles[key], "SectionTitle"))
		var body := Style.label("", "Caption", true)
		v.add_child(body)
		_flap_text[key] = body
	page.add_child(_flap_card)
	learn.toggled.connect(func(on: bool): _flap_card.visible = on)

	_flap_rows["flap_margin"] = _row(page, {"prop": "flap_margin", "label": "Excision margin", "min": 0.0,
		"max": 10.0, "step": 0.5, "fmt": "%.1f mm",
		"tip": "Normal skin removed all round the lesion: the defect is the lesion plus this margin."})
	_flap_rows["flap_length_ratio"] = _row(page, {"prop": "flap_length_ratio", "label": "Length : width",
		"min": 0.5, "max": 6.0, "step": 0.1, "fmt": "%.1f : 1",
		"tip": "Advancement: a longer flap advances with less tension, but a long narrow flap risks the blood supply of its tip. V-Y: length of the V over the defect's width."})
	_flap_rows["flap_arc_deg"] = _row(page, {"prop": "flap_arc_deg", "label": "Arc", "min": 30.0, "max": 240.0,
		"step": 1.0, "fmt": "%.0f°", "tip": "Angle the arc sweeps around its centre. Longer arcs spread the tension."})
	_flap_rows["flap_rhombus_deg"] = _row(page, {"prop": "flap_rhombus_deg", "label": "Rhombus angle",
		"min": 50.0, "max": 90.0, "step": 1.0, "fmt": "%.0f°",
		"tip": "The defect's acute angle. 60° is Limberg's rhombus; the Dufourmentel design works for any."})
	_flap_rows["flap_z_angle_deg"] = _row(page, {"prop": "flap_z_angle_deg", "label": "Limb angle",
		"min": 30.0, "max": 90.0, "step": 5.0, "fmt": "%.0f°",
		"tip": "Angle between the lateral limbs and the central one. 60° is classic (about 75% longer)."})
	_flap_rows["flap_burow"] = _row(page, {"prop": "flap_burow", "label": "Burow's triangles", "type": "bool",
		"tip": "Excise a small triangle beside the end of each incision, so the longer outer lip has no dog-ear."})
	_flap_rows["flap_mirror"] = _row(page, {"prop": "flap_mirror", "label": "Mirror (other side)", "type": "bool",
		"tip": "Build the flap on the other side of the drag direction."})
	_row(page, {"prop": "flap_undermine_margin", "label": "Undermine beyond",
		"min": 0.0, "max": 30.0, "step": 0.5, "fmt": "%.1f mm",
		"tip": "The flaps and the defect are undermined, plus this much skin around them (island flaps keep their pedicle)."})
	var plan := HBoxContainer.new()
	var pl := Style.label("Plan with the marker")
	pl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	plan.add_child(pl)
	var plan_switch := CheckButton.new()
	plan_switch.focus_mode = Control.FOCUS_NONE
	plan_switch.toggled.connect(func(on: bool): flap_plan_only = on)
	plan.add_child(plan_switch)
	page.add_child(plan)
	page.add_child(Style.label("On: the design is drawn on the skin instead of cut. Then cut along it yourself (Cut tool) and remove the defect (Excise tool, tap inside).",
		"Caption", true))
	var tie := _button(page, "Tie key sutures", "suture", func(): tie_key_sutures_requested.emit(), "PrimaryButton")
	tie.tooltip_text = "Ties the last flap's key sutures (thread and tightness from the Suture tool): they pull the flap into place."
	_flap_label = Style.label("", "Caption", true)
	page.add_child(_flap_label)

	# Flaps this simulator does not design.
	var more := _fold_button(page, "Other local flaps", "chevron_down")
	var others := VBoxContainer.new()
	others.visible = false
	others.add_theme_constant_override("separation", 6)
	others.add_child(Style.label("Not simulated: interpolation flaps pass their pedicle over intact skin (a third dimension); the others are variants of the designs above.",
		"Caption", true))
	for o in FlapCatalog.OTHERS:
		var n := Style.label(o[0])
		n.add_theme_font_override("font", Style.FONT_SEMIBOLD)
		others.add_child(n)
		others.add_child(Style.label(o[1], "Caption", true))
	page.add_child(others)
	more.toggled.connect(func(on: bool): others.visible = on)
	show_flap_type(sheet.flap_type)


## A flat toggle button that folds a section open and closed.
func _fold_button(page: VBoxContainer, text: String, icon_name: String) -> Button:
	var b := Button.new()
	b.text = text
	b.theme_type_variation = "FlatButton"
	b.toggle_mode = true
	b.focus_mode = Control.FOCUS_NONE
	b.alignment = HORIZONTAL_ALIGNMENT_LEFT
	b.icon = Style.icon(icon_name)
	b.expand_icon = true
	b.add_theme_constant_override("icon_max_width", 16)
	b.custom_minimum_size.y = 36
	page.add_child(b)
	return b


## The picker chose a flap: set it, and start its length : width where that
## flap is usually drawn (the slider means different things per flap).
func _pick_flap(type: int) -> void:
	sheet.flap_type = type
	var f: Dictionary = FlapCatalog.by_type(type)
	if f.has("ratio"):
		sheet.flap_length_ratio = f.ratio
	refresh()


func show_flap_type(type: int) -> void:
	var f: Dictionary = FlapCatalog.by_type(type)
	for i in _flap_picker.item_count:
		if not _flap_picker.is_item_separator(i) and _flap_picker.get_item_id(i) == type:
			_flap_picker.select(i)
	_flap_short.text = f.short
	for key in _flap_text:
		_flap_text[key].text = f[key]
	for prop in ["flap_length_ratio", "flap_arc_deg", "flap_rhombus_deg", "flap_z_angle_deg", "flap_burow",
			"flap_mirror"]:
		_flap_rows[prop].visible = prop in f.shape
	_flap_rows["flap_margin"].visible = type != 10  # the Z-plasty removes no skin


## Colour, line style and snap: the same settings on every drawing tool's
## page (each page has its own buttons; show_plan_settings keeps them in step).
func _build_draw(page: VBoxContainer) -> void:
	_section(page, "Marker")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	var pens := _segment_row(PEN_NAMES, func(i: int):
		pen_color_changed.emit(i)
		show_plan_settings(i, _style_shown, _snap_shown))
	for i in pens.size():
		pens[i].icon = Style.icon("mark")
		pens[i].expand_icon = true
		pens[i].add_theme_constant_override("icon_max_width", 14)
		pens[i].add_theme_color_override("icon_normal_color", PEN_COLORS[i].lightened(0.3))
		pens[i].add_theme_color_override("icon_pressed_color", PEN_COLORS[i].lightened(0.3))
		row.add_child(pens[i])
	pens[0].set_pressed_no_signal(true)
	_pen_buttons.append_array(pens)
	page.add_child(row)
	var srow := HBoxContainer.new()
	srow.add_theme_constant_override("separation", 4)
	var styles := _segment_row(LINE_STYLES, func(i: int):
		line_style_changed.emit(i)
		show_plan_settings(_color_shown, i, _snap_shown))
	for b in styles:
		srow.add_child(b)
	styles[0].set_pressed_no_signal(true)
	_style_buttons.append_array(styles)
	page.add_child(srow)
	var snap := HBoxContainer.new()
	var sl := Style.label("Snap to 15°")
	sl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sl.tooltip_text = "Lines, rulers and angles click to 15° steps when close to one (30°, 45°, 60°, 90°...)."
	snap.add_child(sl)
	var sw := CheckButton.new()
	sw.focus_mode = Control.FOCUS_NONE
	sw.button_pressed = true
	sw.toggled.connect(func(on: bool):
		snap_changed.emit(on)
		show_plan_settings(_color_shown, _style_shown, on))
	snap.add_child(sw)
	_snap_switches.append(sw)
	page.add_child(snap)
	page.add_child(Style.label("Markings are for planning only: they don't change the skin. Erase them with the Erase tool, or undo.",
		"Caption", true))
	_button(page, "Clear markings", "trash", func(): clear_requested.emit("markings"))


func _build_margin(page: VBoxContainer) -> void:
	_section(page, "Margin")
	var mrow := HBoxContainer.new()
	mrow.add_theme_constant_override("separation", 4)
	var picks := _segment_row(MARGINS.map(func(m): return "%d" % m), func(i: int): margin_changed.emit(MARGINS[i]))
	for b in picks:
		mrow.add_child(b)
	picks[1].set_pressed_no_signal(true)  # 4 mm
	page.add_child(mrow)
	page.add_child(Style.label("mm of normal skin round the lesion. Drag outward from a lesion for any other margin (0.5 mm steps).",
		"Caption", true))
	_section(page, "Marker")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	var pens := _segment_row(PEN_NAMES, func(i: int):
		pen_color_changed.emit(i)
		show_plan_settings(i, _style_shown, _snap_shown))
	for i in pens.size():
		pens[i].icon = Style.icon("mark")
		pens[i].expand_icon = true
		pens[i].add_theme_constant_override("icon_max_width", 14)
		pens[i].add_theme_color_override("icon_normal_color", PEN_COLORS[i].lightened(0.3))
		pens[i].add_theme_color_override("icon_pressed_color", PEN_COLORS[i].lightened(0.3))
		row.add_child(pens[i])
	pens[0].set_pressed_no_signal(true)
	_pen_buttons.append_array(pens)
	page.add_child(row)
	var srow := HBoxContainer.new()
	srow.add_theme_constant_override("separation", 4)
	var styles := _segment_row(LINE_STYLES, func(i: int): margin_style_changed.emit(i))
	for b in styles:
		srow.add_child(b)
	styles[2].set_pressed_no_signal(true)  # margins: dotted unless chosen otherwise
	page.add_child(srow)


func _build_erase(page: VBoxContainer) -> void:
	_button(page, "Clear markings", "trash", func(): clear_requested.emit("markings"))


## Keep every drawing tool's page showing the same colour, style and snap.
func show_plan_settings(color: int, style: int, snap: bool) -> void:
	_color_shown = color
	_style_shown = style
	_snap_shown = snap
	for i in _pen_buttons.size():
		_pen_buttons[i].set_pressed_no_signal(i % PEN_NAMES.size() == color)
	for i in _style_buttons.size():
		_style_buttons[i].set_pressed_no_signal(i % LINE_STYLES.size() == style)
	for sw in _snap_switches:
		sw.set_pressed_no_signal(snap)


# --- updates from main.gd -------------------------------------------------

func show_tool(tool: int) -> void:
	for i in _pages.size():
		_pages[i].visible = i == tool
	_scroll.scroll_vertical = 0


## Pull every setting from the sheet (after a preset or keyboard change).
func refresh() -> void:
	for r in _rows:
		r.refresh()
	if _flap_picker != null:
		show_flap_type(sheet.flap_type)


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


## How many skin hooks hold the skin now.
func show_hooks(n: int) -> void:
	if n == _hooks_shown:
		return
	_hooks_shown = n
	_hooks_label.text = "No hooks." if n == 0 else ("1 hook holding the skin." if n == 1 else "%d hooks holding the skin." % n)


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
		var c: Dictionary = FlapCatalog.by_type(f.type)
		var kind: String = c.name.get_slice(" (", 0)
		var shape := ""
		match int(f.type):
			0, 3:
				shape = ", %.1f : 1" % f.ratio
			1, 7:
				shape = ", arc %.1f × width" % f.ratio
			10:
				shape = ", +%.0f%% length" % (f.lengthening * 100.0)
		var size := "" if f.type == 10 else ", %.0f mm defect" % f.diameter_mm
		# Base stress: a rough sign of how hard the closure pulls on the
		# pedicle; not a model of its blood supply. Island flaps have no base.
		var base := "" if f.type in [5, 6] else " Base stress %+.0f kPa." % f.base_stress_kpa
		lines.append("Flap %d: %s%s%s.%s" % [i, kind, size, shape, base])
	_flap_label.text = "\n".join(lines)
	_flap_label.visible = not lines.is_empty()
