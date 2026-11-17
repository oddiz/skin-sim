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

The loading screen (logo, progress in MB, rotating teaching tips) is our own
page, `game/web/shell.html`, based on Godot 4.7.2's default one (same loading
code, our look); Godot's logo splash is switched off (project.godot,
`application/boot_splash`).

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

The screen: tools on the left rail, in two modes switched at its top (or P):
**Operate** (grab, cut, suture, undermine, excise, flap) and **Plan** (lesion,
pen, line, rectangle, circle, ruler, angle, erase, margin). In Plan one finger on the
skin only draws: the skin can't be pulled by accident; the current tool's settings and results
in the panel on the right; **Fit**, **View** (colour map, mesh, solver
details), **Tissue** (body-site presets and skin properties, raw model
parameters under "Advanced") and **⋯** (reset, clear) along the top. The line
at the bottom says what to do next with the current tool; short messages above
it report what just happened. Every setting has an (i) button with a short
explanation.

On a phone (a window narrower than 820 points) the skin fills the screen: the
tools are a bar along the bottom (swipe it sideways for more), and the tool's
settings are a sheet above it. Tap or drag the sheet's handle to show more
(peek, half, full). The interface is drawn at the screen's pixel density
(about 3 pixels per point on a phone), so it has the same physical size on
every screen; `--ui-scale <x>` overrides it.

**Moving around.** Two fingers pan the view and pinch to zoom; one finger uses
the tool. A second finger that lands within a quarter of a second of the
first, or before the first has moved, turns both into a two-finger gesture
and cancels whatever the first had started (no stray cut or grab). Once a
finger is busy with a tool, more fingers are ignored. With a mouse: the wheel
zooms about the pointer, right or middle drag pans, Home or **Fit** frames the
skin again.

| Input | Action |
|---|---|
| ↺ (top bar), twice; or Ctrl+R | **Start over**: fresh skin, everything cleared (incisions, flaps, sutures, undermining, lesions, markings, hooks). Tissue settings and view stay. The first tap says what it will do; tap again within 3 s |
| Drag (Grab tool, G) | Grab and pull the skin with forceps; let go and it springs back |
| Drag, then hold still ~0.6 s | Set a **skin hook**: a ring fills, and the grab stays when you let go (also while you suture or cut). Drag a hook to move it, tap it to let go. Grab panel: "Every grab holds", "Release all" |
| Two fingers / wheel / right drag | Pan and zoom the view; Home fits the skin again |
| Drag (Cut tool, C), or Shift + drag | Draw an incision; it is made when you release |
| Drag (Suture tool, S) | Press at the needle entry, release at the exit: a suture tied with the panel's knot tightness |
| Tap a suture (Suture tool) | Select it; the knot tightness slider and thread picker then apply to it |
| - / + (or the buttons next to it) | Loosen / tighten the selected suture by 5% |
| Del (or X, or "Cut") | Cut the selected suture out; Z ties it again |
| T | Cycle the suture labels: smart / all / none |
| Drag (Undermine tool, U) | Paint where the skin is freed from the tissue below (shaded darker) |
| Tap (Lesion tool, L) | Plan: mark a lesion: type (nodular BCC, SCC, melanoma) and diameter in the panel. The looks draw it in the skin itself, so it stretches with the skin and goes exactly where it is excised |
| Tap or drag (Margin, N) | Plan: tap a lesion for a circle at the chosen margin round it (2, 4, 5, 10, 20 mm; dotted by default), or drag outward from it for any margin in 0.5 mm steps |
| Drag (Excise tool, E) | Tip to tip: a fusiform excision (shape in the panel), cut and removed on release |
| Tap (Excise tool) | Remove the piece of skin under the finger, if incisions have cut it free all round |
| Drag (Flap tool, F) | From a lesion outward: designs the excision and a local flap in that direction (previewed), then cuts, removes and undermines on release (or draws it with the marker: "Plan with the marker") |
| "Tie key sutures" (Flap section) | Tie the last flap's key sutures: they pull it into place |
| Drag (Pen, M) | Plan: draw on the skin with a surgical marker (no effect on the skin). Markings are kept in tissue coordinates, so they stretch and move with the skin; where an incision crosses one it ends on each lip, and nothing is drawn across an open wound or an excised hole |
| Drag (Line I, Rectangle B, Circle O) | Plan: a straight line; a rectangle corner to corner; a circle from its centre. Panel: colour, solid / dashed / dotted, Snap (lines click to 15° steps) |
| Drag (Ruler, D) | Plan: measure a distance, with mm ticks. The length shows while you drag; with View → Measurements the labels stay, giving the length as drawn and what it measures now once the skin has moved ("30.0 mm → 31.5 now") |
| Drag, then drag again (Angle, A) | Plan: the first arm out of the corner, then the second arm (start anywhere). The angle shows while you drag (kept with View → Measurements, as drawn and now: "60° → 65° now"); Esc cancels a half-made angle |
| Tap or rub (Erase) | Plan: erase the markings touched (undo brings them back) |
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
shows the selected suture in full. The stress heatmaps
show the Cauchy (true) stress in the skin (see "Reading the tension map").
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

## Local flaps (M10, catalogue)

Mark a lesion, pick a flap in the Flap panel, then drag with the Flap tool
from the lesion outward, towards where the flap should come from. While you
drag, the design is previewed: the defect (purple, the lesion plus the
excision margin all round) and any Burow's triangles, the flaps (shaded
blue), their incisions (red), their uncut bases (dashed green: what they stay
attached by) and the key sutures (dotted blue). On release the incisions are
cut, the defect's skin is removed and the flaps, the defect and a margin
around them are undermined, all as one undo step. Prestressed skin retracts,
so the defect opens. "Tie key sutures" then ties the design's key sutures
(thread and tightness from the Suture panel), pulling the flaps into place.
Close the rest with the Suture tool.

Each flap has a teaching card ("About this flap"): how the skin moves, where
it is used, how it is drawn and what to watch for. "Plan with the marker"
draws the design on the skin instead of cutting it (violet: cut, blue: key
sutures), to cut along it yourself.

| Family | Flap | Design here |
|---|---|---|
| Advancement | Advancement (U-flap) | square defect, two parallel limbs (length : width); optional Burow's triangles at their ends |
| | H-plasty | two advancement flaps facing each other, each moving half the defect |
| | A-T plasty | triangular defect, its base (the T's bar) extended both ways; drag towards the line the bar should lie in |
| | V-Y island | a triangle cut free all round, left on its pedicle (not undermined), slides in; the V closes as a Y |
| | Keystone (Behan) | 3 : 1 fusiform defect, a keystone-shaped island beside it with V-Y ends |
| Rotation | Rotation | triangular defect, an arc centred on its apex (arc angle) |
| | O-Z | round defect, two opposite arcs, each flap fills half; the scar is an S |
| Transposition | Rhomboid (Limberg) | 60 / 120 degree rhombus, an identical rhombus beside it, turned 60 degrees |
| | Dufourmentel | rhombus of any angle (50-90 degrees); first limb bisects the extended short diagonal and side, second parallel to the long diagonal |
| | Bilobed (Zitelli) | round defect, pivot one radius from its edge, two lobes 45 degrees apart, Burow's triangle at the pivot |
| | Z-plasty | the drag is the central limb; two equal limbs at the chosen angle (30-90 degrees: about 25-125% longer) |

The Z-plasty is only drawn with the marker: its two flaps are lifted over each
other, which a flat sheet cannot do (tied in the simulation they pull through
each other at forces no thread survives). Interpolation flaps (paramedian
forehead, nasolabial, Abbe) pass their pedicle over intact skin and are listed
in the panel for reference only.

An island flap (V-Y, keystone) is held by its *pedicle*: the fat and vessels
beneath it. Here that is its tethering to the base, so its middle is not
undermined; only a rim around it is. It therefore moves less than a flap
raised on undermined skin, as in surgery.

The panel shows each flap's design and its base stress: the mean largest
principal stress along its uncut base, a rough sign of how hard the closure
pulls on the pedicle. It is not a model of the flap's blood supply.

Checked against surgical rules of thumb (tests in `test_flaps.cpp`): a longer
rotation arc closes with less tension (70 degrees: 2.5 N, 150 degrees: 0.6 N on
the key sutures), and so does a longer advancement flap (1 : 1: 2.3 N, 3 : 1:
1.3 N). Every design closes its defect with its key sutures, none above about
60% of a 4-0 thread except the Limberg's donor-site corner (~85%). The numbers
themselves are illustrative (uncalibrated tissue).

Sources for the designs: StatPearls (local flaps, rhombic flaps); Zitelli's
bilobed geometry (pivot one radius away, 45-50 degree lobes); Webster et al.
1978 and Dufourmentel 1962 via review articles; Behan 2003 (keystone).

## Looks (graphics)

View → **Look** switches the visual style live; nothing in the mechanics
changes. `--look N` picks one from the command line.

- **Classic**: the plain working view (flat colours).
- **Illustration**: stylised realism, like a surgical atlas. Soft
  wrapped light; faint mottling fixed to the tissue (it moves with the skin);
  skin pales where it is stretched and flushes where it is squeezed
  (blanching); puckered skin (dog-ears) is shaded as a bulge and hatched
  across its wrinkles; every incision has a fine ink line, and the cut face
  shows the layers (epidermis, dermis, yellow fat lobules) above the red
  wound bed.
- **Realistic** (default): as under theatre lights. Pores and fine skin lines (bumps
  in the shading that fade out when zoomed out, so they never shimmer),
  melanin and blood variation and a few freckles, all fixed to the tissue;
  light glowing red through the skin at the edge of shadows (faked
  subsurface scattering: the browser renderer has none built in); dog-ears
  as real folds; blanching; wet, glossy cut faces and wound bed; black nylon
  sutures with a knot and its two cut ends (load rings at the needle holes
  with View → Suture load dots); marker ink with
  a soft edge; a blue-green surgical drape glued over the skin's margin
  (drawn by the skin shader on the tissue's rest position, so its edge moves
  with the skin; no cloth physics, no image files). **Skin type**
  (Fitzpatrick I–VI, View) sets the tone and pigment; blanching and redness
  show less on darker skin, as in life. A soft vignette, as under a lamp.
  Tuned for phones: no MSAA (FXAA instead, see below), and the pore and
  skin-line relief costs 7 texture reads per pixel instead of 11.
- **Game**: bright and bold. Cel shading (light in three flat bands, cool
  purple shadows), a cartoon outline along every incision (same width on
  screen at any zoom), layer-cake cut faces, a glossy candy-red wound bed,
  glowing threads and neon marker, a dark background with a 10 mm grid.

**Effects** (View; on by default in Game, off in the others): sparks from the
scalpel while cutting, a ring pulse when a suture is tied (one after another
for 'Tie key sutures') or a flap is raised, and when a suture snaps its two
ends whip back, sparks fly and the view shakes briefly. Drawn in 2D over the
view but pinned to the skin, so they stay put when you pan or zoom
(`game/fx/feedback.gd`). `--demo-fx` fires them all for a screenshot.

**Bleeding** (View, on by default): blood pools in the open wound bed over
about 15 s; it is bright when fresh and darkens over the half minute after
the last cut. None on the skin or the cut faces. Drawing only: no blood
physics, and it does not change the mechanics. Not in Classic.

How it works: SkinSheet gives each vertex extra data for the shaders (its
rest position, the excess skin and the direction the skin is squeezed, the
area change, the distance to the nearest incision), and a shading relief
(normals tilted where skin puckers and at the rounded lips). The cut face is
drawn as a narrow band sloping into the wound, since a vertical wall is
invisible from straight above. The shaders are in `game/looks/`
(`skin_illustration.gdshader`, `skin_realistic.gdshader`, `skin_game.gdshader`,
`walls.gdshader`, `bed.gdshader`, `drape.gdshader`, `game_background.gdshader`) and can be
edited without touching C++. Every look uses Godot's Compatibility renderer
(as the browser does), so the desktop shows what a phone shows.

### Performance

View → Solver details shows the frame rate, the CPU time per frame and the
GPU (graphics card) time per frame, where the browser provides it. For
before/after comparisons on the desktop:

```
Godot_v4.7.2-stable_win64_console.exe --path game --resolution 3840x2160 -- --look 2 --plain --zoom 1.5 --demo-flap --flap-type 2 --bench --screenshot out.png
```

prints the mean GPU time over 60 frames (vsync off; `--msaa 0|1|2` and
`--fxaa` override the look's anti-aliasing). Measured at 4K, Realistic look:
8 ms with 2x MSAA, 4 ms with FXAA. MSAA was the largest single cost: on a
phone's high-resolution screen it would be worse still. The plane around the
skin (drape, Game background) is a frame with a hole, so it is not shaded
under the skin.

The processor side mattered more on phones: with a flap on screen and the
skin at rest, the scripts spent ~7 ms per frame re-asking for things that had
not changed (the legend recomputed the stress at every vertex, lesion
outlines were re-located on the mesh, the value under the pointer searched
the mesh). SkinSheet now keeps those answers until the skin changes
(`get_render_version()`), and the scripts skip panel updates while it is at
rest: ~1.6 ms per frame (desktop: 98 -> 240 fps). `--bench` also prints a
per-section breakdown (`Prof` in game/prof.gd) and waits for the skin to
come to rest (`--bench-frames N`).

## Reading the tension map

The **Tension** map colours the largest principal stress; **Squeezing** the
smallest; **Overall stress** the von Mises stress (View, or H to cycle).

- **Only what the surgery added** (on by default): living skin is always under
  tension (its prestress, Tissue panel), so with absolute stress every bit of
  skin is reddish. This subtracts the resting skin's own stress: untouched
  skin is neutral and the colours show what cutting, pulling and suturing
  changed.
- **Automatic scale** (on by default): the colour scale is a round number
  (1, 2, 5 x 10^n kPa) just above the stress over most of the skin (its 92nd
  percentile), so a few extreme points (needle holes, flap corners) don't turn
  everything red. It only changes when the stresses outgrow it or fall well
  below it. Turn it off for a fixed scale, to compare closures on one scale.
- **Contour lines**: lines of equal stress every tenth of the scale (ticks on
  the legend), like the contours of a map: close together where the stress
  changes fast. They carry on beyond the end of the scale, so a saturated red
  area still shows where it is worst.
- **Direction strokes**: a short stroke every ~20 pixels along the direction
  the skin is pulled (squeezed, on the Squeezing map), longer and darker
  where the stress is higher. Around a flap they trace the lines of tension.
- The value under the mouse is shown next to it.

The colours are computed per pixel on the graphics card: the stress's place
on the scale is passed with each vertex and a small shader looks it up in a
colour ramp and draws the contour lines.

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

- Z-plasty in the simulation (the flaps would need to pass over each other).
- Guided scenarios (UI4) and a tissue drawer polish (UI3).
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
