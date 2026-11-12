extends Node3D
## Main scene logic: touch/mouse input -> grabs, incisions and sutures,
## camera framing, HUD.
##
## Input is handled as *touch* events. With the project setting
## input_devices/pointing/emulate_touch_from_mouse on, a mouse click/drag
## arrives as touch index 0, so the same code works on desktop and iPhone,
## and several fingers can grab the skin at once on a touch screen.
##
## Six tools:
##   GRAB    touches grab and pull the skin (forceps).
##   CUT     a touch draws an incision (scalpel); it is made on release.
##   SUTURE  press where the needle goes in, drag, release where it comes out:
##           the thread is tied with the knot tightness set in the panel.
##           Tap a suture to select it; the tightness slider then re-ties it.
##   UNDERMINE paint the areas where the skin is freed from the underlying
##           tissue (brush size in the panel); they are shaded darker.
##   LESION  tap to mark a lesion (diameter in the panel).
##   EXCISE  drag from one tip of a fusiform (elliptical) excision to the
##           other: it is cut and removed on release (shape in the panel).
##           Or tap inside any outline already cut free (e.g. a rhomboid
##           drawn with the scalpel) to remove it.
## On desktop, Shift + drag cuts whatever the tool.
##
## Command-line options (everything after a lone "--" goes to the game):
##   --screenshot <path.png>   save a screenshot after a moment, then quit
##   --demo-pull               grab the centre and pull it 15 mm right (for checks)
##   --demo-cut                cut a 30 mm vertical incision, then pull its
##                             right lip 8 mm right so it gapes (for checks)
##   --demo-suture             cut, close with three sutures (3 mm bites, the
##                             middle one tied 30% tight), then pull the right
##                             side; heatmap on (for checks)
##   --demo-m6                 prestressed skin: an incision across the RSTL
##                             gapes by itself; one side undermined; max
##                             principal stress heatmap (for checks)
##   --demo-excise             a 6 mm lesion, fusiform excision along the
##                             RSTL with a 2 mm margin, closed with five
##                             sutures; min principal stress heatmap (for checks)
##   --demo-wrinkle            a short, blunt ellipse (1.5:1, 90 degree tips)
##                             excised and closed: dog-ears at both ends;
##                             excess skin heatmap (for checks)

enum Tool { GRAB, CUT, SUTURE, UNDERMINE, LESION, EXCISE }

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
## A suture gesture shorter than this is a tap: it selects a suture instead.
const TAP_PX := 10.0
## A tap within this many pixels of a suture's thread selects it.
const PICK_PX := 14.0

var tool: Tool = Tool.GRAB
var _cut_touch := -1                      # touch index drawing the incision, -1 = none
var _cut_path := PackedVector2Array()     # screen positions of the stroke so far
var _suture_touch := -1                   # touch index placing a suture, -1 = none
var _suture_start := Vector2.ZERO
var _selected_suture := -1                # id, -1 = none
var _undermine_touch := -1                # touch index painting, -1 = none
var _undermine_new := true                # next stamp starts a new stroke (undo step)
var _last_paint_pos := Vector2.ZERO
var _excise_touch := -1                   # touch index designing an excision, -1 = none
var _excise_start := Vector2.ZERO
var _history: Array[Dictionary] = []      # undo stack: {"kind": "cut"} or {"kind": "suture", "id": n}
var _status := ""                         # last action's result, shown in the HUD


func _ready() -> void:
	# Children are ready before their parent, so the sheet already exists.
	panel.setup(sheet)
	panel.reset_requested.connect(sheet.reset_simulation)
	panel.tool_selected.connect(_set_tool)
	panel.undo_requested.connect(_undo)
	panel.tightness_changed.connect(func(t: float):
		if _selected_suture >= 0:
			sheet.set_suture_tightness(_selected_suture, t))
	panel.remove_suture_requested.connect(func():
		if _selected_suture >= 0:
			sheet.remove_suture(_selected_suture)
			_select_suture(-1))
	panel.cuts_cleared.connect(func():  # clearing the incisions also puts excised skin back
		_history = _history.filter(func(h): return h.kind != "cut" and h.kind != "excise"))
	panel.lesions_cleared.connect(func(): _history = _history.filter(func(h): return h.kind != "lesion"))
	panel.undermining_cleared.connect(func(): _history = _history.filter(func(h): return h.kind != "undermine"))
	panel.sutures_cleared.connect(func():
		_history = _history.filter(func(h): return h.kind != "suture")
		_select_suture(-1))
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
	if args.has("--demo-m6"):
		_demo_m6()
	if args.has("--demo-excise"):
		_demo_excise()
	if args.has("--demo-wrinkle"):
		_demo_wrinkle()
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
		elif event.index == _undermine_touch:
			_paint_undermining(event.position)
		elif event.index == _excise_touch:
			_preview_excision(event.position)
		elif event.index == _suture_touch:
			overlay.suture_line = PackedVector2Array([_suture_start, event.position])
			overlay.suture_line_ok = _on_skin(_suture_start) \
				and _on_skin(event.position)
		else:
			sheet.move_grab(event.index, _screen_to_sheet_plane(event.position))
	elif event is InputEventMouseMotion:
		# Preview the brush under the mouse (desktop only; touch has no hover).
		overlay.brush_pos = event.position if tool == Tool.UNDERMINE else Vector2(-1, -1)
		overlay.brush_radius_px = sheet.undermine_radius * get_viewport().get_visible_rect().size.y / camera.size
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
			KEY_U:
				_set_tool(Tool.UNDERMINE)
			KEY_L:
				_set_tool(Tool.LESION)
			KEY_E:
				_set_tool(Tool.EXCISE)
			KEY_H:
				sheet.heatmap = (sheet.heatmap + 1) % 6
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
	elif tool == Tool.UNDERMINE:
		if _undermine_touch == -1:
			_undermine_touch = index
			_undermine_new = true
			_paint_undermining(pos)
	elif tool == Tool.LESION:
		if sheet.add_lesion(_screen_to_sheet_plane(pos)):
			_history.append({"kind": "lesion"})
			_status = "Lesion marked (%.1f mm)." % sheet.lesion_diameter
		else:
			_status = "A lesion must be on the skin."
	elif tool == Tool.EXCISE:
		if _excise_touch == -1:
			_excise_touch = index
			_excise_start = pos
	else:
		sheet.grab(index, _screen_to_sheet_plane(pos))


func _on_release(index: int, pos: Vector2) -> void:
	if index == _cut_touch:
		_cut_path.append(pos)
		_commit_cut()
	elif index == _suture_touch:
		_commit_suture(pos)
	elif index == _excise_touch:
		_commit_excision(pos)
	elif index == _undermine_touch:
		_undermine_touch = -1
		if not _undermine_new:  # something was painted: one undo step
			_history.append({"kind": "undermine"})
			_status = "Undermined (%d areas)." % sheet.get_undermine_count()
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
	_suture_touch = -1
	overlay.suture_line = PackedVector2Array()
	if end.distance_to(_suture_start) < TAP_PX:
		# A tap: select the suture under the finger (or deselect).
		var pick_mm := PICK_PX * camera.size / get_viewport().get_visible_rect().size.y
		_select_suture(sheet.pick_suture(_screen_to_sheet_plane(end), pick_mm))
		return
	var id := sheet.suture(_screen_to_sheet_plane(_suture_start), _screen_to_sheet_plane(end),
		panel.tightness)
	if id >= 0:
		_history.append({"kind": "suture", "id": id})
		_select_suture(id)
		_status = "Suture tied (%+.0f%%)." % (panel.tightness * 100.0)
	else:
		_status = "No suture: the needle must go in and come out on skin."


func _select_suture(id: int) -> void:
	_selected_suture = id
	overlay.selected_suture = id
	if id >= 0:
		for su in sheet.get_suture_info():
			if su.id == id:
				panel.show_suture_tightness(su.tightness)


## True if the screen point is over skin (not over an open wound or outside).
func _on_skin(screen_pos: Vector2) -> bool:
	return sheet.is_on_skin(_screen_to_sheet_plane(screen_pos))


## Undo the most recent cut or suture.
func _undo() -> void:
	if _history.is_empty():
		_status = "Nothing to undo."
		return
	var last: Dictionary = _history.pop_back()
	if last.kind == "cut":
		sheet.undo_cut()
		_status = "Incision undone."
	elif last.kind == "undermine":
		sheet.undo_undermine()
		_status = "Undermining undone."
	elif last.kind == "lesion":
		sheet.undo_lesion()
		_status = "Lesion removed."
	elif last.kind == "excise":
		sheet.undo_excise()
		if last.with_cut:
			sheet.undo_cut()
		_status = "Excision undone."
	else:
		sheet.remove_suture(last.id)
		if last.id == _selected_suture:
			_select_suture(-1)
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
	panel.show_selected_suture(_selected_info())
	panel.show_contact(sheet.get_contact_info())
	panel.show_lesions(sheet.get_lesion_info())
	var tool_line: String = [
		"Tool: GRAB - drag the skin.",
		"Tool: CUT - draw an incision.",
		"Tool: SUTURE - drag from needle entry to exit; tap a suture to select it.",
		"Tool: UNDERMINE - paint where the skin is freed from the tissue below.",
		"Tool: LESION - tap to mark a lesion.",
		"Tool: EXCISE - drag tip to tip for a fusiform excision; tap inside a closed outline to remove it.",
	][tool]
	# The web version can be opened by anyone with the link: say what it is.
	var notice := "Research prototype: illustrative tissue values, not for clinical use.\n" \
		if OS.has_feature("web") else ""
	info.text = notice + "%s\n%d vertices, %d triangles, %d incisions, %d sutures\n" % [
			sheet.get_solver_info(), sheet.get_vertex_count(), sheet.get_triangle_count(),
			sheet.get_cut_count(), sheet.get_suture_count()] \
		+ "%s: %d it/frame, %.2f ms, residual %s N%s\n" % [
			status, s.iterations, s.step_ms, _sci(s.residual), flags] \
		+ "energy %.3f N*mm, area ratio %.2f-%.2f, grabs %d\n" % [
			s.energy, s.min_area_ratio, s.max_area_ratio, s.grabs] \
		+ (_status + "\n" if _status != "" else "") \
		+ tool_line + "\n[G]rab [C]ut [S]uture [U]ndermine [L]esion [E]xcise  [Shift] cut  [Z] undo  [H] heatmap  [W] wire  [R] reset"


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


## Cut, close it with three sutures (needle 3 mm either side of the wound,
## the middle one tied 30% tight), then pull the right side away: the
## sutures hold the wound closed. Von Mises heatmap on.
func _demo_suture() -> void:
	await get_tree().process_frame
	_set_tool(Tool.CUT)
	_touch(0, Vector2(0, -15), true)
	for k in range(1, 31):
		_drag(0, Vector2(0, -15 + k))
	_touch(0, Vector2(0, 15), false)
	await get_tree().process_frame
	_set_tool(Tool.SUTURE)
	for y in [-8.0, 0.0, 8.0]:
		# The slider re-ties the *selected* suture: deselect first, so it only
		# sets the tightness for the next one.
		_select_suture(-1)
		panel.set_tightness(0.3 if y == 0.0 else 0.0)
		_touch(0, Vector2(-3, y), true)
		_drag(0, Vector2(3, y))
		_touch(0, Vector2(3, y), false)
		# Injected events are queued and handled on the next frame: wait, so
		# they are handled with this tool and this tightness.
		await get_tree().process_frame
	_set_tool(Tool.GRAB)
	_touch(0, Vector2(8, 0), true)
	_drag(0, Vector2(15, 0))
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


func _selected_info() -> Dictionary:
	if _selected_suture < 0:
		return {}
	for su in sheet.get_suture_info():
		if su.id == _selected_suture:
			return su
	_select_suture(-1)  # it disappeared (e.g. its skin was re-meshed away)
	return {}


## Paint from the previous brush position to this one. Godot merges drag
## events ("accumulated input": one per frame), so a quick stroke arrives as
## a few far-apart points; stamping only those would leave gaps. Instead,
## stamps are laid every quarter radius along the way.
func _paint_undermining(screen_pos: Vector2) -> void:
	var radius_px := sheet.undermine_radius * get_viewport().get_visible_rect().size.y / camera.size
	overlay.brush_pos = screen_pos
	overlay.brush_radius_px = radius_px
	var from := screen_pos if _undermine_new else _last_paint_pos
	var spacing := maxf(2.0, 0.25 * radius_px)
	var steps := maxi(1, ceili(from.distance_to(screen_pos) / spacing))
	for k in range(1 if not _undermine_new else 0, steps + 1):
		var p := from.lerp(screen_pos, float(k) / steps)
		if sheet.undermine_at(_screen_to_sheet_plane(p), _undermine_new):
			_undermine_new = false
	_last_paint_pos = screen_pos


## Prestressed skin, RSTL horizontal: an incision across them gapes on its
## own. Then undermine the right side and show the max principal stress.
func _demo_m6() -> void:
	await get_tree().process_frame
	_set_tool(Tool.CUT)
	_touch(0, Vector2(0, -15), true)
	for k in range(1, 31):
		_drag(0, Vector2(0, -15 + k))
	_touch(0, Vector2(0, 15), false)
	await get_tree().process_frame
	_set_tool(Tool.UNDERMINE)
	_touch(0, Vector2(8, -18), true)
	for k in range(1, 19):
		_drag(0, Vector2(8, -18 + 2 * k))
	_touch(0, Vector2(8, 18), false)
	await get_tree().process_frame
	sheet.heatmap = 1
	panel.refresh()


## Show the fusiform excision that releasing here would cut.
func _preview_excision(screen_pos: Vector2) -> void:
	if screen_pos.distance_to(_excise_start) < TAP_PX:
		overlay.excise_outline = PackedVector2Array()
		return
	var pts := PackedVector2Array()
	for p in sheet.get_fusiform_outline(_screen_to_sheet_plane(_excise_start), _screen_to_sheet_plane(screen_pos)):
		pts.append(camera.unproject_position(p))
	overlay.excise_outline = pts


## Release of the excision gesture: a drag cuts and removes a fusiform
## excision; a tap removes the piece of skin under the finger, if it has
## been cut free all round.
func _commit_excision(end: Vector2) -> void:
	_excise_touch = -1
	overlay.excise_outline = PackedVector2Array()
	var fusiform := end.distance_to(_excise_start) >= TAP_PX
	var result: int
	if fusiform:
		result = sheet.excise_fusiform(_screen_to_sheet_plane(_excise_start), _screen_to_sheet_plane(end))
	else:
		result = sheet.excise(_screen_to_sheet_plane(end))
	if result == 1:
		_history.append({"kind": "excise", "with_cut": fusiform})
		_status = "Excised. " + _lesion_summary()
	elif result == 0:
		_status = "Nothing excised: not on the skin."
	elif fusiform:
		_status = "Nothing excised: the outline could not be closed (off the skin, or across a defect)."
	else:
		_status = "Nothing excised: this skin is not cut free all round. Close the outline first."


## One line about the lesions: excised completely (with margin) or not.
func _lesion_summary() -> String:
	var parts: Array[String] = []
	var i := 0
	for l in sheet.get_lesion_info():
		i += 1
		if not l.excised:
			continue
		if l.complete:
			parts.append("Lesion %d: complete, margin %.1f mm." % [i, l.margin_mm])
		else:
			parts.append("Lesion %d: INCOMPLETE, the edge runs through it." % i)
	return " ".join(parts)


## A 6 mm lesion, excised along the RSTL (horizontal) with a 2 mm margin:
## a 10 x 30 mm fusiform (3:1, 30 degree tips), then closed with five
## sutures, 3 mm bites. The min principal stress heatmap shows where the
## closure squeezes the skin: at the ends (where dog-ears form).
func _demo_excise() -> void:
	await get_tree().process_frame
	_set_tool(Tool.LESION)
	for c in [Vector2.ZERO, Vector2(28, 14)]:  # the second one is left in place
		_touch(0, c, true)
		_touch(0, c, false)
		await get_tree().process_frame
	_set_tool(Tool.EXCISE)
	_touch(0, Vector2(-15, 0), true)
	for k in range(1, 31):
		_drag(0, Vector2(-15 + k, 0))
	_touch(0, Vector2(15, 0), false)
	await get_tree().process_frame
	# Let the defect open (prestress) before suturing it.
	for _frame in 30:
		await get_tree().process_frame
	_set_tool(Tool.SUTURE)
	panel.set_tightness(0.0)
	for x in [-8.0, -4.0, 0.0, 4.0, 8.0]:
		_select_suture(-1)
		# Needle 3 mm outside the (opened) defect edge on either side.
		var hw := 6.0 * (1.0 - pow(absf(x) / 15.0, 2))
		_touch(0, Vector2(x, hw + 3.0), true)
		_drag(0, Vector2(x, -hw - 3.0))
		_touch(0, Vector2(x, -hw - 3.0), false)
		await get_tree().process_frame
	_set_tool(Tool.GRAB)
	sheet.heatmap = 4
	panel.refresh()


## A short, blunt ellipse (15 x 10 mm: 1.5:1, 90 degree tips) excised along
## the RSTL and closed with four sutures. The skin at its ends has nowhere to
## go: it bunches up (dog-ears). The excess skin heatmap shows how much.
func _demo_wrinkle() -> void:
	await get_tree().process_frame
	sheet.ellipse_ratio = 1.5
	sheet.tip_angle_deg = 90.0
	panel.refresh()
	_set_tool(Tool.EXCISE)
	_touch(0, Vector2(-7.5, 0), true)
	for k in range(1, 16):
		_drag(0, Vector2(-7.5 + k, 0))
	_touch(0, Vector2(7.5, 0), false)
	await get_tree().process_frame
	for _frame in 30:
		await get_tree().process_frame
	_set_tool(Tool.SUTURE)
	panel.set_tightness(0.0)
	for x in [-4.5, -1.5, 1.5, 4.5]:
		_select_suture(-1)
		var hw := 6.0 * (1.0 - pow(absf(x) / 7.5, 2))
		_touch(0, Vector2(x, hw + 3.0), true)
		_drag(0, Vector2(x, -hw - 3.0))
		_touch(0, Vector2(x, -hw - 3.0), false)
		await get_tree().process_frame
	_set_tool(Tool.GRAB)
	sheet.heatmap = 5
	panel.refresh()
