class_name Scenario
extends RefCounted
## A teaching case (game/scenarios/*.json, format in the README there):
## a body site, a lesion and a target margin, a short brief, a suggested
## approach (hidden until asked), and a score for the closure.
##
## Scoring ("Check closure"): five criteria, each green / amber / red, from
## what the simulator measures now (SkinSheet.get_closure_metrics and the
## lesion and suture read-outs):
##   lesion    out completely, with at least the target margin
##   closed    the wound's open area (lips not yet brought together)
##   tension   the tension the closure adds to the skin, averaged over the
##             sheet, against the suggested approach carried out by the
##             simulator (reference, from --probe-scenario): green up to
##             tension_factors[0] times it (default 1.1), amber up to [1] (1.5)
##   dog_ears  the largest excess skin (puckering)
##   sutures   none snapped since the start, none above 75% of its strength
## Stars: 3 all green, 2 no red, 1 otherwise; 0 while the lesion is still in.

const DIR := "res://scenarios"
const LOAD_AMBER := 0.75          # a suture above this fraction of its strength: at risk
const CLOSED_GREEN_MM2 := 1.0     # open area that still counts as closed
const DEFAULT_TIP := {
	"lesion": "The defect must contain the whole lesion with the target margin all round.",
	"closed": "Bring all the lips together with sutures.",
	"tension": "Lower the tension: orientation along the RSTL, undermining, a flap.",
	"dog_ears": "Remove the dog-ears, or lengthen the design.",
	"sutures": "Use stronger thread or reduce the tension.",
}

var data: Dictionary
var index := 0
var snapped := 0  # sutures snapped since the scenario started


## Every scenario, in file-name order: [{"file", "data"}].
static func list() -> Array:
	var out: Array = []
	var dir := DirAccess.open(DIR)
	if dir == null:
		return out
	var files: Array = []
	for f in dir.get_files():
		# (Exported builds list .json files as-is; keep only those.)
		if f.ends_with(".json"):
			files.append(f)
	files.sort()
	for f in files:
		var text := FileAccess.get_file_as_string(DIR + "/" + f)
		var d = JSON.parse_string(text)
		if d is Dictionary and d.has("title"):
			out.append({"file": f, "data": d})
		else:
			push_warning("Scenario %s: not valid" % f)
	return out


static func make(i: int) -> Scenario:
	var all := list()
	if i < 0 or i >= all.size():
		return null
	var s := Scenario.new()
	s.index = i
	s.data = all[i].data
	return s


func title() -> String:
	return data.get("title", "Scenario")


func margin_mm() -> float:
	return float(data.get("margin_mm", 4.0))


## Lesion centre at rest (sheet mm).
func lesion_at() -> Vector2:
	var at: Array = data.lesion.get("at", [0, 0])
	return Vector2(at[0], at[1])


## Fresh skin of the right size and site, the lesion, the Plan mode.
func start(main: Node) -> void:
	var sheet: SkinSheet = main.sheet
	var sh: Dictionary = data.get("sheet", {})
	sheet.element_size = float(sh.get("element_mm", 2.5))
	sheet.width = float(sh.get("width", 100.0))
	sheet.height = float(sh.get("height", 60.0))
	main.on_sheet_resized()
	main.start_over(true)
	var site: String = data.get("site", "")
	if site != "" and not main.ui.drawer.apply_preset_named(site):
		push_warning("Scenario %s: no tissue preset '%s'" % [title(), site])
	var params: Dictionary = data.get("params", {})
	for key in params:
		sheet.set(key, params[key])
	# The tools' defaults for this case.
	sheet.ellipse_ratio = 3.0
	sheet.tip_angle_deg = 30.0
	sheet.flap_margin = margin_mm()
	var l: Dictionary = data.lesion
	sheet.lesion_kind = int(l.get("kind", 0))
	sheet.lesion_diameter = float(l.get("diameter", 6.0))
	var c := lesion_at()
	sheet.add_lesion(sheet.to_global(Vector3(c.x, 0, -c.y)))
	main.ui.refresh()
	main.on_scenario_started(self)
	snapped = 0


## World point (on the skin as it is now) of a rest point.
static func _world(sheet: SkinSheet, rest: Vector2) -> Vector3:
	var p: Dictionary = sheet.rest_point_now(rest)
	return p.world if not p.is_empty() else sheet.to_global(Vector3(rest.x, 0, -rest.y))


## The suggested excision ellipse's tips (rest mm), or [] if not an ellipse.
func _ellipse_tips(sheet: SkinSheet) -> Array:
	var draw: Dictionary = data.get("approach", {}).get("draw", {})
	if draw.get("kind", "") != "ellipse":
		return []
	var angle := deg_to_rad(sheet.fibre_angle_deg) if draw.get("along", "") == "rstl" \
		else deg_to_rad(float(draw.get("angle_deg", 0.0)))
	var width := float(data.lesion.get("diameter", 6.0)) + 2.0 * margin_mm()
	var half := 0.5 * float(draw.get("ratio", 3.0)) * width
	var d := Vector2.from_angle(angle)
	return [lesion_at() - d * half, lesion_at() + d * half]


## Draw the suggested approach on the skin with the marker (one undo step).
func draw_approach(main: Node) -> void:
	var sheet: SkinSheet = main.sheet
	var draw: Dictionary = data.get("approach", {}).get("draw", {})
	match draw.get("kind", ""):
		"ellipse":
			sheet.ellipse_ratio = float(draw.get("ratio", 3.0))
			sheet.tip_angle_deg = float(draw.get("tip_deg", 30.0))
			var tips := _ellipse_tips(sheet)
			var outline: PackedVector3Array = sheet.get_fusiform_outline(_world(sheet, tips[0]), _world(sheet, tips[1]))
			var rest := PackedVector2Array()
			for p in outline:
				var X: Vector2 = sheet.to_rest(p)
				if not is_nan(X.x):
					rest.append(X)
			if rest.size() >= 2:
				main.marks.append({"kind": "pen", "rest": rest, "color": 0, "style": 1})
				main._history.append({"kind": "mark"})
		"flap":
			sheet.flap_type = int(draw.get("type", 2))
			sheet.flap_margin = margin_mm()
			main.ui.refresh()
			var c := lesion_at()
			var dir := Vector2.from_angle(deg_to_rad(float(draw.get("direction_deg", 0.0))))
			main._plan_flap(_world(sheet, c), _world(sheet, c + 12.0 * dir))


## The report card: {"stars": 0..3, "items": [{"key", "name", "grade" (0 green,
## 1 amber, 2 red), "value": text, "tip": text or ""}], "settling": bool}.
func score(main: Node) -> Dictionary:
	var sheet: SkinSheet = main.sheet
	var m: Dictionary = sheet.get_closure_metrics()
	var ref: Dictionary = data.get("reference", {})
	var tips: Dictionary = data.get("tips", {})
	var items: Array = []
	var add := func(key: String, item_name: String, grade: int, value: String) -> void:
		items.append({"key": key, "name": item_name, "grade": grade, "value": value,
			"tip": "" if grade == 0 else tips.get(key, DEFAULT_TIP[key])})

	# Lesion: the scenario's lesion is the first one.
	var lesions: Array = sheet.get_lesion_info()
	var lesion_out := false
	if lesions.is_empty():
		add.call("lesion", "Lesion removed", 2, "no lesion")
	else:
		var l: Dictionary = lesions[0]
		var target := margin_mm()
		if not l.excised:
			add.call("lesion", "Lesion removed", 2, "still in place")
		elif not l.complete:
			add.call("lesion", "Lesion removed", 2, "incomplete: the edge runs through it")
		else:
			lesion_out = true
			var g := 0 if l.margin_mm >= target - 0.25 else 1
			add.call("lesion", "Lesion removed", g, "margin %.1f mm (target %.0f mm)" % [l.margin_mm, target])

	var open: float = m.get("open_area_mm2", 0.0)
	var defect: float = m.get("defect_area_mm2", 0.0)
	if defect <= 0.0:
		add.call("closed", "Wound closed", 2, "no wound yet")
	else:
		var g := 0 if open <= maxf(CLOSED_GREEN_MM2, 0.03 * defect) else (1 if open <= 0.15 * defect else 2)
		add.call("closed", "Wound closed", g, "%.0f mm² still open (of %.0f)" % [open, defect])

	var t: float = m.get("mean_tension_kpa", 0.0)
	var t_ref: float = float(ref.get("tension_kpa", 0.0))
	var f: Array = data.get("tension_factors", [1.1, 1.5])
	if t_ref > 0.0:
		var g := 0 if t <= f[0] * t_ref else (1 if t <= f[1] * t_ref else 2)
		add.call("tension", "Tension", g, "%.1f kPa on average (suggested design: %.1f)" % [t, t_ref])
	else:
		add.call("tension", "Tension", 0, "%.1f kPa on average" % t)

	var x: float = m.get("max_excess", 0.0)
	var x_ref: float = float(ref.get("max_excess", 0.0))
	var gx := 0 if x <= x_ref + 0.05 else (1 if x <= x_ref + 0.15 else 2)
	add.call("dog_ears", "Dog-ears", gx, "%.0f%% excess skin" % (100.0 * x))

	var worst := 0.0
	var at_risk := 0
	var count := 0
	for su in sheet.get_suture_info():
		count += 1
		worst = maxf(worst, su.load)
		if su.load > LOAD_AMBER:
			at_risk += 1
	var gs := 2 if snapped > 0 else (1 if at_risk > 0 else 0)
	var sv := "%d snapped" % snapped if snapped > 0 else \
		("%d sutures, highest load %.0f%%" % [count, 100.0 * worst] if count > 0 else "none")
	add.call("sutures", "Sutures", gs, sv)

	var reds := items.filter(func(i): return i.grade == 2).size()
	var ambers := items.filter(func(i): return i.grade == 1).size()
	var stars := 0 if not lesion_out else (3 if reds == 0 and ambers == 0 else (2 if reds == 0 else 1))
	return {"stars": stars, "items": items, "settling": not sheet.get_step_info().get("converged", true)}


# --- probe (--probe-scenario n) -------------------------------------------
# The suggested approach, carried out by the simulator: for a flap, each of
# 8 harvest directions (raise, tie the key sutures, settle) and the lowest
# tension wins; for an ellipse, excise along the RSTL and close it with
# sutures every 5 mm. Prints the reference values for the scenario file.

static func _settle(main: Node, max_frames := 900) -> void:
	for _f in max_frames:
		await main.get_tree().process_frame
		if main.sheet.get_step_info().get("converged", false):
			return


func probe(main: Node) -> void:
	var sheet: SkinSheet = main.sheet
	var could_snap := sheet.sutures_can_snap
	var draw: Dictionary = data.get("approach", {}).get("draw", {})
	if draw.get("kind", "") == "flap":
		var best := {}
		for k in 8:
			start(main)
			await _settle(main)
			sheet.flap_type = int(draw.get("type", 2))
			sheet.flap_margin = margin_mm()
			var deg := 45.0 * k
			var c := lesion_at()
			var dir := Vector2.from_angle(deg_to_rad(deg))
			var made: int = sheet.make_flap(_world(sheet, c), _world(sheet, c + 12.0 * dir))
			if made != 1:
				print("probe %s: %3.0f deg: no flap (%d)" % [title(), deg, made])
				continue
			await _settle(main)
			sheet.sutures_can_snap = false  # measure tension, not failure
			sheet.tie_key_sutures(0.0)
			await _settle(main)
			var m: Dictionary = sheet.get_closure_metrics()
			print("probe %s: %3.0f deg: mean %.2f kPa, peak %.0f kPa, excess %.2f, open %.0f/%.0f mm2" % [
				title(), deg, m.mean_tension_kpa, m.peak_tension_kpa, m.max_excess, m.open_area_mm2, m.defect_area_mm2])
			if best.is_empty() or m.mean_tension_kpa < best.tension:
				best = {"deg": deg, "tension": m.mean_tension_kpa, "excess": m.max_excess}
		print("PROBE %s: best direction %.0f deg, mean tension %.2f kPa, excess %.3f" % [
			title(), best.get("deg", -1.0), best.get("tension", 0.0), best.get("excess", 0.0)])
	elif draw.get("kind", "") == "ellipse":
		start(main)
		await _settle(main)
		sheet.ellipse_ratio = float(draw.get("ratio", 3.0))
		sheet.tip_angle_deg = float(draw.get("tip_deg", 30.0))
		var tips := _ellipse_tips(sheet)
		# Undermining within undermine_mm of the ellipse (a box round it,
		# painted with stamps on a 4 mm grid), as the approach says.
		var um := float(draw.get("undermine_mm", 0.0))
		if um > 0.0:
			sheet.undermine_radius = 4.0
			var ax: Vector2 = (tips[1] - tips[0]).normalized()
			var len_half: float = 0.5 * tips[0].distance_to(tips[1]) + um
			var w_half: float = 0.5 * tips[0].distance_to(tips[1]) / float(draw.get("ratio", 3.0)) + um
			var first := true
			var s_ := -len_half
			while s_ <= len_half:
				var t_ := -w_half
				while t_ <= w_half:
					var p_: Vector2 = lesion_at() + ax * s_ + ax.orthogonal() * t_
					if sheet.undermine_at(_world(sheet, p_), first):
						first = false
					t_ += 4.0
				s_ += 4.0
		sheet.excise_fusiform(_world(sheet, tips[0]), _world(sheet, tips[1]))
		await _settle(main)
		sheet.sutures_can_snap = false
		var a: Vector2 = tips[0]
		var b: Vector2 = tips[1]
		var along := (b - a).normalized()
		var across := along.orthogonal()
		var length := a.distance_to(b)
		var half_w := 0.5 * length / float(draw.get("ratio", 3.0))
		var s := -0.5 * length + 5.0
		var n := 0
		while s < 0.5 * length - 4.0:
			# Half-width of the fusiform here (roughly: a parabola), plus a 2.5 mm bite.
			var u := s / (0.5 * length)
			var hw := half_w * (1.0 - u * u) + 2.5
			var mid := lesion_at() + along * s
			if sheet.suture(_world(sheet, mid + across * hw), _world(sheet, mid - across * hw), 0.0) >= 0:
				n += 1
			s += 5.0
		await _settle(main)
		var m: Dictionary = sheet.get_closure_metrics()
		var force := 0.0
		var top := 0.0
		for su in sheet.get_suture_info():
			force += su.tension_n
			top = maxf(top, su.tension_n)
		print("PROBE %s: ellipse closed with %d sutures: mean %.2f kPa, tension %.0f kPa, threads %.2f N total, %.2f N max, excess %.3f (max %.3f), open %.0f/%.0f mm2" % [
			title(), n, m.mean_tension_kpa, m.peak_tension_kpa, force, top, m.max_excess, m.peak_excess, m.open_area_mm2, m.defect_area_mm2])
	# How the scoring rates the simulator's own closure (a check of the card).
	var r := score(main)
	print("PROBE %s: score %d stars: %s" % [title(), r.stars,
		", ".join(r.items.map(func(i): return "%s %s (%s)" % [i.key, ["green", "amber", "red"][i.grade], i.value]))])
	sheet.sutures_can_snap = could_snap
