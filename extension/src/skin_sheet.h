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

    // --- Grab (the user's forceps) ---
    void set_grab_radius(double p_mm);
    double get_grab_radius() const { return grab_params_.radius; }
    void set_grab_stiffness(double p_n_per_mm);
    double get_grab_stiffness() const { return grab_params_.stiffness; }
    void set_max_pull_force(double p_newton);
    double get_max_pull_force() const { return grab_params_.max_force; }

    // --- Solver ---
    void set_iterations_per_frame(int p_iterations);
    int get_iterations_per_frame() const { return iterations_per_frame_; }

    // --- Display ---
    void set_show_wireframe(bool p_show);
    bool get_show_wireframe() const { return show_wireframe_; }
    void set_skin_color(const Color& p_color);
    Color get_skin_color() const { return skin_color_; }

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

    // --- Info for the HUD ---
    int get_vertex_count() const { return mesh_.vertex_count(); }
    int get_triangle_count() const { return mesh_.triangle_count(); }
    String get_solver_info() const;
    Dictionary get_step_info() const;
    double get_last_cut_ms() const { return last_cut_ms_; }
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
    // The sheet's current simulation. Cutting replaces it, so never keep this
    // pointer across a cut: call sim() again.
    skinsim::Simulation* sim() const { return sheet_ ? &sheet_->simulation() : nullptr; }

    double width_ = 100.0;
    double height_ = 60.0;
    double element_size_ = 2.5;
    skinsim::TissueParams params_;      // the one struct of tissue parameters
    skinsim::GrabParams grab_params_;   // how the user's finger acts
    int iterations_per_frame_ = 5;
    bool show_wireframe_ = true;
    Color skin_color_ = Color(0.85, 0.64, 0.53);

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
