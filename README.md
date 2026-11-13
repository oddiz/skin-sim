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

## Web version

The same app runs in a browser. Godot's engine and our C++ plugin are both
compiled to WebAssembly (WASM), a compact binary format browsers run at near
native speed (the solver measured the same speed as the Windows build). The
export runs without threads, so any static web host works (no special headers).

One-time setup (Windows):

- Emscripten, the C++ -> WASM compiler, version **4.0.20** (must match the one
  Godot 4.7.2's web templates were built with); commands below.
- Godot export templates 4.7.2 (Editor -> Manage Export Templates), or just the
  `web_dlink_nothreads_*.zip` files from the template package copied into
  `%APPDATA%\Godot\export_templates\4.7.2.stable\`.

```powershell
git clone https://github.com/emscripten-core/emsdk.git C:\emsdk
C:\emsdk\emsdk install 4.0.20
C:\emsdk\emsdk activate 4.0.20
```

Build and export (PowerShell, from the repo root):

```powershell
. C:\emsdk\emsdk_env.ps1          # puts emcc on PATH for this shell only
scons platform=web threads=no target=template_release
mkdir export\web
Godot_v4.7.2-stable_win64_console.exe --headless --path game --export-release "Web" ../export/web/index.html
python -m http.server 8060 --directory export/web    # then open http://127.0.0.1:8060
```

A browser can't open the page from disk (`file://`); it needs a web server,
even a local one like the line above. Notes:

- On the web, user presets are saved in the browser's own storage (one browser,
  one device).
- The first load downloads ~50 MB (mostly Godot's engine), less when the server
  compresses it.
- If the page feels slow while another window has focus, that is Windows
  throttling background browser windows, not the solver.

### Publishing on Vercel

`.github/workflows/deploy-web.yml` builds the page on GitHub's machines on every
push to `main` (or by hand from the Actions tab) and uploads it to Vercel. It
needs three repository secrets: `VERCEL_TOKEN` (create one at
vercel.com/account/tokens), and `VERCEL_ORG_ID` and `VERCEL_PROJECT_ID`, which
`npx vercel link` writes into `.vercel/project.json`. The Vercel project should
*not* be connected to the GitHub repository itself, or Vercel would also try to
build every push on its own (and fail: it has no Emscripten or Godot).

## Controls

The screen: tools on the left rail; the current tool's settings and results
in the panel on the right; **View** (colour map, mesh, solver details),
**Tissue** (body-site presets and skin properties, raw model parameters under
"Advanced") and **⋯** (reset, clear) along the top. The line at the bottom
says what to do next with the current tool; short messages above it report
what just happened. Every setting has an (i) button with a short explanation.

| Input | Action |
|---|---|
| Drag (Grab tool, G) | Grab and pull the skin with forceps; several fingers on a touch screen |
| Drag (Cut tool, C), or Shift + drag | Draw an incision; it is made when you release |
| Drag (Suture tool, S) | Press at the needle entry, release at the exit: a suture tied with the panel's knot tightness |
| Tap a suture (Suture tool) | Select it; the knot tightness slider and thread picker then apply to it |
| - / + (or the buttons next to it) | Loosen / tighten the selected suture by 5% |
| Del (or X, or "Cut") | Cut the selected suture out; Z ties it again |
| T | Cycle the suture labels: smart / all / none |
| Drag (Undermine tool, U) | Paint where the skin is freed from the tissue below (shaded darker) |
| Tap (Lesion tool, L) | Mark a lesion (diameter in the panel) |
| Drag (Excise tool, E) | Tip to tip: a fusiform excision (shape in the panel), cut and removed on release |
| Tap (Excise tool) | Remove the piece of skin under the finger, if incisions have cut it free all round |
| Drag (Flap tool, F) | From a lesion outward: designs the excision and a local flap in that direction (previewed), then cuts, removes and undermines on release |
| "Tie key sutures" (Flap section) | Tie the last flap's key sutures: they pull it into place |
| Drag (Mark tool, M) | Draw on the skin with a surgical marker to plan cuts and flaps (no effect on the skin). Lines are kept in tissue coordinates, so they stretch and move with the skin. Tap a line to erase it |
| Z | Undo the last incision, suture, undermining stroke, lesion, excision, flap or marking |
| Esc / ? | Close panels and deselect / keyboard shortcuts |
| H | Next colour map: plain / tension (max principal) / von Mises / area change / squeezing (min principal) / excess skin |
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
### Suture strength and labels (M9)

Each suture has a thread size (USP 7-0 to 2-0, picked in the panel; 4-0 by
default). A thread snaps when its tension exceeds its knot-pull strength: the
minimum the US Pharmacopeia requires of sterile Class I non-absorbable sutures
(6-0: 2.0 N, 5-0: 3.9 N, 4-0: 5.9 N, 3-0: 9.4 N). Good threads are usually
somewhat stronger, so sutures here snap a little early rather than late. The
knot is a thread's weakest point, hence knot-pull rather than straight-pull
strength.

The thread carries half of the closing force: a simple interrupted suture is a
loop that crosses the wound twice (deep in the dermis, and over the top with the
knot). So a 4-0 thread snaps when its suture pulls the edges together with
about 12 N. A gap that needs more than that can't be closed with that thread;
it needs undermining, more sutures sharing the load, or a flap.

Snapping is checked only once the skin has settled. While the solver is still
moving (just after tying across a wide gap, say) the thread is momentarily
stretched far more than it will be. Then only the most loaded suture snaps; the
skin settles again with its neighbours taking up its load, and they may snap in
turn. An overloaded closure therefore fails one suture at a time, worst first.
"Sutures can snap" (Sutures section) turns this off: overloaded sutures then
hold and flash, with OVER on their label.

Sutures are coloured by load (thread tension over breaking strength): green,
yellow at 50%, red at 100%, grey when slack. Text is shown only where it helps
("Smart" labels): the selected suture, any suture above 75% of its strength,
and the one under the mouse. Labels read closing force and load (e.g.
"3.4 N  86%"); the selected suture also shows its loop stress: the stress along
the thread in the tissue the loop holds (+ stretched, - squeezed). The panel
shows the selected suture in full. The heatmap
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

## Local flaps (M10)

Mark a lesion, pick a flap type in the panel's Flap section, then drag with
the Flap tool from the lesion outward, towards where the flap should come
from. While you drag, the design is previewed: the defect (purple, the lesion
plus the excision margin all round), the flap (shaded blue), its incisions
(red), its uncut base (dashed green: what it stays attached by) and its key
sutures (dotted blue). On release the incisions are cut, the defect's skin is
removed and the flap, the defect and a margin around them are undermined, all
as one undo step. Prestressed skin retracts, so the defect opens. "Tie key
sutures" then ties the design's key sutures (thread and tightness from the
Suture section), pulling the flap into place. Close the rest with the Suture
tool.

| Type | Defect | Flap | Moves by |
|---|---|---|---|
| Advancement | square | two parallel limbs, length : width from the panel | sliding forward |
| Rotation | triangle (height 2 x the defect's diameter) | an arc around the triangle's apex; arc angle from the panel | turning about the apex by the triangle's apex angle |
| Rhomboid (Limberg) | rhombus, 60 / 120 degree angles | an identical rhombus beside it | turning 60 degrees about the far end of its base |

The key sutures pull each flap's leading corner to the far side of the defect
(advancement: two across its width; rotation: the corner and halfway along;
rhomboid: both moving corners plus one across the donor site's V). The panel
shows each flap's design and its base stress: the mean largest principal
stress along its uncut base, a rough sign of how hard the closure pulls on the
pedicle. It is not a model of the flap's blood supply.

Checked against surgical rules of thumb (tests in `test_flaps.cpp`): a longer
rotation arc closes with less tension (70 degrees: 2.5 N, 150 degrees: 0.6 N on
the key sutures), and so does a longer advancement flap (1 : 1: 2.3 N, 3 : 1:
1.3 N). The numbers themselves are illustrative (uncalibrated tissue).

Not modelled yet: Burow's triangles (the small triangles excised at an
advancement flap's base to remove its dog-ears; the excess skin heatmap shows
where they would go), double flaps (bilateral advancement, bilobed), and
choosing the direction to suit the RSTL automatically.

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

- More flap designs: Burow's triangles, bilateral advancement (H-plasty),
  bilobed and note flaps.
- Cheese-wiring (a suture tearing through the tissue it bites) as a second way
  for a suture to fail.
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
