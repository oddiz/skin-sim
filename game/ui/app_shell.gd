class_name AppShell
extends Control
## The whole user interface around the skin, built in code:
##
##   +------+----------------------------------------+-----------+
##   | tool | top bar: title  status  View  Tissue ? |  context  |
##   | rail |                                        |  panel    |
##   |      |               the skin                 | (current  |
##   |      |                                        |   tool)   |
##   | undo |          [ hint / messages ]           |           |
##   +------+----------------------------------------+-----------+
##
## The shell itself lets clicks through (mouse_filter IGNORE): only its
## cards and buttons catch them, so touching the skin reaches main.gd.

signal tool_selected(tool: int)
signal undo_requested
signal reset_requested
## Something was cleared from the menu or a panel: "cuts", "sutures",
## "undermining", "lesions" or "markings" (main.gd trims its undo history).
signal cleared(what: String)

const MAPS := [
	["Plain skin", "No colour map: just the skin and the wound."],
	["Tension (max principal)", "Red = stretched, blue = squeezed, in the direction where it is largest."],
	["Overall stress (von Mises)", "How much stress the skin carries, regardless of direction."],
	["Area change", "Red = skin stretched to a larger area, blue = compressed to a smaller one."],
	["Squeezing (min principal)", "Blue where the skin is squeezed in some direction: where real skin puckers."],
	["Excess skin (dog-ears)", "How much extra skin is bunched into wrinkles: where dog-ears form."],
]
const SHORTCUTS := [
	["G C S U", "Grab, Cut, Suture, Undermine"],
	["L E F M", "Lesion, Excise, Flap, Mark"],
	["Shift + drag", "Cut with any tool"],
	["Z", "Undo"],
	["- / +", "Loosen / tighten the selected suture"],
	["Del", "Cut the selected suture out"],
	["T", "Suture labels: smart / all / none"],
	["H", "Next colour map"],
	["W", "Wireframe on / off"],
	["R", "Reset the skin (incisions and sutures stay)"],
	["Esc", "Close panels, deselect"],
	["?", "This list"],
]

var sheet: SkinSheet
var context: ContextPanel
var drawer: TissueDrawer

var _rail: PanelContainer
var _tool_buttons: Array[Button] = []
var _undo: Button
var _top: HBoxContainer
var _subtitle: Label
var _status_dot: Label
var _view_button: Button
var _view_card: PanelContainer
var _map_buttons: Array[Button] = []
var _map_caption: Label
var _view_rows: Array[ParamRow] = []
var _stats_switch: CheckButton
var _menu: MenuButton
var _help_card: PanelContainer
var _stats_card: PanelContainer
var _stats_label: Label
var _hint_card: PanelContainer
var _hint_label: Label
var _toasts: VBoxContainer
var _drawer_open := false


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
	context.release_grabs_requested.connect(func(): sheet.release_all_grabs())
	context.clear_requested.connect(clear)
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


# --- tool rail ------------------------------------------------------------

func _build_rail() -> void:
	_rail = PanelContainer.new()
	_rail.theme_type_variation = "Rail"
	_rail.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_rail)
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 4)
	_rail.add_child(v)
	var group := ButtonGroup.new()
	for i in ContextPanel.TOOLS.size():
		var t: Dictionary = ContextPanel.TOOLS[i]
		var b := _rail_button(t.icon, t.name, "%s [%s]" % [t.name, t.key])
		b.toggle_mode = true
		b.button_group = group
		b.pressed.connect(func(): tool_selected.emit(i))
		v.add_child(b)
		_tool_buttons.append(b)
	var spacer := Control.new()
	spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	v.add_child(spacer)
	_undo = _rail_button("undo", "Undo", "Undo [Z]")
	_undo.pressed.connect(func(): undo_requested.emit())
	v.add_child(_undo)
	_tool_buttons[0].set_pressed_no_signal(true)


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
	var titles := VBoxContainer.new()
	titles.add_theme_constant_override("separation", 0)
	titles.mouse_filter = Control.MOUSE_FILTER_IGNORE
	titles.add_child(Style.label("skin-sim", "Title"))
	_subtitle = Style.label("", "Caption")
	titles.add_child(_subtitle)
	_top.add_child(titles)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_top.add_child(spacer)
	_status_dot = Style.label("", "Caption")
	_status_dot.tooltip_text = "Settling: the skin is still moving towards equilibrium. At rest: it is in equilibrium."
	_status_dot.mouse_filter = Control.MOUSE_FILTER_PASS
	_top.add_child(_status_dot)
	_view_button = Style.icon_button("heatmap", "View", "Colour map and display [H]", "")
	_view_button.toggle_mode = true
	_view_button.toggled.connect(func(on: bool): _view_card.visible = on)
	_top.add_child(_view_button)
	var tissue := Style.icon_button("tissue", "Tissue", "Body site and skin properties", "")
	tissue.pressed.connect(func(): set_drawer_open(not _drawer_open))
	_top.add_child(tissue)
	var help := Style.icon_button("help", "", "Keyboard shortcuts [?]", "")
	help.pressed.connect(func(): _help_card.visible = not _help_card.visible)
	_top.add_child(help)
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
	var v := VBoxContainer.new()
	v.custom_minimum_size.x = 280
	_view_card.add_child(v)
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
	for spec in [
		{"prop": "heatmap_range_kpa", "label": "Colour scale", "min": 1.0, "max": 2000.0, "step": 1.0,
			"exp": true, "fmt": "%.0f kPa", "tip": "Stress at the end of the colour scale. Keep it fixed to compare closures."},
		{"prop": "show_wireframe", "label": "Show the mesh", "type": "bool",
			"tip": "Draw the triangles the skin is computed on. [W]"},
	]:
		var r := ParamRow.make(sheet, spec)
		v.add_child(r)
		_view_rows.append(r)
	var stats_row := HBoxContainer.new()
	var l := Style.label("Solver details")
	l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	stats_row.add_child(l)
	_stats_switch = CheckButton.new()
	_stats_switch.focus_mode = Control.FOCUS_NONE
	_stats_switch.toggled.connect(func(on: bool): _stats_card.visible = on)
	stats_row.add_child(_stats_switch)
	v.add_child(stats_row)
	set_heatmap(sheet.heatmap)


func set_heatmap(i: int) -> void:
	sheet.heatmap = i
	for k in _map_buttons.size():
		_map_buttons[k].set_pressed_no_signal(k == i)
	_map_caption.text = MAPS[i][1]
	_view_button.text = "View" if i == 0 else MAPS[i][0].get_slice(" (", 0)


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
	var t := Style.label("Keyboard shortcuts", "Title")
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(t)
	var close := Style.icon_button("close", "", "Close [Esc]")
	close.pressed.connect(func(): _help_card.visible = false)
	head.add_child(close)
	v.add_child(head)
	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 18)
	grid.add_theme_constant_override("v_separation", 6)
	for s in SHORTCUTS:
		var k := Style.label(s[0], "Value")
		grid.add_child(k)
		grid.add_child(Style.label(s[1]))
	v.add_child(grid)
	v.add_child(Style.label("On a touch screen: one finger uses the tool.", "Caption", true))


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
	_hint_card.visible = text != ""


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
	card.add_child(l)
	_toasts.add_child(card)
	while _toasts.get_child_count() > 3:
		var old := _toasts.get_child(0)
		_toasts.remove_child(old)
		old.queue_free()
	var tween := create_tween()
	tween.tween_interval(3.5)
	tween.tween_property(card, "modulate:a", 0.0, 0.6)
	tween.tween_callback(card.queue_free)


func set_status(converged: bool, stalled: bool) -> void:
	if converged and not stalled:
		_status_dot.text = "● At rest"
		_status_dot.add_theme_color_override("font_color", Style.OK)
	else:
		_status_dot.text = "● Settling"
		_status_dot.add_theme_color_override("font_color", Style.WARN)


func set_stats(text: String) -> void:
	if _stats_card.visible:
		_stats_label.text = text


func set_undo_enabled(on: bool) -> void:
	_undo.disabled = not on


func show_tool(tool: int) -> void:
	for i in _tool_buttons.size():
		_tool_buttons[i].set_pressed_no_signal(i == tool)
	context.show_tool(tool)


func cycle_heatmap() -> void:
	set_heatmap((sheet.heatmap + 1) % MAPS.size())


func toggle_help() -> void:
	_help_card.visible = not _help_card.visible


func set_drawer_open(open: bool) -> void:
	_drawer_open = open
	drawer.visible = open
	context.visible = not open
	if open:
		drawer.refresh()


## Open a panel by name (command line, for screenshots): "tissue", "view", "help".
func open_panel(what: String) -> void:
	match what:
		"tissue":
			set_drawer_open(true)
		"view":
			_view_button.button_pressed = true
		"help":
			_help_card.visible = true


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


## The part of the window where the skin is visible (not under the rail or
## the panel), for framing the camera and placing the legend.
func free_rect() -> Rect2:
	var vp := get_viewport_rect().size
	var left := float(Style.RAIL_WIDTH)
	var right := vp.x - Style.PANEL_WIDTH - 24.0
	return Rect2(left, 0.0, maxf(right - left, 100.0), vp.y)


func _layout() -> void:
	var vp := get_viewport_rect().size
	_rail.position = Vector2.ZERO
	_rail.size = Vector2(Style.RAIL_WIDTH, vp.y)
	for card in [context, drawer]:
		card.position = Vector2(vp.x - Style.PANEL_WIDTH - 12.0, 12.0)
		card.size = Vector2(Style.PANEL_WIDTH, vp.y - 24.0)
	var fr := free_rect()
	_top.position = Vector2(fr.position.x + 16.0, 12.0)
	_top.size = Vector2(fr.size.x - 20.0, Style.TOUCH)
	_view_card.reset_size()
	_view_card.position = Vector2(fr.end.x - _view_card.size.x - 4.0, 12.0 + Style.TOUCH + 8.0)
	_help_card.reset_size()
	_help_card.position = fr.get_center() - _help_card.size * 0.5
	_stats_card.position = Vector2(fr.position.x + 16.0, 12.0 + Style.TOUCH + 8.0)


func _process(_delta: float) -> void:
	# Hint pill and toasts: bottom centre of the skin area, sized to content.
	var fr := free_rect()
	var vp := get_viewport_rect().size
	_hint_card.reset_size()
	_hint_card.position = Vector2(fr.get_center().x - _hint_card.size.x * 0.5, vp.y - _hint_card.size.y - 16.0)
	_toasts.reset_size()
	_toasts.position = Vector2(fr.get_center().x - _toasts.size.x * 0.5,
		_hint_card.position.y - _toasts.size.y - 8.0)
	if _help_card.visible:
		_help_card.reset_size()
		_help_card.position = fr.get_center() - _help_card.size * 0.5
	if _view_card.visible:
		_view_card.reset_size()  # as tall as its content
		_view_card.position.x = fr.end.x - _view_card.size.x - 4.0
