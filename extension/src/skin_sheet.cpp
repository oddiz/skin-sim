#include "skin_sheet.h"

#include <algorithm>
#include <cmath>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "skinsim/info.hpp"

namespace godot {

namespace {

// Lift the wireframe slightly above the sheet so the GPU (graphics processing
// unit) doesn't flicker between drawing lines and surface at the same depth
// ("z-fighting").
constexpr real_t kWireLift = 0.05f;  // mm
// Incision lips are drawn a little higher still, so they sit on top of the wireframe.
constexpr real_t kLipLift = 0.1f;  // mm
// The wound bed (the tissue under the skin, seen when a wound opens) lies
// this far below the sheet.
constexpr real_t kWoundBedDepth = 0.5f;  // mm

Vector3 to_godot(const Eigen::Vector2d& p, real_t lift = 0.0f) {
    return Vector3(static_cast<real_t>(p.x()), lift, static_cast<real_t>(-p.y()));
}

// Heatmap modes, matching the `heatmap` property's enum hint.
enum HeatmapMode { HEATMAP_OFF = 0, HEATMAP_MAX_PRINCIPAL, HEATMAP_VON_MISES, HEATMAP_AREA_CHANGE };

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
    ClassDB::bind_method(D_METHOD("set_heatmap", "mode"), &SkinSheet::set_heatmap);
    ClassDB::bind_method(D_METHOD("get_heatmap"), &SkinSheet::get_heatmap);
    ClassDB::bind_method(D_METHOD("set_heatmap_range_kpa", "kpa"), &SkinSheet::set_heatmap_range_kpa);
    ClassDB::bind_method(D_METHOD("get_heatmap_range_kpa"), &SkinSheet::get_heatmap_range_kpa);
    ClassDB::bind_method(D_METHOD("set_iterations_per_frame", "n"), &SkinSheet::set_iterations_per_frame);
    ClassDB::bind_method(D_METHOD("get_iterations_per_frame"), &SkinSheet::get_iterations_per_frame);
    ClassDB::bind_method(D_METHOD("set_show_wireframe", "show"), &SkinSheet::set_show_wireframe);
    ClassDB::bind_method(D_METHOD("get_show_wireframe"), &SkinSheet::get_show_wireframe);
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
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "undermine_radius", PROPERTY_HINT_RANGE, "1,30,0.5,suffix:mm"),
                 "set_undermine_radius", "get_undermine_radius");

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
    ADD_PROPERTY(PropertyInfo(Variant::INT, "heatmap", PROPERTY_HINT_ENUM,
                              "Off,Max principal stress,Von Mises stress,Area change"),
                 "set_heatmap", "get_heatmap");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heatmap_range_kpa", PROPERTY_HINT_RANGE,
                              "1,2000,1,exp,suffix:kPa"),
                 "set_heatmap_range_kpa", "get_heatmap_range_kpa");

    ADD_SIGNAL(MethodInfo("mesh_rebuilt"));   // new sheet (geometry changed)
    ADD_SIGNAL(MethodInfo("cuts_changed"));   // incision made, undone or cleared
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

void SkinSheet::set_undermine_radius(double p_mm) {
    undermine_radius_ = p_mm > 0.1 ? p_mm : 0.1;
}

bool SkinSheet::undermine_at(const Vector3& p_world_point, bool p_new_stroke) {
    if (!sheet_ || !sheet_->undermine_at(world_to_sheet(p_world_point), undermine_radius_, p_new_stroke)) return false;
    update_render_mesh();  // show the painted area right away
    return true;
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
    heatmap_ = (p_mode < HEATMAP_OFF || p_mode > HEATMAP_AREA_CHANGE) ? HEATMAP_OFF : p_mode;
    update_skin_material();
    if (is_inside_tree()) update_render_mesh();
}

void SkinSheet::set_heatmap_range_kpa(double p_kpa) {
    heatmap_range_kpa_ = p_kpa > 1e-3 ? p_kpa : 1e-3;
    if (is_inside_tree()) update_render_mesh();
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
}

void SkinSheet::set_iterations_per_frame(int p_iterations) {
    iterations_per_frame_ = p_iterations < 1 ? 1 : p_iterations;
}

void SkinSheet::set_show_wireframe(bool p_show) {
    show_wireframe_ = p_show;
    if (is_inside_tree()) update_render_mesh();
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
    return s->add_suture(a, b, p_tightness);
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
    return s && s->remove_suture(p_id);
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

Color SkinSheet::heat_color(double p_value) const {
    const double range = heatmap_range_kpa_ / 1000.0;  // solver stresses are in MPa
    switch (heatmap_) {
        case HEATMAP_MAX_PRINCIPAL: return ramp(kDiverging, 5, 0.5 + 0.5 * p_value / range);
        case HEATMAP_VON_MISES: return ramp(kSequential, 5, p_value / range);
        case HEATMAP_AREA_CHANGE:
            // Log scale: halving and doubling are equally far from "unchanged".
            return ramp(kDiverging, 5, 0.5 + 0.5 * std::log(p_value > 1e-9 ? p_value : 1e-9) / std::log(kAreaRange));
        default: return skin_color_;
    }
}

Dictionary SkinSheet::get_heatmap_legend() const {
    Dictionary d;
    if (heatmap_ == HEATMAP_OFF || !sim()) return d;
    const double range = heatmap_range_kpa_ / 1000.0;
    double lo = 0.0, hi = range;
    String title, unit = " kPa";
    double scale = 1000.0;  // MPa -> kPa for display
    skinsim::Field field = skinsim::Field::VonMisesStress;
    if (heatmap_ == HEATMAP_MAX_PRINCIPAL) {
        title = "Max principal stress";
        field = skinsim::Field::MaxPrincipalStress;
        lo = -range;
    } else if (heatmap_ == HEATMAP_VON_MISES) {
        title = "Von Mises stress";
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
    const Eigen::VectorXd values = sim()->vertex_field(field);
    String extreme;
    if (values.size() > 0) {
        if (heatmap_ == HEATMAP_AREA_CHANGE)
            extreme = vformat("now %.2f-%.2f%s", values.minCoeff(), values.maxCoeff(), unit);
        else
            extreme = vformat("peak %.1f%s", values.maxCoeff() * scale, unit);
    }
    d["title"] = title;
    d["colors"] = colors;
    d["low"] = vformat(heatmap_ == HEATMAP_AREA_CHANGE ? "%.2f%s" : "%.0f%s", lo * scale, unit);
    d["high"] = vformat(heatmap_ == HEATMAP_AREA_CHANGE ? "%.2f%s" : "%.0f%s", hi * scale, unit);
    d["extreme"] = extreme;
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

    wire_material_.instantiate();
    wire_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    wire_material_->set_albedo(Color(0.35, 0.2, 0.17));

    lip_material_.instantiate();
    lip_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    lip_material_->set_albedo(Color(1, 1, 1));
    lip_material_->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);  // colour per lip node

    // Internal child: hidden from the Scene dock and never saved into the .tscn,
    // so saving the scene doesn't write the whole generated mesh into it.
    mesh_instance_ = memnew(MeshInstance3D);  // memnew = Godot's `new` for engine objects
    mesh_instance_->set_mesh(render_mesh_);
    add_child(mesh_instance_, false, INTERNAL_MODE_FRONT);

    // Wound bed: a flat dark-red layer just under the skin. Covered by the
    // skin everywhere, except where an incision has opened. PlaneMesh lies
    // in the XZ plane facing +Y, like the sheet.
    Ref<StandardMaterial3D> bed_material;
    bed_material.instantiate();
    bed_material->set_albedo(Color(0.42, 0.08, 0.08));
    bed_material->set_roughness(0.6);
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
    wound_bed_mesh_->set_size(Vector2(static_cast<real_t>(width_), static_cast<real_t>(height_)));
    topology_changed();
    emit_signal("mesh_rebuilt");
}

void SkinSheet::topology_changed() {
    mesh_ = sheet_->mesh();
    edges_ = skinsim::unique_edges(mesh_);
    push_params();  // a cut creates a new Simulation; it copies params, but be explicit
    update_render_mesh();
    emit_signal("cuts_changed");
}

void SkinSheet::update_render_mesh() {
    const skinsim::Simulation* s = sim();
    if (!s || render_mesh_.is_null()) return;

    // Rebuilding the surfaces from scratch each frame is the simplest correct
    // approach and is fast enough for a few thousand vertices. If profiling
    // ever shows otherwise, Godot can update just the vertex buffer in place.
    const Eigen::Map<const Eigen::Matrix2Xd> x = s->positions();
    const int n_verts = mesh_.vertex_count();
    const int n_tris = mesh_.triangle_count();

    // --- Surface 0: the skin itself --------------------------------------
    PackedVector3Array vertices;
    PackedVector3Array normals;
    vertices.resize(n_verts);
    normals.resize(n_verts);
    // ptrw() ("pointer, writable") gives direct access to the array's memory.
    Vector3* vw = vertices.ptrw();
    Vector3* nw = normals.ptrw();
    for (int i = 0; i < n_verts; ++i) {
        vw[i] = to_godot(x.col(i));
        nw[i] = Vector3(0, 1, 0);  // the sheet stays flat: every normal points up
    }

    PackedInt32Array indices;
    indices.resize(3 * n_tris);
    int32_t* iw = indices.ptrw();
    for (int t = 0; t < n_tris; ++t) {
        const skinsim::Triangle& tri = mesh_.triangles[t];
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

    // Heatmap: one colour per vertex; the GPU blends them smoothly across
    // each triangle. Values come from the solver's vertex-averaged field.
    if (heatmap_ != HEATMAP_OFF) {
        const skinsim::Field field = heatmap_ == HEATMAP_MAX_PRINCIPAL ? skinsim::Field::MaxPrincipalStress
                                     : heatmap_ == HEATMAP_VON_MISES   ? skinsim::Field::VonMisesStress
                                                                       : skinsim::Field::AreaChange;
        const Eigen::VectorXd values = s->vertex_field(field);
        PackedColorArray colors;
        colors.resize(n_verts);
        Color* cw = colors.ptrw();
        for (int i = 0; i < n_verts; ++i) cw[i] = heat_color(values[i]);
        arrays[Mesh::ARRAY_COLOR] = colors;
    } else {
        // Plain skin; undermined areas a duskier, darker shade so the
        // painted region is visible.
        const Eigen::VectorXd& u = s->undermining();
        const Color undermined = Color(0.58, 0.40, 0.44);
        PackedColorArray colors;
        colors.resize(n_verts);
        Color* cw = colors.ptrw();
        for (int i = 0; i < n_verts; ++i)
            cw[i] = skin_color_.lerp(undermined, static_cast<real_t>(0.75 * (i < u.size() ? u[i] : 0.0)));
        arrays[Mesh::ARRAY_COLOR] = colors;
    }

    render_mesh_->clear_surfaces();
    render_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
    render_mesh_->surface_set_material(0, skin_material_);

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

    // --- Incision lips: always drawn, dark red; blue where pressed ----------
    // against the opposite lip (lip contact), the deeper blue the higher the
    // pressure.
    if (!mesh_.cut_edges.empty()) {
        const Eigen::VectorXd pressure = s->contact_info().node_pressure;  // MPa per vertex
        const Color free_lip(0.45, 0.02, 0.04), pressed_lip(0.1, 0.35, 1.0);
        auto lip_color = [&](int v) {
            // Full blue from ~300 kPa; the square root makes light contact visible too.
            const double s = std::sqrt(std::clamp(pressure[v] / 0.3, 0.0, 1.0));
            return free_lip.lerp(pressed_lip, static_cast<float>(s));
        };
        PackedVector3Array lip_vertices;
        PackedColorArray lip_colors;
        lip_vertices.resize(2 * static_cast<int64_t>(mesh_.cut_edges.size()));
        lip_colors.resize(lip_vertices.size());
        Vector3* lv = lip_vertices.ptrw();
        Color* lc = lip_colors.ptrw();
        for (size_t e = 0; e < mesh_.cut_edges.size(); ++e) {
            for (int k = 0; k < 2; ++k) {
                const int v = mesh_.cut_edges[e][k];
                lv[2 * e + k] = to_godot(x.col(v), kLipLift);
                lc[2 * e + k] = lip_color(v);
            }
        }
        Array lip_arrays;
        lip_arrays.resize(Mesh::ARRAY_MAX);
        lip_arrays[Mesh::ARRAY_VERTEX] = lip_vertices;
        lip_arrays[Mesh::ARRAY_COLOR] = lip_colors;
        render_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, lip_arrays);
        render_mesh_->surface_set_material(render_mesh_->get_surface_count() - 1, lip_material_);
    }
}

}  // namespace godot
