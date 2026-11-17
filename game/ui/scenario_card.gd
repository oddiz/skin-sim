class_name ScenarioCard
extends PanelContainer
## Teaching cases (game/scenario.gd): one floating card, in three states.
##   picker   the list of scenarios; tapping one starts it
##   case     the brief and goals; Check closure, Suggested approach (hidden
##            until asked: text, then "Draw it on the skin"), Restart, End;
##            folds to its title so it doesn't hide the skin
##   report   (under the case) stars and a checklist from Scenario.score
## AppShell places it (top left; full width on a phone).

signal start_requested(index: int)
signal check_requested
signal draw_approach_requested
signal restart_requested
signal end_requested
## Folded or unfolded by the user (main.gd re-frames the skin).
signal folded_changed

const GRADE_COLORS := [Style.OK, Style.WARN, Style.DANGER]

var _title: Label
var _fold: Button
var _body: VBoxContainer
var _scroll: ScrollContainer
var _scenario: Scenario
var _folded := false
## Max height the shell allows (it sets this on layout).
var max_height := 400.0


func _ready() -> void:
	theme_type_variation = "Card"
	mouse_filter = Control.MOUSE_FILTER_STOP
	visible = false
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 8)
	add_child(v)
	var head := HBoxContainer.new()
	_title = Style.label("Scenarios", "Title")
	_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_title.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	head.add_child(_title)
	_fold = Style.icon_button("chevron_up", "", "Fold / unfold")
	_fold.pressed.connect(func():
		set_folded(not _folded)
		folded_changed.emit())
	head.add_child(_fold)
	var close := Style.icon_button("close", "", "Close")
	close.pressed.connect(func():
		if _scenario != null:
			end_requested.emit()
		else:
			visible = false)
	head.add_child(close)
	v.add_child(head)
	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	v.add_child(_scroll)
	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", 10)
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var pad := MarginContainer.new()
	pad.add_theme_constant_override("margin_right", 8)
	pad.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	pad.add_child(_body)
	_scroll.add_child(pad)


func set_folded(on: bool) -> void:
	_folded = on
	_scroll.visible = not on
	_fold.icon = Style.icon("chevron_down" if on else "chevron_up")
	fit()


## Size the scroll area to its content, up to max_height. Wrapped text only
## knows its height once laid out at the card's width, so this measures
## again over the next two frames.
func fit() -> void:
	_fit_now()
	await get_tree().process_frame
	_fit_now()
	await get_tree().process_frame
	_fit_now()


func _fit_now() -> void:
	if not _scroll.visible:
		_scroll.custom_minimum_size.y = 0
	else:
		# The content's height as laid out (the VBox's own size), not its
		# minimum: wrapped labels report a tall minimum before they know
		# their width.
		var want := _body.size.y + 4.0
		_scroll.custom_minimum_size.y = minf(want, maxf(max_height - 70.0, 120.0))
	size.y = 0
	reset_size()


func _clear() -> void:
	for c in _body.get_children():
		_body.remove_child(c)
		c.queue_free()


func _caption(text: String) -> Label:
	var l := Style.label(text, "Caption", true)
	l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_body.add_child(l)
	return l


func _button(text: String, icon_name: String, on_press: Callable, variation := "") -> Button:
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
	return b


# --- picker -------------------------------------------------------------------

const KIND_NAMES := ["BCC", "SCC", "melanoma"]


func show_picker() -> void:
	_scenario = null
	_clear()
	_title.text = "Scenarios"
	_caption("Teaching cases: a site, a lesion and a target margin. Plan, operate, then check your closure.")
	var all := Scenario.list()
	for i in all.size():
		var d: Dictionary = all[i].data
		var l: Dictionary = d.get("lesion", {})
		var b := _button("", "", func(): start_requested.emit(i))
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		b.text = "%s\n%s · %.0f mm %s · %.0f mm margin" % [d.title, d.get("site", ""), float(l.get("diameter", 0)),
			KIND_NAMES[clampi(int(l.get("kind", 0)), 0, 2)], float(d.get("margin_mm", 0))]
		b.custom_minimum_size.y = 56
		_body.add_child(b)
	visible = true
	set_folded(false)
	move_to_front()


# --- case ---------------------------------------------------------------------

func show_case(s: Scenario) -> void:
	_scenario = s
	if s == null:
		visible = false
		return
	_clear()
	_title.text = s.title()
	_caption(s.data.get("brief", ""))
	for g in s.data.get("goals", []):
		_caption("•  " + g)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	row.add_child(_button("Check closure", "check", func(): check_requested.emit(), "PrimaryButton"))
	_body.add_child(row)
	var row2 := HBoxContainer.new()
	row2.add_theme_constant_override("separation", 6)
	var approach := VBoxContainer.new()
	approach.visible = false
	row2.add_child(_button("Suggested approach", "info", func():
		approach.visible = not approach.visible
		fit()))
	row2.add_child(_button("Restart", "reset", func(): restart_requested.emit()))
	_body.add_child(row2)
	_body.add_child(approach)
	var at := Style.label(s.data.get("approach", {}).get("text", ""), "Caption", true)
	at.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	approach.add_child(at)
	approach.add_child(_button("Draw it on the skin", "mark", func(): draw_approach_requested.emit()))
	_report = VBoxContainer.new()
	_report.add_theme_constant_override("separation", 6)
	_body.add_child(_report)
	visible = true
	set_folded(false)


# --- report -------------------------------------------------------------------

var _report: VBoxContainer


func show_report(r: Dictionary) -> void:
	if _report == null:
		return
	for c in _report.get_children():
		_report.remove_child(c)
		c.queue_free()
	_report.add_child(Style.label("RESULT", "SectionTitle"))
	var stars := StarRow.new()
	stars.stars = r.stars
	_report.add_child(stars)
	if r.get("settling", false):
		var w := Style.label("The skin is still settling: check again in a moment for the final values.", "Caption", true)
		w.add_theme_color_override("font_color", Style.WARN)
		_report.add_child(w)
	for item in r.items:
		var line := HBoxContainer.new()
		line.add_theme_constant_override("separation", 8)
		var dot := ColorRect.new()
		dot.color = GRADE_COLORS[item.grade]
		dot.custom_minimum_size = Vector2(10, 10)
		dot.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		line.add_child(dot)
		var t := Style.label("%s: %s" % [item.name, item.value])
		t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		t.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		line.add_child(t)
		_report.add_child(line)
		if item.tip != "":
			var tip := Style.label(item.tip, "Caption", true)
			tip.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			_report.add_child(tip)
	set_folded(false)
	fit()
	# Show the result: scroll to the bottom once laid out.
	await get_tree().process_frame
	_scroll.scroll_vertical = int(_scroll.get_v_scroll_bar().max_value)


## Three stars, filled up to `stars` (drawn: no font glyph needed).
class StarRow extends Control:
	var stars := 0

	func _init() -> void:
		custom_minimum_size = Vector2(0, 30)

	func _draw() -> void:
		for i in 3:
			var c := Vector2(16 + i * 32, 15)
			var pts := PackedVector2Array()
			for k in 10:
				var r := 13.0 if k % 2 == 0 else 5.5
				pts.append(c + r * Vector2.from_angle(-PI / 2 + k * PI / 5))
			if i < stars:
				draw_colored_polygon(pts, Style.WARN)
			else:
				pts.append(pts[0])
				draw_polyline(pts, Style.TEXT_FAINT, 1.5, true)
