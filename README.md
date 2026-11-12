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
| Z | Undo the last incision, suture or undermining stroke |
| H | Cycle the heatmap: off / max principal stress / von Mises stress / area change |
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

Limitation: the skin is a flat membrane, so squeezed tissue can only compress
in-plane. Real skin would bulge (evert) or wrinkle instead, and the strain-stiffening
law stiffens in compression as it does in tension. Over-tight knots (beyond ~20%)
therefore give much higher tension and compression than they would in reality:
trust the sign and the location, not the magnitude.

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
