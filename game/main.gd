extends Node3D
## Main scene logic: touch/mouse input -> grabs, incisions and sutures,
## camera framing, HUD.
##
## Input is handled as *touch* events. With the project setting
## input_devices/pointing/emulate_touch_from_mouse on, a mouse click/drag
## arrives as touch index 0, so the same code works on desktop and iPhone,
## and several fingers can grab the skin at once on a touch screen.
##
## Three tools:
##   GRAB    touches grab and pull the skin (forceps).
##   CUT     a touch draws an incision (scalpel); it is made on release.
##   SUTURE  tap an incision lip: a suture straight across to the matching
##           point of the other lip. Drag from one lip to another: a suture
##           between those two points (e.g. a key suture moving a flap corner).
## On desktop, Shift + drag cuts whatever the tool.
##
## Command-line options (everything after a lone "--" goes to the game):
##   --screenshot <path.png>   save a screenshot after a moment, then quit
##   --demo-pull               grab the centre and pull it 15 mm right (for checks)
##   --demo-cut                cut a 30 mm vertical incision, then pull its
##                             right lip 8 mm right so it gapes (for checks)
##   --demo-suture             as --demo-cut, then three sutures close the
##                             wound against the pull; heatmap on (for checks)

enum Tool { GRAB, CUT, SUTURE }

@onready var sheet: SkinSheet = $SkinSheet
@onready var camera: Camera3D = $Camera3D
@onready var info: Label = $HUD/Info
@onready var panel: ControlPanel = $HUD/ControlPanel
@onready var overlay: GrabOverlay = $HUD/GrabOverlay

## Empty space around the sheet, as a fraction of its size.
const FRAME_MARGIN := 1.3
## A new point is added to the scalpel path once the finger has moved this
## many pixels: enough to follow curves, few enough to keep the path short.
const CUT_SAMPLE_PX := 4.0
## A suture end snaps to a lip within this many pixels of the finger.
const SUTURE_SNAP_PX := 18.0
## A suture gesture shorter than this is a tap: suture straight across.
const TAP_PX := 10.0

var tool: Tool = Tool.GRAB
var _cut_touch := -1                      # touch index drawing the incision, -1 = none
var _cut_path := PackedVector2Array()     # screen positions of the stroke so far
var _suture_touch := -1                   # touch index placing a suture, -1 = none
var _suture_start := Vector2.ZERO
var _history: Array[Dictionary] = []      # undo stack: {"kind": "cut"} or {"kind": "suture", "id": n}
var _status := ""                         # last action's result, shown in the HUD


func _ready() -> void:
	# Children are ready before their parent, so the sheet already exists.
	panel.setup(sheet)
	panel.reset_requested.connect(sheet.reset_simulation)
	panel.tool_selected.connect(_set_tool)
	panel.undo_requested.connect(_undo)
	panel.cuts_cleared.connect(func(): _history = _history.filter(func(h): return h.kind != "cut"))
	panel.sutures_cleared.connect(func(): _history = _history.filter(func(h): return h.kind != "suture"))
	overlay.sheet = sheet
	overlay.camera = camera
	sheet.mesh_rebuilt.connect(_frame_sheet)
	sheet.mesh_rebuilt.connect(func(): _history.clear())  # new geometry: nothing to undo
	get_viewport().size_changed.connect(_frame_sheet)
	_frame_sheet()

	var args := OS.get_cmdline_user_args()
	if args.has("--demo-pull"):
		_demo_pull()
	if args.has("--demo-cut"):
		_demo_cut()
	if args.has("--demo-suture"):
		_demo_suture()
	var i := args.find("--screenshot")
	if i != -1 and i + 1 < args.size():
		_save_screenshot_and_quit(args[i + 1])


func _set_tool(t: int) -> void:
	tool = t as Tool
	panel.show_tool(tool)


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventScreenTouch:
		# Emulated touches are generated even over the UI panel; a press there
		# is meant for a slider, not for the skin underneath.
		if event.pressed and panel.get_global_rect().has_point(event.position):
			return
		if event.pressed:
			_on_press(event.index, event.position)
		else:
			_on_release(event.index, event.position)
	elif event is InputEventScreenDrag:
		if event.index == _cut_touch:
			if event.position.distance_to(_cut_path[_cut_path.size() - 1]) >= CUT_SAMPLE_PX:
				_cut_path.append(event.position)
			overlay.cut_path = _cut_path
		elif event.index == _suture_touch:
			overlay.suture_line = PackedVector2Array([_suture_start, event.position])
		else:
			sheet.move_grab(event.index, _screen_to_sheet_plane(event.position))
	elif event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_W:
				sheet.show_wireframe = not sheet.show_wireframe
			KEY_R:
				sheet.reset_simulation()
			KEY_G:
				_set_tool(Tool.GRAB)
			KEY_C:
				_set_tool(Tool.CUT)
			KEY_S:
				_set_tool(Tool.SUTURE)
			KEY_H:
				sheet.heatmap = (sheet.heatmap + 1) % 4
				panel.refresh()
			KEY_Z:
				_undo()


func _on_press(index: int, pos: Vector2) -> void:
	var cutting := tool == Tool.CUT or Input.is_key_pressed(KEY_SHIFT)
	if cutting:
		if _cut_touch == -1:
			_cut_touch = index
			_cut_path = PackedVector2Array([pos])
	elif tool == Tool.SUTURE:
		if _suture_touch == -1:
			_suture_touch = index
			_suture_start = pos
	else:
		sheet.grab(index, _screen_to_sheet_plane(pos))


func _on_release(index: int, pos: Vector2) -> void:
	if index == _cut_touch:
		_cut_path.append(pos)
		_commit_cut()
	elif index == _suture_touch:
		_commit_suture(pos)
	else:
		sheet.release_grab(index)


## Turn the finished stroke into an incision. The solver does the rest:
## mapping it onto the (possibly deformed) skin, cleaning it, re-meshing.
func _commit_cut() -> void:
	var world := PackedVector3Array()
	for p in _cut_path:
		world.append(_screen_to_sheet_plane(p))
	var segments := sheet.cut(world)
	if segments > 0:
		_history.append({"kind": "cut"})
		_status = "Incision: %d segments, re-meshed in %.1f ms." % [segments, sheet.get_last_cut_ms()]
	else:
		_status = "Nothing cut (too short, or not on the skin)."
	_cut_touch = -1
	_cut_path = PackedVector2Array()
	overlay.cut_path = _cut_path


func _commit_suture(end: Vector2) -> void:
	var snap_mm := SUTURE_SNAP_PX * camera.size / get_viewport().get_visible_rect().size.y
	var id: int
	if end.distance_to(_suture_start) < TAP_PX:
		id = sheet.suture_across(_screen_to_sheet_plane(_suture_start), snap_mm)
	else:
		id = sheet.suture(_screen_to_sheet_plane(_suture_start), _screen_to_sheet_plane(end), snap_mm)
	if id >= 0:
		_history.append({"kind": "suture", "id": id})
		_status = "Suture placed."
	else:
		_status = "No suture: start (and end) on an incision lip, not at its very tip."
	_suture_touch = -1
	overlay.suture_line = PackedVector2Array()


## Undo the most recent cut or suture.
func _undo() -> void:
	if _history.is_empty():
		_status = "Nothing to undo."
		return
	var last: Dictionary = _history.pop_back()
	if last.kind == "cut":
		sheet.undo_cut()
		_status = "Incision undone."
	else:
		sheet.remove_suture(last.id)
		_status = "Suture removed."


## Turn a screen position into the 3D point on the sheet's plane under it:
## cast a ray from the camera through that pixel and intersect it with the
## horizontal plane y = 0 that the sheet lies in.
func _screen_to_sheet_plane(screen_pos: Vector2) -> Vector3:
	var origin := camera.project_ray_origin(screen_pos)
	var direction := camera.project_ray_normal(screen_pos)
	var hit = Plane(Vector3.UP, 0.0).intersects_ray(origin, direction)
	return hit if hit != null else origin


func _process(_delta: float) -> void:
	var s: Dictionary = sheet.get_step_info()
	var status := "at rest" if s.converged else "solving"
	if s.stalled:
		status += " (stalled)"
	var flags := ""
	if s.projected:
		flags += " projected"
	if s.regularized:
		flags += " regularized"
	var tool_line: String = [
		"Tool: GRAB - drag the skin.",
		"Tool: CUT - draw an incision.",
		"Tool: SUTURE - tap a lip (across) or drag lip to lip.",
	][tool]
	info.text = "%s\n%d vertices, %d triangles, %d incisions, %d sutures\n" % [
			sheet.get_solver_info(), sheet.get_vertex_count(), sheet.get_triangle_count(),
			sheet.get_cut_count(), sheet.get_suture_count()] \
		+ "%s: %d it/frame, %.2f ms, residual %s N%s\n" % [
			status, s.iterations, s.step_ms, _sci(s.residual), flags] \
		+ "energy %.3f N*mm, area ratio %.2f-%.2f, grabs %d\n" % [
			s.energy, s.min_area_ratio, s.max_area_ratio, s.grabs] \
		+ (_status + "\n" if _status != "" else "") \
		+ tool_line + "\n[G]rab [C]ut [S]uture  [Shift+drag] cut  [Z] undo  [H] heatmap  [W] wireframe  [R] reset"


## Compact scientific notation, e.g. 1.2e-14 (GDScript's % has no %e).
func _sci(x: float) -> String:
	if x == 0.0:
		return "0"
	var exponent := floori(log(absf(x)) / log(10.0))
	return "%.1fe%d" % [x / pow(10.0, exponent), exponent]


## Inject the touch events a real screen (or emulated mouse) would produce,
## so demos exercise the full input path, including screen->sheet conversion.
func _touch(index: int, sheet_mm: Vector2, pressed: bool) -> void:
	var touch := InputEventScreenTouch.new()
	touch.index = index
	touch.pressed = pressed
	touch.position = _sheet_to_screen(sheet_mm)
	Input.parse_input_event(touch)


func _drag(index: int, sheet_mm: Vector2) -> void:
	var drag := InputEventScreenDrag.new()
	drag.index = index
	drag.position = _sheet_to_screen(sheet_mm)
	Input.parse_input_event(drag)


## Solver coordinates (mm, y up) -> screen pixel. Godot's 3D z is the solver's -y.
func _sheet_to_screen(p: Vector2) -> Vector2:
	return camera.unproject_position(sheet.to_global(Vector3(p.x, 0, -p.y)))


## Simulate a finger pressing the sheet centre and dragging 15 mm right.
func _demo_pull() -> void:
	await get_tree().process_frame  # let the camera settle on its first frame
	_touch(0, Vector2.ZERO, true)
	_drag(0, Vector2(15, 0))


## Draw a vertical incision through the centre with the scalpel, then grab
## just right of it and pull right: the incision should gape.
func _demo_cut() -> void:
	await get_tree().process_frame
	_set_tool(Tool.CUT)
	_touch(0, Vector2(0, -15), true)
	for k in range(1, 31):
		_drag(0, Vector2(0, -15 + k))
	_touch(0, Vector2(0, 15), false)
	await get_tree().process_frame
	_set_tool(Tool.GRAB)
	_touch(0, Vector2(3, 0), true)
	_drag(0, Vector2(11, 0))


## The cut demo, then (still pulling) three sutures tapped across the left
## lip; they close the wound against the pull. Von Mises heatmap on.
func _demo_suture() -> void:
	await _demo_cut()
	for _frame in 30:
		await get_tree().process_frame  # let the wound open
	_set_tool(Tool.SUTURE)
	for y in [-8.0, 0.0, 8.0]:
		# Touch index 1: finger 0 is still holding the forceps.
		_touch(1, Vector2(-0.3, y), true)
		_touch(1, Vector2(-0.3, y), false)
	sheet.heatmap = 2
	panel.refresh()


## An orthographic camera's `size` is the visible *height* in world units (mm).
## Fit the sheet into the part of the window not covered by the side panel,
## then slide the camera right so the sheet is centred in that free area.
func _frame_sheet() -> void:
	var vp := get_viewport().get_visible_rect().size
	var free_width := maxf(vp.x - panel.PANEL_WIDTH, 100.0)
	camera.size = maxf(sheet.height, sheet.width / (free_width / vp.y)) * FRAME_MARGIN
	var mm_per_px := camera.size / vp.y
	camera.position.x = 0.5 * panel.PANEL_WIDTH * mm_per_px


func _save_screenshot_and_quit(path: String) -> void:
	# Let the solver run for a moment (about 2 s at 60 fps), then capture.
	for _frame in 120:
		await RenderingServer.frame_post_draw
	var err := get_viewport().get_texture().get_image().save_png(path)
	print("Screenshot saved to %s (error code %d)" % [path, err])
	get_tree().quit()
