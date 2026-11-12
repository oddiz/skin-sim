# Tissue presets

Each `*.json` file here is a built-in preset, listed in the app in file-name order.
Presets the user saves in the app go to `user://presets/` instead
(on Windows: `%APPDATA%\Godot\app_userdata\skin-sim\presets\`).

## Format (version 1)

```json
{
  "format_version": 1,
  "name": "Shown in the preset list",
  "description": "Shown when the preset is selected",
  "params": {
    "shear_modulus_kpa": 50,             // small-strain shear modulus mu, kPa
    "stiffening": 8,                     // exponential stiffening rate b (0 = Neo-Hookean)
    "thickness": 2.0,                    // dermis thickness, mm
    "tether_stiffness_kpa_per_mm": 1.0,  // attachment to the base, kPa/mm (= 1e-3 N/mm^3)
    "pin_boundary": true,                // hold the sheet's outer edge fixed
    "fibre_angle_deg": 0,                // RSTL direction, degrees from horizontal
    "fibre_stiffness_kpa": 50,           // GOH fibre stiffness k1, kPa (0 = no fibres)
    "fibre_nonlinearity": 5,             // GOH k2: how steeply the fibres stiffen
    "fibre_dispersion": 0.15,            // GOH kappa: 0 aligned .. 0.333 isotropic
    "prestretch_along_pct": 8,           // in-vivo prestretch along the RSTL, %
    "prestretch_across_pct": 3,          // ... and across, %
    "undermined_tethering_pct": 5,       // attachment left where undermined, %
    "wrinkling_pct": 90                  // compression resistance lost by buckling, % (0 = flat membrane)
  }
}
```

(JSON itself has no comments; they are only for explanation here.)

Unknown keys are ignored with a warning; missing keys leave the current value unchanged.
Presets describe *tissue* only. Grab settings (radius, force limit) are not part of a preset.

**The values are illustrative, chosen to feel plausible and differ sensibly between
sites. They are not calibrated against measurements.** Calibrating them against published
skin mechanics data is a research task for later.

Exporting: JSON files are not Godot resources, so an export preset must include
`*.json` in "Filters to export non-resource files" (Export dialog, Resources tab).
