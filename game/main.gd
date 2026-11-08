extends Node3D
## Main scene logic: touch/mouse input -> grabs, camera framing, HUD.
##
## Input is handled as *touch* events. With the project setting
## input_devices/pointing/emulate_touch_from_mouse on, a mouse click/drag
## arrives as touch index 0, so the same code works on desktop and iPhone,
## and several fingers can grab the skin at once on a touch screen.
##
## Command-line options (everything after a lone "--" goes to the game):
##   --screenshot <path.png>   save a screenshot after a moment, then quit
##   --demo-pull               grab the centre and pull it 15 mm right (for checks)

@onready var sheet: SkinSheet = $SkinSheet
@onready var camera: Camera3D = $Camera3D
@onready var info: Label = $HUD/Info

## Empty space around the sheet, as a fraction of its size.
const FRAME_MARGIN := 1.3


func _ready() -> void:
	# Children are ready before their parent, so the sheet already exists.
	sheet.mesh_rebuilt.connect(_frame_sheet)
	get_viewport().size_changed.connect(_frame_sheet)
	_frame_sheet()

	var args := OS.get_cmdline_user_args()
	if args.has("--demo-pull"):
		_demo_pull()
	var i := args.find("--screenshot")
	if i != -1 and i + 1 < args.size():
		_save_screenshot_and_quit(args[i + 1])


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventScreenTouch:
		if event.pressed:
			sheet.grab(event.index, _screen_to_sheet_plane(event.position))
		else:
			sheet.release_grab(event.index)
	elif event is InputEventScreenDrag:
		sheet.move_grab(event.index, _screen_to_sheet_plane(event.position))
	elif event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_W:
				sheet.show_wireframe = not sheet.show_wireframe
			KEY_R:
				sheet.reset_simulation()


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
	info.text = "%s\n%d vertices, %d triangles, mu %.0f kPa\n" % [
			sheet.get_solver_info(), sheet.get_vertex_count(), sheet.get_triangle_count(),
			sheet.shear_modulus_kpa] \
		+ "%s: %d it/frame, %.2f ms, residual %s N%s\n" % [
			status, s.iterations, s.step_ms, _sci(s.residual), flags] \
		+ "energy %.3f N*mm, area ratio %.2f-%.2f, grabs %d\n" % [
			s.energy, s.min_area_ratio, s.max_area_ratio, s.grabs] \
		+ "Drag the skin. [W] wireframe  [R] reset"


## Compact scientific notation, e.g. 1.2e-14 (GDScript's % has no %e).
func _sci(x: float) -> String:
	if x == 0.0:
		return "0"
	var exponent := floori(log(absf(x)) / log(10.0))
	return "%.1fe%d" % [x / pow(10.0, exponent), exponent]


## Simulate a finger pressing the sheet centre and dragging 15 mm right, by
## injecting the same touch events a real screen (or emulated mouse) produces.
## This exercises the full input path, including screen->sheet conversion.
func _demo_pull() -> void:
	await get_tree().process_frame  # let the camera settle on its first frame
	var start := camera.unproject_position(Vector3.ZERO)  # 3D point -> pixel
	var end := camera.unproject_position(Vector3(15, 0, 0))
	var touch := InputEventScreenTouch.new()
	touch.index = 0
	touch.pressed = true
	touch.position = start
	Input.parse_input_event(touch)
	var drag := InputEventScreenDrag.new()
	drag.index = 0
	drag.position = end
	drag.relative = end - start
	Input.parse_input_event(drag)


## An orthographic camera's `size` is the visible *height* in world units (mm).
## Fit both the sheet's height and width (given the window's aspect ratio).
func _frame_sheet() -> void:
	var vp := get_viewport().get_visible_rect().size
	camera.size = maxf(sheet.height, sheet.width / (vp.x / vp.y)) * FRAME_MARGIN


func _save_screenshot_and_quit(path: String) -> void:
	# Let the solver run for a moment (about 1 s at 60 fps), then capture.
	for _frame in 60:
		await RenderingServer.frame_post_draw
	var err := get_viewport().get_texture().get_image().save_png(path)
	print("Screenshot saved to %s (error code %d)" % [path, err])
	get_tree().quit()
