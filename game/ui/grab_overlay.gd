class_name GrabOverlay
extends Control
## Draws each active grab on top of the 3D view:
##   - a circle around the grabbed patch (its radius),
##   - a line from the patch to the finger,
##   - the pulling force in newtons, orange when capped at max_force
##     (the skin is lagging behind the finger: it won't come further).
##
## 3D world points are turned into screen pixels with
## Camera3D.unproject_position(), the reverse of the picking ray in main.gd.

var sheet: SkinSheet
var camera: Camera3D

const COLOR_NORMAL := Color(1, 1, 1, 0.9)
const COLOR_LIMITED := Color(1.0, 0.55, 0.1, 0.95)


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE  # purely visual; let input through


func _process(_delta: float) -> void:
	queue_redraw()  # ask Godot to call _draw() again this frame


func _draw() -> void:
	if sheet == null or camera == null:
		return
	var font := ThemeDB.fallback_font
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
