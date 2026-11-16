class_name FlapCatalog
extends RefCounted
## The local flaps the Flap tool can design, with a short teaching card for
## each: how the skin moves, where it is used, how it is drawn, what to
## watch for. The geometry itself lives in the solver (solver/src/flaps.cpp);
## `type` is the solver's FlapType number.
##
## Grouped the way surgeons classify local flaps, by movement: advancement
## (slides forward), rotation (swings around a pivot), transposition (lifted
## over a bridge of intact skin). Interpolation flaps, whose pedicle crosses
## intact skin, need a third dimension; they are listed in OTHERS only.
##
## `shape` lists the SkinSheet properties that change the design, shown in
## the panel only for this flap. `ratio` is the length : width the flap
## starts with when picked (the same slider means different things per flap).

const FAMILIES := ["Advancement", "Rotation", "Transposition"]

const FLAPS := [
	{"type": 0, "family": 0, "name": "Advancement (U-flap)",
		"short": "Slides straight forward into a squared-off defect.",
		"moves": "The skin beside the defect is freed by two parallel incisions and slides straight forward. Nothing rotates.",
		"uses": "Forehead, eyebrow, helical rim, upper lip: where the incisions can lie in parallel lines (forehead creases, the brow, the vermilion).",
		"design": "Square off the defect. Run two parallel incisions back from its corners, ideally along the RSTL. Length : width up to about 3 : 1 on the face.",
		"watch": "The outer lips of the incisions are longer than the flap's sides: dog-ears form at the base. Burow's triangles remove them. A long narrow flap risks its tip.",
		"shape": ["flap_length_ratio", "flap_burow"], "ratio": 3.0},
	{"type": 3, "family": 0, "name": "H-plasty (bilateral advancement)",
		"short": "Two advancement flaps from opposite sides meet in the middle.",
		"moves": "Two advancement flaps, one from each side, each slides half the width of the defect.",
		"uses": "Forehead and brow (the incisions lie in horizontal creases), scalp: where one flap would have to be too long.",
		"design": "Square defect; extend two opposite edges both ways. Each flap about 1 to 1.5 : 1.",
		"watch": "Four limb ends, four possible dog-ears (Burow's triangles). More scar than a single flap, in exchange for less tension.",
		"shape": ["flap_length_ratio", "flap_burow"], "ratio": 1.5},
	{"type": 4, "family": 0, "name": "A-T plasty (O-T)",
		"short": "A triangle closed by sliding its sides together: a T-shaped scar.",
		"moves": "The skin on both sides of a triangular defect slides towards the middle along a straight base line, and the triangle's sides meet as the T's stem.",
		"uses": "Defects beside a straight anatomical line: hairline, eyebrow, vermilion border, alar crease. The T's bar hides in that line.",
		"design": "Drag towards the line the bar should lie in. The defect becomes a triangle with its base on that line; the base is extended both ways, each about 1.5 x the defect's diameter.",
		"watch": "The highest tension is at the T junction. Base limbs that are too short pull the corners.",
		"shape": []},
	{"type": 5, "family": 0, "name": "V-Y island advancement",
		"short": "A triangle on its subcutaneous pedicle slides in; the V closes as a Y.",
		"moves": "A triangle of skin is cut free all round but stays attached underneath, on its subcutaneous pedicle (fat with perforating vessels), and slides into the defect. The V left behind closes as a Y.",
		"uses": "Upper lip, nasal sidewall, cheek, medial canthus, fingertip (Atasoy, Kutler).",
		"design": "A triangle as wide as the defect and about twice as long, its apex along the RSTL. The legs leave the defect tangentially.",
		"watch": "Never undermine under the island: its blood supply comes from below. Here too it keeps its attachment (only its edges are freed), so it moves less than a flap.",
		"shape": ["flap_length_ratio"], "ratio": 2.0},
	{"type": 6, "family": 0, "name": "Keystone (Behan)",
		"short": "A curved island beside a fusiform defect, with a V-Y at each end.",
		"moves": "A keystone-shaped island on its perforators advances across the defect; each of its ends closes as a V-Y, recruiting skin from the sides.",
		"uses": "Trunk and limbs (back, lower leg) after skin cancer excision: larger defects where a graft would be the alternative.",
		"design": "A 3 : 1 fusiform defect along the RSTL. On its laxer side, an island as wide as the defect, its ends at 90 degrees to the defect's edge.",
		"watch": "Do not undermine the island (perforators). The tension is shared by the two V-Y ends.",
		"shape": []},
	{"type": 1, "family": 1, "name": "Rotation",
		"short": "A semicircular flap swings around a pivot into a triangle.",
		"moves": "A curved flap rotates about a pivot point at the end of its arc. The arc is centred on the pivot, so its edge slides along itself.",
		"uses": "Scalp, cheek (Mustarde), dorsum of the nose (Rieger), sacrum and buttock.",
		"design": "Make the defect a triangle about twice as tall as its base. The arc starts at a base corner, centred on the apex; arc length about 4 x the defect's width.",
		"watch": "The tension is greatest along the line from the pivot to the far corner. A back-cut shortens it, at the cost of the pedicle. A dog-ear forms at the pivot.",
		"shape": ["flap_arc_deg", "flap_mirror"]},
	{"type": 7, "family": 1, "name": "O-Z (double rotation)",
		"short": "Two rotation flaps from opposite sides each fill half of a round defect.",
		"moves": "Two rotation flaps, turning the same way, each fill half the defect; they meet along a diameter. The scar is an S (a Z).",
		"uses": "Scalp vertex and other areas of poor laxity; larger round defects of the trunk.",
		"design": "The arcs leave the defect at opposite ends of a diameter, curving in opposite directions.",
		"watch": "Each flap's leading edge must shorten from a half circle to a straight line: expect puckering near the poles. Long incisions for the defect size.",
		"shape": ["flap_arc_deg", "flap_mirror"]},
	{"type": 2, "family": 2, "name": "Rhomboid (Limberg)",
		"short": "A rhombus of skin jumps across and turns 60 degrees into the defect.",
		"moves": "A copy of the defect is lifted from beside it, over a bridge of intact skin, and turned 60 degrees into place. The donor site closes in a line.",
		"uses": "Cheek, temple, nose, lower eyelid; on the trunk, the pilonidal sinus.",
		"design": "Make the defect a 60 / 120 degree rhombus. Extend its short diagonal by one side length, then draw a limb parallel to the adjacent side. Of the four possible flaps, pick the one whose donor closure runs across the RSTL (along the line of maximal extensibility).",
		"watch": "The tightest point is the donor-site closure (the far corner of the flap to the far corner of the defect). Check it does not pull on a free margin (eyelid, lip).",
		"shape": ["flap_mirror"]},
	{"type": 8, "family": 2, "name": "Dufourmentel",
		"short": "A rhomboid variant: any rhombus angle, broader base, smaller turn.",
		"moves": "Like the Limberg, but the flap is wider at its base and turns less, so the tension spreads more evenly.",
		"uses": "The same sites as the Limberg, when the defect is not a 60 degree rhombus.",
		"design": "Extend the short diagonal and one side of the rhombus; bisect that angle for the first limb (one side long). The second limb runs parallel to the long diagonal.",
		"watch": "Compare its key suture loads with the Limberg's on the same defect.",
		"shape": ["flap_rhombus_deg", "flap_mirror"]},
	{"type": 9, "family": 2, "name": "Bilobed (Zitelli)",
		"short": "Two lobes: the first fills the defect, the second fills the first's place.",
		"moves": "Two transposition lobes on one base, 45 degrees apart: the first fills the defect, the second fills the first's donor site, and the second's donor site closes in a line. Tension is moved to where there is laxity.",
		"uses": "Nasal tip and ala, defects up to about 1.5 cm, where the skin is thick and does not stretch.",
		"design": "Pivot point one defect radius from the defect's edge. Lobes on arcs 45 degrees apart (about 90 to 100 degrees in all): the first as wide as the defect, the second narrower with a pointed tip. A Burow's triangle between the defect and the pivot.",
		"watch": "Pincushioning (trapdoor) and alar retraction. Larger rotations (Esser's original 180 degrees) make dog-ears.",
		"shape": ["flap_mirror"]},
	{"type": 10, "family": 2, "name": "Z-plasty",
		"short": "Two triangles swap places: the central limb lengthens and turns ~90 degrees. (Planned with the marker.)",
		"moves": "No defect: two triangular flaps on either side of a central limb are transposed. The central limb gets longer and turns about 90 degrees.",
		"uses": "Releasing scar contractures and webs, turning a scar into the RSTL, breaking up a straight-line scar.",
		"design": "Here the drag is the central limb (the contracture): drag along it. The two lateral limbs are as long, parallel, at the chosen angle. 30 / 45 / 60 / 75 / 90 degrees gain about 25 / 50 / 75 / 100 / 125% in length.",
		"watch": "Bigger angles gain more length but need more skin from the sides (more tension). The flap tips are the weak point: narrow ones lose their blood supply. Here the Z is drawn with the marker, not cut: the two flaps are lifted over each other, which a flat sheet of skin cannot do.",
		"shape": ["flap_z_angle_deg", "flap_mirror"]},
]

## Flaps not simulated here: their pedicle passes over intact skin
## (interpolation) or they are large versions of the designs above.
const OTHERS := [
	["Paramedian forehead", "Interpolation flap on the supratrochlear artery, for the nose. Its pedicle is divided after about 3 weeks."],
	["Nasolabial (melolabial)", "From the cheek, transposed or interpolated, for the ala and the upper lip."],
	["Abbe (cross-lip)", "Lip switch on the labial artery from the other lip, divided at 2 to 3 weeks."],
	["Note and banner flaps", "Small transposition flaps (a triangle, a finger of skin): relatives of the rhomboid."],
	["Webster 30 degree", "A rhomboid variant with a narrower flap and an M-plasty at the defect."],
	["Hatchet", "A rotation-advancement flap with a back-cut."],
	["Mustarde, Rieger (dorsal nasal)", "Large rotation flaps of the cheek and nose: try the Rotation design."],
]


static func by_type(type: int) -> Dictionary:
	for f in FLAPS:
		if f.type == type:
			return f
	return FLAPS[0]


## Index in FLAPS (the order of the picker) of a solver type.
static func index_of(type: int) -> int:
	for i in FLAPS.size():
		if FLAPS[i].type == type:
			return i
	return 0
