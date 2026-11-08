extends Node3D
## Main scene logic for M1: frames the camera on the sheet, shows a HUD,
## toggles the wireframe, and can save a screenshot for automated checks.
##
## Screenshot mode (used for verification from the command line):
##   Godot_console.exe --path game -- --screenshot C:/path/out.png
## Everything after the lone "--" is passed to the game rather than to Godot.

@onready var sheet: SkinSheet = $SkinSheet
@onready var camera: Camera3D = $Camera3D
@onready var info: Label = $HUD/Info

## Empty space around the sheet, as a fraction of its size.
const FRAME_MARGIN := 1.3


func _ready() -> void:
	# Children are ready before their parent, so the sheet's mesh already exists.
	sheet.mesh_rebuilt.connect(_on_mesh_rebuilt)
	get_viewport().size_changed.connect(_frame_sheet)
	_on_mesh_rebuilt()

	var args := OS.get_cmdline_user_args()
	var i := args.find("--screenshot")
	if i != -1 and i + 1 < args.size():
		_save_screenshot_and_quit(args[i + 1])


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_W:
		sheet.show_wireframe = not sheet.show_wireframe


func _on_mesh_rebuilt() -> void:
	_frame_sheet()
	info.text = "%s\nSheet %.0f x %.0f mm, elements <= %.1f mm\n%d vertices, %d triangles\n[W] toggle wireframe" % [
		sheet.get_solver_info(), sheet.width, sheet.height, sheet.element_size,
		sheet.get_vertex_count(), sheet.get_triangle_count()]


## An orthographic camera's `size` is the visible *height* in world units (mm here).
## Pick the height that fits both the sheet's height and its width, given the
## window's aspect ratio, plus a margin.
func _frame_sheet() -> void:
	var vp := get_viewport().get_visible_rect().size
	var aspect := vp.x / vp.y
	camera.size = maxf(sheet.height, sheet.width / aspect) * FRAME_MARGIN


func _save_screenshot_and_quit(path: String) -> void:
	# Wait until two frames have actually been drawn so the image isn't blank.
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var err := get_viewport().get_texture().get_image().save_png(path)
	print("Screenshot saved to %s (error code %d)" % [path, err])
	get_tree().quit()
