// SkinSheet: Godot node that runs and displays the skin simulation.
//
// Responsibilities (and nothing more, since physics stays in the solver):
//   1. hold the geometry and tissue settings the user edits (Inspector / UI),
//   2. own a skinsim::Sheet (mesh + incisions + simulation) and step it a few
//      Newton iterations per frame,
//   3. forward grabs and incisions (touch/mouse) to the solver, converting coordinates,
//   4. copy the current shape into a Godot ArrayMesh for rendering.
//
// Coordinate mapping, solver 2D (mm) -> Godot 3D (node-local):
//   solver (x, y)  ->  Godot (x, 0, -y)
// The sheet lies in Godot's horizontal XZ plane with +Y (Godot's "up") as its
// normal. The camera looks straight down with screen-up = -Z, so the solver's
// +y appears at the top of the screen. 1 Godot unit = 1 mm.
#pragma once

#include <memory>
#include <vector>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "skinsim/params.hpp"
#include "skinsim/sheet.hpp"

namespace godot {

class SkinSheet : public Node3D {
    GDCLASS(SkinSheet, Node3D)

public:
    void _ready() override;
    void _process(double p_delta) override;

    // --- Geometry (changing these rebuilds the mesh and resets the simulation;
    //     width/height also remove all incisions, element size keeps them) ---
    void set_width(double p_mm);
    double get_width() const { return width_; }
    void set_height(double p_mm);
    double get_height() const { return height_; }
    void set_element_size(double p_mm);
    double get_element_size() const { return element_size_; }

    // --- Tissue (changing these just makes the solver relax to a new equilibrium) ---
    // The Inspector uses kPa, which is friendlier for skin; the solver uses MPa.
    void set_shear_modulus_kpa(double p_kpa);
    double get_shear_modulus_kpa() const { return params_.shear_modulus * 1000.0; }
    void set_stiffening(double p_b);
    double get_stiffening() const { return params_.stiffening; }
    void set_thickness(double p_mm);
    double get_thickness() const { return params_.thickness; }
    void set_tether_stiffness_kpa_per_mm(double p_value);
    double get_tether_stiffness_kpa_per_mm() const { return params_.tether_stiffness * 1000.0; }
    void set_pin_boundary(bool p_pin);
    bool get_pin_boundary() const { return params_.pin_boundary; }

    // --- Collagen fibres along the RSTL, in-vivo prestretch, undermining ---
    void set_fibre_angle_deg(double p_deg);
    double get_fibre_angle_deg() const { return params_.fibre_angle; }
    void set_fibre_stiffness_kpa(double p_kpa);
    double get_fibre_stiffness_kpa() const { return params_.fibre_stiffness * 1000.0; }
    void set_fibre_nonlinearity(double p_k2);
    double get_fibre_nonlinearity() const { return params_.fibre_nonlinearity; }
    void set_fibre_dispersion(double p_kappa);
    double get_fibre_dispersion() const { return params_.fibre_dispersion; }
    // Prestretch in percent: 8 means the in-vivo skin is 8% longer than unloaded.
    void set_prestretch_along_pct(double p_pct);
    double get_prestretch_along_pct() const { return (params_.prestretch_along - 1.0) * 100.0; }
    void set_prestretch_across_pct(double p_pct);
    double get_prestretch_across_pct() const { return (params_.prestretch_across - 1.0) * 100.0; }
    void set_undermined_tethering_pct(double p_pct);
    double get_undermined_tethering_pct() const { return params_.undermined_tether * 100.0; }

    // --- Grab (the user's forceps) ---
    void set_grab_radius(double p_mm);
    double get_grab_radius() const { return grab_params_.radius; }
    void set_grab_stiffness(double p_n_per_mm);
    double get_grab_stiffness() const { return grab_params_.stiffness; }
    void set_max_pull_force(double p_newton);
    double get_max_pull_force() const { return grab_params_.max_force; }

    // --- Sutures ---
    void set_suture_stiffness(double p_n_per_mm);
    double get_suture_stiffness() const { return suture_params_.stiffness; }
    void set_suture_strip_width(double p_mm);
    double get_suture_strip_width() const { return suture_params_.strip_width; }

    // --- Lip contact (the lips of incisions press on each other instead of
    // passing through) ---
    void set_lip_contact(bool p_on);
    bool get_lip_contact() const { return contact_params_.enabled; }
    void set_contact_stiffness(double p_n_per_mm3);
    double get_contact_stiffness() const { return contact_params_.stiffness; }

    // --- Solver ---
    void set_iterations_per_frame(int p_iterations);
    int get_iterations_per_frame() const { return iterations_per_frame_; }

    // --- Display ---
    void set_show_wireframe(bool p_show);
    bool get_show_wireframe() const { return show_wireframe_; }
    void set_skin_color(const Color& p_color);
    Color get_skin_color() const { return skin_color_; }
    // Heatmap: 0 = off, 1 = max principal stress, 2 = von Mises stress,
    // 3 = area change. The range sets the colour scale for the stresses (kPa);
    // area change always spans 1/1.5 .. 1.5x.
    void set_heatmap(int p_mode);
    int get_heatmap() const { return heatmap_; }
    void set_heatmap_range_kpa(double p_kpa);
    double get_heatmap_range_kpa() const { return heatmap_range_kpa_; }

    // --- Interaction (positions in Godot world space) ---
    bool grab(int p_id, const Vector3& p_world_point);
    void move_grab(int p_id, const Vector3& p_world_point);
    void release_grab(int p_id);
    void release_all_grabs();
    void reset_simulation();  // undeformed, grabs released; incisions stay

    // --- Incisions ---
    // Cut along a path drawn on the skin (world-space points, on the skin as
    // it looks now). Returns the number of cut segments made; 0 = nothing cut.
    int cut(const PackedVector3Array& p_world_path);
    bool undo_cut();
    void clear_cuts();
    int get_cut_count() const { return sheet_ ? sheet_->cut_count() : 0; }

    // --- Undermining brush (world-space points) ---
    void set_undermine_radius(double p_mm);
    double get_undermine_radius() const { return undermine_radius_; }
    // Paint at a point; p_new_stroke starts a new undo step. False if not on skin.
    bool undermine_at(const Vector3& p_world_point, bool p_new_stroke);
    bool undo_undermine();
    void clear_undermining();
    int get_undermine_count() const { return sheet_ ? sheet_->undermine_count() : 0; }

    // --- Sutures (world-space points) ---
    // A thread from the entry point to the exit point (both on the skin), tied
    // with the given tightness: 0 = the wound edges just meet, 0.2 = the loop
    // is 20% shorter than that (squeezing the tissue in it), < 0 = loose.
    // Returns the suture id, or -1 if a point is not on the skin.
    int suture(const Vector3& p_entry, const Vector3& p_exit, double p_tightness);
    // Re-tie a suture with a new tightness (same meaning as above).
    bool set_suture_tightness(int p_id, double p_tightness);
    // True if there is skin under the point (not an open wound, not outside).
    bool is_on_skin(const Vector3& p_world_point) const;
    // The suture whose thread passes within p_max_distance_mm of the point, or -1.
    int pick_suture(const Vector3& p_world_point, double p_max_distance_mm) const;
    bool remove_suture(int p_id);
    void clear_sutures();
    int get_suture_count() const;
    // One Dictionary per suture: id, a (entry), b (exit) in world space,
    // force (N), tightness, slack (bool), loop_stress_kpa (stress along the
    // thread in the tissue the loop holds; negative = squeezed).
    Array get_suture_info() const;
    // Lips pressed together: pairs (lip nodes in contact), max_pressure_kpa,
    // force_n (total across the contact), max_overlap_mm.
    Dictionary get_contact_info() const;

    // --- Info for the HUD ---
    int get_vertex_count() const { return mesh_.vertex_count(); }
    int get_triangle_count() const { return mesh_.triangle_count(); }
    String get_solver_info() const;
    Dictionary get_step_info() const;
    double get_last_cut_ms() const { return last_cut_ms_; }
    // For drawing a colour bar: title, colors (PackedColorArray, low to high),
    // low/high labels, and the current extreme value as text.
    Dictionary get_heatmap_legend() const;
    // One Dictionary per active grab: point/target (world space), force (N),
    // force_limited (bool), radius (mm).
    Array get_grab_info() const;

protected:
    static void _bind_methods();

private:
    void rebuild(bool p_keep_cuts);  // new mesh + new simulation
    void topology_changed();         // after a cut: refresh caches, redraw, notify
    void push_params();              // hand params_/grab_params_ to the running simulation
    void update_render_mesh();       // copy current positions into the ArrayMesh
    void ensure_render_objects();
    Eigen::Vector2d world_to_sheet(const Vector3& p_world) const;
    Color heat_color(double p_value) const;  // colour scale of the current heatmap mode
    void update_skin_material();
    // The sheet's current simulation. Cutting replaces it, so never keep this
    // pointer across a cut: call sim() again.
    skinsim::Simulation* sim() const { return sheet_ ? &sheet_->simulation() : nullptr; }

    double width_ = 100.0;
    double height_ = 60.0;
    double element_size_ = 2.5;
    // The one struct of tissue parameters, starting from the app's defaults
    // (fibres and prestretch on; the solver's own defaults have them off).
    skinsim::TissueParams params_ = app_default_tissue();
    static skinsim::TissueParams app_default_tissue();
    skinsim::GrabParams grab_params_;   // how the user's finger acts
    skinsim::SutureParams suture_params_;
    skinsim::ContactParams contact_params_;
    int iterations_per_frame_ = 5;
    bool show_wireframe_ = true;
    Color skin_color_ = Color(0.85, 0.64, 0.53);
    int heatmap_ = 0;
    double undermine_radius_ = 8.0;  // mm
    double heatmap_range_kpa_ = 50.0;

    // Cached copies of the mesh topology: it only changes when cutting.
    skinsim::TriMesh mesh_;
    std::vector<skinsim::Edge> edges_;

    // std::unique_ptr owns a heap object and deletes it automatically when the
    // pointer is destroyed or reassigned: C++'s answer to "who frees this?".
    std::unique_ptr<skinsim::Sheet> sheet_;
    skinsim::StepInfo last_step_;
    double last_step_ms_ = 0.0;
    double last_cut_ms_ = 0.0;

    Ref<ArrayMesh> render_mesh_;
    Ref<StandardMaterial3D> skin_material_;
    Ref<StandardMaterial3D> wire_material_;
    Ref<StandardMaterial3D> lip_material_;
    Ref<PlaneMesh> wound_bed_mesh_;
    MeshInstance3D* mesh_instance_ = nullptr;  // our internal children; Godot frees them
    MeshInstance3D* wound_bed_ = nullptr;      // dark layer under the sheet, seen through open wounds
};

}  // namespace godot
