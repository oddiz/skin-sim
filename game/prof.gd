class_name Prof
extends RefCounted
## A tiny section timer for --bench: Prof.begin("name") ... Prof.end("name")
## adds the microseconds spent; main.gd prints the totals. Costs next to
## nothing when nobody reads it.

static var totals := {}
static var _start := {}


static func begin(name: String) -> void:
	_start[name] = Time.get_ticks_usec()


static func end(name: String) -> void:
	totals[name] = totals.get(name, 0) + Time.get_ticks_usec() - _start.get(name, Time.get_ticks_usec())
