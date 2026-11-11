class_name GrabOverlay
extends Control
## Draws each active grab on top of the 3D view:
##   - a circle around the grabbed patch (its radius),
##   - a line from the patch to the finger,
##   - the pulling force in newtons, orange when capped at max_force
##     (the skin is lagging behind the finger: it won't come further).
## and, while an incision is being drawn, the scalpel's path.
## Also each suture (coloured and labelled by its tension), the suture being
## placed, and the heatmap's colour bar.
##
## 3D world points are turned into screen pixels with
## Camera3D.unproject_position(), the reverse of the picking ray in main.gd.

var sheet: SkinSheet
var camera: Camera3D
## Screen positions of the incision being drawn (empty when not cutting).
var cut_path := PackedVector2Array()
## Start and end (screen) of a suture being dragged out (empty otherwise).
var suture_line := PackedVector2Array()

## Suture colour runs green -> yellow -> red up to this tension. Illustrative:
## a few newtons is a tight skin closure (a 4-0 nylon breaks at roughly 10 N).
const SUTURE_HIGH_N := 3.0

const COLOR_NORMAL := Color(1, 1, 1, 0.9)
const COLOR_LIMITED := Color(1.0, 0.55, 0.1, 0.95)
const COLOR_SCALPEL := Color(0.9, 0.1, 0.15, 0.95)


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE  # purely visual; let input through


func _process(_delta: float) -> void:
	queue_redraw()  # ask Godot to call _draw() again this frame


func _draw() -> void:
	if sheet == null or camera == null:
		return
	if cut_path.size() >= 2:
		# Dark halo underneath so the line shows on skin and background alike.
		draw_polyline(cut_path, Color(0, 0, 0, 0.6), 5.0, true)
		draw_polyline(cut_path, COLOR_SCALPEL, 2.5, true)
	var font := ThemeDB.fallback_font
	_draw_sutures(font)
	_draw_legend(font)
	# Orthographic camera: `size` mm spans the viewport height in pixels.
	var px_per_mm := get_viewport_rect().size.y / camera.size
	for g in sheet.get_grab_info():
		var p := camera.unproject_position(g.point)
		var t := camera.unproject_position(g.target)
		var color := COLOR_LIMITED if g.force_limited else COLOR_NORMAL
		draw_line(p, t, color, 2.0, true)
		if g.radius > 0.0:
			draw_arc(p, g.radius * px_per_mm, 0.0, TAU, 48, color, 2.0, true)
		else:
			draw_circle(p, 4.0, color)
		draw_circle(t, 5.0, color)
		var label := "%.2f N%s" % [g.force, "  (max)" if g.force_limited else ""]
		# Dark outline first, then the text on top: readable over any background.
		var pos := t + Vector2(10, -10)
		draw_string_outline(font, pos, label, HORIZONTAL_ALIGNMENT_LEFT, -1, 16, 5, Color(0, 0, 0, 0.85))
		draw_string(font, pos, label, HORIZONTAL_ALIGNMENT_LEFT, -1, 16, color)


func _draw_sutures(font: Font) -> void:
	if suture_line.size() == 2:
		draw_dashed_line(suture_line[0], suture_line[1], Color(0.2, 0.3, 1.0), 2.0, 6.0)
	for su in sheet.get_suture_info():
		var a := camera.unproject_position(su.a)
		var b := camera.unproject_position(su.b)
		var t := clampf(su.force / SUTURE_HIGH_N, 0.0, 1.0)
		var color: Color
		if t < 0.5:
			color = Color(0.1, 0.8, 0.2).lerp(Color(1.0, 0.85, 0.1), 2.0 * t)
		else:
			color = Color(1.0, 0.85, 0.1).lerp(Color(0.95, 0.1, 0.1), 2.0 * t - 1.0)
		# Drawn as a short dark bar across the wound with a dot at each end,
		# at least ~10 px long so it stays visible once the edges meet.
		var dir := (b - a).normalized() if a.distance_to(b) > 0.5 else Vector2.RIGHT
		var bite := dir * 5.0
		draw_line(a - bite, b + bite, Color(0.05, 0.05, 0.15), 3.0, true)
		draw_circle(a - bite, 2.5, color)
		draw_circle(b + bite, 2.5, color)
		var label := "%.2f N" % su.force
		var pos := (a + b) * 0.5 + Vector2(8, -6)
		draw_string_outline(font, pos, label, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, 4, Color(0, 0, 0, 0.85))
		draw_string(font, pos, label, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, color)


## Vertical colour bar, bottom-left, with its scale and the current extreme.
func _draw_legend(font: Font) -> void:
	var legend: Dictionary = sheet.get_heatmap_legend()
	if legend.is_empty():
		return
	var colors: PackedColorArray = legend.colors
	var h := 180.0
	var w := 18.0
	var origin := Vector2(20, get_viewport_rect().size.y - h - 40)
	draw_rect(Rect2(origin - Vector2(8, 26), Vector2(150, h + 52)), Color(0, 0, 0, 0.55))
	var n := colors.size()
	for i in n:
		# Index 0 = low value, drawn at the bottom.
		var y := origin.y + h * (1.0 - float(i + 1) / n)
		draw_rect(Rect2(Vector2(origin.x, y), Vector2(w, h / n + 1.0)), colors[i])
	var white := Color(0.92, 0.92, 0.92)
	draw_string(font, origin + Vector2(0, -8), legend.title, HORIZONTAL_ALIGNMENT_LEFT, -1, 13, white)
	draw_string(font, origin + Vector2(w + 6, 10), legend.high, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, white)
	draw_string(font, origin + Vector2(w + 6, h), legend.low, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, white)
	draw_string(font, origin + Vector2(0, h + 20), legend.extreme, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, white)
