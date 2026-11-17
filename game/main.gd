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
##           Tap a suture to select it; the tightness slider then re-ties it,
##           - / + (or the buttons floating next to it) loosen / tighten it,
##           Del cuts it out (Z ties it again). T cycles the suture labels.
##           A thread pulled beyond its knot-pull strength snaps.
##   UNDERMINE paint the areas where the skin is freed from the underlying
##           tissue (brush size in the panel); they are shaded darker.
##   LESION  tap to mark a lesion (diameter in the panel).
##   EXCISE  drag from one tip of a fusiform (elliptical) excision to the
##           other: it is cut and removed on release (shape in the panel).
##           Or tap inside any outline already cut free (e.g. a rhomboid
##           drawn with the scalpel) to remove it.
##   FLAP    drag from a lesion outward: designs the excision around it and
##           a local flap (one of eleven designs, ui/flap_catalog.gd) in the
##           drag's direction, previewed while dragging; on release it is
##           cut, the defect removed and the area undermined ("Plan with the
##           marker": drawn with the pen instead). "Tie key sutures" then
##           pulls the flap into place.
##   MARK    draw on the skin with a surgical marker (planning only). The
##           lines are kept in rest (tissue) coordinates, so they stretch and
##           move with the skin. Tap a line to erase it.
## On desktop, Shift + drag cuts whatever the tool.
##
## Navigation: two fingers pan and pinch-zoom the view. A second finger that
## lands soon after the first (or before the first has moved) turns both
## into navigation and cancels what the first had started; once a finger
## is busy with a tool, more fingers are ignored. Mouse: the wheel zooms
## about the pointer, right or middle drag pans, Home (or the Fit button)
## fits the skin in the window.
##
## The user interface around the skin (tool rail, panels, hints) is built by
## ui/app_shell.gd; this script handles the skin: input, tools, undo, demos.
##
## Command-line options (everything after a lone "--" goes to the game):
##   --screenshot <path.png>   save a screenshot after a moment, then quit
##   --ui-scale <x>            size of the interface (default: the screen's
##                             pixel density, e.g. 3 on a phone)
##   --open <panel>            open tissue | view | help | sheet | sheet-full
##   --tool <n>                pick a tool (0 grab .. 7 mark)
##   --look <n>                visual style (0 classic, 1 illustration)
##   --plain                   demos leave the colour map off
##   --zoom <x>                screenshots: zoom in x times on the middle
##   --demo-pull               grab the centre and pull it 15 mm right (for checks)
##   --demo-cut                cut a 30 mm vertical incision, then pull its
##                             right lip 8 mm right so it gapes (for checks)
##   --demo-flap [--flap-type N]  mark a lesion, raise a flap to its right
##                             (N: skinsim FlapType, 0..10; default 2,
##                             rhomboid), tie its key sutures; tension map
##   --demo-snap               close a cut with three 6-0 sutures and pull hard:
##                             they snap, worst first (for checks)
##   --demo-suture             cut, close with three sutures (3 mm bites, the
##                             middle one tied 30% tight), then pull the right
##                             side; heatmap on (for checks)
##   --demo-m6                 prestressed skin: an incision across the RSTL
##                             gapes by itself; one side undermined; max
##                             principal stress heatmap (for checks)
##   --demo-excise             a 6 mm lesion, fusiform excision along the
##                             RSTL with a 2 mm margin, closed with five
##                             sutures; min principal stress heatmap (for checks)
##   --demo-nav                two fingers: pinch out (zoom in) and pan, with
##                             the Cut tool on; prints what happened (for checks)
##   --demo-wrinkle            a short, blunt ellipse (1.5:1, 90 degree tips)
##                             excised and closed: dog-ears at both ends;
##                             excess skin heatmap (for checks)

enum Tool { GRAB, CUT, SUTURE, UNDERMINE, LESION, EXCISE, FLAP, MARK }

@onready var sheet: SkinSheet = $SkinSheet
@onready var camera: Camera3D = $Camera3D
@onready var ui: AppShell = $HUD/AppShell
@onready var overlay: GrabOverlay = $HUD/GrabOverlay
@onready var sun: DirectionalLight3D = $Sun
@onready var world_env: WorldEnvironment = $WorldEnvironment
## The visual style (game/looks/look.gd).
var look: Look
## The surgical drape around the skin (shown by looks that have one).
var drape: MeshInstance3D
## Bleeding: how much blood has come out so far (0..1), across all cuts.
var _blood := 0.0
## Game-style effects (game/fx/feedback.gd), drawn over the 3D view.
var fx: Feedback
var _measuring := false
var _bench_gpu := 0.0
var _bench_cpu := 0.0
## The current tool's settings (part of ui; kept as a short name).
var panel: ContextPanel

## Empty space around the sheet, as a fraction of its size.
const FRAME_MARGIN := 1.3
## A new point is added to the scalpel path once the finger has moved this
## many pixels: enough to follow curves, few enough to keep the path short.
const CUT_SAMPLE_PX := 4.0
## A suture gesture shorter than this is a tap: it selects a suture instead.
const TAP_PX := 10.0
## A tap within this many pixels of a suture's thread selects it.
const PICK_PX := 14.0
## A second finger within this time (s) of the first, or before the first
## has moved this far (px), makes a two-finger navigation gesture.
const NAV_WINDOW_S := 0.25
const NAV_STILL_PX := 10.0
## Zoom limits, relative to the size that fits the skin in the window.
const ZOOM_IN_MAX := 10.0
const ZOOM_OUT_MAX := 2.0

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
var _flap_touch := -1                     # touch index designing a flap, -1 = none
var _flap_start := Vector2.ZERO
var _history: Array[Dictionary] = []      # undo stack: {"kind": "cut"}, {"kind": "suture", "id": n}, ...
## Last action's result: setting it shows a short message (a "toast").
var _status := "":
	set(v):
		_status = v
		if ui != null and ui.context != null:
			ui.toast(v, "danger" if v.contains("snapped") else ("warn" if v.begins_with("No") \
				or v.begins_with("Nothing") else "info"))
## Pen markings: {"rest": PackedVector2Array (tissue coordinates), "color": int}.
var marks: Array[Dictionary] = []
var _mark_touch := -1                     # touch index drawing with the pen, -1 = none
var _mark_rest := PackedVector2Array()    # the stroke so far, rest coordinates
var _mark_screen := PackedVector2Array()  # and on screen, while drawing
var _mark_start := Vector2.ZERO
var _pen_color := 0
var _suture_buttons: HBoxContainer        # floating Loosen / Tighten / Cut next to the selected suture
var _lesion_touch := -1                   # touch index about to mark a lesion (on release)
## Touches on the skin: index -> position. The first one drives the tool;
## see _unhandled_input for how a second one becomes navigation.
var _touches := {}
var _first_touch := -1
var _first_time := 0.0                    # s
var _first_start := Vector2.ZERO
var _first_moved := false
var _ignored := {}                        # touch indices that do nothing
var _navigating := false
var _nav_centre := Vector2.ZERO           # two-finger midpoint, last frame
var _nav_span := 0.0                      # distance between them, last frame
var _pan_button := false                  # right / middle mouse drag
var _fit_size := 100.0                    # camera.size that fits the skin


func _ready() -> void:
	_apply_ui_scale()
	get_window().size_changed.connect(_apply_ui_scale)
	# Children are ready before their parent, so the sheet already exists.
	ui.setup(sheet)
	panel = ui.context
	ui.reset_requested.connect(sheet.reset_simulation)
	ui.tool_selected.connect(_set_tool)
	ui.undo_requested.connect(_undo)
	ui.fit_requested.connect(_frame_sheet)
	ui.look_changed.connect(apply_look)
	ui.skin_type_changed.connect(func(i: int): look.set_skin_type(i))
	var st := OS.get_cmdline_user_args().find("--skin-type")  # 1..6 (Fitzpatrick I-VI)
	if st != -1 and st + 1 < OS.get_cmdline_user_args().size():
		ui.show_skin_type(clampi(int(OS.get_cmdline_user_args()[st + 1]) - 1, 0, 5))
	ui.cleared.connect(_on_cleared)
	panel.pen_color_changed.connect(func(i: int): _pen_color = i)
	panel.tightness_changed.connect(func(t: float):
		if _selected_suture >= 0:
			sheet.set_suture_tightness(_selected_suture, t))
	panel.remove_suture_requested.connect(_cut_selected_suture)
	panel.adjust_tightness_requested.connect(_adjust_selected_tightness)
	panel.thread_changed.connect(func(size: int):
		sheet.suture_thread = size
		if _selected_suture >= 0:
			sheet.set_suture_thread_of(_selected_suture, size))
	panel.label_mode_changed.connect(func(mode: int): overlay.label_mode = mode)
	sheet.suture_snapped.connect(_on_suture_snapped)
	panel.tie_key_sutures_requested.connect(_tie_key_sutures)
	_build_suture_buttons()
	overlay.sheet = sheet
	overlay.camera = camera
	overlay.ui = ui
	overlay.main = self
	sheet.mesh_rebuilt.connect(_frame_sheet)
	sheet.mesh_rebuilt.connect(func(): _history.clear())  # new geometry: nothing to undo
	get_viewport().size_changed.connect(_frame_sheet)
	_frame_sheet()

	fx = Feedback.new()
	fx.camera = camera
	overlay.add_sibling(fx)  # above the overlay, under the UI
	ui.effects_changed.connect(func(on: bool): fx.enabled = on)

	drape = MeshInstance3D.new()
	drape.position.y = -1.5  # below the wound bed: never seen through a wound
	add_child(drape)
	_build_drape()
	sheet.mesh_rebuilt.connect(_build_drape)  # the sheet's size may have changed

	var args := OS.get_cmdline_user_args()
	if args.has("--bench"):
		# Run flat out: with vsync the GPU idles between frames, clocks down,
		# and its timings swing widely.
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	var lk := args.find("--look")
	apply_look(int(args[lk + 1]) if lk != -1 and lk + 1 < args.size() else 1)
	if args.has("--demo-pull"):
		_demo_pull()
	if args.has("--demo-cut"):
		_demo_cut()
	if args.has("--demo-suture"):
		_demo_suture()
	if args.has("--demo-mark"):
		_demo_mark()
	if args.has("--demo-snap"):
		_demo_snap()
	if args.has("--demo-flap"):
		var t := args.find("--flap-type")
		_demo_flap(int(args[t + 1]) if t != -1 and t + 1 < args.size() else 2)
	if args.has("--demo-m6"):
		_demo_m6()
	if args.has("--demo-excise"):
		_demo_excise()
	if args.has("--demo-wrinkle"):
		_demo_wrinkle()
	if args.has("--demo-nav"):
		_demo_nav()
	var o := args.find("--open")  # for screenshots: --open tissue | view | help
	if o != -1 and o + 1 < args.size():
		ui.open_panel(args[o + 1])
	var tl := args.find("--tool")  # for screenshots: --tool 2 (suture)...
	if tl != -1 and tl + 1 < args.size():
		_set_tool(int(args[tl + 1]))
	var i := args.find("--screenshot")
	if i != -1 and i + 1 < args.size():
		_save_screenshot_and_quit(args[i + 1])


## Demos turn a colour map on at the end; --plain keeps the plain skin
## (for screenshots of the looks).
func _demo_map(i: int) -> void:
	sheet.heatmap = 0 if OS.get_cmdline_user_args().has("--plain") else i


## The plane around the skin (drape or background): a large frame with a
## hole under the skin, so the GPU does not shade it where the skin covers it
## (it would be drawn there and then hidden: wasted work on every pixel).
## The hole is a little smaller than the skin, so a skin edge pulled inwards
## never opens a gap.
func _build_drape() -> void:
	var outer := 2000.0
	var hx := maxf(1.0, sheet.width * 0.5 - 4.0)
	var hz := maxf(1.0, sheet.height * 0.5 - 4.0)
	var o := [Vector3(-outer, 0, -outer), Vector3(outer, 0, -outer), Vector3(outer, 0, outer), Vector3(-outer, 0, outer)]
	var h := [Vector3(-hx, 0, -hz), Vector3(hx, 0, -hz), Vector3(hx, 0, hz), Vector3(-hx, 0, hz)]
	var verts := PackedVector3Array()
	for k in 4:  # one quad per side, between the outer and inner corners
		var n := (k + 1) % 4
		verts.append_array(PackedVector3Array([o[k], o[n], h[n], o[k], h[n], h[k]]))
	var normals := PackedVector3Array()
	normals.resize(verts.size())
	normals.fill(Vector3.UP)
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = normals
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	drape.mesh = mesh


## Switch the visual style (0 Classic, 1 Illustration, ...).
func apply_look(i: int) -> void:
	look = Look.make(i)
	look.apply(sheet, sun, world_env.environment, overlay, drape)
	look.set_skin_type(ui.skin_type)
	# For performance tests, override the look's anti-aliasing:
	# --msaa 0 / 1 (2x) / 2 (4x), --fxaa.
	var args := OS.get_cmdline_user_args()
	var ms := args.find("--msaa")
	if ms != -1 and ms + 1 < args.size():
		get_viewport().msaa_3d = int(args[ms + 1]) as Viewport.MSAA
	if args.has("--fxaa"):
		get_viewport().screen_space_aa = Viewport.SCREEN_SPACE_AA_FXAA
	ui.show_effects(look.effects_default)
	fx.enabled = look.effects_default
	ui.show_look(look.index)


func _set_tool(t: int) -> void:
	tool = t as Tool
	ui.show_tool(tool)


## Something was cleared (menu or panel): drop its steps from the undo history.
func _on_cleared(what: String) -> void:
	var kinds: Array = {
		"cuts": ["cut", "excise", "flap"],  # clearing the incisions also puts excised skin back
		"sutures": ["suture", "suture_cut"],
		"undermining": ["undermine"],
		"lesions": ["lesion"],
		"markings": ["mark", "mark_erase", "mark_group"],
	}.get(what, [])
	_history = _history.filter(func(h): return not h.kind in kinds)
	if what == "sutures":
		_select_suture(-1)
	if what == "markings":
		marks.clear()


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventScreenTouch:
		# Emulated touches are generated even over the UI panel; a press there
		# is meant for a slider, not for the skin underneath.
		if event.pressed and (ui.is_over_ui(event.position) \
				or (_suture_buttons.visible and _suture_buttons.get_global_rect().has_point(event.position))):
			return
		if event.pressed:
			_touch_down(event.index, event.position)
		else:
			_touch_up(event.index, event.position)
	elif event is InputEventScreenDrag:
		if not _touches.has(event.index):
			return
		_touches[event.index] = event.position
		if _navigating:
			_navigate()
			return
		if _ignored.has(event.index):
			return
		if event.index == _first_touch and event.position.distance_to(_first_start) > NAV_STILL_PX:
			_first_moved = true
		if event.index == _cut_touch:
			if event.position.distance_to(_cut_path[_cut_path.size() - 1]) >= CUT_SAMPLE_PX:
				_cut_path.append(event.position)
			fx.cut_spark(_screen_to_sheet_plane(event.position))
			overlay.cut_path = _cut_path
		elif event.index == _undermine_touch:
			_paint_undermining(event.position)
		elif event.index == _excise_touch:
			_preview_excision(event.position)
		elif event.index == _flap_touch:
			_preview_flap(event.position)
		elif event.index == _mark_touch:
			_extend_mark(event.position)
		elif event.index == _suture_touch:
			overlay.suture_line = PackedVector2Array([_suture_start, event.position])
			overlay.suture_line_ok = _on_skin(_suture_start) \
				and _on_skin(event.position)
		elif event.index == _lesion_touch:
			pass
		else:
			sheet.move_grab(event.index, _screen_to_sheet_plane(event.position))
	elif event is InputEventMouseButton and event.pressed and not ui.is_over_ui(event.position) \
			and event.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN]:
		var steps: float = event.factor if event.factor > 0.0 else 1.0
		_zoom_about(event.position, pow(0.88, steps) if event.button_index == MOUSE_BUTTON_WHEEL_UP \
			else pow(1.0 / 0.88, steps))
	elif event is InputEventMouseButton and event.button_index in [MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE]:
		_pan_button = event.pressed and not ui.is_over_ui(event.position)
	elif event is InputEventMagnifyGesture:  # trackpad pinch (macOS)
		_zoom_about(event.position, 1.0 / maxf(event.factor, 0.01))
	elif event is InputEventPanGesture:  # trackpad two-finger scroll
		_pan_by(-event.delta * 4.0)
	elif event is InputEventMouseMotion:
		if _pan_button:
			_pan_by(event.relative)
		# Preview the brush under the mouse (desktop only; touch has no hover),
		# and read the colour map there.
		overlay.brush_pos = event.position if tool == Tool.UNDERMINE else Vector2(-1, -1)
		overlay.probe_pos = event.position
		overlay.brush_radius_px = sheet.undermine_radius * get_viewport().get_visible_rect().size.y / camera.size
	elif event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_W:
				sheet.show_wireframe = not sheet.show_wireframe
				ui.refresh()
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
			KEY_F:
				_set_tool(Tool.FLAP)
			KEY_H:
				ui.cycle_heatmap()
			KEY_M:
				_set_tool(Tool.MARK)
			KEY_ESCAPE:
				if not ui.close_overlays():
					_select_suture(-1)
			KEY_SLASH, KEY_QUESTION:
				ui.toggle_help()
			KEY_HOME:
				_frame_sheet()
			KEY_Z:
				_undo()
			KEY_MINUS, KEY_KP_SUBTRACT:
				_adjust_selected_tightness(-panel.TIGHTNESS_STEP)
			KEY_EQUAL, KEY_PLUS, KEY_KP_ADD:
				_adjust_selected_tightness(panel.TIGHTNESS_STEP)
			KEY_DELETE, KEY_BACKSPACE, KEY_X:
				_cut_selected_suture()
			KEY_T:
				overlay.label_mode = (overlay.label_mode + 1) % 3
				panel.show_label_mode(overlay.label_mode)


## A finger (or the mouse) comes down on the skin. The first one uses the
## tool. A second one, soon after or while the first is still, turns both
## into a two-finger pan / pinch (the first one's action is cancelled);
## otherwise it is ignored, as are any further fingers.
func _touch_down(index: int, pos: Vector2) -> void:
	_touches[index] = pos
	if _navigating:
		_ignored[index] = true
		return
	var now := Time.get_ticks_msec() / 1000.0
	if _touches.size() == 1:
		_first_touch = index
		_first_time = now
		_first_start = pos
		_first_moved = false
		_on_press(index, pos)
	elif _touches.size() == 2 and not _ignored.has(_first_touch) \
			and (now - _first_time < NAV_WINDOW_S or not _first_moved):
		_cancel_action(_first_touch)
		_navigating = true
		_nav_centre = _two_finger_centre()
		_nav_span = _two_finger_span()
	else:
		_ignored[index] = true


func _touch_up(index: int, pos: Vector2) -> void:
	if not _touches.has(index):
		return
	_touches.erase(index)
	if _navigating:
		if _touches.is_empty():
			_navigating = false
			_ignored.clear()
		else:
			_ignored[_touches.keys()[0]] = true  # the finger left behind does nothing
		return
	if _ignored.has(index):
		_ignored.erase(index)
		return
	_on_release(index, pos)
	if _touches.is_empty():
		_ignored.clear()


## Stop what a finger started, without committing it (it became part of a
## two-finger gesture).
func _cancel_action(index: int) -> void:
	if index == _cut_touch:
		_cut_touch = -1
		_cut_path = PackedVector2Array()
		overlay.cut_path = _cut_path
	elif index == _suture_touch:
		_suture_touch = -1
		overlay.suture_line = PackedVector2Array()
	elif index == _undermine_touch:
		_undermine_touch = -1
		if not _undermine_new:
			sheet.undo_undermine()  # the few stamps already painted
		overlay.brush_pos = Vector2(-1, -1)
	elif index == _excise_touch:
		_excise_touch = -1
		overlay.excise_outline = PackedVector2Array()
	elif index == _flap_touch:
		_flap_touch = -1
		overlay.flap_preview = {}
	elif index == _mark_touch:
		_mark_touch = -1
		overlay.pen_path = PackedVector2Array()
	elif index == _lesion_touch:
		_lesion_touch = -1
	else:
		sheet.release_grab(index)


func _two_finger_centre() -> Vector2:
	var k: Array = _touches.keys()
	return (_touches[k[0]] + _touches[k[1]]) * 0.5


func _two_finger_span() -> float:
	var k: Array = _touches.keys()
	return (_touches[k[0]] as Vector2).distance_to(_touches[k[1]])


## Two fingers: the view follows their midpoint and zooms with their spread.
func _navigate() -> void:
	if _touches.size() < 2:
		return
	var centre := _two_finger_centre()
	var span := _two_finger_span()
	_pan_by(centre - _nav_centre)
	if _nav_span > 1.0 and span > 1.0:
		_zoom_about(centre, _nav_span / span)
	_nav_centre = centre
	_nav_span = span


## Move the view so the skin follows a drag of `delta` pixels.
func _pan_by(delta: Vector2) -> void:
	var a := _screen_to_sheet_plane(Vector2.ZERO)
	var b := _screen_to_sheet_plane(delta)
	camera.position.x -= b.x - a.x
	camera.position.z -= b.z - a.z


## Zoom by `factor` (< 1 = in) keeping the skin under `screen_pos` in place.
func _zoom_about(screen_pos: Vector2, factor: float) -> void:
	var before := _screen_to_sheet_plane(screen_pos)
	camera.size = clampf(camera.size * factor, _fit_size / ZOOM_IN_MAX, _fit_size * ZOOM_OUT_MAX)
	var after := _screen_to_sheet_plane(screen_pos)
	camera.position.x += before.x - after.x
	camera.position.z += before.z - after.z


## Interface size: the screen's pixel density (a phone draws 2-3 pixels per
## "point"), so text and buttons are the same physical size everywhere.
## The window's content is laid out at (window size / scale) and scaled up.
func _apply_ui_scale() -> void:
	var w := get_window()
	var scale := DisplayServer.screen_get_scale()
	if OS.get_name() == "Windows" or OS.get_name() == "Linux":
		# These report 1 here; the DPI says how dense the screen is.
		scale = DisplayServer.screen_get_dpi() / 96.0
	var args := OS.get_cmdline_user_args()
	var i := args.find("--ui-scale")
	if i != -1 and i + 1 < args.size():
		scale = float(args[i + 1])
	scale = clampf(scale, 1.0, 4.0)
	var logical := Vector2i((Vector2(w.size) / scale).round())
	if w.content_scale_mode != Window.CONTENT_SCALE_MODE_CANVAS_ITEMS or w.content_scale_size != logical:
		w.content_scale_mode = Window.CONTENT_SCALE_MODE_CANVAS_ITEMS
		w.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_EXPAND
		w.content_scale_size = logical


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
		_lesion_touch = index  # marked on release: the finger may yet start a pinch
	elif tool == Tool.EXCISE:
		if _excise_touch == -1:
			_excise_touch = index
			_excise_start = pos
	elif tool == Tool.FLAP:
		if _flap_touch == -1:
			_flap_touch = index
			_flap_start = pos
	elif tool == Tool.MARK:
		if _mark_touch == -1:
			_mark_touch = index
			_mark_start = pos
			_mark_rest = PackedVector2Array()
			_mark_screen = PackedVector2Array()
			_extend_mark(pos)
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
	elif index == _flap_touch:
		_commit_flap(pos)
	elif index == _mark_touch:
		_commit_mark(pos)
	elif index == _lesion_touch:
		_lesion_touch = -1
		if sheet.add_lesion(_screen_to_sheet_plane(pos)):
			_history.append({"kind": "lesion"})
			_status = "Lesion marked (%.1f mm)." % sheet.lesion_diameter
		else:
			_status = "A lesion must be on the skin."
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
		fx.tie_pulse((_screen_to_sheet_plane(_suture_start) + _screen_to_sheet_plane(end)) * 0.5)
		_status = "Suture tied (%+.0f%%)." % (panel.tightness * 100.0)
	else:
		_status = "No suture: the needle must go in and come out on skin."


func _select_suture(id: int) -> void:
	_selected_suture = id
	overlay.selected_suture = id
	if id >= 0:
		for su in sheet.get_suture_info():
			if su.id == id:
				# Like the tightness, the selected suture's thread becomes the
				# one for new sutures too: what the panel shows is what you get.
				panel.show_suture_tightness(su.tightness)
				sheet.suture_thread = su.thread
				panel.show_thread(su.thread)


## Re-tie the selected suture looser (delta < 0) or tighter (delta > 0).
func _adjust_selected_tightness(delta: float) -> void:
	if _selected_suture < 0:
		_status = "Select a suture first (tap it with the Suture tool)."
		return
	var t := clampf(panel.tightness + delta, -0.5, 0.6)
	panel.show_suture_tightness(t)
	sheet.set_suture_tightness(_selected_suture, t)
	_status = "Suture #%d re-tied at %+.0f%%." % [_selected_suture, t * 100.0]


## Cut the selected suture out (undo ties it again).
func _cut_selected_suture() -> void:
	if _selected_suture < 0:
		return
	if sheet.remove_suture(_selected_suture):
		_history.append({"kind": "suture_cut", "id": _selected_suture})
		_status = "Suture #%d cut." % _selected_suture
	_select_suture(-1)


func _on_suture_snapped(id: int, a: Vector3, b: Vector3, tension_n: float, thread: int) -> void:
	overlay.add_snap(a, b, "SNAPPED %d-0 at %.1f N" % [thread, tension_n])
	fx.snap(a, b)
	_status = "Suture #%d (%d-0) snapped: thread tension %.1f N, over its %.1f N strength." % [
		id, thread, tension_n, SkinSheet.thread_strength(thread)]
	if id == _selected_suture:
		_select_suture(-1)


## Small Loosen / Tighten / Cut buttons that float next to the selected
## suture, so it can be adjusted without going to the panel (handy on touch).
func _build_suture_buttons() -> void:
	_suture_buttons = HBoxContainer.new()
	_suture_buttons.add_theme_constant_override("separation", 4)
	for spec in [["−", "Loosen [-]", -1], ["+", "Tighten [+]", 1], ["", "Cut the suture [Del]", 0]]:
		var b := Button.new()
		b.text = spec[0]
		b.tooltip_text = spec[1]
		b.custom_minimum_size = Vector2(Style.TOUCH, Style.TOUCH)
		b.add_theme_font_size_override("font_size", 20)
		b.focus_mode = Control.FOCUS_NONE  # keep keyboard shortcuts working
		if spec[2] == 0:
			b.icon = Style.icon("scissors")
			b.expand_icon = true
			b.add_theme_constant_override("icon_max_width", 20)
		var step: int = spec[2]
		if step == 0:
			b.pressed.connect(_cut_selected_suture)
		else:
			b.pressed.connect(func(): _adjust_selected_tightness(step * panel.TIGHTNESS_STEP))
		_suture_buttons.add_child(b)
	_suture_buttons.visible = false
	ui.add_child(_suture_buttons)  # inside the UI: it gets the app's look


func _place_suture_buttons(su: Dictionary) -> void:
	_suture_buttons.visible = not su.is_empty()
	if su.is_empty():
		return
	var a := camera.unproject_position(su.a)
	var b := camera.unproject_position(su.b)
	# Just past the thread's left end, in line with it: neighbouring sutures
	# sit side by side along the wound, so this spot is usually clear.
	var size := _suture_buttons.get_combined_minimum_size()
	var left := a if a.x <= b.x else b
	var right := b if a.x <= b.x else a
	var dir := (left - right).normalized() if left.distance_to(right) > 1.0 else Vector2.LEFT
	var centre := left + dir * (14.0 + 0.5 * size.x)
	var anchor := centre - 0.5 * size
	var vp := get_viewport().get_visible_rect().size
	var fr := ui.free_rect()
	anchor.x = clampf(anchor.x, fr.position.x + 4.0, fr.end.x - size.x - 4.0)
	anchor.y = clampf(anchor.y, 4.0, vp.y - size.y - 4.0)
	_suture_buttons.position = anchor


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
	elif last.kind == "mark":
		marks.pop_back()
		_status = "Marking removed."
	elif last.kind == "mark_group":
		for _k in last.count:
			marks.pop_back()
		_status = "Planned flap removed."
	elif last.kind == "mark_erase":
		marks.insert(mini(last.index, marks.size()), last.mark)
		_status = "Marking restored."
	elif last.kind == "flap":
		sheet.undo_flap()
		_status = "Flap undone."
	elif last.kind == "excise":
		sheet.undo_excise()
		if last.with_cut:
			sheet.undo_cut()
		_status = "Excision undone."
	elif last.kind == "suture_cut":
		if sheet.restore_suture(last.id):
			_select_suture(last.id)
			_status = "Suture #%d tied again." % last.id
		else:
			_status = "That suture's skin is gone; nothing to tie."
	else:
		if not sheet.remove_suture(last.id):
			# It snapped since: nothing left to remove, undo the step before.
			_undo()
			return
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


func _process(delta: float) -> void:
	_update_bleeding(delta)
	var s: Dictionary = sheet.get_step_info()
	var selected := _selected_info()
	panel.show_selected_suture(selected)
	_place_suture_buttons(selected)
	panel.show_contact(sheet.get_contact_info())
	var lesions: Array = sheet.get_lesion_info()
	panel.show_lesions(lesions)
	panel.show_flaps(sheet.get_flap_info())
	overlay.show_tension_lines = ui.show_tension_lines
	overlay.show_suture_dots = ui.show_suture_dots
	ui.set_status(s.converged, s.stalled)
	ui.set_undo_enabled(not _history.is_empty())
	ui.set_hint(_hint(selected, lesions))
	var flags := ""
	if s.projected:
		flags += " projected"
	if s.regularized:
		flags += " regularized"
	ui.set_stats("%s\n%d vertices, %d triangles, %d incisions, %d sutures\n" % [
			sheet.get_solver_info(), sheet.get_vertex_count(), sheet.get_triangle_count(),
			sheet.get_cut_count(), sheet.get_suture_count()] \
		+ "%s: %d it/frame, %.2f ms, residual %s N%s\n" % [
			"at rest" if s.converged else "solving", s.iterations, s.step_ms, _sci(s.residual), flags] \
		+ "energy %.3f N·mm, area ratio %.2f–%.2f, grabs %d
" % [
			s.energy, s.min_area_ratio, s.max_area_ratio, s.grabs] 		+ _render_stats())


## Frame rate and where the time goes: CPU (scripts + solver + building the
## mesh) and GPU (drawing). The GPU time comes from timer queries, which
## some browsers do not offer (then it shows "n/a").
func _render_stats() -> String:
	var vp := get_viewport().get_viewport_rid()
	if not _measuring:
		RenderingServer.viewport_set_measure_render_time(vp, true)
		_measuring = true
	var gpu := RenderingServer.viewport_get_measured_render_time_gpu(vp)
	var cpu := Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0
	return "%d fps, frame CPU %.1f ms, GPU %s, %s px" % [
		Engine.get_frames_per_second(), cpu, "%.2f ms" % gpu if gpu > 0.0 else "n/a",
		"%dx%d" % [get_viewport().get_texture().get_width(), get_viewport().get_texture().get_height()]]


## Blood comes out of open incisions over about 15 s and stays (no
## physics: a level the looks' shaders turn into ooze and pooling). Fresh
## blood is bright; it darkens over the half minute after the last cut.
func _update_bleeding(delta: float) -> void:
	var w: Dictionary = sheet.get_wound_info()
	if w.has_cuts:
		_blood = minf(1.0, _blood + delta / 15.0)
	else:
		_blood = 0.0
	var fresh := exp(-float(w.since_cut_s) / 30.0)
	look.update_wound(_blood if ui.bleeding else 0.0, fresh)


## What to do next, in one line, for the current tool and state.
func _hint(selected: Dictionary, lesions: Array) -> String:
	match tool:
		Tool.GRAB:
			return "Drag the skin with the forceps." if sheet.get_grab_info().is_empty() \
				else "Let go to release; the skin settles back."
		Tool.CUT:
			return "Draw an incision across the skin. Lift to cut."
		Tool.SUTURE:
			if not selected.is_empty():
				return "Suture #%d selected: − / + to re-tie it, scissors to cut it. Tap elsewhere to deselect." % selected.id
			if sheet.get_cut_count() == 0 and sheet.get_flap_count() == 0:
				return "Make an incision first (Cut tool), then suture it here."
			return "Press at the needle entry, drag across the wound, lift at the exit."
		Tool.UNDERMINE:
			return "Paint over the skin to free it from the tissue below."
		Tool.LESION:
			return "Tap on the skin to mark a lesion."
		Tool.EXCISE:
			if lesions.is_empty():
				return "Drag tip to tip to design an ellipse. (Mark a lesion first to check the margins.)"
			return "Drag across the lesion from one tip of the ellipse to the other, along the tension lines."
		Tool.FLAP:
			if sheet.flap_type == 10:
				return "Z-plasty: drag along the scar (the central limb)."
			if lesions.is_empty():
				return "Mark a lesion first (Lesion tool), then drag outward from it."
			if sheet.get_flap_count() > 0:
				return "Flap raised: 'Tie key sutures' pulls it in, then close the gaps with the Suture tool."
			if sheet.flap_type == 4:
				return "A-T: drag from the lesion towards the line the T's bar should lie in."
			return "Drag from the lesion outward, towards where the flap should come from."
		Tool.MARK:
			return "Draw on the skin to plan. Tap a line to erase it."
	return ""


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
	_demo_map(2)
	ui.refresh()


## Lesion in the middle, a flap of the given type harvested to its right,
## key sutures tied with 4-0 (for checks and screenshots).
func _demo_flap(type: int) -> void:
	await get_tree().process_frame
	sheet.flap_type = type
	ui.refresh()
	_set_tool(Tool.LESION)
	_touch(0, Vector2(0, 0), true)
	_touch(0, Vector2(0, 0), false)
	await get_tree().process_frame
	_set_tool(Tool.FLAP)
	_touch(0, Vector2(0, 0), true)
	_drag(0, Vector2(12, 0))
	await get_tree().process_frame
	_touch(0, Vector2(12, 0), false)
	await get_tree().process_frame
	# Let the defect open (prestress) before pulling the flap in.
	for _frame in 30:
		await get_tree().process_frame
	_tie_key_sutures()
	_demo_map(1)
	ui.refresh()


## Draw a planning line with the pen, then pull the skin: the line stretches
## with it (for checks).
func _demo_mark() -> void:
	await get_tree().process_frame
	_set_tool(Tool.MARK)
	_touch(0, Vector2(-20, -10), true)
	for k in range(1, 41):
		_drag(0, Vector2(-20 + k, -10 + 10.0 * sin(k / 6.0)))
	_touch(0, Vector2(20, -10), false)
	await get_tree().process_frame
	_set_tool(Tool.GRAB)
	_touch(0, Vector2(0, -15), true)
	_drag(0, Vector2(0, -25))


## Close an incision with three fine 6-0 sutures, then pull one side hard:
## the most loaded suture snaps first, then possibly the next (for checks).
func _demo_snap() -> void:
	await get_tree().process_frame
	_set_tool(Tool.CUT)
	_touch(0, Vector2(0, -15), true)
	for k in range(1, 31):
		_drag(0, Vector2(0, -15 + k))
	_touch(0, Vector2(0, 15), false)
	await get_tree().process_frame
	_set_tool(Tool.SUTURE)
	sheet.suture_thread = 6
	panel.show_thread(6)
	for y in [-8.0, 0.0, 8.0]:
		_select_suture(-1)
		panel.set_tightness(0.0)
		_touch(0, Vector2(-3, y), true)
		_drag(0, Vector2(3, y))
		_touch(0, Vector2(3, y), false)
		await get_tree().process_frame
	_set_tool(Tool.GRAB)
	sheet.max_pull_force = 20.0
	_touch(0, Vector2(6, 0), true)
	_drag(0, Vector2(18, 0))


## Fire every effect around the middle of the view (for screenshots of the
## Effects; --demo-fx with --screenshot, which captures a few frames later).
func _demo_fx() -> void:
	fx.enabled = true
	var c := camera.position * Vector3(1, 0, 1)
	fx.snap(c + Vector3(-6, 0, -4), c + Vector3(6, 0, -4))
	fx.tie_pulse(c + Vector3(-12, 0, 8))
	fx.big_pulse(c + Vector3(12, 0, 8))
	for k in 12:
		fx.cut_spark(c + Vector3(-4 + k * 0.6, 0, 12), 0.05)


## An orthographic camera's `size` is the visible *height* in world units (mm).
## Fit the sheet into the part of the window not covered by the side panel,
## then slide the camera right so the sheet is centred in that free area.
func _frame_sheet() -> void:
	var vp := get_viewport().get_visible_rect().size
	var fr := ui.free_rect()
	var free_width := fr.size.x
	# Fit both ways: the sheet's height in the free height, its width in the
	# free width (camera.size is the height of the whole window, in mm).
	camera.size = maxf(sheet.height * vp.y / fr.size.y, sheet.width * vp.y / free_width) * FRAME_MARGIN
	_fit_size = camera.size
	var mm_per_px := camera.size / vp.y
	# Put the sheet's centre (world 0, 0) in the middle of the free area:
	# a world point x shows at screen x = vp.x / 2 + (x - camera.x) / mm_per_px,
	# and z likewise downwards.
	camera.position.x = (0.5 * vp.x - fr.get_center().x) * mm_per_px
	camera.position.z = (0.5 * vp.y - fr.get_center().y) * mm_per_px


func _save_screenshot_and_quit(path: String) -> void:
	var args := OS.get_cmdline_user_args()
	var z := args.find("--zoom")  # closer look at the middle of the skin
	if z != -1 and z + 1 < args.size():
		_zoom_about(get_viewport().get_visible_rect().size * 0.5, 1.0 / float(args[z + 1]))
	# Let the solver run for a moment (about 2 s at 60 fps), then capture.
	for _frame in 120:
		await RenderingServer.frame_post_draw
		if _frame >= 60 and args.has("--bench"):
			_bench_gpu += RenderingServer.viewport_get_measured_render_time_gpu(get_viewport().get_viewport_rid())
			_bench_cpu += RenderingServer.viewport_get_measured_render_time_cpu(get_viewport().get_viewport_rid())
		if _frame == 108 and args.has("--demo-fx"):
			_demo_fx()
	if args.has("--bench"):
		print("BENCH gpu %.3f ms, render cpu %.3f ms (mean of 60 frames)" % [_bench_gpu / 60.0, _bench_cpu / 60.0])
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
	_demo_map(1)
	ui.refresh()


## Show the fusiform excision that releasing here would cut.
func _preview_excision(screen_pos: Vector2) -> void:
	if screen_pos.distance_to(_excise_start) < TAP_PX:
		overlay.excise_outline = PackedVector2Array()
		return
	var pts := PackedVector2Array()
	for p in sheet.get_fusiform_outline(_screen_to_sheet_plane(_excise_start), _screen_to_sheet_plane(screen_pos)):
		pts.append(camera.unproject_position(p))
	overlay.excise_outline = pts


## While dragging with the Flap tool: preview the design (screen space).
func _preview_flap(screen_pos: Vector2) -> void:
	if screen_pos.distance_to(_flap_start) < TAP_PX:
		overlay.flap_preview = {}
		return
	var d: Dictionary = sheet.get_flap_preview(_screen_to_sheet_plane(_flap_start),
		_screen_to_sheet_plane(screen_pos))
	if not d.valid:
		overlay.flap_preview = {}
		return
	var screen := func(pts: PackedVector3Array) -> PackedVector2Array:
		var out := PackedVector2Array()
		for p in pts:
			out.append(camera.unproject_position(p))
		return out
	var all := func(list: Array) -> Array:
		var out: Array = []
		for p in list:
			out.append(screen.call(p))
		return out
	overlay.flap_preview = {
		"defect": screen.call(d.defect), "flaps": all.call(d.flaps), "bases": all.call(d.bases),
		"burow": all.call(d.burow), "incisions": all.call(d.incisions),
		"key_sutures": all.call(d.key_sutures), "text": _flap_text(d),
	}


## One line describing a flap design (preview or made).
func _flap_text(d: Dictionary) -> String:
	var kind: int = d.type
	var name: String = FlapCatalog.by_type(kind).name
	match kind:
		0, 3:
			return "%s · %.0f mm defect · %.1f : 1" % [name, d.diameter_mm, d.ratio]
		1, 7:
			return "%s · %.0f mm defect · arc %.1f x width · turns %.0f°" % [name, d.diameter_mm, d.ratio, d.turn_deg]
		10:
			return "%s · +%.0f%% length" % [name, d.lengthening * 100.0]
		2, 8, 9:
			return "%s · %.0f mm defect · turns %.0f°" % [name, d.diameter_mm, d.turn_deg]
	return "%s · %.0f mm defect" % [name, d.diameter_mm]


## Release of the flap gesture: cut it, remove the defect, undermine. Or,
## with "Plan with the marker", draw the design on the skin instead.
func _commit_flap(end: Vector2) -> void:
	_flap_touch = -1
	overlay.flap_preview = {}
	if end.distance_to(_flap_start) < TAP_PX:
		_status = "Flap: drag from the lesion outward, towards where the flap should come from."
		return
	var a := _screen_to_sheet_plane(_flap_start)
	var b := _screen_to_sheet_plane(end)
	if panel.flap_plan_only or sheet.flap_type == 10:
		# (The Z-plasty is always planned: its two flaps cross over each
		# other, which a flat sheet cannot do. See FlapCatalog.)
		_plan_flap(a, b)
		return
	var result := sheet.make_flap(a, b)
	if result == 1:
		_history.append({"kind": "flap"})
		fx.big_pulse(a)
		_status = "Flap raised and undermined. 'Tie key sutures' pulls it into place. " \
			+ _lesion_summary()
	elif result == 0:
		_status = "No flap: start on the skin, and keep the design on the sheet."
	else:
		_status = "No flap: the defect's outline did not close (too near the edge of the sheet?)."


## Draw a flap design with the marker: incisions in violet, key sutures in
## blue. One undo step.
func _plan_flap(a: Vector3, b: Vector3) -> void:
	var d: Dictionary = sheet.get_flap_design_rest(a, b)
	if d.is_empty():
		_status = "No plan: start on the skin."
		return
	var n := 0
	for line in d.cuts:
		marks.append({"rest": line, "color": 0})
		n += 1
	for k in d.key_sutures:
		marks.append({"rest": k, "color": 1})
		n += 1
	_history.append({"kind": "mark_group", "count": n})
	_status = "Flap planned on the skin: violet = cut, blue = key sutures. Cut along it, then excise the defect."


## Tie the last flap's key sutures with the panel's thread and tightness.
func _tie_key_sutures() -> void:
	if sheet.get_flap_count() == 0:
		_status = "Make a flap first (Flap tool)."
		return
	var ids: Array = sheet.tie_key_sutures(panel.tightness)
	for id in ids:
		_history.append({"kind": "suture", "id": id})
	var k := 0
	for su in sheet.get_suture_info():
		if ids.has(su.id):
			fx.tie_pulse((su.a + su.b) * 0.5, 0.12 * k)
			k += 1
	_status = "%d key sutures tied (%d-0)." % [ids.size(), sheet.suture_thread]


## Pen: add the point under the finger to the stroke (rest coordinates, so
## the line belongs to the tissue and follows it as it moves).
func _extend_mark(screen_pos: Vector2) -> void:
	if not _mark_screen.is_empty() and screen_pos.distance_to(_mark_screen[_mark_screen.size() - 1]) < CUT_SAMPLE_PX:
		return
	var X: Vector2 = sheet.to_rest(_screen_to_sheet_plane(screen_pos))
	if is_nan(X.x):
		return  # not over skin
	_mark_rest.append(X)
	_mark_screen.append(screen_pos)
	overlay.pen_path = _mark_screen
	overlay.pen_color = ContextPanel.PEN_COLORS[_pen_color]


func _commit_mark(end: Vector2) -> void:
	_mark_touch = -1
	overlay.pen_path = PackedVector2Array()
	if end.distance_to(_mark_start) < TAP_PX:
		_erase_mark_at(end)
		return
	_extend_mark(end)
	if _mark_rest.size() >= 2:
		marks.append({"rest": _mark_rest, "color": _pen_color})
		_history.append({"kind": "mark"})


## A tap with the pen: erase the marking under the finger (undoable).
func _erase_mark_at(screen_pos: Vector2) -> void:
	for i in range(marks.size() - 1, -1, -1):
		for piece in sheet.rest_to_world(marks[i].rest):
			var pts := PackedVector2Array()
			for p in piece:
				pts.append(camera.unproject_position(p))
			for k in range(pts.size() - 1):
				var q := Geometry2D.get_closest_point_to_segment(screen_pos, pts[k], pts[k + 1])
				if q.distance_to(screen_pos) < PICK_PX:
					_history.append({"kind": "mark_erase", "mark": marks[i], "index": i})
					marks.remove_at(i)
					_status = "Marking erased."
					return


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
	_demo_map(4)
	ui.refresh()


## A short, blunt ellipse (15 x 10 mm: 1.5:1, 90 degree tips) excised along
## the RSTL and closed with four sutures. The skin at its ends has nowhere to
## go: it bunches up (dog-ears). The excess skin heatmap shows how much.
func _demo_wrinkle() -> void:
	await get_tree().process_frame
	sheet.ellipse_ratio = 1.5
	sheet.tip_angle_deg = 90.0
	ui.refresh()
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
	_demo_map(5)
	ui.refresh()


## Two fingers land together with the Cut tool on, spread apart and move:
## the view zooms in and pans, and nothing is cut. Prints the result.
func _demo_nav() -> void:
	await get_tree().process_frame
	_set_tool(Tool.CUT)
	var size_before := camera.size
	var cuts_before := sheet.get_cut_count()
	_touch(0, Vector2(-10, 0), true)
	_touch(1, Vector2(10, 0), true)
	await get_tree().process_frame
	# Drag events in screen pixels: spread the fingers and move them right.
	var a0 := _sheet_to_screen(Vector2(-10, 0))
	var b0 := _sheet_to_screen(Vector2(10, 0))
	for k in range(1, 11):
		for f in [[0, a0 + Vector2(-6.0 * k + 3.0 * k, 0)], [1, b0 + Vector2(6.0 * k + 3.0 * k, 0)]]:
			var drag := InputEventScreenDrag.new()
			drag.index = f[0]
			drag.position = f[1]
			Input.parse_input_event(drag)
		await get_tree().process_frame
	_touch(0, Vector2(-10, 0), false)
	_touch(1, Vector2(10, 0), false)
	await get_tree().process_frame
	print("nav demo: camera size %.1f -> %.1f mm, camera x %.1f, cuts %d -> %d, navigating %s" % [
		size_before, camera.size, camera.position.x, cuts_before, sheet.get_cut_count(), _navigating])
