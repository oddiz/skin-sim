class_name ParamRow
extends VBoxContainer
## One setting, bound to a SkinSheet property: a slider, a switch, a
## drop-down or a segmented choice, with its label, its current value, and an
## (i) button that unfolds a short explanation. Tooltips only exist on desktop
## (hover); the (i) button works with a finger too.
##
## Built from a spec Dictionary, the same format as the old panel's table:
##   {"prop": "thickness", "label": "Dermis thickness", "min": 0.2, "max": 6.0,
##    "step": 0.05, "fmt": "%.2f mm", "exp": false, "tip": "..."}
## "type": "bool" (switch), "enum" (drop-down, "options"), "segment"
## (row of buttons, "options"; "values" optional, default 0, 1, 2...).

var sheet: SkinSheet
var spec: Dictionary
var _slider: HSlider
var _value: Label
var _switch: CheckButton
var _option: OptionButton
var _segments: Array[Button] = []
var _tip: Label


static func make(p_sheet: SkinSheet, p_spec: Dictionary) -> ParamRow:
	var row := ParamRow.new()
	row.sheet = p_sheet
	row.spec = p_spec
	row._build()
	return row


func _build() -> void:
	add_theme_constant_override("separation", 4)
	var head := HBoxContainer.new()
	add_child(head)
	var name_label := Style.label(spec.label)
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	head.add_child(name_label)
	var kind: String = spec.get("type", "slider")
	match kind:
		"bool":
			_switch = CheckButton.new()
			_switch.focus_mode = Control.FOCUS_NONE
			_switch.toggled.connect(func(on: bool): sheet.set(spec.prop, on))
			head.add_child(_switch)
		"enum":
			_option = OptionButton.new()
			_option.focus_mode = Control.FOCUS_NONE
			for o in spec.options:
				_option.add_item(o)
			_option.item_selected.connect(func(i: int): sheet.set(spec.prop, _values()[i]))
			_option.custom_minimum_size.x = 150
			head.add_child(_option)
		"segment":
			pass
		_:
			_value = Style.label("", "Value")
			head.add_child(_value)
	if spec.has("tip"):
		var info := Style.icon_button("info", "", "What is this?")
		info.custom_minimum_size = Vector2(32, 32)
		info.add_theme_constant_override("icon_max_width", 18)
		info.toggle_mode = true
		info.toggled.connect(func(on: bool): _tip.visible = on)
		info.tooltip_text = spec.tip
		head.add_child(info)
	if kind == "segment":
		var seg := HBoxContainer.new()
		seg.add_theme_constant_override("separation", 4)
		var group := ButtonGroup.new()
		var values := _values()
		for i in spec.options.size():
			var b := Button.new()
			b.text = spec.options[i]
			b.theme_type_variation = "SegmentButton"
			b.toggle_mode = true
			b.button_group = group
			b.focus_mode = Control.FOCUS_NONE
			b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			b.custom_minimum_size.y = 36
			var v = values[i]
			b.pressed.connect(func(): sheet.set(spec.prop, v))
			seg.add_child(b)
			_segments.append(b)
		add_child(seg)
	elif kind == "slider":
		_slider = HSlider.new()
		# Step first: a Range snaps its value to min + n * step.
		_slider.step = spec.get("step", 0.01)
		_slider.min_value = spec.min
		_slider.max_value = spec.max
		_slider.exp_edit = spec.get("exp", false)
		_slider.custom_minimum_size.y = 28
		_slider.focus_mode = Control.FOCUS_NONE
		_slider.value_changed.connect(func(v: float):
			sheet.set(spec.prop, v)
			_show_value(v))
		add_child(_slider)
	_tip = Style.label(spec.get("tip", ""), "Caption", true)
	_tip.visible = false
	add_child(_tip)
	refresh()


func _values() -> Array:
	return spec.get("values", range(spec.get("options", []).size()))


func _show_value(v: float) -> void:
	if _value == null:
		return
	var fmt: String = spec.get("fmt", "%.2f")
	_value.text = fmt % (int(v) if fmt.contains("%d") else v)


## Pull the current value from the sheet, without writing it back.
func refresh() -> void:
	var v = sheet.get(spec.prop)
	if _switch != null:
		_switch.set_pressed_no_signal(v)
	elif _option != null:
		_option.select(_values().find(v))
	elif not _segments.is_empty():
		var i := _values().find(v)
		for k in _segments.size():
			_segments[k].set_pressed_no_signal(k == i)
	elif _slider != null:
		_slider.set_value_no_signal(v)
		_show_value(v)
