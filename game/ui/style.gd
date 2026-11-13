class_name Style
extends RefCounted
## The app's look in one place: colours, sizes and the Godot Theme built from
## them ("design tokens"). Every control gets its appearance from this theme
## (set once on the UI root), so nothing is styled node by node.
##
## A Theme maps (control type, item name) -> value: e.g. ("Button", "normal")
## -> a StyleBox (background), ("Label", "font_color") -> a colour. A "type
## variation" is a named variant of a type ("ToolButton" based on Button),
## picked per node with `theme_type_variation`.

# --- Colours: calm dark neutrals, one teal accent -------------------------
const BG := Color("#121418")          # behind the skin
const SURFACE := Color("#1b1f26")     # cards, panels
const RAISED := Color("#232933")      # buttons, inputs
const HOVER := Color("#2b323d")
const BORDER := Color("#2e3541")
const TEXT := Color("#e8eaed")
const TEXT_DIM := Color("#9aa3ae")
const TEXT_FAINT := Color("#6b7480")
const ACCENT := Color("#2bb3a3")      # teal
const ACCENT_DIM := Color("#1d7f74")
const ACCENT_SOFT := Color(0.169, 0.702, 0.639, 0.16)
const WARN := Color("#f0b429")
const DANGER := Color("#e5484d")
const OK := Color("#46c46b")

# --- Sizes ----------------------------------------------------------------
const RADIUS := 8
const RADIUS_CARD := 12
const GAP := 8
const PAD := 14
const TOUCH := 44          # minimum touch target, px (Apple/Google guidance)
const FONT_SIZE := 14
const FONT_SMALL := 12
const FONT_TITLE := 16
const RAIL_WIDTH := 76
const PANEL_WIDTH := 320

const FONT_REGULAR := preload("res://ui/assets/fonts/Inter-Regular.ttf")
const FONT_SEMIBOLD := preload("res://ui/assets/fonts/Inter-SemiBold.ttf")

static var _theme: Theme
static var _icons := {}


## A tool or UI icon (white SVG, tint with modulate or a button's icon colours).
static func icon(name: String) -> Texture2D:
	if not _icons.has(name):
		_icons[name] = load("res://ui/assets/icons/%s.svg" % name)
	return _icons[name]


static func box(bg: Color, radius := RADIUS, border := Color(0, 0, 0, 0), pad_x := 12.0,
		pad_y := 8.0) -> StyleBoxFlat:
	var s := StyleBoxFlat.new()
	s.bg_color = bg
	s.set_corner_radius_all(radius)
	if border.a > 0.0:
		s.border_color = border
		s.set_border_width_all(1)
	s.content_margin_left = pad_x
	s.content_margin_right = pad_x
	s.content_margin_top = pad_y
	s.content_margin_bottom = pad_y
	s.anti_aliasing = true
	return s


## A filled circle (slider grabbers, switch knobs), drawn into an image.
static func _disc(diameter: int, fill: Color, ring := Color(0, 0, 0, 0)) -> ImageTexture:
	var img := Image.create(diameter, diameter, false, Image.FORMAT_RGBA8)
	var r := diameter * 0.5
	for y in diameter:
		for x in diameter:
			var d := Vector2(x + 0.5 - r, y + 0.5 - r).length()
			var a := clampf(r - d, 0.0, 1.0)  # 1-pixel soft edge (anti-aliasing)
			var c := fill
			if ring.a > 0.0 and d > r - 3.0:
				c = ring
			img.set_pixel(x, y, Color(c, c.a * a))
	return ImageTexture.create_from_image(img)


## A pill-shaped switch (on/off) for CheckButton.
static func _switch(on: bool) -> ImageTexture:
	var w := 40
	var h := 22
	var img := Image.create(w, h, false, Image.FORMAT_RGBA8)
	var track := ACCENT if on else Color("#3a4250")
	var knob_x := w - h * 0.5 if on else h * 0.5
	for y in h:
		for x in w:
			var p := Vector2(x + 0.5, y + 0.5)
			# Distance to the pill's centre line, minus its radius.
			var cx := clampf(p.x, h * 0.5, w - h * 0.5)
			var d_track := p.distance_to(Vector2(cx, h * 0.5)) - h * 0.5
			var d_knob := p.distance_to(Vector2(knob_x, h * 0.5)) - (h * 0.5 - 3.0)
			var c := Color(0, 0, 0, 0)
			if d_track < 0.5:
				c = Color(track, clampf(0.5 - d_track, 0.0, 1.0))
			if d_knob < 0.5:
				c = Color(Color.WHITE, 1.0).lerp(c, clampf(d_knob + 0.5, 0.0, 1.0)) \
					if c.a > 0.0 else Color(1, 1, 1, clampf(0.5 - d_knob, 0.0, 1.0))
			img.set_pixel(x, y, c)
	return ImageTexture.create_from_image(img)


## The app's Theme (built once).
static func theme() -> Theme:
	if _theme != null:
		return _theme
	var t := Theme.new()
	t.default_font = FONT_REGULAR
	t.default_font_size = FONT_SIZE

	# Labels and their variations.
	t.set_color("font_color", "Label", TEXT)
	t.set_type_variation("Caption", "Label")
	t.set_color("font_color", "Caption", TEXT_DIM)
	t.set_font_size("font_size", "Caption", FONT_SMALL)
	t.set_type_variation("Title", "Label")
	t.set_font("font", "Title", FONT_SEMIBOLD)
	t.set_font_size("font_size", "Title", FONT_TITLE)
	t.set_type_variation("SectionTitle", "Label")
	t.set_font("font", "SectionTitle", FONT_SEMIBOLD)
	t.set_font_size("font_size", "SectionTitle", FONT_SMALL)
	t.set_color("font_color", "SectionTitle", TEXT_DIM)
	t.set_type_variation("Value", "Label")
	t.set_font("font", "Value", FONT_SEMIBOLD)
	t.set_color("font_color", "Value", ACCENT)

	# Buttons: raised by default.
	t.set_stylebox("normal", "Button", box(RAISED))
	t.set_stylebox("hover", "Button", box(HOVER))
	t.set_stylebox("pressed", "Button", box(ACCENT_DIM))
	t.set_stylebox("hover_pressed", "Button", box(ACCENT_DIM))
	t.set_stylebox("disabled", "Button", box(Color(RAISED, 0.5)))
	t.set_stylebox("focus", "Button", StyleBoxEmpty.new())
	for item in ["font_color", "font_hover_color", "icon_normal_color", "icon_hover_color"]:
		t.set_color(item, "Button", TEXT)
	for item in ["font_pressed_color", "font_hover_pressed_color", "icon_pressed_color",
			"icon_hover_pressed_color"]:
		t.set_color(item, "Button", Color.WHITE)
	t.set_color("font_disabled_color", "Button", TEXT_FAINT)
	t.set_color("icon_disabled_color", "Button", TEXT_FAINT)
	t.set_constant("h_separation", "Button", 8)

	# Primary action: filled accent.
	t.set_type_variation("PrimaryButton", "Button")
	t.set_stylebox("normal", "PrimaryButton", box(ACCENT))
	t.set_stylebox("hover", "PrimaryButton", box(ACCENT.lightened(0.1)))
	t.set_stylebox("pressed", "PrimaryButton", box(ACCENT_DIM))
	t.set_font("font", "PrimaryButton", FONT_SEMIBOLD)
	for item in ["font_color", "font_hover_color", "font_pressed_color", "icon_normal_color"]:
		t.set_color(item, "PrimaryButton", Color("#0b1f1c"))

	# Flat icon button (top bar, rail): transparent until hovered or toggled on.
	t.set_type_variation("FlatButton", "Button")
	t.set_stylebox("normal", "FlatButton", box(Color(0, 0, 0, 0), RADIUS, Color(0, 0, 0, 0), 6, 6))
	t.set_stylebox("hover", "FlatButton", box(HOVER, RADIUS, Color(0, 0, 0, 0), 6, 6))
	t.set_stylebox("pressed", "FlatButton", box(ACCENT_SOFT, RADIUS, Color(0, 0, 0, 0), 6, 6))
	t.set_stylebox("hover_pressed", "FlatButton", box(ACCENT_SOFT, RADIUS, Color(0, 0, 0, 0), 6, 6))
	t.set_stylebox("disabled", "FlatButton", box(Color(0, 0, 0, 0), RADIUS, Color(0, 0, 0, 0), 6, 6))
	for item in ["font_color", "font_hover_color", "icon_normal_color", "icon_hover_color"]:
		t.set_color(item, "FlatButton", TEXT_DIM)
	for item in ["font_pressed_color", "font_hover_pressed_color", "icon_pressed_color",
			"icon_hover_pressed_color"]:
		t.set_color(item, "FlatButton", ACCENT)

	# Segmented control: a row of toggle buttons, the chosen one filled.
	t.set_type_variation("SegmentButton", "Button")
	t.set_stylebox("normal", "SegmentButton", box(RAISED, RADIUS, Color(0, 0, 0, 0), 6, 7))
	t.set_stylebox("hover", "SegmentButton", box(HOVER, RADIUS, Color(0, 0, 0, 0), 6, 7))
	t.set_stylebox("pressed", "SegmentButton", box(ACCENT_SOFT, RADIUS, ACCENT, 6, 7))
	t.set_stylebox("hover_pressed", "SegmentButton", box(ACCENT_SOFT, RADIUS, ACCENT, 6, 7))
	t.set_font_size("font_size", "SegmentButton", 13)
	t.set_color("font_pressed_color", "SegmentButton", ACCENT)
	t.set_color("font_hover_pressed_color", "SegmentButton", ACCENT)

	# Option buttons (drop-downs) and their popup menus.
	t.set_stylebox("normal", "OptionButton", box(RAISED))
	t.set_stylebox("hover", "OptionButton", box(HOVER))
	t.set_stylebox("pressed", "OptionButton", box(HOVER))
	t.set_stylebox("focus", "OptionButton", StyleBoxEmpty.new())
	t.set_icon("arrow", "OptionButton", _scaled_icon("chevron_down", 16))
	t.set_stylebox("panel", "PopupMenu", box(RAISED, RADIUS, BORDER, 6, 6))
	t.set_stylebox("hover", "PopupMenu", box(ACCENT_SOFT, 6))
	t.set_color("font_color", "PopupMenu", TEXT)
	t.set_color("font_hover_color", "PopupMenu", Color.WHITE)
	t.set_constant("v_separation", "PopupMenu", 10)
	t.set_icon("radio_checked", "PopupMenu", _disc(12, ACCENT))
	t.set_icon("radio_unchecked", "PopupMenu", _disc(12, Color(0, 0, 0, 0), BORDER))

	# Switches.
	t.set_icon("checked", "CheckButton", _switch(true))
	t.set_icon("unchecked", "CheckButton", _switch(false))
	t.set_icon("checked_disabled", "CheckButton", _switch(true))
	t.set_icon("unchecked_disabled", "CheckButton", _switch(false))
	for s in ["normal", "hover", "pressed", "hover_pressed", "focus"]:
		t.set_stylebox(s, "CheckButton", box(Color(0, 0, 0, 0), RADIUS, Color(0, 0, 0, 0), 0, 4))
	t.set_color("font_color", "CheckButton", TEXT)
	t.set_color("font_hover_color", "CheckButton", TEXT)
	t.set_color("font_pressed_color", "CheckButton", TEXT)
	t.set_color("font_hover_pressed_color", "CheckButton", TEXT)

	# Sliders: thin track, accent fill up to a round grabber.
	var track := box(Color("#323a47"), 3, Color(0, 0, 0, 0), 0, 2)
	var fill := box(ACCENT, 3, Color(0, 0, 0, 0), 0, 2)
	t.set_stylebox("slider", "HSlider", track)
	t.set_stylebox("grabber_area", "HSlider", fill)
	t.set_stylebox("grabber_area_highlight", "HSlider", fill)
	t.set_icon("grabber", "HSlider", _disc(18, Color.WHITE))
	t.set_icon("grabber_highlight", "HSlider", _disc(20, Color.WHITE))
	t.set_constant("center_grabber", "HSlider", 1)

	# Cards and panels.
	t.set_stylebox("panel", "PanelContainer", box(SURFACE, RADIUS_CARD, BORDER, PAD, PAD))
	t.set_type_variation("Card", "PanelContainer")
	t.set_stylebox("panel", "Card", box(SURFACE, RADIUS_CARD, BORDER, PAD, PAD))
	t.set_type_variation("Inset", "PanelContainer")
	t.set_stylebox("panel", "Inset", box(RAISED, RADIUS, Color(0, 0, 0, 0), 12, 10))
	t.set_type_variation("Pill", "PanelContainer")
	t.set_stylebox("panel", "Pill", box(Color(SURFACE, 0.92), 18, BORDER, 14, 8))
	t.set_type_variation("Rail", "PanelContainer")
	t.set_stylebox("panel", "Rail", box(SURFACE, 0, Color(0, 0, 0, 0), 6, 10))

	# Text input.
	t.set_stylebox("normal", "LineEdit", box(RAISED))
	t.set_stylebox("focus", "LineEdit", box(RAISED, RADIUS, ACCENT))
	t.set_color("font_color", "LineEdit", TEXT)
	t.set_color("font_placeholder_color", "LineEdit", TEXT_FAINT)

	# Thin scrollbars.
	var bar := box(Color(0, 0, 0, 0), 3, Color(0, 0, 0, 0), 3, 0)
	t.set_stylebox("scroll", "VScrollBar", bar)
	t.set_stylebox("grabber", "VScrollBar", box(Color("#3a4250"), 3, Color(0, 0, 0, 0), 3, 0))
	t.set_stylebox("grabber_highlight", "VScrollBar", box(Color("#4a5464"), 3, Color(0, 0, 0, 0), 3, 0))
	t.set_stylebox("grabber_pressed", "VScrollBar", box(ACCENT_DIM, 3, Color(0, 0, 0, 0), 3, 0))

	# Tooltips.
	t.set_stylebox("panel", "TooltipPanel", box(RAISED, RADIUS, BORDER, 10, 6))
	t.set_color("font_color", "TooltipLabel", TEXT)

	t.set_constant("separation", "VBoxContainer", GAP)
	t.set_constant("separation", "HBoxContainer", GAP)
	_theme = t
	return t


## An icon resized to `px` (for theme items that don't scale their icons).
static func _scaled_icon(name: String, px: int) -> Texture2D:
	var tex := icon(name)
	if tex == null:
		return null
	var img := tex.get_image()
	img.resize(px, px, Image.INTERPOLATE_LANCZOS)
	return ImageTexture.create_from_image(img)


## A label with a type variation, in one line.
static func label(text: String, variation := "", wrap := false) -> Label:
	var l := Label.new()
	l.text = text
	l.theme_type_variation = variation
	if wrap:
		l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	return l


## A button with an icon (and optional text), touch-sized.
static func icon_button(icon_name: String, text := "", tooltip := "", variation := "FlatButton") -> Button:
	var b := Button.new()
	b.icon = icon(icon_name)
	b.text = text
	b.tooltip_text = tooltip
	b.theme_type_variation = variation
	b.expand_icon = true
	b.custom_minimum_size = Vector2(TOUCH, TOUCH)
	b.focus_mode = Control.FOCUS_NONE  # keep keyboard shortcuts for the skin
	b.add_theme_constant_override("icon_max_width", 20)
	return b
