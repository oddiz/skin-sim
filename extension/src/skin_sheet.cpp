#include "skin_sheet.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <queue>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "skinsim/excision.hpp"
#include "skinsim/info.hpp"

namespace godot {

namespace {

// Lift the wireframe slightly above the sheet so the GPU (graphics processing
// unit) doesn't flicker between drawing lines and surface at the same depth
// ("z-fighting").
constexpr real_t kWireLift = 0.05f;  // mm
// Markings and lesion outlines are sampled this densely (mm) along the skin,
// so they bend with it (see solver/src/marks.cpp).
constexpr double kMarkStepMm = 0.5;
// Incision lips are drawn a little higher still, so they sit on top of the wireframe.
constexpr real_t kLipLift = 0.1f;  // mm
// The wound bed (the tissue under the skin, seen when a wound opens) lies
// this far below the sheet.
constexpr real_t kWoundBedDepth = 0.5f;  // mm

Vector3 to_godot(const Eigen::Vector2d& p, real_t lift = 0.0f) {
    return Vector3(static_cast<real_t>(p.x()), lift, static_cast<real_t>(-p.y()));
}

// Heatmap modes, matching the `heatmap` property's enum hint.
enum HeatmapMode {
    HEATMAP_OFF = 0,
    HEATMAP_MAX_PRINCIPAL,
    HEATMAP_VON_MISES,
    HEATMAP_AREA_CHANGE,
    HEATMAP_MIN_PRINCIPAL,
    HEATMAP_EXCESS_SKIN,
    HEATMAP_LAST = HEATMAP_EXCESS_SKIN
};

// Excess skin heatmap spans 0 .. kExcessRange (0.3 = 30% more skin than room for it).
constexpr double kExcessRange = 0.3;

// Area change heatmap spans 1/kAreaRange .. kAreaRange (log scale).
constexpr double kAreaRange = 1.5;

// Piecewise-linear colour scale through evenly spaced stops; t in [0, 1].
Color ramp(const Color* stops, int count, double t) {
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const double f = t * (count - 1);
    const int i = f >= count - 1 ? count - 2 : static_cast<int>(f);
    return stops[i].lerp(stops[i + 1], static_cast<real_t>(f - i));
}

// Diverging blue-white-red (after Moreland's "cool to warm"): for signed
// quantities, compression/shrinking blue, tension/stretching red. Pale
// in the middle so "nothing happening" reads as neutral.
const Color kDiverging[] = {Color(0.23, 0.30, 0.75), Color(0.55, 0.69, 0.99), Color(0.87, 0.87, 0.87),
                            Color(0.96, 0.60, 0.48), Color(0.71, 0.02, 0.15)};
// Sequential pale yellow -> dark red (after ColorBrewer "YlOrRd"): for
// magnitudes. Monotone in lightness, so it also reads in greyscale.
const Color kSequential[] = {Color(1.0, 1.0, 0.80), Color(0.996, 0.85, 0.46), Color(0.99, 0.55, 0.24),
                             Color(0.89, 0.10, 0.11), Color(0.50, 0.0, 0.15)};

}  // namespace

// The app lets threads snap at their breaking strength (the solver's own
// default is off, for its tests). The panel has a switch to turn it off.
skinsim::SutureParams SkinSheet::app_default_sutures() {
    skinsim::SutureParams p;
    p.can_break = true;
    return p;
}

skinsim::TissueParams SkinSheet::app_default_tissue() {
    // Illustrative values chosen to feel plausible, NOT calibrated: horizontal
    // RSTL; collagen fibres carry the strain-stiffening along them, so the
    // isotropic matrix stiffens less than in M3; living skin pre-stretched 8%
    // along the RSTL and 3% across, so incisions gape.
    skinsim::TissueParams p;
    p.stiffening = 4.0;
    p.fibre_angle = 0.0;
    p.fibre_stiffness = 0.05;  // 50 kPa
    p.fibre_nonlinearity = 5.0;
    p.fibre_dispersion = 0.15;
    p.prestretch_along = 1.08;
    p.prestretch_across = 1.03;
    p.undermined_tether = 0.05;
    // Buckled skin keeps 10% of its resistance to compression (bending of
    // the dermis, the tissue underneath): dog-ears pucker instead of pushing.
    p.wrinkling = 0.9;
    return p;
}

void SkinSheet::_bind_methods() {
    // Every method GDScript may call, or that a property uses, must be bound.
    ClassDB::bind_method(D_METHOD("set_width", "mm"), &SkinSheet::set_width);
    ClassDB::bind_method(D_METHOD("get_width"), &SkinSheet::get_width);
    ClassDB::bind_method(D_METHOD("set_height", "mm"), &SkinSheet::set_height);
    ClassDB::bind_method(D_METHOD("get_height"), &SkinSheet::get_height);
    ClassDB::bind_method(D_METHOD("set_element_size", "mm"), &SkinSheet::set_element_size);
    ClassDB::bind_method(D_METHOD("get_element_size"), &SkinSheet::get_element_size);
    ClassDB::bind_method(D_METHOD("set_shear_modulus_kpa", "kpa"), &SkinSheet::set_shear_modulus_kpa);
    ClassDB::bind_method(D_METHOD("get_shear_modulus_kpa"), &SkinSheet::get_shear_modulus_kpa);
    ClassDB::bind_method(D_METHOD("set_stiffening", "b"), &SkinSheet::set_stiffening);
    ClassDB::bind_method(D_METHOD("get_stiffening"), &SkinSheet::get_stiffening);
    ClassDB::bind_method(D_METHOD("set_grab_radius", "mm"), &SkinSheet::set_grab_radius);
    ClassDB::bind_method(D_METHOD("get_grab_radius"), &SkinSheet::get_grab_radius);
    ClassDB::bind_method(D_METHOD("set_max_pull_force", "newton"), &SkinSheet::set_max_pull_force);
    ClassDB::bind_method(D_METHOD("get_max_pull_force"), &SkinSheet::get_max_pull_force);
    ClassDB::bind_method(D_METHOD("set_thickness", "mm"), &SkinSheet::set_thickness);
    ClassDB::bind_method(D_METHOD("get_thickness"), &SkinSheet::get_thickness);
    ClassDB::bind_method(D_METHOD("set_tether_stiffness_kpa_per_mm", "value"),
                         &SkinSheet::set_tether_stiffness_kpa_per_mm);
    ClassDB::bind_method(D_METHOD("get_tether_stiffness_kpa_per_mm"),
                         &SkinSheet::get_tether_stiffness_kpa_per_mm);
    ClassDB::bind_method(D_METHOD("set_grab_stiffness", "n_per_mm"), &SkinSheet::set_grab_stiffness);
    ClassDB::bind_method(D_METHOD("get_grab_stiffness"), &SkinSheet::get_grab_stiffness);
    ClassDB::bind_method(D_METHOD("set_fibre_angle_deg", "deg"), &SkinSheet::set_fibre_angle_deg);
    ClassDB::bind_method(D_METHOD("get_fibre_angle_deg"), &SkinSheet::get_fibre_angle_deg);
    ClassDB::bind_method(D_METHOD("set_fibre_stiffness_kpa", "kpa"), &SkinSheet::set_fibre_stiffness_kpa);
    ClassDB::bind_method(D_METHOD("get_fibre_stiffness_kpa"), &SkinSheet::get_fibre_stiffness_kpa);
    ClassDB::bind_method(D_METHOD("set_fibre_nonlinearity", "k2"), &SkinSheet::set_fibre_nonlinearity);
    ClassDB::bind_method(D_METHOD("get_fibre_nonlinearity"), &SkinSheet::get_fibre_nonlinearity);
    ClassDB::bind_method(D_METHOD("set_fibre_dispersion", "kappa"), &SkinSheet::set_fibre_dispersion);
    ClassDB::bind_method(D_METHOD("get_fibre_dispersion"), &SkinSheet::get_fibre_dispersion);
    ClassDB::bind_method(D_METHOD("set_prestretch_along_pct", "pct"), &SkinSheet::set_prestretch_along_pct);
    ClassDB::bind_method(D_METHOD("get_prestretch_along_pct"), &SkinSheet::get_prestretch_along_pct);
    ClassDB::bind_method(D_METHOD("set_prestretch_across_pct", "pct"), &SkinSheet::set_prestretch_across_pct);
    ClassDB::bind_method(D_METHOD("get_prestretch_across_pct"), &SkinSheet::get_prestretch_across_pct);
    ClassDB::bind_method(D_METHOD("set_undermined_tethering_pct", "pct"), &SkinSheet::set_undermined_tethering_pct);
    ClassDB::bind_method(D_METHOD("get_undermined_tethering_pct"), &SkinSheet::get_undermined_tethering_pct);
    ClassDB::bind_method(D_METHOD("set_wrinkling_pct", "pct"), &SkinSheet::set_wrinkling_pct);
    ClassDB::bind_method(D_METHOD("get_wrinkling_pct"), &SkinSheet::get_wrinkling_pct);
    ClassDB::bind_method(D_METHOD("excise", "point"), &SkinSheet::excise);
    ClassDB::bind_method(D_METHOD("to_rest", "point"), &SkinSheet::to_rest);
    ClassDB::bind_method(D_METHOD("rest_to_world", "rest_points"), &SkinSheet::rest_to_world);
    ClassDB::bind_method(D_METHOD("rest_point_now", "rest_point"), &SkinSheet::rest_point_now);
    ClassDB::bind_method(D_METHOD("get_closure_metrics"), &SkinSheet::get_closure_metrics);
    ClassDB::bind_method(D_METHOD("get_flap_preview", "start", "end"), &SkinSheet::get_flap_preview);
    ClassDB::bind_method(D_METHOD("make_flap", "start", "end"), &SkinSheet::make_flap);
    ClassDB::bind_method(D_METHOD("undo_flap"), &SkinSheet::undo_flap);
    ClassDB::bind_method(D_METHOD("tie_key_sutures", "tightness"), &SkinSheet::tie_key_sutures);
    ClassDB::bind_method(D_METHOD("get_flap_count"), &SkinSheet::get_flap_count);
    ClassDB::bind_method(D_METHOD("get_flap_info"), &SkinSheet::get_flap_info);
    ClassDB::bind_method(D_METHOD("get_flap_design_rest", "start", "end"), &SkinSheet::get_flap_design_rest);
    ClassDB::bind_method(D_METHOD("set_flap_burow", "on"), &SkinSheet::set_flap_burow);
    ClassDB::bind_method(D_METHOD("get_flap_burow"), &SkinSheet::get_flap_burow);
    ClassDB::bind_method(D_METHOD("set_flap_rhombus_deg", "deg"), &SkinSheet::set_flap_rhombus_deg);
    ClassDB::bind_method(D_METHOD("get_flap_rhombus_deg"), &SkinSheet::get_flap_rhombus_deg);
    ClassDB::bind_method(D_METHOD("set_flap_z_angle_deg", "deg"), &SkinSheet::set_flap_z_angle_deg);
    ClassDB::bind_method(D_METHOD("get_flap_z_angle_deg"), &SkinSheet::get_flap_z_angle_deg);
    ClassDB::bind_method(D_METHOD("set_flap_type", "type"), &SkinSheet::set_flap_type);
    ClassDB::bind_method(D_METHOD("get_flap_type"), &SkinSheet::get_flap_type);
    ClassDB::bind_method(D_METHOD("set_flap_margin", "mm"), &SkinSheet::set_flap_margin);
    ClassDB::bind_method(D_METHOD("get_flap_margin"), &SkinSheet::get_flap_margin);
    ClassDB::bind_method(D_METHOD("set_flap_length_ratio", "ratio"), &SkinSheet::set_flap_length_ratio);
    ClassDB::bind_method(D_METHOD("get_flap_length_ratio"), &SkinSheet::get_flap_length_ratio);
    ClassDB::bind_method(D_METHOD("set_flap_arc_deg", "deg"), &SkinSheet::set_flap_arc_deg);
    ClassDB::bind_method(D_METHOD("get_flap_arc_deg"), &SkinSheet::get_flap_arc_deg);
    ClassDB::bind_method(D_METHOD("set_flap_mirror", "on"), &SkinSheet::set_flap_mirror);
    ClassDB::bind_method(D_METHOD("get_flap_mirror"), &SkinSheet::get_flap_mirror);
    ClassDB::bind_method(D_METHOD("set_flap_undermine_margin", "mm"), &SkinSheet::set_flap_undermine_margin);
    ClassDB::bind_method(D_METHOD("get_flap_undermine_margin"), &SkinSheet::get_flap_undermine_margin);
    ClassDB::bind_method(D_METHOD("excise_fusiform", "tip_a", "tip_b"), &SkinSheet::excise_fusiform);
    ClassDB::bind_method(D_METHOD("get_fusiform_outline", "tip_a", "tip_b"), &SkinSheet::get_fusiform_outline);
    ClassDB::bind_method(D_METHOD("undo_excise"), &SkinSheet::undo_excise);
    ClassDB::bind_method(D_METHOD("get_excision_count"), &SkinSheet::get_excision_count);
    ClassDB::bind_method(D_METHOD("set_ellipse_ratio", "ratio"), &SkinSheet::set_ellipse_ratio);
    ClassDB::bind_method(D_METHOD("get_ellipse_ratio"), &SkinSheet::get_ellipse_ratio);
    ClassDB::bind_method(D_METHOD("set_tip_angle_deg", "deg"), &SkinSheet::set_tip_angle_deg);
    ClassDB::bind_method(D_METHOD("get_tip_angle_deg"), &SkinSheet::get_tip_angle_deg);
    ClassDB::bind_method(D_METHOD("add_lesion", "point"), &SkinSheet::add_lesion);
    ClassDB::bind_method(D_METHOD("undo_lesion"), &SkinSheet::undo_lesion);
    ClassDB::bind_method(D_METHOD("clear_lesions"), &SkinSheet::clear_lesions);
    ClassDB::bind_method(D_METHOD("get_lesion_count"), &SkinSheet::get_lesion_count);
    ClassDB::bind_method(D_METHOD("set_lesion_diameter", "mm"), &SkinSheet::set_lesion_diameter);
    ClassDB::bind_method(D_METHOD("get_lesion_diameter"), &SkinSheet::get_lesion_diameter);
    ClassDB::bind_method(D_METHOD("set_lesion_kind", "kind"), &SkinSheet::set_lesion_kind);
    ClassDB::bind_method(D_METHOD("get_lesion_kind"), &SkinSheet::get_lesion_kind);
    ClassDB::bind_method(D_METHOD("get_lesion_info"), &SkinSheet::get_lesion_info);
    ClassDB::bind_method(D_METHOD("set_undermine_radius", "mm"), &SkinSheet::set_undermine_radius);
    ClassDB::bind_method(D_METHOD("get_undermine_radius"), &SkinSheet::get_undermine_radius);
    ClassDB::bind_method(D_METHOD("undermine_at", "point", "new_stroke"), &SkinSheet::undermine_at);
    ClassDB::bind_method(D_METHOD("undo_undermine"), &SkinSheet::undo_undermine);
    ClassDB::bind_method(D_METHOD("clear_undermining"), &SkinSheet::clear_undermining);
    ClassDB::bind_method(D_METHOD("get_undermine_count"), &SkinSheet::get_undermine_count);
    ClassDB::bind_method(D_METHOD("set_pin_boundary", "pin"), &SkinSheet::set_pin_boundary);
    ClassDB::bind_method(D_METHOD("get_pin_boundary"), &SkinSheet::get_pin_boundary);
    ClassDB::bind_method(D_METHOD("set_suture_stiffness", "n_per_mm"), &SkinSheet::set_suture_stiffness);
    ClassDB::bind_method(D_METHOD("get_suture_stiffness"), &SkinSheet::get_suture_stiffness);
    ClassDB::bind_method(D_METHOD("set_suture_strip_width", "mm"), &SkinSheet::set_suture_strip_width);
    ClassDB::bind_method(D_METHOD("get_suture_strip_width"), &SkinSheet::get_suture_strip_width);
    ClassDB::bind_method(D_METHOD("set_sutures_can_snap", "on"), &SkinSheet::set_sutures_can_snap);
    ClassDB::bind_method(D_METHOD("get_sutures_can_snap"), &SkinSheet::get_sutures_can_snap);
    ClassDB::bind_method(D_METHOD("set_suture_thread", "usp_size"), &SkinSheet::set_suture_thread);
    ClassDB::bind_method(D_METHOD("get_suture_thread"), &SkinSheet::get_suture_thread);
    ClassDB::bind_static_method("SkinSheet", D_METHOD("thread_strength", "usp_size"), &SkinSheet::thread_strength);
    ClassDB::bind_method(D_METHOD("set_heatmap", "mode"), &SkinSheet::set_heatmap);
    ClassDB::bind_method(D_METHOD("get_heatmap"), &SkinSheet::get_heatmap);
    ClassDB::bind_method(D_METHOD("set_heatmap_range_kpa", "kpa"), &SkinSheet::set_heatmap_range_kpa);
    ClassDB::bind_method(D_METHOD("get_heatmap_range_kpa"), &SkinSheet::get_heatmap_range_kpa);
    ClassDB::bind_method(D_METHOD("set_heatmap_relative", "on"), &SkinSheet::set_heatmap_relative);
    ClassDB::bind_method(D_METHOD("get_heatmap_relative"), &SkinSheet::get_heatmap_relative);
    ClassDB::bind_method(D_METHOD("set_heatmap_auto_range", "on"), &SkinSheet::set_heatmap_auto_range);
    ClassDB::bind_method(D_METHOD("get_heatmap_auto_range"), &SkinSheet::get_heatmap_auto_range);
    ClassDB::bind_method(D_METHOD("set_heatmap_contours", "on"), &SkinSheet::set_heatmap_contours);
    ClassDB::bind_method(D_METHOD("get_heatmap_contours"), &SkinSheet::get_heatmap_contours);
    ClassDB::bind_method(D_METHOD("get_heatmap_range_now"), &SkinSheet::get_heatmap_range_now);
    ClassDB::bind_method(D_METHOD("get_render_version"), &SkinSheet::get_render_version);
    ClassDB::bind_method(D_METHOD("get_tension_lines", "spacing_mm"), &SkinSheet::get_tension_lines);
    ClassDB::bind_method(D_METHOD("sample_heatmap", "world_point"), &SkinSheet::sample_heatmap);
    ClassDB::bind_method(D_METHOD("set_iterations_per_frame", "n"), &SkinSheet::set_iterations_per_frame);
    ClassDB::bind_method(D_METHOD("get_iterations_per_frame"), &SkinSheet::get_iterations_per_frame);
    ClassDB::bind_method(D_METHOD("set_show_wireframe", "show"), &SkinSheet::set_show_wireframe);
    ClassDB::bind_method(D_METHOD("get_show_wireframe"), &SkinSheet::get_show_wireframe);
    ClassDB::bind_method(D_METHOD("set_skin_material", "material"), &SkinSheet::set_skin_material);
    ClassDB::bind_method(D_METHOD("get_skin_material"), &SkinSheet::get_skin_material);
    ClassDB::bind_method(D_METHOD("set_wall_material", "material"), &SkinSheet::set_wall_material);
    ClassDB::bind_method(D_METHOD("get_wall_material"), &SkinSheet::get_wall_material);
    ClassDB::bind_method(D_METHOD("set_bed_material", "material"), &SkinSheet::set_bed_material);
    ClassDB::bind_method(D_METHOD("get_bed_material"), &SkinSheet::get_bed_material);
    ClassDB::bind_method(D_METHOD("set_relief_mm", "mm"), &SkinSheet::set_relief_mm);
    ClassDB::bind_method(D_METHOD("get_relief_mm"), &SkinSheet::get_relief_mm);
    ClassDB::bind_method(D_METHOD("set_wall_width_mm", "mm"), &SkinSheet::set_wall_width_mm);
    ClassDB::bind_method(D_METHOD("get_wall_width_mm"), &SkinSheet::get_wall_width_mm);
    ClassDB::bind_method(D_METHOD("get_wound_info"), &SkinSheet::get_wound_info);
    ClassDB::bind_method(D_METHOD("set_skin_color", "color"), &SkinSheet::set_skin_color);
    ClassDB::bind_method(D_METHOD("get_skin_color"), &SkinSheet::get_skin_color);

    ClassDB::bind_method(D_METHOD("grab", "id", "world_point"), &SkinSheet::grab);
    ClassDB::bind_method(D_METHOD("move_grab", "id", "world_point"), &SkinSheet::move_grab);
    ClassDB::bind_method(D_METHOD("release_grab", "id"), &SkinSheet::release_grab);
    ClassDB::bind_method(D_METHOD("release_all_grabs"), &SkinSheet::release_all_grabs);
    ClassDB::bind_method(D_METHOD("reset_simulation"), &SkinSheet::reset_simulation);
    ClassDB::bind_method(D_METHOD("cut", "world_path"), &SkinSheet::cut);
    ClassDB::bind_method(D_METHOD("undo_cut"), &SkinSheet::undo_cut);
    ClassDB::bind_method(D_METHOD("clear_cuts"), &SkinSheet::clear_cuts);
    ClassDB::bind_method(D_METHOD("get_cut_count"), &SkinSheet::get_cut_count);
    ClassDB::bind_method(D_METHOD("get_last_cut_ms"), &SkinSheet::get_last_cut_ms);
    ClassDB::bind_method(D_METHOD("suture", "entry", "exit", "tightness"), &SkinSheet::suture);
    ClassDB::bind_method(D_METHOD("set_suture_tightness", "id", "tightness"), &SkinSheet::set_suture_tightness);
    ClassDB::bind_method(D_METHOD("pick_suture", "point", "max_distance_mm"), &SkinSheet::pick_suture);
    ClassDB::bind_method(D_METHOD("is_on_skin", "point"), &SkinSheet::is_on_skin);
    ClassDB::bind_method(D_METHOD("remove_suture", "id"), &SkinSheet::remove_suture);
    ClassDB::bind_method(D_METHOD("restore_suture", "id"), &SkinSheet::restore_suture);
    ClassDB::bind_method(D_METHOD("set_suture_thread_of", "id", "usp_size"), &SkinSheet::set_suture_thread_of);
    ClassDB::bind_method(D_METHOD("clear_sutures"), &SkinSheet::clear_sutures);
    ClassDB::bind_method(D_METHOD("get_suture_count"), &SkinSheet::get_suture_count);
    ClassDB::bind_method(D_METHOD("get_suture_info"), &SkinSheet::get_suture_info);
    ClassDB::bind_method(D_METHOD("set_lip_contact", "on"), &SkinSheet::set_lip_contact);
    ClassDB::bind_method(D_METHOD("get_lip_contact"), &SkinSheet::get_lip_contact);
    ClassDB::bind_method(D_METHOD("set_contact_stiffness", "n_per_mm3"), &SkinSheet::set_contact_stiffness);
    ClassDB::bind_method(D_METHOD("get_contact_stiffness"), &SkinSheet::get_contact_stiffness);
    ClassDB::bind_method(D_METHOD("get_contact_info"), &SkinSheet::get_contact_info);
    ClassDB::bind_method(D_METHOD("get_heatmap_legend"), &SkinSheet::get_heatmap_legend);

    ClassDB::bind_method(D_METHOD("get_vertex_count"), &SkinSheet::get_vertex_count);
    ClassDB::bind_method(D_METHOD("get_triangle_count"), &SkinSheet::get_triangle_count);
    ClassDB::bind_method(D_METHOD("get_solver_info"), &SkinSheet::get_solver_info);
    ClassDB::bind_method(D_METHOD("get_step_info"), &SkinSheet::get_step_info);
    ClassDB::bind_method(D_METHOD("get_grab_info"), &SkinSheet::get_grab_info);

    // ADD_GROUP folds the following properties under a heading in the Inspector.
    // Hint strings: "min,max,step[,exp][,suffix:unit]"; "exp" = logarithmic slider,
    // useful when a value spans orders of magnitude (moduli do).
    ADD_GROUP("Geometry", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "width", PROPERTY_HINT_RANGE, "5,300,0.5,suffix:mm"),
                 "set_width", "get_width");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "5,300,0.5,suffix:mm"),
                 "set_height", "get_height");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "element_size", PROPERTY_HINT_RANGE, "0.5,50,0.1,suffix:mm"),
                 "set_element_size", "get_element_size");

    ADD_GROUP("Tissue", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shear_modulus_kpa", PROPERTY_HINT_RANGE,
                              "1,2000,0.1,exp,suffix:kPa"),
                 "set_shear_modulus_kpa", "get_shear_modulus_kpa");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stiffening", PROPERTY_HINT_RANGE, "0,30,0.1"),
                 "set_stiffening", "get_stiffening");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "thickness", PROPERTY_HINT_RANGE, "0.2,6,0.05,suffix:mm"),
                 "set_thickness", "get_thickness");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tether_stiffness_kpa_per_mm", PROPERTY_HINT_RANGE,
                              "0,50,0.01,suffix:kPa/mm"),
                 "set_tether_stiffness_kpa_per_mm", "get_tether_stiffness_kpa_per_mm");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pin_boundary"), "set_pin_boundary", "get_pin_boundary");

    ADD_GROUP("Fibres and prestress", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fibre_angle_deg", PROPERTY_HINT_RANGE, "-90,90,1,suffix:deg"),
                 "set_fibre_angle_deg", "get_fibre_angle_deg");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fibre_stiffness_kpa", PROPERTY_HINT_RANGE, "0,1000,0.5,suffix:kPa"),
                 "set_fibre_stiffness_kpa", "get_fibre_stiffness_kpa");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fibre_nonlinearity", PROPERTY_HINT_RANGE, "0,50,0.1"),
                 "set_fibre_nonlinearity", "get_fibre_nonlinearity");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fibre_dispersion", PROPERTY_HINT_RANGE, "0,0.3333,0.005"),
                 "set_fibre_dispersion", "get_fibre_dispersion");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "prestretch_along_pct", PROPERTY_HINT_RANGE, "0,30,0.5,suffix:%"),
                 "set_prestretch_along_pct", "get_prestretch_along_pct");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "prestretch_across_pct", PROPERTY_HINT_RANGE, "0,30,0.5,suffix:%"),
                 "set_prestretch_across_pct", "get_prestretch_across_pct");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "undermined_tethering_pct", PROPERTY_HINT_RANGE, "0,100,1,suffix:%"),
                 "set_undermined_tethering_pct", "get_undermined_tethering_pct");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wrinkling_pct", PROPERTY_HINT_RANGE, "0,99,1,suffix:%"),
                 "set_wrinkling_pct", "get_wrinkling_pct");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "undermine_radius", PROPERTY_HINT_RANGE, "1,30,0.5,suffix:mm"),
                 "set_undermine_radius", "get_undermine_radius");

    ADD_GROUP("Excision", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lesion_diameter", PROPERTY_HINT_RANGE, "0.5,40,0.5,suffix:mm"),
                 "set_lesion_diameter", "get_lesion_diameter");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "lesion_kind", PROPERTY_HINT_ENUM, "Nodular BCC,SCC,Melanoma"),
                 "set_lesion_kind", "get_lesion_kind");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ellipse_ratio", PROPERTY_HINT_RANGE, "1,6,0.1"),
                 "set_ellipse_ratio", "get_ellipse_ratio");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tip_angle_deg", PROPERTY_HINT_RANGE, "10,120,1,suffix:deg"),
                 "set_tip_angle_deg", "get_tip_angle_deg");

    ADD_GROUP("Flaps", "");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "flap_type", PROPERTY_HINT_ENUM,
                              "Advancement,Rotation,Rhomboid,H-plasty,A-T plasty,V-Y island,Keystone,O-Z,"
                              "Dufourmentel,Bilobed,Z-plasty"),
                 "set_flap_type", "get_flap_type");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "flap_burow"), "set_flap_burow", "get_flap_burow");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flap_rhombus_deg", PROPERTY_HINT_RANGE, "50,90,1,suffix:deg"),
                 "set_flap_rhombus_deg", "get_flap_rhombus_deg");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flap_z_angle_deg", PROPERTY_HINT_RANGE, "30,90,1,suffix:deg"),
                 "set_flap_z_angle_deg", "get_flap_z_angle_deg");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flap_margin", PROPERTY_HINT_RANGE, "0,20,0.5,suffix:mm"),
                 "set_flap_margin", "get_flap_margin");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flap_length_ratio", PROPERTY_HINT_RANGE, "0.5,6,0.1"),
                 "set_flap_length_ratio", "get_flap_length_ratio");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flap_arc_deg", PROPERTY_HINT_RANGE, "30,240,1,suffix:deg"),
                 "set_flap_arc_deg", "get_flap_arc_deg");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "flap_mirror"), "set_flap_mirror", "get_flap_mirror");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flap_undermine_margin", PROPERTY_HINT_RANGE, "0,30,0.5,suffix:mm"),
                 "set_flap_undermine_margin", "get_flap_undermine_margin");

    ADD_GROUP("Grab", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grab_radius", PROPERTY_HINT_RANGE, "0,10,0.1,suffix:mm"),
                 "set_grab_radius", "get_grab_radius");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grab_stiffness", PROPERTY_HINT_RANGE,
                              "0.1,100,0.1,exp,suffix:N/mm"),
                 "set_grab_stiffness", "get_grab_stiffness");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_pull_force", PROPERTY_HINT_RANGE,
                              "0.1,50,0.1,exp,suffix:N"),
                 "set_max_pull_force", "get_max_pull_force");

    ADD_GROUP("Sutures", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "suture_stiffness", PROPERTY_HINT_RANGE,
                              "0.1,200,0.1,exp,suffix:N/mm"),
                 "set_suture_stiffness", "get_suture_stiffness");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "suture_strip_width", PROPERTY_HINT_RANGE, "0.1,6,0.1,suffix:mm"),
                 "set_suture_strip_width", "get_suture_strip_width");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sutures_can_snap"), "set_sutures_can_snap", "get_sutures_can_snap");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "suture_thread", PROPERTY_HINT_RANGE, "2,7,1"), "set_suture_thread",
                 "get_suture_thread");

    ADD_GROUP("Lip contact", "");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lip_contact"), "set_lip_contact", "get_lip_contact");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "contact_stiffness", PROPERTY_HINT_RANGE,
                              "1,1000,1,exp,suffix:N/mm^3"),
                 "set_contact_stiffness", "get_contact_stiffness");

    ADD_GROUP("Solver", "");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "iterations_per_frame", PROPERTY_HINT_RANGE, "1,50,1"),
                 "set_iterations_per_frame", "get_iterations_per_frame");

    ADD_GROUP("Display", "");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_wireframe"), "set_show_wireframe", "get_show_wireframe");
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "skin_color"), "set_skin_color", "get_skin_color");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "skin_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"),
                 "set_skin_material", "get_skin_material");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "wall_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"),
                 "set_wall_material", "get_wall_material");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "bed_material", PROPERTY_HINT_RESOURCE_TYPE, "Material"),
                 "set_bed_material", "get_bed_material");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "relief_mm", PROPERTY_HINT_RANGE, "0,5,0.1,suffix:mm"),
                 "set_relief_mm", "get_relief_mm");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_width_mm", PROPERTY_HINT_RANGE, "0,3,0.05,suffix:mm"),
                 "set_wall_width_mm", "get_wall_width_mm");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "heatmap", PROPERTY_HINT_ENUM,
                              "Off,Max principal stress,Von Mises stress,Area change,Min principal stress,"
                              "Excess skin (wrinkles)"),
                 "set_heatmap", "get_heatmap");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heatmap_range_kpa", PROPERTY_HINT_RANGE,
                              "1,2000,1,exp,suffix:kPa"),
                 "set_heatmap_range_kpa", "get_heatmap_range_kpa");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heatmap_relative"), "set_heatmap_relative", "get_heatmap_relative");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heatmap_auto_range"), "set_heatmap_auto_range",
                 "get_heatmap_auto_range");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heatmap_contours"), "set_heatmap_contours", "get_heatmap_contours");

    ADD_SIGNAL(MethodInfo("mesh_rebuilt"));   // new sheet (geometry changed)
    ADD_SIGNAL(MethodInfo("cuts_changed"));   // incision made, undone or cleared
    // A thread pulled beyond its strength broke (and the suture is gone).
    ADD_SIGNAL(MethodInfo("suture_snapped", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::VECTOR3, "a"),
                          PropertyInfo(Variant::VECTOR3, "b"), PropertyInfo(Variant::FLOAT, "tension_n"),
                          PropertyInfo(Variant::INT, "thread")));
}

void SkinSheet::_ready() {
    rebuild(false);
    set_process(true);  // ask Godot to call _process() every frame
}

void SkinSheet::_process(double /*p_delta*/) {
    skinsim::Simulation* s = sim();
    if (!s) return;
    // Measure solver time so the HUD can show whether we fit in the frame budget
    // (16.7 ms at 60 frames per second, shared with rendering).
    const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
    last_step_ = s->step(iterations_per_frame_);
    last_step_ms_ = static_cast<double>(Time::get_singleton()->get_ticks_usec() - t0) / 1000.0;
    if (last_step_.iterations > 0) update_render_mesh();  // only re-upload when something moved
    for (const skinsim::SutureBreak& b : s->take_suture_breaks())
        emit_signal("suture_snapped", b.id, to_global(to_godot(b.a)), to_global(to_godot(b.b)), b.tension, b.thread);
}

// ---------------------------------------------------------------------------
// Property setters
// ---------------------------------------------------------------------------
// While a scene loads, Godot calls setters before the node enters the tree;
// _ready() builds everything once afterwards, so skip work until then.

// A new width/height moves the border, so incisions may no longer fit:
// they are removed. A new element size only re-meshes the same incisions
// (handy for checking that results converge as the mesh is refined).
void SkinSheet::set_width(double p_mm) {
    width_ = p_mm;
    if (is_inside_tree()) rebuild(false);
}

void SkinSheet::set_height(double p_mm) {
    height_ = p_mm;
    if (is_inside_tree()) rebuild(false);
}

void SkinSheet::set_element_size(double p_mm) {
    element_size_ = p_mm;
    if (is_inside_tree()) rebuild(true);
}

void SkinSheet::set_shear_modulus_kpa(double p_kpa) {
    params_.shear_modulus = p_kpa / 1000.0;
    push_params();
}

void SkinSheet::set_stiffening(double p_b) {
    params_.stiffening = p_b;
    push_params();
}

void SkinSheet::set_thickness(double p_mm) {
    params_.thickness = p_mm;
    push_params();
}

void SkinSheet::set_tether_stiffness_kpa_per_mm(double p_value) {
    params_.tether_stiffness = p_value / 1000.0;
    push_params();
}

void SkinSheet::set_grab_radius(double p_mm) {
    grab_params_.radius = p_mm;
    push_params();
}

void SkinSheet::set_grab_stiffness(double p_n_per_mm) {
    grab_params_.stiffness = p_n_per_mm;
    push_params();
}

void SkinSheet::set_max_pull_force(double p_newton) {
    grab_params_.max_force = p_newton;
    push_params();
}

void SkinSheet::set_fibre_angle_deg(double p_deg) {
    params_.fibre_angle = p_deg;
    push_params();
}

void SkinSheet::set_fibre_stiffness_kpa(double p_kpa) {
    params_.fibre_stiffness = p_kpa / 1000.0;
    push_params();
}

void SkinSheet::set_fibre_nonlinearity(double p_k2) {
    params_.fibre_nonlinearity = p_k2;
    push_params();
}

void SkinSheet::set_fibre_dispersion(double p_kappa) {
    params_.fibre_dispersion = p_kappa;
    push_params();
}

void SkinSheet::set_prestretch_along_pct(double p_pct) {
    params_.prestretch_along = 1.0 + p_pct / 100.0;
    push_params();
}

void SkinSheet::set_prestretch_across_pct(double p_pct) {
    params_.prestretch_across = 1.0 + p_pct / 100.0;
    push_params();
}

void SkinSheet::set_undermined_tethering_pct(double p_pct) {
    params_.undermined_tether = p_pct / 100.0;
    push_params();
}

void SkinSheet::set_wrinkling_pct(double p_pct) {
    params_.wrinkling = p_pct / 100.0;  // the solver clamps it to 0..0.99
    push_params();
}

void SkinSheet::set_undermine_radius(double p_mm) {
    undermine_radius_ = p_mm > 0.1 ? p_mm : 0.1;
}

bool SkinSheet::undermine_at(const Vector3& p_world_point, bool p_new_stroke) {
    if (!sheet_ || !sheet_->undermine_at(world_to_sheet(p_world_point), undermine_radius_, p_new_stroke)) return false;
    update_render_mesh();  // show the painted area right away
    return true;
}

int SkinSheet::excise(const Vector3& p_world_point) {
    if (!sheet_) return 0;
    const int result = sheet_->excise(world_to_sheet(p_world_point));
    if (result == 1) topology_changed();
    return result;
}

int SkinSheet::excise_fusiform(const Vector3& p_tip_a, const Vector3& p_tip_b) {
    if (!sheet_) return 0;
    const Eigen::Vector2d a = world_to_sheet(p_tip_a), b = world_to_sheet(p_tip_b);
    const skinsim::Polyline outline = skinsim::fusiform_outline(a, b, ellipse_ratio_, tip_angle_deg_);
    if (outline.empty()) return 0;
    const int result = sheet_->excise_outline(outline, 0.5 * (a + b));
    if (result == 1) topology_changed();
    return result;
}

PackedVector3Array SkinSheet::get_fusiform_outline(const Vector3& p_tip_a, const Vector3& p_tip_b) const {
    PackedVector3Array out;
    for (const Eigen::Vector2d& p :
         skinsim::fusiform_outline(world_to_sheet(p_tip_a), world_to_sheet(p_tip_b), ellipse_ratio_, tip_angle_deg_))
        out.push_back(to_global(to_godot(p)));
    return out;
}

Vector2 SkinSheet::to_rest(const Vector3& p_world_point) const {
    const skinsim::Simulation* s = sim();
    skinsim::MaterialPoint m;
    if (!s || !s->material_point_at(world_to_sheet(p_world_point), m)) return Vector2(NAN, NAN);
    const Eigen::Vector2d X = s->rest_position_of(m);
    return Vector2(static_cast<real_t>(X.x()), static_cast<real_t>(X.y()));
}

Array SkinSheet::rest_to_world(const PackedVector2Array& p_rest_points) const {
    // Densely sampled and split at incision lips by the solver (marks.cpp):
    // the line follows the skin and is never drawn across a wound.
    Array pieces;
    const skinsim::Simulation* s = sim();
    if (!s) return pieces;
    skinsim::Polyline rest;
    auto map = [&]() {
        for (const skinsim::Polyline& p : s->map_rest_polyline(rest, kMarkStepMm)) {
            PackedVector3Array piece;
            piece.resize(static_cast<int64_t>(p.size()));
            for (size_t k = 0; k < p.size(); ++k)
                piece.set(static_cast<int64_t>(k), to_global(to_godot(p[k], 0.05f)));  // a hair above the skin
            pieces.push_back(piece);
        }
        rest.clear();
    };
    for (int i = 0; i < p_rest_points.size(); ++i) {
        const Vector2 X = p_rest_points[i];
        if (std::isfinite(X.x) && std::isfinite(X.y))
            rest.emplace_back(X.x, X.y);
        else
            map();  // a NaN point is a gap in the line
    }
    map();
    return pieces;
}

Dictionary SkinSheet::rest_point_now(const Vector2& p_rest_point) const {
    Dictionary out;
    const skinsim::Simulation* s = sim();
    Eigen::Vector2d now;
    if (!s || !s->current_position_at_rest(Eigen::Vector2d(p_rest_point.x, p_rest_point.y), now)) return out;
    out["world"] = to_global(to_godot(now, 0.05f));
    out["mm"] = Vector2(static_cast<real_t>(now.x()), static_cast<real_t>(now.y()));
    return out;
}

skinsim::FlapDesign SkinSheet::flap_design(const Vector3& p_start, const Vector3& p_end) const {
    const skinsim::Simulation* s = sim();
    if (!s) return {};
    skinsim::MaterialPoint m;
    const Eigen::Vector2d start = world_to_sheet(p_start), end = world_to_sheet(p_end);
    if (!s->material_point_at(start, m)) return {};
    // Designs live in rest (tissue) coordinates, like incisions and lesions.
    Eigen::Vector2d centre = s->rest_position_of(m);
    double diameter = lesion_diameter_ + 2.0 * flap_margin_;
    for (const skinsim::Sheet::Lesion& l : sheet_->lesions())
        if ((l.centre - centre).norm() <= l.radius) {  // pressed on a lesion: design around it
            centre = l.centre;
            diameter = 2.0 * l.radius + 2.0 * flap_margin_;
            break;
        }
    // The drag gives the direction (on deformed skin it is close enough).
    return skinsim::design_flap(static_cast<skinsim::FlapType>(flap_type_), centre, end - start, diameter, flap_shape_,
                                std::min(2.0, 0.25 * diameter));
}

Dictionary SkinSheet::get_flap_preview(const Vector3& p_start, const Vector3& p_end) const {
    Dictionary out;
    const skinsim::FlapDesign d = flap_design(p_start, p_end);
    out["valid"] = d.valid;
    if (!d.valid) return out;
    const skinsim::Simulation* s = sim();
    // Rest -> where that tissue is now (points over no skin are left out).
    auto now = [&](const skinsim::Polyline& poly) {
        PackedVector3Array pts;
        for (const Eigen::Vector2d& X : poly) {
            Eigen::Vector2d p;
            if (s->current_position_at_rest(X, p)) pts.push_back(to_global(to_godot(p)));
        }
        return pts;
    };
    auto all = [&](const std::vector<skinsim::Polyline>& polys) {
        Array a;
        for (const skinsim::Polyline& p : polys) a.push_back(now(p));
        return a;
    };
    out["type"] = static_cast<int>(d.type);
    out["defect"] = now(d.defect);
    out["flaps"] = all(d.flaps);
    out["bases"] = all(d.bases);
    out["burow"] = all(d.burow);
    out["incisions"] = all(d.incisions);
    Array keys;
    for (const auto& kp : d.key_sutures) keys.push_back(now({kp.first, kp.second}));
    out["key_sutures"] = keys;
    out["ratio"] = d.length_to_width;
    out["turn_deg"] = d.turn_deg;
    out["diameter_mm"] = d.defect_diameter;
    out["lengthening"] = d.lengthening;
    return out;
}

Dictionary SkinSheet::get_flap_design_rest(const Vector3& p_start, const Vector3& p_end) const {
    Dictionary out;
    const skinsim::FlapDesign d = flap_design(p_start, p_end);
    if (!d.valid) return out;
    auto rest = [](const skinsim::Polyline& poly) {
        PackedVector2Array pts;
        for (const Eigen::Vector2d& X : poly) pts.push_back(Vector2(static_cast<real_t>(X.x()), static_cast<real_t>(X.y())));
        return pts;
    };
    Array cuts, flaps, keys;
    if (!d.defect.empty()) cuts.push_back(rest(d.defect));
    for (const skinsim::Polyline& p : d.burow) cuts.push_back(rest(p));
    for (const skinsim::Polyline& p : d.incisions) cuts.push_back(rest(p));
    for (const skinsim::Polyline& p : d.flaps) flaps.push_back(rest(p));
    for (const auto& kp : d.key_sutures) keys.push_back(rest({kp.first, kp.second}));
    out["cuts"] = cuts;
    out["flaps"] = flaps;
    out["key_sutures"] = keys;
    return out;
}

int SkinSheet::make_flap(const Vector3& p_start, const Vector3& p_end) {
    if (!sheet_) return 0;
    const skinsim::FlapDesign d = flap_design(p_start, p_end);
    if (!d.valid) return 0;
    const int result = sheet_->make_flap(d, flap_undermine_margin_);
    if (result == 1) topology_changed();
    return result;
}

bool SkinSheet::undo_flap() {
    if (!sheet_ || !sheet_->undo_flap()) return false;
    topology_changed();
    return true;
}

Array SkinSheet::tie_key_sutures(double p_tightness) {
    Array out;
    if (!sheet_ || sheet_->flap_count() == 0) return out;
    for (int id : sheet_->tie_key_sutures(sheet_->flap_count() - 1, p_tightness, suture_thread_)) out.push_back(id);
    return out;
}

Array SkinSheet::get_flap_info() const {
    if (flaps_version_ != render_version_) {
        flaps_cache_ = compute_flap_info();
        flaps_version_ = render_version_;
    }
    return flaps_cache_;
}

Array SkinSheet::compute_flap_info() const {
    Array out;
    if (!sheet_) return out;
    for (int i = 0; i < sheet_->flap_count(); ++i) {
        const skinsim::FlapDesign& d = sheet_->flap(i);
        Dictionary f;
        f["type"] = static_cast<int>(d.type);
        f["ratio"] = d.length_to_width;
        f["turn_deg"] = d.turn_deg;
        f["diameter_mm"] = d.defect_diameter;
        f["lengthening"] = d.lengthening;
        f["base_stress_kpa"] = sheet_->flap_base_stress(i) * 1000.0;
        out.push_back(f);
    }
    return out;
}

bool SkinSheet::undo_excise() {
    if (!sheet_ || !sheet_->undo_excise()) return false;
    topology_changed();
    return true;
}

// Lesions are drawn by the looks' skin shaders (from get_lesion_info) and the
// overlay, not the mesh: changing them only invalidates the cached info.
// Their type (lesion_kind) is for drawing only; the solver doesn't need it.
bool SkinSheet::add_lesion(const Vector3& p_world_point) {
    ++render_version_;
    if (!sheet_) return false;
    lesion_kinds_.resize(sheet_->lesions().size(), 0);
    if (!sheet_->add_lesion(world_to_sheet(p_world_point), lesion_diameter_)) return false;
    lesion_kinds_.push_back(lesion_kind_);
    return true;
}

bool SkinSheet::undo_lesion() {
    ++render_version_;
    if (!sheet_ || !sheet_->undo_lesion()) return false;
    lesion_kinds_.resize(sheet_->lesions().size(), 0);
    return true;
}

void SkinSheet::clear_lesions() {
    ++render_version_;
    if (sheet_) sheet_->clear_lesions();
    lesion_kinds_.clear();
}

Array SkinSheet::get_lesion_info() const {
    if (lesions_version_ != render_version_) {
        lesions_cache_ = compute_lesion_info();
        lesions_version_ = render_version_;
    }
    return lesions_cache_;
}

Array SkinSheet::compute_lesion_info() const {
    Array out;
    const skinsim::Simulation* s = sim();
    if (!s) return out;
    const auto& lesions = sheet_->lesions();
    for (int i = 0; i < static_cast<int>(lesions.size()); ++i) {
        const skinsim::Sheet::Lesion& l = lesions[i];
        // The outline at rest, carried to where that tissue is now. Parts
        // that have been excised have no skin to sit on: they are left out.
        // map_rest_polyline also splits it where an incision crosses it.
        skinsim::Polyline circle;
        constexpr int kPoints = 48;
        for (int k = 0; k <= kPoints; ++k) {
            const double a = 2.0 * 3.14159265358979323846 * k / kPoints;
            circle.push_back(l.centre + l.radius * Eigen::Vector2d(std::cos(a), std::sin(a)));
        }
        Array pieces;
        for (const skinsim::Polyline& p : s->map_rest_polyline(circle, kMarkStepMm)) {
            PackedVector3Array piece;
            for (const Eigen::Vector2d& now : p) piece.push_back(to_global(to_godot(now)));
            pieces.push_back(piece);
        }
        const skinsim::Sheet::LesionStatus st = sheet_->lesion_status(i);
        Dictionary d;
        d["outline"] = pieces;
        d["centre"] = Vector2(static_cast<real_t>(l.centre.x()), static_cast<real_t>(l.centre.y()));  // rest mm
        d["radius"] = l.radius;
        d["kind"] = i < static_cast<int>(lesion_kinds_.size()) ? lesion_kinds_[i] : 0;
        d["excised"] = st.excised;
        d["complete"] = st.complete;
        d["margin_mm"] = st.margin;
        out.push_back(d);
    }
    return out;
}

bool SkinSheet::undo_undermine() {
    if (!sheet_ || !sheet_->undo_undermine()) return false;
    update_render_mesh();
    return true;
}

void SkinSheet::clear_undermining() {
    if (!sheet_) return;
    sheet_->clear_undermining();
    update_render_mesh();
}

void SkinSheet::set_pin_boundary(bool p_pin) {
    params_.pin_boundary = p_pin;
    push_params();
}

void SkinSheet::set_suture_stiffness(double p_n_per_mm) {
    suture_params_.stiffness = p_n_per_mm;
    push_params();
}

void SkinSheet::set_suture_strip_width(double p_mm) {
    suture_params_.strip_width = p_mm;
    push_params();
}

void SkinSheet::set_sutures_can_snap(bool p_on) {
    suture_params_.can_break = p_on;
    push_params();
}

void SkinSheet::set_suture_thread(int p_usp_size) {
    suture_thread_ = std::clamp(p_usp_size, 2, 7);
}

double SkinSheet::thread_strength(int p_usp_size) {
    return skinsim::thread_strength(p_usp_size);
}

void SkinSheet::set_lip_contact(bool p_on) {
    contact_params_.enabled = p_on;
    push_params();
    if (is_inside_tree()) update_render_mesh();  // lip colours
}

void SkinSheet::set_contact_stiffness(double p_n_per_mm3) {
    contact_params_.stiffness = p_n_per_mm3;
    push_params();
}

void SkinSheet::set_heatmap(int p_mode) {
    heatmap_ = (p_mode < HEATMAP_OFF || p_mode > HEATMAP_LAST) ? HEATMAP_OFF : p_mode;
    update_skin_material();
    if (is_inside_tree()) update_render_mesh();
}

void SkinSheet::set_heatmap_range_kpa(double p_kpa) {
    heatmap_range_kpa_ = p_kpa > 1e-3 ? p_kpa : 1e-3;
    if (is_inside_tree()) update_render_mesh();
}

void SkinSheet::set_heatmap_relative(bool p_on) {
    heatmap_relative_ = p_on;
    auto_range_kpa_ = 0.0;  // re-pick the scale for the new quantity
    if (is_inside_tree()) update_render_mesh();
}

void SkinSheet::set_heatmap_auto_range(bool p_on) {
    heatmap_auto_range_ = p_on;
    auto_range_kpa_ = 0.0;
    if (is_inside_tree()) update_render_mesh();
}

void SkinSheet::set_heatmap_contours(bool p_on) {
    heatmap_contours_ = p_on;
    ++render_version_;  // the legend's ticks follow the contours
    if (heat_material_.is_valid()) heat_material_->set_shader_parameter("line_strength", p_on ? 0.55 : 0.0);
}

bool SkinSheet::stress_map() const {
    return heatmap_ == HEATMAP_MAX_PRINCIPAL || heatmap_ == HEATMAP_MIN_PRINCIPAL || heatmap_ == HEATMAP_VON_MISES;
}

skinsim::Field SkinSheet::heat_field() const {
    return heatmap_ == HEATMAP_MAX_PRINCIPAL   ? skinsim::Field::MaxPrincipalStress
           : heatmap_ == HEATMAP_MIN_PRINCIPAL ? skinsim::Field::MinPrincipalStress
           : heatmap_ == HEATMAP_VON_MISES     ? skinsim::Field::VonMisesStress
           : heatmap_ == HEATMAP_EXCESS_SKIN   ? skinsim::Field::ExcessSkin
                                               : skinsim::Field::AreaChange;
}

double SkinSheet::get_heatmap_range_now() const { return stress_range() * 1000.0; }

double SkinSheet::stress_range() const {
    const double kpa = heatmap_auto_range_ && auto_range_kpa_ > 0.0 ? auto_range_kpa_ : heatmap_range_kpa_;
    return kpa / 1000.0;  // solver stresses are in MPa
}

// The automatic scale: a round number (1, 2, 5 x 10^k kPa) just above the
// 92nd percentile of the stress magnitude over the skin, so a few extreme
// spots (suture holes, flap corners) do not wash everything else out. It
// only changes when the stresses outgrow it or fall well below it, so the
// colours do not flicker while the skin settles.
void SkinSheet::update_auto_range(const Eigen::VectorXd& p_values) {
    if (!heatmap_auto_range_ || !stress_map() || p_values.size() == 0) return;
    std::vector<double> mag(p_values.size());
    for (Eigen::Index i = 0; i < p_values.size(); ++i) mag[i] = std::abs(p_values[i]) * 1000.0;  // kPa
    const size_t k = static_cast<size_t>(0.92 * (mag.size() - 1));
    std::nth_element(mag.begin(), mag.begin() + k, mag.end());
    const double level = std::max(mag[k], 5.0);  // floor: noise on intact skin stays pale
    const double decade = std::pow(10.0, std::floor(std::log10(level)));
    double nice = 10.0 * decade;
    for (double m : {1.0, 2.0, 5.0})
        if (m * decade >= level) {
            nice = m * decade;
            break;
        }
    if (auto_range_kpa_ <= 0.0 || nice > auto_range_kpa_ || nice < 0.4 * auto_range_kpa_) auto_range_kpa_ = nice;
}

void SkinSheet::update_skin_material() {
    if (skin_material_.is_null()) return;
    if (is_inside_tree()) update_render_mesh();  // skin colour lives in the vertex colours
    const bool heat = heatmap_ != HEATMAP_OFF;
    // The skin's colour always comes from the vertex colours (skin colour,
    // darker where undermined, or the heatmap). With a heatmap the colours
    // *are* the data: draw them unlit, so lighting can't change their meaning.
    skin_material_->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
    skin_material_->set_flag(BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR, true);
    skin_material_->set_shading_mode(heat ? BaseMaterial3D::SHADING_MODE_UNSHADED
                                          : BaseMaterial3D::SHADING_MODE_PER_PIXEL);
    skin_material_->set_albedo(Color(1, 1, 1));
    if (heat_material_.is_null()) return;
    // The ramp for this map, 256 steps: the shader samples it per pixel.
    Ref<Image> img = Image::create_empty(256, 1, false, Image::FORMAT_RGBA8);
    for (int i = 0; i < 256; ++i) {
        const double t = i / 255.0;
        const bool diverging = heatmap_ == HEATMAP_MAX_PRINCIPAL || heatmap_ == HEATMAP_MIN_PRINCIPAL ||
                               heatmap_ == HEATMAP_AREA_CHANGE;
        img->set_pixel(i, 0, diverging ? ramp(kDiverging, 5, t) : ramp(kSequential, 5, t));
    }
    const bool two_sided = heatmap_ == HEATMAP_MAX_PRINCIPAL || heatmap_ == HEATMAP_MIN_PRINCIPAL ||
                           heatmap_ == HEATMAP_AREA_CHANGE;
    heat_material_->set_shader_parameter("skip_line", two_sided ? 5.0 : -10.0);
    if (heat_ramp_.is_null())
        heat_ramp_ = ImageTexture::create_from_image(img);
    else
        heat_ramp_->update(img);
    heat_material_->set_shader_parameter("ramp", heat_ramp_);
}

void SkinSheet::set_iterations_per_frame(int p_iterations) {
    iterations_per_frame_ = p_iterations < 1 ? 1 : p_iterations;
}

void SkinSheet::set_show_wireframe(bool p_show) {
    show_wireframe_ = p_show;
    if (is_inside_tree()) update_render_mesh();
}

void SkinSheet::set_skin_material(const Ref<Material>& p_material) {
    custom_skin_material_ = p_material;
    if (is_inside_tree()) update_render_mesh();
}

void SkinSheet::set_wall_material(const Ref<Material>& p_material) {
    custom_wall_material_ = p_material;
    if (is_inside_tree()) update_render_mesh();
}

void SkinSheet::set_bed_material(const Ref<Material>& p_material) {
    custom_bed_material_ = p_material;
    if (wound_bed_mesh_.is_valid())
        wound_bed_mesh_->set_material(p_material.is_valid() ? p_material : Ref<Material>(bed_material_));
}

void SkinSheet::set_relief_mm(double p_mm) {
    relief_mm_ = std::clamp(std::isfinite(p_mm) ? p_mm : 0.0, 0.0, 5.0);
    if (is_inside_tree()) update_render_mesh();
}

void SkinSheet::set_wall_width_mm(double p_mm) {
    wall_width_mm_ = std::clamp(std::isfinite(p_mm) ? p_mm : 0.0, 0.0, 3.0);
    if (is_inside_tree()) update_render_mesh();
}

// The lip edges, each oriented with its skin on the left, bound the wounds
// clockwise: the shoelace sum over them is minus the open area, without
// having to trace each wound's outline.
Dictionary SkinSheet::get_closure_metrics() const {
    Dictionary d;
    const skinsim::Simulation* s = sim();
    if (!s) return d;
    const Eigen::Map<const Eigen::Matrix2Xd> x = s->positions();
    double open = 0.0, defect = 0.0;
    for (size_t e = 0; e < mesh_.cut_edges.size(); ++e) {
        int a = mesh_.cut_edges[e][0], b = mesh_.cut_edges[e][1];
        const int t = e < cut_edge_third_.size() ? cut_edge_third_[e] : -1;
        if (t < 0) continue;
        const Eigen::Vector2d A = mesh_.rest_positions.col(a), B = mesh_.rest_positions.col(b),
                              T = mesh_.rest_positions.col(t);
        if ((B - A).x() * (T - A).y() - (B - A).y() * (T - A).x() < 0.0) std::swap(a, b);
        const Eigen::Vector2d ra = mesh_.rest_positions.col(a), rb = mesh_.rest_positions.col(b);
        defect -= 0.5 * (ra.x() * rb.y() - rb.x() * ra.y());
        open -= 0.5 * (x(0, a) * x(1, b) - x(0, b) * x(1, a));
    }
    d["open_area_mm2"] = std::max(0.0, open);
    d["defect_area_mm2"] = std::max(0.0, defect);
    auto percentile = [](Eigen::VectorXd v, double q) {
        if (v.size() == 0) return 0.0;
        std::vector<double> w(v.data(), v.data() + v.size());
        const size_t k = static_cast<size_t>(q * static_cast<double>(w.size() - 1));
        std::nth_element(w.begin(), w.begin() + static_cast<long>(k), w.end());
        return w[k];
    };
    const Eigen::VectorXd added = s->vertex_field(skinsim::Field::MaxPrincipalStress, true);
    d["peak_tension_kpa"] = 1000.0 * percentile(added, 0.98);
    // The closure's load on the skin as a whole: the added tension averaged
    // over the sheet (vertices weighted by their area), where it is pulled.
    // Unlike a peak, hardly affected by the stress round each needle hole.
    double sum = 0.0, area = 0.0;
    for (const skinsim::Triangle& t : mesh_.triangles) {
        const double a = 0.5 * std::abs((mesh_.rest_positions.col(t[1]) - mesh_.rest_positions.col(t[0])).x() *
                                            (mesh_.rest_positions.col(t[2]) - mesh_.rest_positions.col(t[0])).y() -
                                        (mesh_.rest_positions.col(t[1]) - mesh_.rest_positions.col(t[0])).y() *
                                            (mesh_.rest_positions.col(t[2]) - mesh_.rest_positions.col(t[0])).x());
        for (int v : t)
            if (v < added.size()) sum += a / 3.0 * std::max(0.0, added[v]);
        area += a;
    }
    d["mean_tension_kpa"] = area > 0.0 ? 1000.0 * sum / area : 0.0;
    const Eigen::VectorXd excess = s->vertex_field(skinsim::Field::ExcessSkin);
    // Dog-ears are areas; a few single nodes bunched at a suture's bite are
    // not: the 99th percentile ignores them, the maximum doesn't.
    d["max_excess"] = percentile(excess, 0.99);
    d["peak_excess"] = excess.size() > 0 ? excess.maxCoeff() : 0.0;
    return d;
}

Dictionary SkinSheet::get_wound_info() const {
    Dictionary d;
    d["since_cut_s"] = static_cast<double>(Time::get_singleton()->get_ticks_msec()) / 1000.0 - topology_time_s_;
    d["has_cuts"] = !mesh_.cut_edges.empty();
    return d;
}

// Distance from every vertex to the nearest incision lip, along mesh edges
// in the rest shape (Dijkstra from all lip vertices at once), and for each
// lip edge the triangle it borders. Only the topology matters, so this runs
// once per cut, not per frame.
void SkinSheet::update_lip_data() {
    constexpr double kCap = 30.0;  // mm: far enough for any shading
    const int n = mesh_.vertex_count();
    lip_distance_.assign(n, kCap);
    cut_edge_third_.assign(mesh_.cut_edges.size(), -1);
    bridge_mid_.clear();
    render_tris_ = mesh_.triangles;
    if (mesh_.cut_edges.empty()) return;
    std::vector<std::vector<std::pair<int, double>>> adj(n);
    for (const skinsim::Edge& e : edges_) {
        const double len = (mesh_.rest_positions.col(e[0]) - mesh_.rest_positions.col(e[1])).norm();
        adj[e[0]].push_back({e[1], len});
        adj[e[1]].push_back({e[0], len});
    }
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    for (const skinsim::Edge& e : mesh_.cut_edges)
        for (int v : e) {
            lip_distance_[v] = 0.0;
            queue.push({0.0, v});
        }
    while (!queue.empty()) {
        const auto [d, v] = queue.top();
        queue.pop();
        if (d > lip_distance_[v]) continue;
        for (const auto& [w, len] : adj[v])
            if (d + len < lip_distance_[w]) {
                lip_distance_[w] = d + len;
                queue.push({d + len, w});
            }
    }
    // Which triangle borders each lip edge (its third vertex says which side is skin).
    std::map<std::pair<int, int>, int> third;
    for (const skinsim::Triangle& t : mesh_.triangles)
        for (int k = 0; k < 3; ++k) {
            const int a = t[k], b = t[(k + 1) % 3];
            third[{std::min(a, b), std::max(a, b)}] = t[(k + 2) % 3];
        }
    for (size_t i = 0; i < mesh_.cut_edges.size(); ++i) {
        const auto it = third.find({mesh_.cut_edges[i][0], mesh_.cut_edges[i][1]});
        if (it != third.end()) cut_edge_third_[i] = it->second;
    }

    // Bridges: edges through the tissue (not lip edges) with both ends on a
    // lip. Their midpoint gets its true distance to the nearest incision
    // (straight line at rest; both lips of an incision share positions).
    std::set<std::pair<int, int>> lip_edges;
    for (const skinsim::Edge& e : mesh_.cut_edges) lip_edges.insert({e[0], e[1]});
    auto rest_dist_to_cut = [&](const Eigen::Vector2d& X) {
        double best = kCap;
        for (const skinsim::Edge& e : mesh_.cut_edges) {
            const Eigen::Vector2d A = mesh_.rest_positions.col(e[0]), AB = mesh_.rest_positions.col(e[1]) - A;
            const double len2 = AB.squaredNorm();
            const double t = len2 > 0.0 ? std::clamp((X - A).dot(AB) / len2, 0.0, 1.0) : 0.0;
            best = std::min(best, (A + t * AB - X).norm());
        }
        return best;
    };
    std::map<std::pair<int, int>, int> mid_of;  // edge -> vertex index of its midpoint
    for (const skinsim::Edge& e : edges_) {
        if (lip_distance_[e[0]] > 0.0 || lip_distance_[e[1]] > 0.0 || lip_edges.count({e[0], e[1]})) continue;
        const Eigen::Vector2d M = 0.5 * (mesh_.rest_positions.col(e[0]) + mesh_.rest_positions.col(e[1]));
        mid_of[{e[0], e[1]}] = n + static_cast<int>(bridge_mid_.size());
        bridge_mid_.push_back({e[0], e[1], rest_dist_to_cut(M)});
    }
    if (bridge_mid_.empty()) return;
    // Split the triangles beside them (counter-clockwise kept): one, two or
    // three of a triangle's edges may be bridges.
    render_tris_.clear();
    for (const skinsim::Triangle& t : mesh_.triangles) {
        int m[3];  // m[k]: midpoint of the edge from t[k] to t[k+1], or -1
        int count = 0;
        for (int k = 0; k < 3; ++k) {
            const int a = t[k], b = t[(k + 1) % 3];
            const auto it = mid_of.find({std::min(a, b), std::max(a, b)});
            m[k] = it != mid_of.end() ? it->second : -1;
            count += m[k] >= 0;
        }
        if (count == 0) {
            render_tris_.push_back(t);
        } else if (count == 3) {
            render_tris_.push_back({t[0], m[0], m[2]});
            render_tris_.push_back({m[0], t[1], m[1]});
            render_tris_.push_back({m[2], m[1], t[2]});
            render_tris_.push_back({m[0], m[1], m[2]});
        } else {
            // Rotate so that the edge t[k] -> t[k+1] is split (count 1), or
            // so that the edge t[k+1] -> t[k+2] is the one not split (count 2).
            int k = 0;
            if (count == 1)
                while (m[k] < 0) ++k;
            else
                while (m[(k + 1) % 3] >= 0) ++k;
            const int a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
            const int mab = m[k], mca = m[(k + 2) % 3];
            if (count == 1) {
                render_tris_.push_back({a, mab, c});
                render_tris_.push_back({mab, b, c});
            } else {
                render_tris_.push_back({a, mab, mca});
                render_tris_.push_back({mab, b, c});
                render_tris_.push_back({mab, c, mca});
            }
        }
    }
}

void SkinSheet::set_skin_color(const Color& p_color) {
    skin_color_ = p_color;
    update_skin_material();
}

void SkinSheet::push_params() {
    skinsim::Simulation* s = sim();
    if (!s) return;
    s->set_params(params_);
    s->set_grab_params(grab_params_);
    s->set_suture_params(suture_params_);
    s->set_contact_params(contact_params_);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

Eigen::Vector2d SkinSheet::world_to_sheet(const Vector3& p_world) const {
    // to_local undoes this node's position/rotation/scale, so the sheet can be
    // moved around the scene without breaking input.
    const Vector3 local = to_local(p_world);
    return Eigen::Vector2d(local.x, -local.z);
}

bool SkinSheet::grab(int p_id, const Vector3& p_world_point) {
    skinsim::Simulation* s = sim();
    return s && s->grab(p_id, world_to_sheet(p_world_point));
}

void SkinSheet::move_grab(int p_id, const Vector3& p_world_point) {
    if (skinsim::Simulation* s = sim()) s->move_grab(p_id, world_to_sheet(p_world_point));
}

void SkinSheet::release_grab(int p_id) {
    if (skinsim::Simulation* s = sim()) s->release_grab(p_id);
}

void SkinSheet::release_all_grabs() {
    if (skinsim::Simulation* s = sim()) s->release_all_grabs();
}

void SkinSheet::reset_simulation() {
    skinsim::Simulation* s = sim();
    if (!s) return;
    s->reset();
    update_render_mesh();
}

int SkinSheet::cut(const PackedVector3Array& p_world_path) {
    if (!sheet_) return 0;
    skinsim::Polyline path;
    path.reserve(p_world_path.size());
    for (int64_t i = 0; i < p_world_path.size(); ++i) path.push_back(world_to_sheet(p_world_path[i]));
    const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
    const int added = sheet_->cut(path);
    if (added > 0) {
        last_cut_ms_ = static_cast<double>(Time::get_singleton()->get_ticks_usec() - t0) / 1000.0;
        topology_changed();
    }
    return added;
}

bool SkinSheet::undo_cut() {
    if (!sheet_ || !sheet_->undo_cut()) return false;
    topology_changed();
    return true;
}

void SkinSheet::clear_cuts() {
    if (!sheet_ || sheet_->cuts().empty()) return;
    sheet_->clear_cuts();
    topology_changed();
}

// Tightness: 0 = the wound edges just meet; t shortens the loop by
// t x (bite a + bite b). The solver turns it into a tied length between the
// two bite strips (see Simulation::add_suture).
int SkinSheet::suture(const Vector3& p_entry, const Vector3& p_exit, double p_tightness) {
    skinsim::Simulation* s = sim();
    skinsim::MaterialPoint a, b;
    if (!s || !s->material_point_at(world_to_sheet(p_entry), a) || !s->material_point_at(world_to_sheet(p_exit), b))
        return -1;
    if ((s->position_of(a) - s->position_of(b)).norm() < 1e-6) return -1;  // both ends on the same spot
    return s->add_suture(a, b, p_tightness, suture_thread_);
}

bool SkinSheet::set_suture_thread_of(int p_id, int p_usp_size) {
    skinsim::Simulation* s = sim();
    return s && s->set_suture_thread(p_id, std::clamp(p_usp_size, 2, 7));
}

bool SkinSheet::set_suture_tightness(int p_id, double p_tightness) {
    skinsim::Simulation* s = sim();
    return s && s->set_suture_tightness(p_id, p_tightness);
}

bool SkinSheet::is_on_skin(const Vector3& p_world_point) const {
    const skinsim::Simulation* s = sim();
    skinsim::MaterialPoint m;
    return s && s->material_point_at(world_to_sheet(p_world_point), m);
}

int SkinSheet::pick_suture(const Vector3& p_world_point, double p_max_distance_mm) const {
    const skinsim::Simulation* s = sim();
    if (!s) return -1;
    const Eigen::Vector2d p = world_to_sheet(p_world_point);
    int best = -1;
    double best_d = p_max_distance_mm;
    for (const skinsim::SutureInfo& su : s->suture_info()) {
        const Eigen::Vector2d ab = su.b - su.a;
        const double len2 = ab.squaredNorm();
        const double t = len2 > 0.0 ? std::clamp((p - su.a).dot(ab) / len2, 0.0, 1.0) : 0.0;
        const double d = (su.a + t * ab - p).norm();
        if (d < best_d) {
            best_d = d;
            best = su.id;
        }
    }
    return best;
}

bool SkinSheet::remove_suture(int p_id) {
    skinsim::Simulation* s = sim();
    if (!s) return false;
    skinsim::SutureRecord record;
    if (s->suture_record(p_id, record)) removed_sutures_[p_id] = record;
    return s->remove_suture(p_id);
}

bool SkinSheet::restore_suture(int p_id) {
    skinsim::Simulation* s = sim();
    auto it = removed_sutures_.find(p_id);
    if (!s || it == removed_sutures_.end()) return false;
    const bool ok = s->restore_suture(it->second) >= 0;
    removed_sutures_.erase(it);
    return ok;
}

void SkinSheet::clear_sutures() {
    if (skinsim::Simulation* s = sim()) s->clear_sutures();
}

int SkinSheet::get_suture_count() const {
    const skinsim::Simulation* s = sim();
    return s ? s->suture_count() : 0;
}

Array SkinSheet::get_suture_info() const {
    Array out;
    const skinsim::Simulation* s = sim();
    if (!s) return out;
    for (const skinsim::SutureInfo& su : s->suture_info()) {
        Dictionary d;
        d["id"] = su.id;
        d["a"] = to_global(to_godot(su.a));
        d["b"] = to_global(to_godot(su.b));
        d["force"] = su.force;
        d["tightness"] = su.tightness;
        d["slack"] = su.force == 0.0;
        d["loop_stress_kpa"] = su.loop_stress * 1000.0;
        d["thread"] = su.thread;
        d["tension_n"] = su.tension;
        d["strength_n"] = su.strength;
        d["load"] = su.load;
        out.push_back(d);
    }
    return out;
}

Dictionary SkinSheet::get_contact_info() const {
    Dictionary d;
    const skinsim::Simulation* s = sim();
    const skinsim::ContactInfo c = s ? s->contact_info() : skinsim::ContactInfo{};
    d["pairs"] = c.pairs;
    d["max_pressure_kpa"] = c.max_pressure * 1000.0;  // MPa -> kPa
    d["force_n"] = c.total_force;
    d["max_overlap_mm"] = c.max_overlap;
    return d;
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------

double SkinSheet::heat_t(double p_value) const {
    const double range = stress_range();
    switch (heatmap_) {
        case HEATMAP_MAX_PRINCIPAL:
        case HEATMAP_MIN_PRINCIPAL: return 0.5 + 0.5 * p_value / range;
        case HEATMAP_VON_MISES: return p_value / range;
        case HEATMAP_EXCESS_SKIN: return p_value / kExcessRange;
        case HEATMAP_AREA_CHANGE:
            // Log scale: halving and doubling are equally far from "unchanged".
            return 0.5 + 0.5 * std::log(p_value > 1e-9 ? p_value : 1e-9) / std::log(kAreaRange);
        default: return 0.0;
    }
}

Color SkinSheet::heat_color(double p_value) const {
    switch (heatmap_) {
        case HEATMAP_MAX_PRINCIPAL:
        case HEATMAP_MIN_PRINCIPAL:
        case HEATMAP_AREA_CHANGE: return ramp(kDiverging, 5, heat_t(p_value));
        case HEATMAP_VON_MISES:
        case HEATMAP_EXCESS_SKIN: return ramp(kSequential, 5, heat_t(p_value));
        default: return skin_color_;
    }
}

Dictionary SkinSheet::get_heatmap_legend() const {
    if (legend_version_ != render_version_) {
        legend_cache_ = compute_heatmap_legend();
        legend_version_ = render_version_;
    }
    return legend_cache_;
}

Dictionary SkinSheet::compute_heatmap_legend() const {
    Dictionary d;
    if (heatmap_ == HEATMAP_OFF || !sim()) return d;
    const double range = stress_range();
    double lo = 0.0, hi = range;
    String title, unit = " kPa";
    double scale = 1000.0;  // MPa -> kPa for display
    skinsim::Field field = skinsim::Field::VonMisesStress;
    if (heatmap_ == HEATMAP_MAX_PRINCIPAL) {
        title = heatmap_relative_ ? "Added tension" : "Max principal stress";
        field = skinsim::Field::MaxPrincipalStress;
        lo = -range;
    } else if (heatmap_ == HEATMAP_MIN_PRINCIPAL) {
        title = heatmap_relative_ ? "Added squeezing" : "Min principal stress";
        field = skinsim::Field::MinPrincipalStress;
        lo = -range;
    } else if (heatmap_ == HEATMAP_VON_MISES) {
        title = heatmap_relative_ ? "Added von Mises stress" : "Von Mises stress";
    } else if (heatmap_ == HEATMAP_EXCESS_SKIN) {
        title = "Excess skin (wrinkles)";
        field = skinsim::Field::ExcessSkin;
        hi = kExcessRange;
        unit = "%";
        scale = 100.0;
    } else {
        title = "Area change";
        field = skinsim::Field::AreaChange;
        lo = 1.0 / kAreaRange;
        hi = kAreaRange;
        unit = "x";
        scale = 1.0;
    }
    PackedColorArray colors;
    for (int i = 0; i < 32; ++i) {
        const double t = i / 31.0;
        // Sample along the legend's own axis (log for area change).
        const double v = (heatmap_ == HEATMAP_AREA_CHANGE) ? lo * std::pow(hi / lo, t) : lo + t * (hi - lo);
        colors.push_back(heat_color(v));
    }
    const Eigen::VectorXd values = sim()->vertex_field(field, heatmap_relative_);
    String extreme;
    if (values.size() > 0) {
        if (heatmap_ == HEATMAP_AREA_CHANGE)
            extreme = vformat("now %.2f-%.2f%s", values.minCoeff(), values.maxCoeff(), unit);
        else if (heatmap_ == HEATMAP_MIN_PRINCIPAL)  // what matters here is the most squeezed spot
            extreme = vformat("lowest %.1f%s", values.minCoeff() * scale, unit);
        else
            extreme = vformat("peak %.1f%s", values.maxCoeff() * scale, unit);
    }
    d["title"] = title;
    d["colors"] = colors;
    d["low"] = vformat(heatmap_ == HEATMAP_AREA_CHANGE ? "%.2f%s" : "%.0f%s", lo * scale, unit);
    d["high"] = vformat(heatmap_ == HEATMAP_AREA_CHANGE ? "%.2f%s" : "%.0f%s", hi * scale, unit);
    d["extreme"] = extreme;
    // Contour lines: every tenth of the scale (every fifth either side of 0
    // on the two-sided maps); the overlay draws matching ticks.
    d["ticks"] = heatmap_contours_ ? 10 : 0;
    d["auto"] = heatmap_auto_range_ && stress_map();
    return d;
}

String SkinSheet::get_solver_info() const {
    return String(skinsim::build_info().c_str());
}

Dictionary SkinSheet::get_step_info() const {
    // A Dictionary is Godot's equivalent of a Python dict: handy for passing a
    // bundle of values to GDScript without defining a new class.
    Dictionary d;
    d["iterations"] = last_step_.iterations;
    d["line_search_cuts"] = last_step_.line_search_cuts;
    d["projected"] = last_step_.projected;
    d["regularized"] = last_step_.regularized;
    d["converged"] = last_step_.converged;
    d["stalled"] = last_step_.stalled;
    d["energy"] = last_step_.energy;
    d["residual"] = last_step_.residual;
    d["step_ms"] = last_step_ms_;
    const skinsim::Simulation* s = sim();
    d["grabs"] = s ? s->grab_count() : 0;
    d["min_area_ratio"] = s ? s->min_area_ratio() : 1.0;
    d["max_area_ratio"] = s ? s->max_area_ratio() : 1.0;
    return d;
}

Array SkinSheet::get_grab_info() const {
    Array out;
    const skinsim::Simulation* s = sim();
    if (!s) return out;
    for (const skinsim::GrabInfo& g : s->grab_info()) {
        Dictionary d;
        d["id"] = g.id;
        // to_global: node-local -> world space, so the overlay can project it to the screen.
        d["point"] = to_global(to_godot(g.point));
        d["target"] = to_global(to_godot(g.target));
        d["force"] = g.force;
        d["force_limited"] = g.force_limited;
        d["radius"] = g.radius;
        out.push_back(d);
    }
    return out;
}

Dictionary SkinSheet::get_tension_lines(double p_spacing_mm) const {
    if (lines_version_ != render_version_ || lines_key_ != p_spacing_mm) {
        lines_cache_ = compute_tension_lines(p_spacing_mm);
        lines_version_ = render_version_;
        lines_key_ = p_spacing_mm;
    }
    return lines_cache_;
}

Dictionary SkinSheet::compute_tension_lines(double p_spacing_mm) const {
    Dictionary out;
    const skinsim::Simulation* s = sim();
    if (!s || !stress_map()) return out;
    const double spacing = std::clamp(p_spacing_mm, 1.0, 50.0);
    // One triangle per grid point of the rest shape: the strokes ride with
    // the tissue instead of swimming over it. Found again only after a cut.
    if (stroke_spacing_ != spacing || stroke_triangles_.empty()) {
        stroke_triangles_.clear();
        stroke_spacing_ = spacing;
        const skinsim::TriangleLocator locator(mesh_.rest_positions, mesh_.triangles);
        const double half_w = 0.5 * width_, half_h = 0.5 * height_;
        for (double y = -half_h + 0.5 * spacing; y < half_h; y += spacing)
            for (double x = -half_w + 0.5 * spacing; x < half_w; x += spacing) {
                const int t = locator.locate(Eigen::Vector2d(x, y), nullptr);
                if (t >= 0) stroke_triangles_.push_back(t);
            }
    }
    const std::vector<skinsim::PrincipalStress> p = s->triangle_principal(heatmap_relative_);
    const Eigen::Map<const Eigen::Matrix2Xd> x = s->positions();
    const double range = stress_range();
    const bool squeezing = heatmap_ == HEATMAP_MIN_PRINCIPAL;
    PackedVector3Array segments;
    PackedFloat32Array strength;
    for (int t : stroke_triangles_) {
        if (t >= static_cast<int>(p.size())) continue;
        // Tension: along the largest principal stress. Squeezing: the
        // smallest, at right angles to it.
        const double value = squeezing ? -p[t].s2 : p[t].s1;
        const double rel = value / range;
        if (rel < 0.08) continue;
        Eigen::Vector2d dir = squeezing ? Eigen::Vector2d(-p[t].dir1.y(), p[t].dir1.x()) : p[t].dir1;
        const skinsim::Triangle& tri = mesh_.triangles[t];
        const Eigen::Vector2d centre = (x.col(tri[0]) + x.col(tri[1]) + x.col(tri[2])) / 3.0;
        const double len = spacing * (0.3 + 0.55 * std::min(rel, 1.0));
        segments.push_back(to_global(to_godot(centre - 0.5 * len * dir, kWireLift)));
        segments.push_back(to_global(to_godot(centre + 0.5 * len * dir, kWireLift)));
        strength.push_back(static_cast<float>(rel));
    }
    out["segments"] = segments;
    out["strength"] = strength;
    return out;
}

Dictionary SkinSheet::sample_heatmap(const Vector3& p_world_point) const {
    Dictionary out;
    out["ok"] = false;
    const skinsim::Simulation* s = sim();
    if (!s || heatmap_ == HEATMAP_OFF) return out;
    const Eigen::Map<const Eigen::Matrix2Xd> x = s->positions();
    const skinsim::TriangleLocator locator(Eigen::Matrix2Xd(x), mesh_.triangles);
    const int t = locator.locate(world_to_sheet(p_world_point), nullptr);
    if (t < 0) return out;
    const double v = s->triangle_field(heat_field(), heatmap_relative_)[t];
    String text;
    if (heatmap_ == HEATMAP_AREA_CHANGE)
        text = vformat("%.2fx area", v);
    else if (heatmap_ == HEATMAP_EXCESS_SKIN)
        text = vformat("%.0f%% excess", v * 100.0);
    else
        text = vformat("%+.0f kPa", v * 1000.0);
    out["ok"] = true;
    out["text"] = text;
    out["value"] = v;
    return out;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void SkinSheet::ensure_render_objects() {
    if (mesh_instance_ != nullptr) return;

    render_mesh_.instantiate();

    skin_material_.instantiate();
    skin_material_->set_albedo(skin_color_);
    skin_material_->set_roughness(0.85);
    update_skin_material();

    // The heatmap shader. UV.x holds the value's position on the colour
    // scale (0..1; beyond = off the scale). Colours come from the ramp; the
    // contour lines are where UV.x * bands crosses a whole number, drawn a
    // constant ~1 px wide (fwidth: how fast it changes per pixel). They carry
    // on beyond the scale, so a saturated area still shows its gradient.
    Ref<Shader> shader;
    shader.instantiate();
    shader->set_code(R"(shader_type spatial;
render_mode unshaded;
uniform sampler2D ramp : source_color, filter_linear, repeat_disable;
uniform float bands = 10.0;
uniform float line_strength = 0.55;
uniform float skip_line = -10.0;  // the zero line of two-sided maps (it would trace noise)
varying float t;
void vertex() { t = UV.x; }
void fragment() {
    vec3 c = texture(ramp, vec2(clamp(t, 0.0, 1.0), 0.5)).rgb;
    float f = t * bands;
    float w = max(fwidth(f), 1e-4);
    float d = abs(fract(f + 0.5) - 0.5);
    float line = 1.0 - smoothstep(0.5 * w, 1.5 * w, d);
    line *= step(0.5, abs(f - skip_line));
    // Fade the lines where they would crowd into a smear.
    line *= clamp(1.5 - 4.0 * w, 0.0, 1.0);
    ALBEDO = mix(c, c * 0.35, line * line_strength);
}
)");
    heat_material_.instantiate();
    heat_material_->set_shader(shader);
    heat_material_->set_shader_parameter("line_strength", heatmap_contours_ ? 0.55 : 0.0);
    update_skin_material();

    wire_material_.instantiate();
    wire_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    wire_material_->set_albedo(Color(0.35, 0.2, 0.17));

    lip_material_.instantiate();
    lip_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    lip_material_->set_albedo(Color(1, 1, 1));
    lip_material_->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);  // colour per lip node
    lip_material_->set_cull_mode(BaseMaterial3D::CULL_DISABLED);  // cut walls face either way

    // Internal child: hidden from the Scene dock and never saved into the .tscn,
    // so saving the scene doesn't write the whole generated mesh into it.
    mesh_instance_ = memnew(MeshInstance3D);  // memnew = Godot's `new` for engine objects
    mesh_instance_->set_mesh(render_mesh_);
    add_child(mesh_instance_, false, INTERNAL_MODE_FRONT);

    // Wound bed: a flat dark-red layer just under the skin. Covered by the
    // skin everywhere, except where an incision has opened. PlaneMesh lies
    // in the XZ plane facing +Y, like the sheet.
    bed_material_.instantiate();
    bed_material_->set_albedo(Color(0.42, 0.08, 0.08));
    bed_material_->set_roughness(0.6);
    const Ref<Material> bed_material =
        custom_bed_material_.is_valid() ? custom_bed_material_ : Ref<Material>(bed_material_);
    wound_bed_mesh_.instantiate();
    wound_bed_mesh_->set_material(bed_material);
    wound_bed_ = memnew(MeshInstance3D);
    wound_bed_->set_mesh(wound_bed_mesh_);
    wound_bed_->set_position(Vector3(0, -kWoundBedDepth, 0));
    add_child(wound_bed_, false, INTERNAL_MODE_FRONT);
}

void SkinSheet::rebuild(bool p_keep_cuts) {
    ensure_render_objects();
    skinsim::CutGraph cuts;
    if (p_keep_cuts && sheet_) cuts = sheet_->cuts();
    const skinsim::SheetGeometry geometry{width_, height_, element_size_};
    sheet_ = std::make_unique<skinsim::Sheet>(geometry, cuts);  // the old sheet is deleted here
    push_params();
    last_step_ = skinsim::StepInfo{};
    // Same size as the sheet: it lies underneath, so it only shows through wounds.
    // Inset by the drape's glued margin (see main.gd _build_drape): if the
    // skin's edges are free, prestressed skin draws back from them, and the
    // bed must not show there (the drape does).
    const double inset = std::clamp(0.12 * std::min(width_, height_), 4.0, 15.0);
    wound_bed_mesh_->set_size(Vector2(static_cast<real_t>(std::max(1.0, width_ - 2.0 * inset)),
                                      static_cast<real_t>(std::max(1.0, height_ - 2.0 * inset))));
    topology_changed();
    emit_signal("mesh_rebuilt");
}

void SkinSheet::topology_changed() {
    mesh_ = sheet_->mesh();
    stroke_triangles_.clear();  // new triangles: sample them again
    edges_ = skinsim::unique_edges(mesh_);
    push_params();  // a cut creates a new Simulation; it copies params, but be explicit
    update_lip_data();
    topology_time_s_ = static_cast<double>(Time::get_singleton()->get_ticks_msec()) / 1000.0;
    update_render_mesh();
    emit_signal("cuts_changed");
}

void SkinSheet::update_render_mesh() {
    ++render_version_;  // cached answers (legend, lesions, ...) are now stale
    const skinsim::Simulation* s = sim();
    if (!s || render_mesh_.is_null()) return;

    // Rebuilding the surfaces from scratch each frame is the simplest correct
    // approach and is fast enough for a few thousand vertices. If profiling
    // ever shows otherwise, Godot can update just the vertex buffer in place.
    const Eigen::Map<const Eigen::Matrix2Xd> x = s->positions();
    const int n_verts = mesh_.vertex_count();
    const int n_tris = mesh_.triangle_count();

    // --- Surface 0: the skin itself --------------------------------------
    const bool heat = heatmap_ != HEATMAP_OFF;
    const bool custom = custom_skin_material_.is_valid() && !heat;
    if (lip_distance_.size() != static_cast<size_t>(n_verts)) update_lip_data();

    // Relief for shading: puckered skin (excess skin: dog-ears, wrinkles)
    // rises, and the lips round off into the wound. Drawn through the
    // normals only (the sheet stays flat, so nothing pokes through anything).
    Eigen::VectorXd excess;
    if (!heat || custom) excess = s->vertex_field(skinsim::Field::ExcessSkin);
    Eigen::VectorXd height = Eigen::VectorXd::Zero(n_verts);
    if (excess.size() == n_verts)
        for (int i = 0; i < n_verts; ++i)
            height[i] = relief_mm_ * std::clamp(excess[i] / kExcessRange, 0.0, 1.5) -
                        0.35 * relief_mm_ * std::exp(-lip_distance_[i] / 0.6);
    // Slope of that height per triangle, area-weighted onto the vertices.
    Eigen::Matrix2Xd slope = Eigen::Matrix2Xd::Zero(2, n_verts);
    Eigen::VectorXd slope_w = Eigen::VectorXd::Zero(n_verts);
    if (!heat && relief_mm_ > 0.0)
        for (const skinsim::Triangle& t : mesh_.triangles) {
            const Eigen::Vector2d p0 = x.col(t[0]), e1 = x.col(t[1]) - p0, e2 = x.col(t[2]) - p0;
            const double det = e1.x() * e2.y() - e1.y() * e2.x();
            if (std::abs(det) < 1e-12) continue;
            const double dh1 = height[t[1]] - height[t[0]], dh2 = height[t[2]] - height[t[0]];
            // Solve [e1 e2]^T g = (dh1, dh2) for the gradient g.
            const Eigen::Vector2d g((dh1 * e2.y() - dh2 * e1.y()) / det, (dh2 * e1.x() - dh1 * e2.x()) / det);
            const double area = 0.5 * std::abs(det);
            for (int v : t) {
                slope.col(v) += area * g;
                slope_w[v] += area;
            }
        }

    PackedVector3Array vertices;
    PackedVector3Array normals;
    vertices.resize(n_verts);
    normals.resize(n_verts);
    // ptrw() ("pointer, writable") gives direct access to the array's memory.
    Vector3* vw = vertices.ptrw();
    Vector3* nw = normals.ptrw();
    for (int i = 0; i < n_verts; ++i) {
        vw[i] = to_godot(x.col(i));
        // Height h(x, y) has normal (-hx, -hy, 1); in Godot's axes (x, up, -y)
        // that is (-hx, 1, hy).
        const Eigen::Vector2d g = slope_w[i] > 0.0 ? Eigen::Vector2d(slope.col(i) / slope_w[i]) : Eigen::Vector2d::Zero();
        nw[i] = Vector3(static_cast<real_t>(-g.x()), 1, static_cast<real_t>(g.y())).normalized();
    }

    // render_tris_: the mesh's triangles, those beside lip-to-lip edges split
    // (see update_lip_data); the extra midpoint vertices are added below.
    const int n_draw = static_cast<int>(render_tris_.size());
    PackedInt32Array indices;
    indices.resize(3 * n_draw);
    int32_t* iw = indices.ptrw();
    for (int t = 0; t < n_draw; ++t) {
        const skinsim::Triangle& tri = render_tris_[t];
        // Solver triangles are counter-clockwise; Godot's front faces are
        // clockwise as seen by the camera, so swap two indices.
        iw[3 * t + 0] = tri[0];
        iw[3 * t + 1] = tri[2];
        iw[3 * t + 2] = tri[1];
    }

    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = vertices;
    arrays[Mesh::ARRAY_NORMAL] = normals;
    arrays[Mesh::ARRAY_INDEX] = indices;

    const Eigen::VectorXd& und = s->undermining();
    if (heat) {
        // Heatmap: the value's position on the colour scale goes into the
        // UVs; the shader turns it into a colour (and contour lines) per pixel.
        const Eigen::VectorXd values = s->vertex_field(heat_field(), heatmap_relative_);
        update_auto_range(values);
        PackedVector2Array uvs;
        uvs.resize(n_verts);
        Vector2* uw = uvs.ptrw();
        for (int i = 0; i < n_verts; ++i) uw[i] = Vector2(static_cast<real_t>(heat_t(values[i])), 0);
        arrays[Mesh::ARRAY_TEX_UV] = uvs;
    } else if (custom) {
        // Data for the look's shader (see set_skin_material).
        const Eigen::VectorXd area = s->vertex_field(skinsim::Field::AreaChange);
        const std::vector<skinsim::PrincipalStress> p = s->triangle_principal(false);
        // Compression direction per vertex. A direction and its opposite are
        // the same, so average the doubled angle (cos 2a, sin 2a).
        Eigen::Matrix2Xd dir = Eigen::Matrix2Xd::Zero(2, n_verts);
        for (int t = 0; t < n_tris; ++t) {
            const Eigen::Vector2d d1 = p[t].dir1;  // tension; compression is at right angles
            const Eigen::Vector2d c2(d1.y() * d1.y() - d1.x() * d1.x(), -2.0 * d1.x() * d1.y());
            for (int v : mesh_.triangles[t]) dir.col(v) += c2;
        }
        PackedVector2Array uvs, uv2s;
        PackedColorArray colors;
        uvs.resize(n_verts);
        uv2s.resize(n_verts);
        colors.resize(n_verts);
        Vector2* uw = uvs.ptrw();
        Vector2* u2 = uv2s.ptrw();
        Color* cw = colors.ptrw();
        for (int i = 0; i < n_verts; ++i) {
            const Eigen::Vector2d r = mesh_.rest_positions.col(i);
            uw[i] = Vector2(static_cast<real_t>(r.x()), static_cast<real_t>(r.y()));
            u2[i] = Vector2(static_cast<real_t>(area[i]), static_cast<real_t>(lip_distance_[i]));
            Eigen::Vector2d d = dir.col(i);
            if (d.norm() > 1e-12) d.normalize();
            cw[i] = Color(static_cast<real_t>(i < und.size() ? und[i] : 0.0),
                          static_cast<real_t>(std::clamp(excess[i] / kExcessRange, 0.0, 1.0)),
                          static_cast<real_t>(0.5 + 0.5 * d.x()), static_cast<real_t>(0.5 + 0.5 * d.y()));
        }
        arrays[Mesh::ARRAY_TEX_UV] = uvs;
        arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
        arrays[Mesh::ARRAY_COLOR] = colors;
    } else {
        // Plain skin; undermined areas a duskier, darker shade so the
        // painted region is visible.
        const Color undermined = Color(0.58, 0.40, 0.44);
        PackedColorArray colors;
        colors.resize(n_verts);
        Color* cw = colors.ptrw();
        for (int i = 0; i < n_verts; ++i)
            cw[i] = skin_color_.lerp(undermined, static_cast<real_t>(0.75 * (i < und.size() ? und[i] : 0.0)));
        arrays[Mesh::ARRAY_COLOR] = colors;
    }

    // Midpoints of lip-to-lip edges: the average of their two ends, except
    // the lip distance (UV2.y), which is their own.
    if (!bridge_mid_.empty()) {
        for (int slot : {Mesh::ARRAY_VERTEX, Mesh::ARRAY_NORMAL}) {
            PackedVector3Array a = arrays[slot];
            for (const BridgeMid& m : bridge_mid_) {
                Vector3 v = (a[m.a] + a[m.b]) * 0.5f;
                a.push_back(slot == Mesh::ARRAY_NORMAL ? v.normalized() : v);
            }
            arrays[slot] = a;
        }
        for (int slot : {Mesh::ARRAY_TEX_UV, Mesh::ARRAY_TEX_UV2}) {
            if (arrays[slot].get_type() != Variant::PACKED_VECTOR2_ARRAY) continue;
            PackedVector2Array a = arrays[slot];
            for (const BridgeMid& m : bridge_mid_) {
                Vector2 v = (a[m.a] + a[m.b]) * 0.5f;
                if (slot == Mesh::ARRAY_TEX_UV2) v.y = static_cast<real_t>(m.dist);
                a.push_back(v);
            }
            arrays[slot] = a;
        }
        if (arrays[Mesh::ARRAY_COLOR].get_type() == Variant::PACKED_COLOR_ARRAY) {
            PackedColorArray a = arrays[Mesh::ARRAY_COLOR];
            for (const BridgeMid& m : bridge_mid_) a.push_back(a[m.a].lerp(a[m.b], 0.5f));
            arrays[Mesh::ARRAY_COLOR] = a;
        }
    }

    render_mesh_->clear_surfaces();
    render_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
    render_mesh_->surface_set_material(0, heat && heat_material_.is_valid() ? Ref<Material>(heat_material_)
                                          : custom                          ? custom_skin_material_
                                                                            : Ref<Material>(skin_material_));

    // --- Surface 1: wireframe ------------------------------------------------
    if (show_wireframe_) {
        PackedVector3Array line_vertices;
        line_vertices.resize(n_verts);
        Vector3* lw = line_vertices.ptrw();
        for (int i = 0; i < n_verts; ++i) lw[i] = to_godot(x.col(i), kWireLift);

        PackedInt32Array line_indices;
        line_indices.resize(2 * static_cast<int64_t>(edges_.size()));
        int32_t* liw = line_indices.ptrw();
        for (size_t e = 0; e < edges_.size(); ++e) {
            liw[2 * e + 0] = edges_[e][0];
            liw[2 * e + 1] = edges_[e][1];
        }

        Array line_arrays;
        line_arrays.resize(Mesh::ARRAY_MAX);
        line_arrays[Mesh::ARRAY_VERTEX] = line_vertices;
        line_arrays[Mesh::ARRAY_INDEX] = line_indices;
        render_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, line_arrays);
        render_mesh_->surface_set_material(render_mesh_->get_surface_count() - 1, wire_material_);
    }

    // --- Cut walls: the cut face of each lip, seen from above ---------------
    // Straight down, a cut wall would be invisible from the top-down camera.
    // Drawn sloping into the wound instead, it shows as a band as wide as
    // wall_width_mm: the layers of the skin (epidermis, dermis, fat), as a
    // surgeon sees them in a gaping wound. Where the lips meet, the band
    // slides under the other lip and is hidden. Colour: dark red, blue where
    // pressed against the opposite lip (lip contact), the deeper blue the
    // higher the pressure; alpha = that pressure for custom shaders.
    if (!mesh_.cut_edges.empty()) {
        const Eigen::VectorXd pressure = s->contact_info().node_pressure;  // MPa per vertex
        const Color free_lip(0.45, 0.02, 0.04), pressed_lip(0.1, 0.35, 1.0);
        const size_t n_cut = mesh_.cut_edges.size();
        // Outward direction of each lip edge (away from its own skin), and
        // smoothed per vertex so neighbouring quads join up.
        std::vector<Eigen::Vector2d> edge_out(n_cut, Eigen::Vector2d::Zero());
        std::map<int, Eigen::Vector2d> vertex_out;
        for (size_t e = 0; e < n_cut; ++e) {
            const int a = mesh_.cut_edges[e][0], b = mesh_.cut_edges[e][1];
            const Eigen::Vector2d d = x.col(b) - x.col(a);
            if (d.norm() < 1e-9) continue;
            Eigen::Vector2d o(d.y(), -d.x());
            o.normalize();
            const int t = e < cut_edge_third_.size() ? cut_edge_third_[e] : -1;
            if (t >= 0 && o.dot(x.col(t) - x.col(a)) > 0.0) o = -o;
            edge_out[e] = o;
            vertex_out[a] += o;
            vertex_out[b] += o;
        }
        const real_t depth = kWoundBedDepth * 0.8f;
        const double w = wall_width_mm_;
        PackedVector3Array wv, wn;
        PackedVector2Array wuv;
        PackedColorArray wc;
        PackedInt32Array wi;
        wv.resize(4 * n_cut);
        wn.resize(4 * n_cut);
        wuv.resize(4 * n_cut);
        wc.resize(4 * n_cut);
        wi.resize(6 * n_cut);
        for (size_t e = 0; e < n_cut; ++e) {
            const int ends[2] = {mesh_.cut_edges[e][0], mesh_.cut_edges[e][1]};
            const Eigen::Vector2d& o = edge_out[e];
            // Normal of a slope going down by `depth` over `w`: tilted towards the wound.
            const Vector3 n = Vector3(static_cast<real_t>(o.x() * depth), static_cast<real_t>(w),
                                      static_cast<real_t>(-o.y() * depth)).normalized();
            for (int k = 0; k < 2; ++k) {
                const int v = ends[k];
                Eigen::Vector2d vo = vertex_out[v];
                vo = vo.norm() > 0.3 ? Eigen::Vector2d(vo.normalized()) : o;  // (tip of a cut: both lips)
                const double s_along = mesh_.rest_positions(0, v) * 0.71 + mesh_.rest_positions(1, v) * 0.71;
                const double press = std::sqrt(std::clamp((v < pressure.size() ? pressure[v] : 0.0) / 0.3, 0.0, 1.0));
                Color col = free_lip.lerp(pressed_lip, static_cast<float>(press));
                col.a = static_cast<float>(press);
                const int top = static_cast<int>(4 * e) + 2 * k, bottom = top + 1;
                wv.set(top, to_godot(x.col(v), -0.02f));
                wv.set(bottom, to_godot(Eigen::Vector2d(x.col(v) + w * vo), -depth));
                wuv.set(top, Vector2(static_cast<real_t>(s_along), 0));
                wuv.set(bottom, Vector2(static_cast<real_t>(s_along), 1));
                wn.set(top, n);
                wn.set(bottom, n);
                wc.set(top, col);
                wc.set(bottom, col);
            }
            const int b0 = static_cast<int>(4 * e);
            // Two triangles facing the camera: seen from above (solver x, y)
            // a counter-clockwise triangle needs its last two indices swapped,
            // as for the skin. (Faces turned away would be lit from behind.)
            const Vector3 p0 = wv[b0], p1 = wv[b0 + 1], p3 = wv[b0 + 3];
            // Godot (x, z) = solver (x, -y): counter-clockwise in solver terms
            // is clockwise in (x, z).
            const real_t cross = (p1.x - p0.x) * (p3.z - p0.z) - (p1.z - p0.z) * (p3.x - p0.x);
            const bool ccw = cross < 0;
            const int quad[6] = {b0, ccw ? b0 + 3 : b0 + 1, ccw ? b0 + 1 : b0 + 3,
                                 b0, ccw ? b0 + 2 : b0 + 3, ccw ? b0 + 3 : b0 + 2};
            for (int k = 0; k < 6; ++k) wi.set(6 * e + k, quad[k]);
        }
        Array wall_arrays;
        wall_arrays.resize(Mesh::ARRAY_MAX);
        wall_arrays[Mesh::ARRAY_VERTEX] = wv;
        wall_arrays[Mesh::ARRAY_NORMAL] = wn;
        wall_arrays[Mesh::ARRAY_TEX_UV] = wuv;
        wall_arrays[Mesh::ARRAY_COLOR] = wc;
        wall_arrays[Mesh::ARRAY_INDEX] = wi;
        render_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, wall_arrays);
        render_mesh_->surface_set_material(render_mesh_->get_surface_count() - 1,
                                           custom_wall_material_.is_valid() ? custom_wall_material_
                                                                            : Ref<Material>(lip_material_));
    }
}

}  // namespace godot
