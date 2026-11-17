class_name GrabOverlay
extends Control
## Draws each active grab on top of the 3D view:
##   - a circle around the grabbed patch (its radius),
##   - a line from the patch to the finger,
##   - the pulling force in newtons, orange when capped at max_force
##     (the skin is lagging behind the finger: it won't come further).
## and, while an incision is being drawn, the scalpel's path.
## Also each suture (coloured by how close it is to breaking, labelled where
## it helps), recently snapped sutures, the suture being placed, and the
## heatmap's colour bar.
##
## 3D world points are turned into screen pixels with
## Camera3D.unproject_position(), the reverse of the picking ray in main.gd.

var sheet: SkinSheet
var camera: Camera3D
var ui: AppShell
var main: Node            # main.gd: holds the pen markings
## The pen stroke being drawn (screen) and its colour.
var pen_path := PackedVector2Array()
var pen_color := Color.BLACK
## Screen positions of the incision being drawn (empty when not cutting).
var cut_path := PackedVector2Array()
## Needle entry and current exit (screen) of a suture being placed (empty otherwise).
var suture_line := PackedVector2Array()
var suture_line_ok := true   # both ends on skin
var selected_suture := -1
## Fusiform excision being designed (screen points, closed; empty otherwise).
var excise_outline := PackedVector2Array()
## Flap being designed (screen points): defect (PackedVector2Array); flaps,
## bases, burow, incisions, key_sutures (Arrays of them); text. Empty when
## not designing.
var flap_preview: Dictionary = {}
## Stress maps: short strokes along the direction of the tension.
var show_tension_lines := true
## Load dots at each suture's needle holes (View panel; off by default).
var show_suture_dots := false
## Skin hooks (main.gd): grab id -> true; and for fingers still held, how far
## the ring has filled towards setting a hook (id -> 0..1).
var hooks := {}
var hook_progress := {}
var _probe_key: Array = []
var _probe_text := ""
## The look's way of drawing (Look.overlay_style): thread_width,
## thread_color (a real thread; load as rings at the needle holes), ink_bleed,
## outline
## (dark edge under the thread), knot (a knot on the thread). Empty: plain.
var style := {}
## Where to read the colour map's value (mouse or finger; x < 0: nowhere).
var probe_pos := Vector2(-1, -1)
## Undermining brush outline (x < 0: hidden).
var brush_pos := Vector2(-1, -1)
var brush_radius_px := 0.0

## Suture labels: 0 smart (selected, near breaking, under the mouse), 1 all, 2 none.
var label_mode := 0
## Sutures that snapped recently: {"a", "b" (world), "time" (s, counts down), "text"}.
var snaps: Array[Dictionary] = []

## Smart labels show sutures loaded beyond this fraction of their strength.
const LABEL_LOAD := 0.75
## A snapped suture's mark stays this long (s).
const SNAP_MARK_TIME := 4.0
## An unbreakable thread is coloured green -> red up to this closing force.
const SUTURE_HIGH_N := 3.0

const COLOR_NORMAL := Color(1, 1, 1, 0.9)
const COLOR_LIMITED := Color(1.0, 0.55, 0.1, 0.95)
const COLOR_SCALPEL := Color(0.9, 0.1, 0.15, 0.95)


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE  # purely visual; let input through


func _process(delta: float) -> void:
	for s in snaps:
		s.time -= delta
	snaps = snaps.filter(func(s): return s.time > 0.0)
	queue_redraw()  # ask Godot to call _draw() again this frame


## Mark a suture that just snapped (called by main.gd on SkinSheet.suture_snapped).
func add_snap(a: Vector3, b: Vector3, text: String) -> void:
	snaps.append({"a": a, "b": b, "time": SNAP_MARK_TIME, "text": text})


func _draw() -> void:
	if sheet == null or camera == null:
		return
	Prof.begin("draw")
	Prof.begin("draw tension lines")
	if show_tension_lines:
		_draw_tension_lines()
	Prof.end("draw tension lines")
	if cut_path.size() >= 2:
		# Dark halo underneath so the line shows on skin and background alike.
		draw_polyline(cut_path, Color(0, 0, 0, 0.6), 5.0, true)
		draw_polyline(cut_path, COLOR_SCALPEL, 2.5, true)
	var font: Font = Style.FONT_REGULAR
	Prof.begin("draw lesions+marks")
	_draw_lesions()
	_draw_marks()
	Prof.end("draw lesions+marks")
	if excise_outline.size() >= 2:
		draw_polyline(excise_outline, Color(0, 0, 0, 0.6), 4.0, true)
		draw_polyline(excise_outline, Color(0.55, 0.2, 0.85, 0.95), 2.0, true)
	_draw_flap_preview(font)
	Prof.begin("draw sutures")
	_draw_sutures(font)
	Prof.end("draw sutures")
	Prof.begin("draw legend+rstl+probe")
	Prof.begin("legend")
	_draw_legend(font)
	Prof.end("legend")
	Prof.begin("rstl")
	_draw_rstl(font)
	Prof.end("rstl")
	_draw_probe(font)
	Prof.end("draw legend+rstl+probe")
	if brush_pos.x >= 0.0:
		draw_arc(brush_pos, brush_radius_px, 0.0, TAU, 48, Color(0.35, 0.15, 0.45, 0.9), 2.0, true)
		draw_arc(brush_pos, 0.6 * brush_radius_px, 0.0, TAU, 48, Color(0.35, 0.15, 0.45, 0.5), 1.0, true)
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
		var label := "%.2f N%s" % [g.force, "  (max)" if g.force_limited else ""]
		var pos := t + Vector2(10, -10)
		if hooks.has(g.id):
			_draw_hook(t, p, color)
			# Hooks stay for long: a smaller label, beside the hook.
			_pill(font, t + Vector2(18, 5), label, 12, Color(color, 0.85))
			continue
		draw_circle(t, 5.0, color)
		var k: float = hook_progress.get(g.id, 0.0)
		if k > 0.08:
			# The finger is still: a ring fills up; when full, a hook is set.
			draw_arc(t, 20.0, -PI * 0.5, -PI * 0.5 + TAU * k, 40, Color(1, 1, 1, 0.9), 3.0, true)
			draw_arc(t, 20.0, 0.0, TAU, 40, Color(1, 1, 1, 0.2), 3.0, true)
		_pill(font, pos, label, 16, color)
	Prof.end("draw")


## A skin hook: a ring where it holds (tap or drag it there) with a small
## hook shape pointing at the skin it pulls.
func _draw_hook(at: Vector2, skin: Vector2, color: Color) -> void:
	var dir := (skin - at).normalized() if at.distance_to(skin) > 2.0 else Vector2.DOWN
	draw_circle(at + Vector2(1.5, 2), 11.0, Color(0, 0, 0, 0.3))
	draw_circle(at, 11.0, Color(0.12, 0.13, 0.16, 0.92))
	draw_arc(at, 11.0, 0.0, TAU, 32, color, 2.0, true)
	# The hook: a shank towards the skin ending in a curl.
	var side := Vector2(-dir.y, dir.x)
	var pts := PackedVector2Array()
	for i in 10:
		var u := i / 9.0
		pts.append(at - dir * 5.0 + dir * 9.0 * minf(u * 1.6, 1.0) + side * 3.5 * maxf(0.0, u * 1.6 - 1.0) * 1.6)
	pts.append(at + dir * 4.0 + side * 3.5)
	pts.append(at + dir * 1.0 + side * 4.0)
	draw_polyline(pts, Color(0.92, 0.94, 1.0), 2.0, true)


func _draw_sutures(font: Font) -> void:
	if suture_line.size() == 2:
		# Blue while valid; red when the needle is not over skin.
		var c := Color(0.2, 0.35, 1.0) if suture_line_ok else Color(0.9, 0.15, 0.15)
		draw_dashed_line(suture_line[0], suture_line[1], c, 2.0, 6.0)
		draw_circle(suture_line[0], 3.5, c)
		draw_circle(suture_line[1], 3.5, c)
	var mouse := get_viewport().get_mouse_position()
	var hovered := -1
	var best := 12.0  # px
	var infos: Array = sheet.get_suture_info()
	for su in infos:
		var d := Geometry2D.get_closest_point_to_segment(mouse, camera.unproject_position(su.a),
			camera.unproject_position(su.b)).distance_to(mouse)
		if d < best:
			best = d
			hovered = su.id
	for su in infos:
		var a := camera.unproject_position(su.a)  # needle entry
		var b := camera.unproject_position(su.b)  # needle exit
		# Colour by load: how close the thread is to its breaking strength
		# (green, yellow at 50%, red at 100%). Over 100% (snapping off) it
		# flashes. An unbreakable thread is coloured by closing force instead.
		var t: float = su.load if su.strength_n > 0.0 else su.force / SUTURE_HIGH_N
		var color := _load_color(t)
		if su.slack:
			color = Color(0.7, 0.7, 0.7)
		var overloaded: bool = su.load > 1.0
		if overloaded and fmod(Time.get_ticks_msec() / 250.0, 2.0) < 1.0:
			color = Color(1, 1, 1)
		# The thread between the two needle holes; a white halo when selected.
		var tw: float = style.get("thread_width", 2.5)
		if su.id == selected_suture:
			draw_line(a, b, Color(1, 1, 1, 0.9), tw + 4.5, true)
		var thread: Variant = style.get("thread_color")
		if thread != null:
			# A real thread (black nylon): its own colour with a soft shadow;
			# the load shows only as rings around the needle holes.
			var sh := Vector2(1.5, 2.0)
			draw_line(a + sh, b + sh, Color(0, 0, 0, 0.3), tw + 1.5, true)
			draw_line(a, b, thread, tw, true)
			# Monofilament catches the light: a thin highlight along it.
			draw_line(a - Vector2(0.4, 0.5), b - Vector2(0.4, 0.5), Color(1, 1, 1, 0.22), maxf(0.8, tw * 0.35), true)
			var k := a.lerp(b, 0.3)
			# The knot's two cut ends ("ears"), a few mm long, splayed apart.
			var dir := (b - a).normalized() if a.distance_to(b) > 1.0 else Vector2.RIGHT
			var ear := tw * 4.0
			for ang in [0.6, -0.6]:
				var e := k + dir.rotated(PI * 0.5 + ang) * ear
				draw_line(k + sh, e + sh, Color(0, 0, 0, 0.25), tw * 0.9, true)
				draw_line(k, e, thread, tw * 0.8, true)
			draw_circle(k + sh, tw * 1.5, Color(0, 0, 0, 0.3))
			draw_circle(k, tw * 1.4, thread)
			draw_circle(k - Vector2(0.4, 0.6) * tw * 0.5, tw * 0.45, Color(1, 1, 1, 0.35))
			for h in [a, b]:
				draw_circle(h, 2.2, Color(0.25, 0.04, 0.04, 0.9))
				if show_suture_dots:
					draw_arc(h, 4.5 if overloaded else 4.0, 0.0, TAU, 20, Color(color, 0.85), 1.5, true)
		else:
			if style.get("glow", false):
				# A soft halo in the thread's colour (the 2D overlay is not
				# part of the 3D glow, so it is drawn).
				draw_line(a, b, Color(color, 0.18), tw + 10.0, true)
				draw_line(a, b, Color(color, 0.3), tw + 5.0, true)
			if style.get("outline", false):
				# A thread coloured by its load, on a dark edge so it reads on skin.
				draw_line(a, b, Color(0.08, 0.04, 0.04, 0.75), tw + 2.0, true)
				draw_line(a, b, color, tw, true)
			else:
				draw_line(a, b, Color(0.05, 0.05, 0.15), tw, true)
			if style.get("knot", false):
				var k := a.lerp(b, 0.3)
				draw_circle(k, tw * 1.3, Color(0.08, 0.04, 0.04, 0.85))
				draw_circle(k, tw * 0.8, color)
			if show_suture_dots:
				draw_circle(a, 3.5 if overloaded else 3.0, color)
				draw_circle(b, 3.5 if overloaded else 3.0, color)
		# Text only where it helps (see label_mode): with many sutures, labels
		# on every one cover the wound.
		var show := label_mode == 1
		if label_mode == 0:
			show = su.id == selected_suture or su.id == hovered or su.load >= LABEL_LOAD
		if not show:
			continue
		var label: String
		if su.slack:
			label = "slack"
		elif su.strength_n > 0.0:
			label = "%.1f N  %.0f%%" % [su.force, su.load * 100.0]
		else:
			label = "%.1f N" % su.force
		if overloaded:
			label += "  OVER"
		if su.id == selected_suture:
			# Loop stress: stress along the thread in the tissue the loop
			# holds (+ stretched, - squeezed).
			label += "  %+.0f kPa" % su.loop_stress_kpa
		_pill(font, _beyond_end(font, a, b, label, 12), label, 12, color)
	_draw_snaps(font)


## Where a suture's label goes: past the thread's right-hand end, in line
## with the thread, so it sits on intact skin beside the wound rather than
## over it (the selected suture's buttons go past the other end). Returns
## the text's baseline start for _pill.
func _beyond_end(font: Font, a: Vector2, b: Vector2, text: String, size: int) -> Vector2:
	var right := b if b.x >= a.x else a
	var left := a if b.x >= a.x else b
	var dir := (right - left).normalized() if left.distance_to(right) > 1.0 else Vector2.RIGHT
	var p := right + dir * 16.0
	var w := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x
	# A thread pointing up or down: put the text beside the end, centred.
	if absf(dir.x) < 0.35:
		p.x -= 0.5 * w
		p.y += 6.0 if dir.y > 0.0 else -2.0
		if dir.y > 0.0:
			p.y += font.get_ascent(size)
	else:
		p.y += 0.35 * font.get_ascent(size)
	return p


## Green (0) -> yellow (0.5) -> red (1 and beyond).
func _load_color(t: float) -> Color:
	t = clampf(t, 0.0, 1.0)
	if t < 0.5:
		return Color(0.1, 0.8, 0.2).lerp(Color(1.0, 0.85, 0.1), 2.0 * t)
	return Color(1.0, 0.85, 0.1).lerp(Color(0.95, 0.1, 0.1), 2.0 * t - 1.0)


## A snapped suture: the two needle holes with short loose thread ends,
## fading out, and what happened.
func _draw_snaps(font: Font) -> void:
	for s in snaps:
		var alpha := clampf(s.time / SNAP_MARK_TIME * 2.0, 0.0, 1.0)
		var red := Color(0.95, 0.15, 0.1, alpha)
		var a := camera.unproject_position(s.a)
		var b := camera.unproject_position(s.b)
		var dir := (b - a).normalized() if a.distance_to(b) > 1.0 else Vector2.RIGHT
		var side := Vector2(-dir.y, dir.x)
		# Each end: a hole and a curled thread end pointing back at the wound.
		draw_circle(a, 3.0, red)
		draw_circle(b, 3.0, red)
		draw_polyline(PackedVector2Array([a, a + dir * 7.0 + side * 3.0, a + dir * 10.0 - side * 2.0]),
			red, 2.0, true)
		draw_polyline(PackedVector2Array([b, b - dir * 7.0 - side * 3.0, b - dir * 10.0 + side * 2.0]),
			red, 2.0, true)
		var pos := (a + b) * 0.5 + Vector2(8, 14)
		_pill(font, pos, s.text, 13, red)


## The flap design under the finger: the flaps shaded, the defect (purple,
## removed) and any Burow's triangles, the incisions (red), the uncut bases
## (green, what the flaps stay attached by) and dotted lines for the key
## sutures.
func _draw_flap_preview(font: Font) -> void:
	if flap_preview.is_empty():
		return
	for flap in flap_preview.flaps:
		_fill(flap, Color(0.3, 0.6, 1.0, 0.25))
	var defect: PackedVector2Array = flap_preview.defect
	var removed: Array = [defect]
	removed.append_array(flap_preview.burow)
	for d in removed:
		if d.size() >= 2:
			_fill(d, Color(0.55, 0.2, 0.85, 0.25))
			draw_polyline(d, Color(0, 0, 0, 0.6), 4.0, true)
			draw_polyline(d, Color(0.55, 0.2, 0.85, 0.95), 2.0, true)
	for inc in flap_preview.incisions:
		if inc.size() >= 2:
			draw_polyline(inc, Color(0, 0, 0, 0.6), 4.0, true)
			draw_polyline(inc, COLOR_SCALPEL, 2.0, true)
	for base in flap_preview.bases:
		for k in range(base.size() - 1):
			draw_dashed_line(base[k], base[k + 1], Color(0.2, 0.85, 0.3, 0.95), 2.0, 6.0)
	for k in flap_preview.key_sutures:
		if k.size() == 2:
			draw_dashed_line(k[0], k[1], Color(0.2, 0.35, 1.0, 0.9), 1.5, 4.0)
			draw_circle(k[0], 3.0, Color(0.2, 0.35, 1.0))
			draw_circle(k[1], 3.0, Color(0.2, 0.35, 1.0))
	var anchor := defect[0] if defect.size() > 0 else Vector2.ZERO
	if defect.is_empty() and not flap_preview.incisions.is_empty() and flap_preview.incisions[0].size() > 0:
		anchor = flap_preview.incisions[0][0]
	_pill(font, anchor + Vector2(10, -12), flap_preview.text, 13, Color(0.95, 0.95, 0.95))


## Fill a closed outline (its last point repeats the first) if it is a
## proper polygon.
func _fill(outline: PackedVector2Array, color: Color) -> void:
	if outline.size() < 4:
		return
	var poly := outline.slice(0, outline.size() - 1)
	if Geometry2D.triangulate_polygon(poly).size() > 0:
		draw_colored_polygon(poly, color)


## Stress maps: a stroke along the principal direction every few mm, longer
## and darker where the stress is higher. Around a flap they show the lines
## of tension: where the closure pulls, and which way.
func _draw_tension_lines() -> void:
	if sheet.heatmap not in [1, 2, 4]:
		return
	var px_per_mm := get_viewport_rect().size.y / camera.size
	# About 22 px apart on screen, whatever the zoom (but not finer than 1.5 mm).
	var spacing := maxf(22.0 / px_per_mm, 1.5)
	var d: Dictionary = sheet.get_tension_lines(snappedf(spacing, 0.5))
	if d.is_empty():
		return
	var seg: PackedVector3Array = d.segments
	var strength: PackedFloat32Array = d.strength
	for i in strength.size():
		var a := camera.unproject_position(seg[2 * i])
		var b := camera.unproject_position(seg[2 * i + 1])
		var t := clampf(strength[i], 0.0, 1.0)
		draw_line(a, b, Color(0.05, 0.04, 0.08, 0.45 + 0.45 * t), 1.2 + 2.0 * t, true)


## The colour map's value under the mouse or finger, in a small pill.
func _draw_probe(font: Font) -> void:
	if probe_pos.x < 0.0 or sheet.heatmap == 0 or ui.is_over_ui(probe_pos):
		return
	# Looking up the point on the mesh is not free: ask again only when the
	# pointer, the view or the skin changed.
	var key := [probe_pos, camera.position, camera.size, sheet.get_render_version(), sheet.heatmap]
	if key != _probe_key:
		_probe_key = key
		_probe_text = ""
		var origin := camera.project_ray_origin(probe_pos)
		var hit = Plane(Vector3.UP, 0.0).intersects_ray(origin, camera.project_ray_normal(probe_pos))
		if hit != null:
			var r: Dictionary = sheet.sample_heatmap(hit)
			if r.ok:
				_probe_text = r.text
	if _probe_text != "":
		_pill(font, probe_pos + Vector2(16, 28), _probe_text, 12, Style.TEXT)


## The colour map's legend: a compact horizontal bar in a card at the
## bottom-left of the skin area, low values on the left. Ticks mark the
## contour lines drawn on the skin.
func _draw_legend(font: Font) -> void:
	var legend: Dictionary = sheet.get_heatmap_legend()
	if legend.is_empty():
		return
	var colors: PackedColorArray = legend.colors
	var fr := ui.free_rect()
	var w := clampf(fr.size.x * 0.45, 170.0, 240.0)
	var h := 10.0
	# Bottom-left of the skin area; on a phone, top-left (the bottom is busy
	# with hints and the tool sheet).
	var y := fr.position.y + 4.0 if ui.narrow else fr.end.y - 104.0
	var card := Rect2(Vector2(fr.position.x + 12.0, y), Vector2(w + 28.0, 92.0))
	draw_style_box(Style.box(Color(Style.SURFACE, 0.94), Style.RADIUS_CARD, Style.BORDER), card)
	var origin := card.position + Vector2(14, 36)
	var n := colors.size()
	for i in n:
		draw_rect(Rect2(origin + Vector2(w * i / n, 0), Vector2(w / n + 1.0, h)), colors[i])
	var ticks: int = legend.get("ticks", 0)
	for k in range(1, ticks):
		var x := origin.x + w * k / ticks
		draw_line(Vector2(x, origin.y), Vector2(x, origin.y + h), Color(0, 0, 0, 0.45), 1.0)
	var dim := Style.TEXT_DIM
	var title: String = legend.title
	if legend.get("auto", false):
		title += "  ·  auto scale"
	draw_string(font, card.position + Vector2(14, 24), title, HORIZONTAL_ALIGNMENT_LEFT, w, 13, Style.TEXT)
	draw_string(font, origin + Vector2(0, h + 16), legend.low, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, dim)
	draw_string(font, origin + Vector2(0, h + 16), legend.high, HORIZONTAL_ALIGNMENT_RIGHT, w, 11, dim)
	var note: String = legend.extreme
	if show_tension_lines and sheet.heatmap in [1, 2, 4]:
		note += "  ·  strokes: " + ("squeeze" if sheet.heatmap == 4 else "pull")
	draw_string(font, origin + Vector2(0, h + 32), note, HORIZONTAL_ALIGNMENT_LEFT, w, 11, dim)


## Small compass, top-right of the 3D view: the RSTL (fibre) direction.
func _draw_rstl(font: Font) -> void:
	if sheet.fibre_stiffness_kpa <= 0.0 and sheet.prestretch_along_pct <= 0.0 \
			and sheet.prestretch_across_pct <= 0.0:
		return
	# Bottom-right corner of the skin area (clear of the top bar and panel);
	# on a phone, smaller and top-right (the bottom is busy).
	var fr := ui.free_rect()
	var k := 0.75 if ui.narrow else 1.0
	var centre := Vector2(fr.end.x - 32.0, fr.position.y + 36.0) if ui.narrow \
		else Vector2(fr.end.x - 52.0, fr.end.y - 64.0)
	# Screen y points down while the sheet's y points up: flip the angle.
	var a := deg_to_rad(sheet.fibre_angle_deg)
	var d := Vector2(cos(a), -sin(a)) * 28.0 * k
	draw_circle(centre, 40.0 * k, Color(0, 0, 0, 0.45))
	for off in [-10.0, 0.0, 10.0]:
		var n: Vector2 = Vector2(-d.y, d.x).normalized() * off * k
		draw_line(centre - d + n, centre + d + n, Color(0.95, 0.85, 0.7, 0.9), 2.0, true)
	draw_string(font, centre + Vector2(-16, 34) * k, "RSTL", HORIZONTAL_ALIGNMENT_LEFT, -1, int(12 * k),
		Color(0.95, 0.85, 0.7))


## Lesions: a brown disc on the skin, following it as it deforms. Parts
## that have been cut out are not drawn; an incompletely excised lesion is
## outlined in red.
func _draw_lesions() -> void:
	for l in sheet.get_lesion_info():
		var pieces: Array = l.outline
		var edge := Color(0.9, 0.1, 0.1, 0.95) if l.excised and not l.complete else Color(0.2, 0.1, 0.05, 0.9)
		for piece in pieces:
			var pts := PackedVector2Array()
			for p in piece:
				pts.append(camera.unproject_position(p))
			if pts.size() < 2:
				continue
			if pieces.size() == 1 and pts.size() > 40:  # whole lesion still on skin: fill it
				# (Without the repeated closing point: the polygon fill wants each corner once.)
				draw_colored_polygon(pts.slice(0, pts.size() - 1), Color(0.35, 0.2, 0.12, 0.85))
			draw_polyline(pts, edge, 2.0, true)


## A text label on a rounded dark pill (readable over skin and heatmaps).
## `pos` is the text's baseline start, as for draw_string.
func _pill(font: Font, pos: Vector2, text: String, size: int, color: Color) -> void:
	var w := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x
	var asc := font.get_ascent(size)
	var r := Rect2(pos + Vector2(-7, -asc - 4), Vector2(w + 14, asc + font.get_descent(size) + 8))
	var bg := Style.box(Color(Style.BG, 0.82 * color.a), 9)
	draw_style_box(bg, r)
	draw_string(font, pos, text, HORIZONTAL_ALIGNMENT_LEFT, -1, size, color)


## Pen markings: each line is kept in rest coordinates and drawn where that
## tissue is now, so it stretches with the skin; parts over a wound or an
## excised area are not drawn. Then the stroke being drawn.
## The markings on screen are cached: mapping them (dense, ~2 points per mm)
## and projecting every point each frame would cost a phone real time. They
## only change when the skin moves, the view moves or a marking is edited.
var _marks_key: Array = []
var _marks_screen: Array = []  # [{"color": Color, "pieces": [PackedVector2Array]}]


func _marks_on_screen() -> Array:
	var key: Array = [sheet.get_render_version(), camera.position, camera.size,
		get_viewport_rect().size, main.marks.size(), main.marks.hash()]
	if key == _marks_key:
		return _marks_screen
	_marks_key = key
	_marks_screen = []
	for m in main.marks:
		var pieces: Array = []
		for piece in sheet.rest_to_world(m.rest):
			if piece.size() < 2:
				continue
			var pts := PackedVector2Array()
			pts.resize(piece.size())
			for k in piece.size():
				pts[k] = camera.unproject_position(piece[k])
			pieces.append(pts)
		_marks_screen.append({"color": ContextPanel.PEN_COLORS[m.color], "pieces": pieces})
	return _marks_screen


func _draw_marks() -> void:
	if main != null:
		for m in _marks_on_screen():
			var c: Color = m.color
			for pts in m.pieces:
				if style.get("neon", false):
					draw_polyline(pts, Color(c.lightened(0.3), 0.18), 10.0, true)
					draw_polyline(pts, Color(c.lightened(0.3), 0.35), 5.5, true)
					draw_polyline(pts, c.lightened(0.6), 2.2, true)
				elif style.get("ink_bleed", false):
					# Marker ink soaking into the skin: a soft halo under the line.
					draw_polyline(pts, Color(c, 0.22), 7.0, true)
					draw_polyline(pts, Color(c, 0.75), 2.6, true)
				else:
					draw_polyline(pts, Color(c, 0.9), 3.0, true)
	if pen_path.size() >= 2:
		draw_polyline(pen_path, Color(pen_color, 0.9), 3.0, true)
