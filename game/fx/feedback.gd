class_name Feedback
extends Control
## Game-style feedback ("Effects" in the View panel): sparks while cutting,
## a pulse when a suture is tied, a whip-back of the thread and a short
## camera shake when one snaps. Drawing only: nothing here touches the
## mechanics.
##
## Everything is drawn in 2D over the 3D view, but anchored to points on
## the skin (world positions, projected every frame), so effects stay in
## place when the view is panned or zoomed. Cheap enough for phones: a few
## dozen circles and lines at most, and nothing at all when idle.

var camera: Camera3D
var enabled := false

var _sparks: Array[Dictionary] = []   # {at: Vector3, off: Vector2, v: Vector2, t, life, color, size}
var _rings: Array[Dictionary] = []    # {at, t, delay, life, color, radius}
var _whips: Array[Dictionary] = []    # {a, b, t, life, color}
var _shake := 0.0                     # px, decays
var _shake_t := 0.0
var _spark_clock := 0.0

const MAX_SPARKS := 160


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)


## Sparks from the scalpel tip while an incision is drawn (call every move).
func cut_spark(at: Vector3, dt_hint := 0.016) -> void:
	if not enabled:
		return
	_spark_clock += dt_hint
	if _spark_clock < 0.02:
		return
	_spark_clock = 0.0
	for i in 3:
		var a := randf() * TAU
		_spark(at, Vector2(cos(a), sin(a)) * randf_range(40.0, 140.0), randf_range(0.25, 0.5),
			Color(1.0, randf_range(0.75, 0.95), 0.4) if randf() < 0.6 else Color(1.0, 0.25, 0.3),
			randf_range(1.5, 3.0))


## A suture was tied: a ring pulses out from it, with a small burst.
func tie_pulse(at: Vector3, delay := 0.0, color := Color(0.35, 1.0, 0.6)) -> void:
	if not enabled:
		return
	_rings.append({"at": at, "t": -delay, "life": 0.5, "color": color, "radius": 34.0})
	if delay <= 0.0:
		burst(at, 10, color)


## A flap was raised: a bigger, slower ring.
func big_pulse(at: Vector3, color := Color(0.4, 0.75, 1.0)) -> void:
	if not enabled:
		return
	_rings.append({"at": at, "t": 0.0, "life": 0.8, "color": color, "radius": 90.0})
	_rings.append({"at": at, "t": -0.15, "life": 0.8, "color": color, "radius": 60.0})


## A suture snapped: the two thread ends whip back, sparks fly, the view shakes.
func snap(a: Vector3, b: Vector3) -> void:
	if not enabled:
		return
	_whips.append({"a": a, "b": b, "t": 0.0, "life": 0.6})
	var mid := (a + b) * 0.5
	burst(mid, 24, Color(1.0, 0.3, 0.25))
	_shake = maxf(_shake, 9.0)
	_shake_t = 0.0


func burst(at: Vector3, n: int, color: Color) -> void:
	for i in n:
		var a := randf() * TAU
		_spark(at, Vector2(cos(a), sin(a)) * randf_range(80.0, 260.0), randf_range(0.3, 0.7),
			color.lightened(randf() * 0.4), randf_range(2.0, 4.0))


func _spark(at: Vector3, v: Vector2, life: float, color: Color, size: float) -> void:
	if _sparks.size() >= MAX_SPARKS:
		_sparks.pop_front()
	_sparks.append({"at": at, "off": Vector2.ZERO, "v": v, "t": 0.0, "life": life,
		"color": color, "size": size})


func _process(delta: float) -> void:
	var busy := not (_sparks.is_empty() and _rings.is_empty() and _whips.is_empty())
	for s in _sparks:
		s.t += delta
		s.off += s.v * delta
		s.v *= 1.0 - minf(1.0, 4.0 * delta)  # drag
	_sparks = _sparks.filter(func(s): return s.t < s.life)
	for r in _rings:
		r.t += delta
	_rings = _rings.filter(func(r): return r.t < r.life)
	for w in _whips:
		w.t += delta
	_whips = _whips.filter(func(w): return w.t < w.life)
	# Camera shake: a decaying wobble, as offsets so it never moves the view for good.
	if camera != null:
		if _shake > 0.05:
			_shake_t += delta
			_shake *= exp(-delta * 9.0)
			var px_to_world := camera.size / maxf(1.0, get_viewport_rect().size.y)
			camera.h_offset = sin(_shake_t * 73.0) * _shake * px_to_world
			camera.v_offset = cos(_shake_t * 59.0) * _shake * px_to_world
		elif camera.h_offset != 0.0 or camera.v_offset != 0.0:
			_shake = 0.0
			camera.h_offset = 0.0
			camera.v_offset = 0.0
	if busy or not (_sparks.is_empty() and _rings.is_empty() and _whips.is_empty()):
		queue_redraw()


func _draw() -> void:
	if camera == null:
		return
	for r in _rings:
		if r.t < 0.0:
			continue
		var k: float = r.t / r.life
		var e := 1.0 - pow(1.0 - k, 3.0)  # ease out
		var c: Color = r.color
		c.a = 1.0 - k
		draw_arc(camera.unproject_position(r.at), 6.0 + e * r.radius, 0.0, TAU, 40, c, 3.0 * (1.0 - k) + 1.0, true)
	for s in _sparks:
		var k: float = s.t / s.life
		var c: Color = s.color
		c.a = 1.0 - k * k
		var p: Vector2 = camera.unproject_position(s.at) + s.off
		# A short streak along the spark's motion, fading out.
		draw_line(p, p - s.v * 0.03, Color(c, c.a * 0.6), s.size * (1.0 - k), true)
		draw_circle(p, s.size * (1.0 - 0.6 * k), c)
	for w in _whips:
		_draw_whip(w)


## The two halves of a snapped thread spring back from the break and curl.
func _draw_whip(w: Dictionary) -> void:
	var a := camera.unproject_position(w.a)
	var b := camera.unproject_position(w.b)
	var k: float = w.t / w.life
	var mid := (a + b) * 0.5
	var col := Color(1.0, 0.85, 0.3, 1.0 - k)
	for end in [[a, 1.0], [b, -1.0]]:
		var hole: Vector2 = end[0]
		var sgn: float = end[1]
		var dir := (mid - hole)
		var length := dir.length() * (1.0 - 0.8 * k)  # recoils towards its hole
		dir = dir.normalized() if dir.length() > 0.1 else Vector2.RIGHT
		var side := Vector2(-dir.y, dir.x) * sgn
		var pts := PackedVector2Array()
		for i in 9:
			var u := i / 8.0
			# A damped wave travelling along the loose end.
			var wave := sin(u * 9.0 - w.t * 40.0) * 10.0 * u * (1.0 - k)
			pts.append(hole + dir * length * u + side * wave)
		draw_polyline(pts, col, 2.5, true)
