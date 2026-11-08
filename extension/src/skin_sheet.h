// SkinSheet: Godot node that runs and displays the skin simulation.
//
// Responsibilities (and nothing more, since physics stays in the solver):
//   1. hold the geometry and tissue settings the user edits (Inspector / UI),
//   2. own a skinsim::Simulation and step it a few Newton iterations per frame,
//   3. forward grabs (touch/mouse) to the solver, converting coordinates,
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
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "skinsim/params.hpp"
#include "skinsim/simulation.hpp"

namespace godot {

class SkinSheet : public Node3D {
    GDCLASS(SkinSheet, Node3D)

public:
    void _ready() override;
    void _process(double p_delta) override;

    // --- Geometry (changing these rebuilds the mesh and resets the simulation) ---
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
    void set_thickness(double p_mm);
    double get_thickness() const { return params_.thickness; }
    void set_tether_stiffness_kpa_per_mm(double p_value);
    double get_tether_stiffness_kpa_per_mm() const { return params_.tether_stiffness * 1000.0; }
    void set_grab_stiffness(double p_n_per_mm);
    double get_grab_stiffness() const { return params_.grab_stiffness; }
    void set_pin_boundary(bool p_pin);
    bool get_pin_boundary() const { return params_.pin_boundary; }

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
    void reset_simulation();

    // --- Info for the HUD ---
    int get_vertex_count() const { return mesh_.vertex_count(); }
    int get_triangle_count() const { return mesh_.triangle_count(); }
    String get_solver_info() const;
    Dictionary get_step_info() const;

protected:
    static void _bind_methods();

private:
    void rebuild();               // new mesh + new simulation
    void push_params();           // hand params_ to the running simulation
    void update_render_mesh();    // copy current positions into the ArrayMesh
    void ensure_render_objects();
    Eigen::Vector2d world_to_sheet(const Vector3& p_world) const;

    double width_ = 100.0;
    double height_ = 60.0;
    double element_size_ = 5.0;
    skinsim::TissueParams params_;  // the one struct of tissue parameters
    int iterations_per_frame_ = 5;
    bool show_wireframe_ = true;
    Color skin_color_ = Color(0.85, 0.64, 0.53);

    skinsim::TriMesh mesh_;
    std::vector<skinsim::Edge> edges_;  // cached: topology doesn't change while dragging

    // std::unique_ptr owns a heap object and deletes it automatically when the
    // pointer is destroyed or reassigned: C++'s answer to "who frees this?".
    std::unique_ptr<skinsim::Simulation> sim_;
    skinsim::StepInfo last_step_;
    double last_step_ms_ = 0.0;

    Ref<ArrayMesh> render_mesh_;
    Ref<StandardMaterial3D> skin_material_;
    Ref<StandardMaterial3D> wire_material_;
    MeshInstance3D* mesh_instance_ = nullptr;  // our internal child; Godot frees it
};

}  // namespace godot
