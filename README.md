# skin-sim

Local flap simulator for plastic surgery: a skin membrane you can cut, drag and suture.
See `CLAUDE.md` for goals, architecture and milestones.

## Layout

| Path | What |
|---|---|
| `solver/` | Standalone C++ mechanics library (no Godot). Tests in `solver/tests/`. |
| `extension/` | Thin GDExtension layer exposing solver classes to Godot. |
| `game/` | Godot 4.7 project. The built DLL lands in `game/bin/`. |
| `thirdparty/` | Pinned git submodules: godot-cpp, Eigen, CDT, doctest. |

## Prerequisites (Windows)

- Git, Python 3, SCons (`python -m pip install scons`)
- Visual Studio 2022 Build Tools with "Desktop development with C++" (MSVC)
- Godot 4.7 standard build

## Build

```
git clone --recurse-submodules <repo-url>     # or: git submodule update --init
scons                # build godot-cpp, solver, tests, extension (debug)
scons run_tests      # build + run solver unit tests
scons target=template_release   # optimised build
```

The first build compiles godot-cpp and takes a few minutes; later builds are incremental.

## Run

Open `game/project.godot` in Godot 4.7, or headless:

```
Godot_v4.7.2-stable_win64_console.exe --headless --path game --quit-after 1
```

Note: scripts declare global types with `class_name`. Godot registers these when the editor
scans the project. If you run the game from the command line on a fresh checkout (no
`game/.godot/` folder yet), run an import first:

```
Godot_v4.7.2-stable_win64_console.exe --headless --path game --import
```

## Controls

| Input | Action |
|---|---|
| Drag (Grab tool, G) | Grab and pull the skin with forceps; several fingers on a touch screen |
| Drag (Cut tool, C), or Shift + drag | Draw an incision; it is made when you release |
| Drag (Suture tool, S) | Press at the needle entry, release at the exit: a suture tied with the panel's knot tightness |
| Tap a suture (Suture tool) | Select it; the knot tightness slider then re-ties it live |
| Drag (Undermine tool, U) | Paint where the skin is freed from the tissue below (shaded darker) |
| Tap (Lesion tool, L) | Mark a lesion (diameter in the panel) |
| Drag (Excise tool, E) | Tip to tip: a fusiform excision (shape in the panel), cut and removed on release |
| Tap (Excise tool) | Remove the piece of skin under the finger, if incisions have cut it free all round |
| Z | Undo the last incision, suture, undermining stroke, lesion or excision |
| H | Cycle the heatmap: off / max principal / von Mises / area change / min principal stress / excess skin |
| W / R | Wireframe on/off / reset the deformation (incisions and sutures stay) |

Incisions are stored in rest (tissue) coordinates, so you can cut skin that is
being pulled. They stay one element away from the pinned outer border, and a
stroke starting within ~2 mm of an existing cut snaps onto it, so flap limbs join cleanly.

Knot tightness is relative to the two bites (entry and exit distances from the
incision): at 0% the thread is exactly their sum, so the edges just meet; +20%
ties it 20% shorter, squeezing the tissue in the loop; negative leaves it loose.
Each bite pulls on a strip of dermis from the needle hole to the wound edge
("Bite strip width", default 2 mm), not on a single point, so the stress around
the needle holes is finite and settles as the mesh is refined.
Each suture shows its tension in newtons (green to red up to 3 N, grey = slack)
and its loop stress: the stress along the thread in the tissue the loop holds
(+ stretched, - squeezed). The heatmap
shows the Cauchy (true) stress in the skin; the colour scale is set by the
"Stress scale" slider so that different closures can be compared on one scale.
Where a single suture closes a gaping wound, the corner between the closed point
and the open wound is a sharp notch: the stress right there keeps growing as the
mesh is refined (as it would at any sharp notch in an elastic material). Suture
tension and loop stress are mesh-independent.

### Lip contact

The lips of incisions cannot pass through each other ("Lip contact", on by
default): a knot tied tighter than "edges touching" presses them together and
squeezes the tissue in the loop (negative loop stress), forceps cannot push one
lip across the other, and a flap can slide along the edge it is inset into.
Lips pressed together are drawn blue, and the panel shows the contact pressure.

How it works: a lip node that has entered skin other than its own is pushed
back by a stiff spring (a *penalty*) proportional to how deep it is, measured as
the distance to the nearest lip. That leaves a tiny overlap (pressure / contact
stiffness, hundredths of a mm). Both lips are checked against each other, each
carrying half the pressure. Each solver step is also checked continuously, so a
fast move cannot carry a lip right through a narrow strip of skin. Contact is
frictionless.

Limitation: squeezed tissue can only compress in-plane or wrinkle (see
Wrinkling below); real skin would also bulge (evert). Over-tight knots (beyond
~20%) impose very large strains on the tissue in the loop, so their tension and
compression are still high: trust the sign and the location, not the magnitude.

## Lesion excision and defect closure (M7)

Mark a lesion, then design its excision with the Excise tool: drag from one tip
of the fusiform (elliptical) excision to the other, ideally along the RSTL.
Its shape comes from the panel: length to width (3 : 1 is classic) and the
angle at each tip (about 30 degrees). On release the outline is cut and the
skin inside removed. Any other shape works too: draw a closed outline with the
scalpel (a stroke ending near its own start closes itself), then tap inside it
with the Excise tool.

After an excision the panel reports each lesion: excised with its margin (from
the lesion's edge to the defect's edge), or INCOMPLETE if the defect's edge runs
through it (the lesion is outlined in red). Prestressed skin retracts, so the
defect opens wider than the skin removed; close it with sutures. The heatmap's
"min principal stress" shows where the closure squeezes the skin: at the ends of
an ellipse that is too short or too blunt. That is where real skin would pucker
into a dog-ear; the "excess skin" heatmap (M8) shows how much skin bunches up there.

## Wrinkling (M8)

A membrane has no bending stiffness: pushed together in its plane, real skin
does not shorten like a block of rubber, it buckles out of the plane into
wrinkles or a dog-ear. The 2D model has no "out of the plane", so a plain flat
membrane resists squeezing far too much. The wrinkling model (tension-field
theory) lets the skin hide extra length in fine wrinkles instead:

- **Taut** skin (tension in every direction) behaves exactly as before.
- **Wrinkled** skin (tension one way, compression the other) carries only the
  tension; across the wrinkles the stress drops to zero.
- **Slack** skin (shorter than its natural size in every direction) lies in
  folds and carries nothing.

The "Wrinkling" slider (Tissue section, default 90%) sets how much of its
resistance to squeezing the skin loses by buckling. The remaining 10% stands for
the bending stiffness of the dermis and the tissue under it, and keeps elements
from being squashed flat. At 0% you get the old flat membrane, for comparison.

The heatmap "Excess skin (wrinkles)" shows how much extra skin is bunched up:
how much longer the skin is than the distance it spans in its most crumpled
direction (10% = 10% more skin than room for it). This is where real skin
puckers. A 15 x 10 mm blunt ellipse (1.5 : 1, 90 degree tips) leaves ~37% excess
at its ends; a classic 30 x 10 mm (3 : 1, 30 degrees) ~14%. With wrinkling on,
the compression at the blunt ellipse's ends drops from -122 to -33 kPa, while the
closure tension hardly changes, since a closure works in tension.

How it works (for the curious): the energy of the skin is replaced by the lowest
energy it can reach by adding length in any directions (wrinkles add material
length, never remove it). Because the skin's energy is convex in the metric
C = F^T F, this small optimisation per triangle has a unique answer, found in a
few Newton iterations, and the resulting stress is never compressive. Details
in `solver/include/skinsim/wrinkling.hpp`.

## Planned features

- Local flaps (advancement, rotation, transposition) on the excision defects.
- Friction between the lips; calibration against measured skin properties.

## Tissue model (M6)

The skin is an incompressible membrane: an isotropic strain-stiffening matrix
(Demiray) plus one family of collagen fibres along the relaxed skin tension lines
(RSTL), Gasser-Ogden-Holzapfel model. Living skin is pre-stretched (by default 8%
along the RSTL, 3% across): the sheet is in equilibrium while intact, and an
incision releases that tension and gapes, more when it runs across the RSTL. The
compass at the top right shows the RSTL direction.

Undermining lowers the attachment to the tissue below (to "Tethering left").
That matters most when skin has to be *advanced* (closing a defect, insetting a
flap); re-approximating a plain incision only restores the in-vivo state, so its
closing tension is set by the skin's own prestress.

All values are illustrative, not calibrated against measurements.

## Tests

`scons run_tests` runs the unit tests. An opt-in stress test throws ~1600
adversarial strokes at the cutter and mesher (it takes ~40 s):

```
build\skinsim_tests.windows.template_debug.x86_64.exe --no-skip -tc="*adversarial*"
```

## Presets

Built-in tissue presets live in `game/presets/*.json` (format documented in
`game/presets/README.md`). Presets saved from the app go to `user://presets/`.
