class_name AppShell
extends Control
## The whole user interface around the skin, built in code. Two layouts,
## switched live as the window changes (a phone turning, a browser resized):
##
## Wide (a computer, a tablet on its side):
##   +------+----------------------------------------+-----------+
##   | tool | top bar: title  status  View  Tissue ? |  context  |
##   | rail |                                        |  panel    |
##   |      |               the skin                 | (current  |
##   |      |                                        |   tool)   |
##   | undo |          [ hint / messages ]           |           |
##   +------+----------------------------------------+-----------+
##
## Narrow (a phone upright): the skin fills the screen; the tools are a bar
## along the bottom, and the context panel is a sheet above it that is
## pulled up (tap or drag its handle): a peek of the tool's name, half the
## screen, or nearly all of it.
##   +-------------------------------+
##   | top bar                       |
##   |           the skin            |
##   |      [ hint / messages ]      |
##   |===== context sheet (peek) ====|
##   | tools (scroll sideways) | undo|
##   +-------------------------------+
##
## The shell itself lets clicks through (mouse_filter IGNORE): only its
## cards and buttons catch them, so touching the skin reaches main.gd.

signal tool_selected(tool: int)
## Plan | Operate switch (true = Plan).
signal mode_selected(plan: bool)
signal undo_requested
signal reset_requested
## Back to the original skin: no incisions, sutures, lesions, markings or hooks.
signal start_over_requested
signal fit_requested
signal look_changed(index: int)
signal effects_changed(on: bool)
signal skin_type_changed(index: int)
## Something was cleared from the menu or a panel: "cuts", "sutures",
## "undermining", "lesions" or "markings" (main.gd trims its undo history).
signal cleared(what: String)

const MAPS := [
	["Plain skin", "No colour map: just the skin and the wound."],
	["Tension", "Largest principal stress. Red = pulled, blue = squeezed (in the direction where that is largest). Strokes show which way the skin is pulled."],
	["Overall stress", "Von Mises stress: how much stress the skin carries, whatever its direction."],
	["Area change", "Red = skin stretched to a larger area, blue = compressed to a smaller one."],
	["Squeezing", "Smallest principal stress: blue where the skin is squeezed in some direction, where real skin puckers. Strokes show which way."],
	["Excess skin (dog-ears)", "How much extra skin is bunched into wrinkles: where dog-ears form."],
]
const SHORTCUTS := [
	["P", "Plan / Operate"],
	["G C S U E F", "Grab, Cut, Suture, Undermine, Excise, Flap"],
	["L M I B O D A N", "Lesion, Pen, Line, Rectangle, Circle, Ruler, Angle, Margin"],
	["Shift + drag", "Cut with any Operate tool"],
	["Wheel", "Zoom (around the pointer)"],
	["Right / middle drag", "Move the view"],
	["Home", "Fit the skin in the window"],
	["Z", "Undo"],
	["- / +", "Loosen / tighten the selected suture"],
	["Del", "Cut the selected suture out"],
	["T", "Suture labels: smart / all / none"],
	["H", "Next colour map"],
	["W", "Wireframe on / off"],
	["R", "Reset the skin (incisions and sutures stay)"],
	["Ctrl+R", "Start over: fresh skin, everything cleared (also the ↺ button)"],
	["Esc", "Close panels, deselect"],
	["?", "This list"],
]
## Below this width (logical pixels) the phone layout is used.
const NARROW_WIDTH := 820.0
const BAR_HEIGHT := 64.0
## Bottom sheet heights: peek (tool name only), half, full (fractions of the
## space above the tool bar; peek in pixels).
const SHEET_PEEK := 96.0
const SHEET_HALF := 0.45
const SHEET_FULL := 0.86

var sheet: SkinSheet
var context: ContextPanel
var drawer: TissueDrawer
## Phone layout in use.
var narrow := false
## View option (drawn by the overlay): tension direction strokes.
var show_tension_lines := true
## Blood from open incisions (looks that draw it).
var bleeding := true
## Load-coloured dots at the sutures' needle holes.
var show_suture_dots := false
## Labels on placed rulers and angles (off: only while drawing one).
var show_measurements := false
var _effects_switch: CheckButton
## Fitzpatrick skin type 0..5 (I-VI) for the Realistic look.
var skin_type := 1
var _skin_row: Control
var _skin_buttons: Array[Button] = []

var _rail: PanelContainer
var _rail_box: BoxContainer
var _tool_scroll: ScrollContainer
var _mode_box: VBoxContainer
var _mode_buttons: Array[Button] = []  # [Plan, Operate]
var _tool_box: BoxContainer
var _tool_buttons: Array[Button] = []
var _rail_spacer: Control
var _undo: Button
var _top: HBoxContainer
var _titles: VBoxContainer
var _subtitle: Label
var _status_dot: Label
var _view_button: Button
var _tissue_button: Button
var _help_button: Button
var _view_card: PanelContainer
var _map_buttons: Array[Button] = []
var _map_caption: Label
var _view_rows: Array[ParamRow] = []
var _view_content: VBoxContainer
var _look_buttons: Array[Button] = []
var _look_caption: Label
var _laid_out := false
var _range_row: ParamRow
var _stats_switch: CheckButton
var _menu: MenuButton
var _start_over: Button
var _start_over_armed_until := 0.0   # s: a second tap before this starts over
var _help_card: PanelContainer
var _stats_card: PanelContainer
var _stats_label: Label
var _hint_card: PanelContainer
var _hint_label: Label
var _toasts: VBoxContainer
var _drawer_open := false
var _sheet_state := 0          # bottom sheet: 0 peek, 1 half, 2 full
var _sheet_height := SHEET_PEEK
var _sheet_drag := false       # dragging the handle
var _sheet_drag_from := 0.0
var _sheet_height_from := 0.0
var _sheet_dragged := false


func setup(p_sheet: SkinSheet) -> void:
	sheet = p_sheet
	theme = Style.theme()
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_build_rail()
	_build_top_bar()
	context = ContextPanel.new()
	add_child(context)
	context.setup(sheet)
	context.clear_requested.connect(clear)
	context.handle.gui_input.connect(_on_handle_input)
	drawer = TissueDrawer.new()
	add_child(drawer)
	drawer.setup(sheet)
	drawer.visible = false
	drawer.closed.connect(func(): set_drawer_open(false))
	drawer.preset_applied.connect(func(n: String):
		_subtitle.text = n
		context.refresh())
	_subtitle.text = drawer.current_preset
	_build_view_card()
	_build_help()
	_build_stats()
	_build_hint()
	get_viewport().size_changed.connect(_layout)
	_layout()


# --- tool rail / bottom bar --------------------------------------------------

func _build_rail() -> void:
	_rail = PanelContainer.new()
	_rail.theme_type_variation = "Rail"
	_rail.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_rail)
	_rail_box = BoxContainer.new()
	_rail_box.vertical = true
	_rail_box.add_theme_constant_override("separation", 4)
	_rail.add_child(_rail_box)
	# Plan | Operate: which set of tools the bar shows. At the head of the
	# bar (top of the rail; left end on a phone, where it doesn't scroll away).
	_mode_box = VBoxContainer.new()
	_mode_box.add_theme_constant_override("separation", 2)
	_rail_box.add_child(_mode_box)
	var modes := ButtonGroup.new()
	for plan in [true, false]:
		var m := Button.new()
		m.text = "Plan" if plan else "Operate"
		m.tooltip_text = "Planning: mark, measure and draw on the skin; the skin can't be pulled [P]" if plan \
			else "Operating: grab, cut, suture, undermine, excise, raise flaps [P]"
		m.theme_type_variation = "SegmentButton"
		m.toggle_mode = true
		m.button_group = modes
		m.focus_mode = Control.FOCUS_NONE
		m.add_theme_font_size_override("font_size", 11)
		m.pressed.connect(func(): mode_selected.emit(plan))
		_mode_box.add_child(m)
		_mode_buttons.append(m)
	_mode_buttons[1].set_pressed_no_signal(true)
	# The tools scroll if they do not fit (sideways on a phone).
	_tool_scroll = ScrollContainer.new()
	_tool_scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tool_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_rail_box.add_child(_tool_scroll)
	_tool_box = BoxContainer.new()
	_tool_box.vertical = true
	_tool_box.add_theme_constant_override("separation", 4)
	_tool_scroll.add_child(_tool_box)
	var group := ButtonGroup.new()
	for i in ContextPanel.TOOLS.size():
		var t: Dictionary = ContextPanel.TOOLS[i]
		var b := _rail_button(t.icon, t.name, "%s [%s]" % [t.name, t.key])
		b.toggle_mode = true
		b.button_group = group
		b.pressed.connect(func(): tool_selected.emit(i))
		b.gui_input.connect(_tool_drag_input.bind(b))
		_tool_box.add_child(b)
		_tool_buttons.append(b)
		b.visible = not t.plan  # the app starts in Operate
	_rail_spacer = Control.new()
	_rail_spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_rail_box.add_child(_rail_spacer)
	_undo = _rail_button("undo", "Undo", "Undo [Z]")
	_undo.pressed.connect(func(): undo_requested.emit())
	_rail_box.add_child(_undo)
	_tool_buttons[0].set_pressed_no_signal(true)


## Swipe to scroll the tools, starting on a button.
## The buttons take the touch, so the ScrollContainer behind them never sees a
## drag that starts on one: on a phone, where the bar is nearly all buttons,
## it hardly scrolled. Each button forwards its drags here instead. Past a
## small dead zone the press is cancelled (so a swipe never switches tools)
## and the bar follows the finger; on release it coasts and slows down.
const TOOL_DRAG_DEADZONE := 10.0   # px before a press becomes a swipe
const TOOL_FLING_FRICTION := 6.0   # 1/s: how fast the coasting slows
var _tool_press_at := Vector2.ZERO
var _tool_scroll_at := Vector2.ZERO
var _tool_pressing := false
var _tool_swiping := false
var _tool_velocity := 0.0          # px/s along the bar, while coasting
var _tool_last_move := 0


func _tools_horizontal() -> bool:
	return _tool_scroll.horizontal_scroll_mode != ScrollContainer.SCROLL_MODE_DISABLED


func _tool_drag_input(event: InputEvent, b: Button) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			_tool_pressing = true
			_tool_swiping = false
			_tool_velocity = 0.0
			_tool_press_at = event.global_position
			_tool_scroll_at = Vector2(_tool_scroll.scroll_horizontal, _tool_scroll.scroll_vertical)
			_tool_last_move = Time.get_ticks_msec()
		else:
			_tool_pressing = false
			# A finger that stopped before lifting should not coast.
			if Time.get_ticks_msec() - _tool_last_move > 80:
				_tool_velocity = 0.0
			_tool_swiping = false
	elif event is InputEventMouseMotion and _tool_pressing:
		var d: Vector2 = event.global_position - _tool_press_at
		var along := d.x if _tools_horizontal() else d.y
		if not _tool_swiping and absf(along) > TOOL_DRAG_DEADZONE:
			_tool_swiping = true
			# Cancel the button's press: disabling resets it, so the release
			# will not select the tool under the finger.
			b.disabled = true
			b.disabled = false
		if _tool_swiping:
			_set_tool_scroll(_tool_scroll_at - d)
			var now := Time.get_ticks_msec()
			var step := maxf((now - _tool_last_move) / 1000.0, 1.0 / 240.0)
			var rel: Vector2 = event.relative
			var v := -(rel.x if _tools_horizontal() else rel.y) / step
			_tool_velocity = lerpf(_tool_velocity, v, 0.5)
			_tool_last_move = now


func _set_tool_scroll(at: Vector2) -> void:
	if _tools_horizontal():
		_tool_scroll.scroll_horizontal = int(at.x)
	else:
		_tool_scroll.scroll_vertical = int(at.y)


## Coasting after a fling (called from _process).
func _coast_tools(delta: float) -> void:
	if _tool_pressing:
		return
	if absf(_tool_velocity) < 20.0:
		_tool_velocity = 0.0
		return
	var move := _tool_velocity * delta
	if _tools_horizontal():
		var before := _tool_scroll.scroll_horizontal
		_tool_scroll.scroll_horizontal = before + int(round(move))
		if _tool_scroll.scroll_horizontal == before:
			_tool_velocity = 0.0  # hit the end
	else:
		var before := _tool_scroll.scroll_vertical
		_tool_scroll.scroll_vertical = before + int(round(move))
		if _tool_scroll.scroll_vertical == before:
			_tool_velocity = 0.0
	_tool_velocity *= exp(-TOOL_FLING_FRICTION * delta)


func _rail_button(icon_name: String, text: String, tip: String) -> Button:
	var b := Button.new()
	b.theme_type_variation = "FlatButton"
	b.icon = Style.icon(icon_name)
	b.text = text
	b.tooltip_text = tip
	b.expand_icon = false
	b.icon_alignment = HORIZONTAL_ALIGNMENT_CENTER
	b.vertical_icon_alignment = VERTICAL_ALIGNMENT_TOP
	b.add_theme_font_size_override("font_size", 11)
	b.add_theme_constant_override("icon_max_width", 22)
	b.custom_minimum_size = Vector2(Style.RAIL_WIDTH - 12, 54)
	b.focus_mode = Control.FOCUS_NONE
	return b


# --- top bar --------------------------------------------------------------

func _build_top_bar() -> void:
	_top = HBoxContainer.new()
	_top.add_theme_constant_override("separation", 6)
	_top.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_top)
	_titles = VBoxContainer.new()
	_titles.add_theme_constant_override("separation", 0)
	_titles.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_titles.add_child(Style.label("skin-sim", "Title"))
	_subtitle = Style.label("", "Caption")
	_titles.add_child(_subtitle)
	_top.add_child(_titles)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_top.add_child(spacer)
	_status_dot = Style.label("", "Caption")
	_status_dot.tooltip_text = "Settling: the skin is still moving towards equilibrium. At rest: it is in equilibrium."
	_status_dot.mouse_filter = Control.MOUSE_FILTER_PASS
	_top.add_child(_status_dot)
	# Start over: two taps (it cannot be undone), the first one says what it does.
	_start_over = Style.icon_button("reset", "", "Start over: fresh skin, no surgery [Ctrl+R]", "")
	_start_over.pressed.connect(_on_start_over_pressed)
	_top.add_child(_start_over)
	var fit := Style.icon_button("fit", "", "Fit the skin in the window [Home]", "")
	fit.pressed.connect(func(): fit_requested.emit())
	_top.add_child(fit)
	_view_button = Style.icon_button("heatmap", "View", "Colour map and display [H]", "")
	_view_button.toggle_mode = true
	_view_button.toggled.connect(func(on: bool):
		_view_card.visible = on
		if on:
			_view_card.move_to_front()  # above hints and messages
			_help_card.visible = false)
	_top.add_child(_view_button)
	_tissue_button = Style.icon_button("tissue", "Tissue", "Body site and skin properties", "")
	_tissue_button.pressed.connect(func(): set_drawer_open(not _drawer_open))
	_top.add_child(_tissue_button)
	_help_button = Style.icon_button("help", "", "Help and shortcuts [?]", "")
	_help_button.pressed.connect(toggle_help)
	_top.add_child(_help_button)
	_menu = MenuButton.new()
	_menu.icon = Style.icon("menu")
	_menu.expand_icon = true
	_menu.add_theme_constant_override("icon_max_width", 20)
	_menu.custom_minimum_size = Vector2(Style.TOUCH, Style.TOUCH)
	_menu.tooltip_text = "More"
	_menu.flat = false
	_menu.focus_mode = Control.FOCUS_NONE
	for s in ["normal", "hover", "pressed"]:
		_menu.add_theme_stylebox_override(s, Style.box(Style.RAISED if s == "normal" else Style.HOVER))
	var popup := _menu.get_popup()
	popup.add_item("Start over (fresh skin)  Ctrl+R", 7)
	popup.add_separator()
	popup.add_item("Reset skin  (R)", 0)
	popup.add_item("Release forceps", 1)
	popup.add_separator()
	popup.add_item("Clear incisions", 2)
	popup.add_item("Clear sutures", 3)
	popup.add_item("Clear undermining", 4)
	popup.add_item("Clear lesions", 5)
	popup.add_item("Clear markings", 6)
	popup.id_pressed.connect(_on_menu)
	_top.add_child(_menu)


func _on_menu(id: int) -> void:
	match id:
		0:
			reset_requested.emit()
		1:
			sheet.release_all_grabs()
		2:
			clear("cuts")
		3:
			clear("sutures")
		4:
			clear("undermining")
		5:
			clear("lesions")
		6:
			clear("markings")
		7:
			start_over_requested.emit()


func _on_start_over_pressed() -> void:
	var now := Time.get_ticks_msec() / 1000.0
	if now < _start_over_armed_until:
		_start_over_armed_until = 0.0
		_start_over.modulate = Color.WHITE
		start_over_requested.emit()
		return
	_start_over_armed_until = now + 3.0
	_start_over.modulate = Color(1.0, 0.55, 0.45)
	toast("Tap again to start over: every incision, suture, lesion, marking and hook goes. Tissue settings and the view stay.", "warn")
	get_tree().create_timer(3.0).timeout.connect(func():
		if Time.get_ticks_msec() / 1000.0 >= _start_over_armed_until:
			_start_over.modulate = Color.WHITE)


func clear(what: String) -> void:
	match what:
		"cuts":
			sheet.clear_cuts()
			toast("Incisions cleared (excised skin is back).")
		"sutures":
			sheet.clear_sutures()
			toast("Sutures cleared.")
		"undermining":
			sheet.clear_undermining()
			toast("Undermining cleared.")
		"lesions":
			sheet.clear_lesions()
			toast("Lesions cleared.")
		"markings":
			toast("Markings cleared.")
	cleared.emit(what)


# --- view popover: colour map and display ---------------------------------

func _build_view_card() -> void:
	_view_card = PanelContainer.new()
	_view_card.theme_type_variation = "Card"
	_view_card.mouse_filter = Control.MOUSE_FILTER_STOP
	_view_card.visible = false
	add_child(_view_card)
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_view_card.add_child(scroll)
	var v := VBoxContainer.new()
	v.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(v)
	_view_content = v
	v.add_child(Style.label("LOOK", "SectionTitle"))
	var looks := HBoxContainer.new()
	looks.add_theme_constant_override("separation", 4)
	var look_group := ButtonGroup.new()
	for i in Look.NAMES.size():
		var lb := Button.new()
		lb.text = Look.NAMES[i]
		lb.theme_type_variation = "SegmentButton"
		lb.toggle_mode = true
		lb.button_group = look_group
		lb.focus_mode = Control.FOCUS_NONE
		lb.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		lb.custom_minimum_size.y = 36
		lb.pressed.connect(func(): look_changed.emit(i))
		looks.add_child(lb)
		_look_buttons.append(lb)
	v.add_child(looks)
	_look_caption = Style.label("", "Caption", true)
	v.add_child(_look_caption)
	_skin_row = VBoxContainer.new()
	_skin_row.add_child(Style.label("Skin type (Fitzpatrick)", "Caption"))
	var types := HBoxContainer.new()
	types.add_theme_constant_override("separation", 4)
	var type_group := ButtonGroup.new()
	for i in 6:
		var tb := Button.new()
		tb.text = ["I", "II", "III", "IV", "V", "VI"][i]
		tb.theme_type_variation = "SegmentButton"
		tb.toggle_mode = true
		tb.button_group = type_group
		tb.focus_mode = Control.FOCUS_NONE
		tb.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		tb.custom_minimum_size.y = 32
		tb.button_pressed = i == skin_type
		tb.tooltip_text = "Fitzpatrick type %s: skin tone and pigment. Blanching and redness show less on darker skin." % tb.text
		tb.pressed.connect(func():
			skin_type = i
			skin_type_changed.emit(i))
		types.add_child(tb)
		_skin_buttons.append(tb)
	_skin_row.add_child(types)
	v.add_child(_skin_row)
	var bleed := _switch_row(v, "Bleeding", bleeding, func(on: bool): bleeding = on)
	bleed.tooltip_text = "Blood pools in the open wound, darkening as it clots. Drawing only: it does not change the mechanics."
	_effects_switch = _switch_row(v, "Effects", false, func(on: bool): effects_changed.emit(on))
	_effects_switch.tooltip_text = "Game-style feedback: sparks while cutting, a pulse when a suture is tied, a whip and a shake when one snaps."
	v.add_child(Style.label("COLOUR MAP", "SectionTitle"))
	var group := ButtonGroup.new()
	for i in MAPS.size():
		var b := Button.new()
		b.text = MAPS[i][0]
		b.theme_type_variation = "SegmentButton"
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		b.toggle_mode = true
		b.button_group = group
		b.focus_mode = Control.FOCUS_NONE
		b.custom_minimum_size.y = 36
		b.pressed.connect(func(): set_heatmap(i))
		v.add_child(b)
		_map_buttons.append(b)
	_map_caption = Style.label("", "Caption", true)
	v.add_child(_map_caption)
	v.add_child(Style.label("STRESS MAPS", "SectionTitle"))
	for spec in [
		{"prop": "heatmap_relative", "label": "Only what the surgery added", "type": "bool",
			"tip": "Subtract the tension living skin always has (its prestress). Untouched skin then shows neutral, and the colours show only what cutting, pulling and suturing changed."},
		{"prop": "heatmap_auto_range", "label": "Automatic scale", "type": "bool",
			"tip": "Fit the colour scale to the stresses on the skin now (a round number just above most of them), so a few extreme spots do not wash everything else out. Off: a fixed scale, to compare one closure with another."},
		{"prop": "heatmap_range_kpa", "label": "Fixed scale", "min": 1.0, "max": 2000.0, "step": 1.0,
			"exp": true, "fmt": "%.0f kPa", "tip": "Stress at the end of the colour scale, when the automatic scale is off."},
		{"prop": "heatmap_contours", "label": "Contour lines", "type": "bool",
			"tip": "Lines of equal stress, every tenth of the scale (ticks on the legend), like the contours of a map: close together where the stress changes fast. They carry on beyond the scale."},
	]:
		var r := ParamRow.make(sheet, spec)
		v.add_child(r)
		_view_rows.append(r)
		if spec.prop == "heatmap_range_kpa":
			_range_row = r
	var lines := _switch_row(v, "Tension direction strokes", show_tension_lines,
		func(on: bool): show_tension_lines = on)
	lines.tooltip_text = "Short strokes along the direction the skin is pulled (squeezed, on the Squeezing map)."
	v.add_child(Style.label("DISPLAY", "SectionTitle"))
	var wire := ParamRow.make(sheet, {"prop": "show_wireframe", "label": "Show the mesh", "type": "bool",
		"tip": "Draw the triangles the skin is computed on. [W]"})
	v.add_child(wire)
	_view_rows.append(wire)
	var dots := _switch_row(v, "Suture load dots", show_suture_dots,
		func(on: bool): show_suture_dots = on)
	dots.tooltip_text = "A dot at each needle hole coloured by the thread's load (green, yellow at 50%, red at its breaking strength)."
	var measures := _switch_row(v, "Measurements", show_measurements,
		func(on: bool): show_measurements = on)
	measures.tooltip_text = "Keep the labels on rulers and angles: the value as drawn, and what it measures now. Off: they show only while you draw."
	_stats_switch = _switch_row(v, "Solver details", false, func(on: bool): _stats_card.visible = on)
	set_heatmap(sheet.heatmap)


## A label and a switch on one row. Returns the switch.
func _switch_row(parent: Container, text: String, on: bool, toggled: Callable) -> CheckButton:
	var row := HBoxContainer.new()
	var l := Style.label(text)
	l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(l)
	var sw := CheckButton.new()
	sw.focus_mode = Control.FOCUS_NONE
	sw.button_pressed = on
	sw.toggled.connect(toggled)
	row.add_child(sw)
	parent.add_child(row)
	return sw


func set_heatmap(i: int) -> void:
	sheet.heatmap = i
	for k in _map_buttons.size():
		_map_buttons[k].set_pressed_no_signal(k == i)
	_map_caption.text = MAPS[i][1]
	_view_button.text = "" if narrow else ("View" if i == 0 else MAPS[i][0].get_slice(" (", 0))


## Select a skin type (without emitting skin_type_changed).
func show_skin_type(i: int) -> void:
	skin_type = i
	for k in _skin_buttons.size():
		_skin_buttons[k].set_pressed_no_signal(k == i)


## The Effects switch follows the look's default when a look is picked.
func show_effects(on: bool) -> void:
	_effects_switch.set_pressed_no_signal(on)


## Show which look is on (the switch does not apply it: main.gd does).
func show_look(i: int) -> void:
	for k in _look_buttons.size():
		_look_buttons[k].set_pressed_no_signal(k == i)
	_look_caption.text = Look.BLURBS[i]
	_skin_row.visible = i == 2  # only the Realistic look has skin types


func refresh() -> void:
	context.refresh()
	drawer.refresh()
	for r in _view_rows:
		r.refresh()
	set_heatmap(sheet.heatmap)


# --- help, stats, hint ----------------------------------------------------

func _build_help() -> void:
	_help_card = PanelContainer.new()
	_help_card.theme_type_variation = "Card"
	_help_card.mouse_filter = Control.MOUSE_FILTER_STOP
	_help_card.visible = false
	add_child(_help_card)
	var v := VBoxContainer.new()
	_help_card.add_child(v)
	var head := HBoxContainer.new()
	var t := Style.label("Help", "Title")
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(t)
	var close := Style.icon_button("close", "", "Close [Esc]")
	close.pressed.connect(func(): _help_card.visible = false)
	head.add_child(close)
	v.add_child(head)
	v.add_child(Style.label("TOUCH", "SectionTitle"))
	v.add_child(Style.label("One finger uses the tool. Two fingers move the view and pinch to zoom.", "Caption", true))
	v.add_child(Style.label("KEYBOARD AND MOUSE", "SectionTitle"))
	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 18)
	grid.add_theme_constant_override("v_separation", 6)
	for s in SHORTCUTS:
		var k := Style.label(s[0], "Value")
		grid.add_child(k)
		var d := Style.label(s[1], "Caption")
		grid.add_child(d)
	v.add_child(grid)


func _build_stats() -> void:
	_stats_card = PanelContainer.new()
	_stats_card.theme_type_variation = "Card"
	_stats_card.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_stats_card.visible = false
	add_child(_stats_card)
	_stats_label = Style.label("", "Caption")
	_stats_label.add_theme_font_size_override("font_size", 11)
	_stats_card.add_child(_stats_label)


func _build_hint() -> void:
	_toasts = VBoxContainer.new()
	_toasts.alignment = BoxContainer.ALIGNMENT_END
	_toasts.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_toasts)
	_hint_card = PanelContainer.new()
	_hint_card.theme_type_variation = "Pill"
	_hint_card.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_hint_card)
	_hint_label = Style.label("", "")
	_hint_label.add_theme_color_override("font_color", Style.TEXT_DIM)
	_hint_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_hint_card.add_child(_hint_label)


## The one-line instruction for what to do now.
func set_hint(text: String) -> void:
	_hint_label.text = text


## A short message about what just happened; fades after a few seconds.
func toast(text: String, kind := "info") -> void:
	if text == "":
		return
	var card := PanelContainer.new()
	card.theme_type_variation = "Pill"
	card.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var color: Color = {"info": Style.ACCENT, "warn": Style.WARN, "danger": Style.DANGER}.get(kind, Style.ACCENT)
	card.add_theme_stylebox_override("panel", Style.box(Color(Style.SURFACE, 0.96), 18, color, 14, 8))
	card.size_flags_horizontal = Control.SIZE_SHRINK_CENTER  # as wide as its text
	var l := Style.label(text, "")
	if narrow:
		# On a phone: wrap within the screen instead of running off it.
		l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		l.custom_minimum_size.x = get_viewport_rect().size.x - 64.0
		l.add_theme_font_size_override("font_size", 13)
	card.add_child(l)
	_toasts.add_child(card)
	while _toasts.get_child_count() > (1 if narrow else 3):
		var old := _toasts.get_child(0)
		_toasts.remove_child(old)
		old.queue_free()
	var tween := create_tween()
	tween.tween_interval(3.5)
	tween.tween_property(card, "modulate:a", 0.0, 0.6)
	tween.tween_callback(card.queue_free)


func set_status(converged: bool, stalled: bool) -> void:
	var at_rest := converged and not stalled
	_status_dot.text = "●" if narrow else ("● At rest" if at_rest else "● Settling")
	_status_dot.add_theme_color_override("font_color", Style.OK if at_rest else Style.WARN)


func set_stats(text: String) -> void:
	if _stats_card.visible:
		_stats_label.text = text


func set_undo_enabled(on: bool) -> void:
	_undo.disabled = not on


func show_tool(tool: int) -> void:
	# The bar shows the tools of the tool's mode (Plan or Operate).
	var plan: bool = ContextPanel.TOOLS[tool].plan
	var mode_changed := _mode_buttons[0].button_pressed != plan
	_mode_buttons[0].set_pressed_no_signal(plan)
	_mode_buttons[1].set_pressed_no_signal(not plan)
	for i in _tool_buttons.size():
		_tool_buttons[i].visible = ContextPanel.TOOLS[i].plan == plan
		_tool_buttons[i].set_pressed_no_signal(i == tool)
	context.show_tool(tool)
	if mode_changed:
		_tool_scroll.scroll_horizontal = 0
		_tool_scroll.scroll_vertical = 0
		_tool_velocity = 0.0
	if narrow:
		_tool_scroll.ensure_control_visible.call_deferred(_tool_buttons[tool])


func cycle_heatmap() -> void:
	set_heatmap((sheet.heatmap + 1) % MAPS.size())


func toggle_help() -> void:
	_help_card.visible = not _help_card.visible
	if _help_card.visible:
		_help_card.move_to_front()
		_view_button.button_pressed = false


func set_drawer_open(open: bool) -> void:
	_drawer_open = open
	drawer.visible = open
	context.visible = not open
	if open:
		drawer.move_to_front()
		drawer.refresh()
	_layout()


## Open a panel by name (command line, for screenshots): "tissue", "view",
## "help", "sheet" (phone: the context sheet half open), "sheet-full".
func open_panel(what: String) -> void:
	match what:
		"tissue":
			set_drawer_open(true)
		"view":
			_view_button.button_pressed = true
		"help":
			_help_card.visible = true
		"sheet":
			set_sheet_state(1)
		"sheet-full":
			set_sheet_state(2)


## Esc: close whatever is open. True if something was closed.
func close_overlays() -> bool:
	var closed := false
	if _help_card.visible:
		_help_card.visible = false
		closed = true
	if _view_card.visible:
		_view_button.button_pressed = false
		closed = true
	if _drawer_open:
		set_drawer_open(false)
		closed = true
	if narrow and _sheet_state > 0:
		set_sheet_state(0)
		closed = true
	return closed


## True if a screen point is over a part of the UI (not over the skin).
func is_over_ui(p: Vector2) -> bool:
	for c in [_rail, context, drawer, _view_card, _help_card]:
		if c.visible and c.get_global_rect().has_point(p):
			return true
	return _top.get_global_rect().has_point(p) and _over_top_button(p)


func _over_top_button(p: Vector2) -> bool:
	for c in _top.get_children():
		if c is BaseButton and c.get_global_rect().has_point(p):
			return true
	return false


## The part of the window where the skin is visible (not under the rail,
## the panel, the bottom bar or the sheet), for framing the camera and
## placing the legend.
func free_rect() -> Rect2:
	var vp := get_viewport_rect().size
	if narrow:
		var top := 8.0 + Style.TOUCH + 4.0
		var bottom := vp.y - BAR_HEIGHT - _sheet_height - 8.0
		return Rect2(0.0, top, vp.x, maxf(bottom - top, 80.0))
	var left := float(Style.RAIL_WIDTH)
	var right := vp.x - Style.PANEL_WIDTH - 24.0
	return Rect2(left, 0.0, maxf(right - left, 100.0), vp.y)


# --- bottom sheet (phone) ----------------------------------------------------

## 0 = peek (the tool's name), 1 = half the screen, 2 = nearly all of it.
func set_sheet_state(state: int) -> void:
	_sheet_state = clampi(state, 0, 2)
	_sheet_height = _sheet_target(_sheet_state)
	_layout()


func _sheet_target(state: int) -> float:
	var room := get_viewport_rect().size.y - BAR_HEIGHT - 8.0 - Style.TOUCH - 16.0
	match state:
		1:
			return maxf(SHEET_PEEK, room * SHEET_HALF)
		2:
			return maxf(SHEET_PEEK, room * SHEET_FULL)
	return SHEET_PEEK


## The handle: a tap steps peek -> half -> full -> peek; a drag resizes and,
## on release, snaps to the nearest of the three.
func _on_handle_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			_sheet_drag = true
			_sheet_dragged = false
			_sheet_drag_from = event.global_position.y
			_sheet_height_from = _sheet_height
		elif _sheet_drag:
			_sheet_drag = false
			if not _sheet_dragged:
				set_sheet_state((_sheet_state + 1) % 3)
				return
			var best := 0
			for s in 3:
				if absf(_sheet_target(s) - _sheet_height) < absf(_sheet_target(best) - _sheet_height):
					best = s
			set_sheet_state(best)
	elif event is InputEventMouseMotion and _sheet_drag:
		var dy: float = _sheet_drag_from - event.global_position.y
		if absf(dy) > 6.0:
			_sheet_dragged = true
		if _sheet_dragged:
			_sheet_height = clampf(_sheet_height_from + dy, SHEET_PEEK, _sheet_target(2))
			_layout()


# --- layout ------------------------------------------------------------------

func _layout() -> void:
	var vp := get_viewport_rect().size
	var was_narrow := narrow
	narrow = vp.x < NARROW_WIDTH
	if narrow != was_narrow or not _laid_out:
		_laid_out = true
		_switch_layout()
	if narrow:
		_layout_narrow(vp)
	else:
		_layout_wide(vp)
	_help_card.reset_size()
	_help_card.size.x = minf(_help_card.size.x, vp.x - 24.0)
	var fr := free_rect()
	_help_card.position = Vector2(clampf(fr.get_center().x - _help_card.size.x * 0.5, 12.0, vp.x),
		clampf(fr.get_center().y - _help_card.size.y * 0.5, 8.0 + Style.TOUCH + 8.0, vp.y))
	_stats_card.position = Vector2(fr.position.x + 16.0, 12.0 + Style.TOUCH + 8.0)


## Wide <-> narrow: everything that changes shape, not just position.
func _switch_layout() -> void:
	_rail_box.vertical = not narrow
	_tool_box.vertical = not narrow
	_rail_spacer.visible = false  # the tools' scroll area takes the room
	_tool_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_SHOW_NEVER if narrow \
		else ScrollContainer.SCROLL_MODE_DISABLED
	_tool_scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED if narrow \
		else ScrollContainer.SCROLL_MODE_SHOW_NEVER
	var buttons: Array[Button] = _tool_buttons.duplicate()
	buttons.append(_undo)
	for b in buttons:
		b.custom_minimum_size = Vector2(58, 52) if narrow else Vector2(Style.RAIL_WIDTH - 12, 54)
		b.add_theme_font_size_override("font_size", 10 if narrow else 11)
	for m in _mode_buttons:
		m.custom_minimum_size = Vector2(62, 25) if narrow else Vector2(Style.RAIL_WIDTH - 12, 30)
		m.add_theme_font_size_override("font_size", 10 if narrow else 11)
	_rail.add_theme_stylebox_override("panel", Style.box(Style.SURFACE, 0, Color(0, 0, 0, 0), 4, 4) if narrow \
		else Style.box(Style.SURFACE, 0, Color(0, 0, 0, 0), 6, 10))
	# Compact top bar: icons only, no subtitle.
	_titles.visible = not narrow
	_tissue_button.text = "" if narrow else "Tissue"
	context.handle.visible = narrow
	_sheet_state = 0
	_sheet_height = SHEET_PEEK
	set_heatmap(sheet.heatmap)


func _layout_wide(vp: Vector2) -> void:
	_rail.position = Vector2.ZERO
	_rail.size = Vector2(Style.RAIL_WIDTH, vp.y)
	for card in [context, drawer]:
		card.position = Vector2(vp.x - Style.PANEL_WIDTH - 12.0, 12.0)
		card.size = Vector2(Style.PANEL_WIDTH, vp.y - 24.0)
	var fr := free_rect()
	_top.position = Vector2(fr.position.x + 16.0, 12.0)
	_top.size = Vector2(fr.size.x - 20.0, Style.TOUCH)
	_fit_view_card(300.0, vp.y - Style.TOUCH - 40.0)
	_view_card.position = Vector2(fr.end.x - _view_card.size.x - 4.0, 12.0 + Style.TOUCH + 8.0)


func _layout_narrow(vp: Vector2) -> void:
	_rail.position = Vector2(0.0, vp.y - BAR_HEIGHT)
	_rail.size = Vector2(vp.x, BAR_HEIGHT)
	_sheet_height = clampf(_sheet_height, SHEET_PEEK, _sheet_target(2))
	context.position = Vector2(6.0, vp.y - BAR_HEIGHT - _sheet_height - 4.0)
	context.size = Vector2(vp.x - 12.0, _sheet_height)
	context.handle.icon = Style.icon("chevron_down" if _sheet_state == 2 else "chevron_up")
	# The tissue drawer covers the screen above the tool bar.
	drawer.position = Vector2(6.0, 6.0)
	drawer.size = Vector2(vp.x - 12.0, vp.y - BAR_HEIGHT - 12.0)
	_top.position = Vector2(8.0, 8.0)
	_top.size = Vector2(vp.x - 16.0, Style.TOUCH)
	_fit_view_card(minf(300.0, vp.x - 16.0), vp.y - BAR_HEIGHT - Style.TOUCH - 32.0)
	_view_card.position = Vector2(vp.x - _view_card.size.x - 8.0, 8.0 + Style.TOUCH + 8.0)


func _process(delta: float) -> void:
	Prof.begin("shell")
	_coast_tools(delta)
	_layout_overlays()
	Prof.end("shell")


func _layout_overlays() -> void:
	# Hint pill and toasts: bottom centre of the skin area, sized to content.
	var fr := free_rect()
	var vp := get_viewport_rect().size
	_range_row.visible = not sheet.heatmap_auto_range
	# On a phone an open card or drawer covers the skin: hints and messages
	# about the skin would only clutter it.
	var covered := narrow and (_drawer_open or _view_card.visible or _help_card.visible)
	_hint_card.visible = _hint_label.text != "" and not covered
	_toasts.visible = not covered
	if narrow:
		_hint_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_hint_label.custom_minimum_size.x = minf(vp.x - 48.0, 520.0)
		_hint_label.add_theme_font_size_override("font_size", 12)
	else:
		_hint_label.autowrap_mode = TextServer.AUTOWRAP_OFF
		_hint_label.custom_minimum_size.x = 0.0
		_hint_label.remove_theme_font_size_override("font_size")
	_hint_card.reset_size()
	var hint_bottom := fr.end.y - 8.0 if narrow else vp.y - 16.0
	_hint_card.position = Vector2(fr.get_center().x - _hint_card.size.x * 0.5, hint_bottom - _hint_card.size.y)
	_toasts.reset_size()
	_toasts.position = Vector2(fr.get_center().x - _toasts.size.x * 0.5,
		_hint_card.position.y - _toasts.size.y - 8.0)
	if _help_card.visible:
		_help_card.reset_size()
		_help_card.size.x = minf(_help_card.size.x, vp.x - 24.0)
		_help_card.position.x = clampf(fr.get_center().x - _help_card.size.x * 0.5, 12.0, vp.x)
	if _view_card.visible:
		if narrow:
			_fit_view_card(minf(300.0, vp.x - 16.0), vp.y - BAR_HEIGHT - Style.TOUCH - 32.0)
			_view_card.position.x = vp.x - _view_card.size.x - 8.0
		else:
			_fit_view_card(300.0, vp.y - Style.TOUCH - 40.0)
			_view_card.position.x = fr.end.x - _view_card.size.x - 4.0


## The view card: as tall as its content, but within the screen (it
## scrolls beyond that).
func _fit_view_card(width: float, max_height: float) -> void:
	var want := _view_content.get_combined_minimum_size().y + 2.0 * Style.PAD + 4.0
	_view_card.size = Vector2(width, minf(want, max_height))
