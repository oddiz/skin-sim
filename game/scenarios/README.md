# Scenarios

Teaching cases, one JSON file each (loaded in file-name order). Opened from
the menu (⋯ → Scenarios) or with `--scenario <n>` (1-based). See
`game/scenario.gd` for how they are run and scored.

| Key | Meaning |
|---|---|
| `title`, `brief`, `goals` | What the user reads. |
| `site` | Tissue preset name (`game/presets`). |
| `sheet` | `width`, `height` (mm) and `element_mm` of the skin patch. |
| `lesion` | `kind` (0 nodular BCC, 1 SCC, 2 melanoma), `diameter` mm, `at` [x, y] mm. |
| `margin_mm` | Target excision margin. |
| `approach` | `text`, and `draw`: `{"kind": "ellipse", "along": "rstl", "ratio", "tip_deg", "undermine_mm"}` or `{"kind": "flap", "type": FlapType, "direction_deg"}` (harvest direction, degrees, x axis = 0, counter-clockwise). |
| `params` | Tissue overrides; the cases use `pin_boundary: false` (the patch's edges free, held only by tethering: a finite patch with pinned edges is far stiffer than a body). |
| `reference` | `tension_kpa` (added tension averaged over the sheet) and `max_excess` (99th percentile): the suggested approach carried out by `--probe-scenario <n>` (ellipses fully closed; flaps: key sutures only). |
| `tension_factors` | [green, amber] multiples of the reference tension (default [1.1, 1.5]). Dog-ears: green up to the reference + 0.05, amber + 0.15. |
| `tips` | One teaching line per criterion (`lesion`, `closed`, `tension`, `dog_ears`, `sutures`), shown when it is missed. |
