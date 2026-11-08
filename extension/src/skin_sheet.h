// SkinSheet: Godot node that displays the solver's skin mesh.
//
// Responsibilities (and nothing more, since physics stays in the solver):
//   1. hold the tissue/geometry settings the user edits in the Inspector,
//   2. ask the solver library for a mesh,
//   3. convert it into a Godot ArrayMesh for rendering.
//
// Coordinate mapping, solver 2D (mm) -> Godot 3D:
//   solver (x, y)  ->  Godot (x, 0, -y)
// The sheet lies in Godot's horizontal XZ plane with +Y (Godot's "up") as its
// normal. The camera looks straight down with screen-up = -Z, so the solver's
// +y appears at the top of the screen. 1 Godot unit = 1 mm.
#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>

#include "skinsim/mesh.hpp"

namespace godot {

class SkinSheet : public Node3D {
    GDCLASS(SkinSheet, Node3D)

public:
    void _ready() override;

    // Properties (shown in the Inspector, readable/writable from GDScript).
    void set_width(double p_mm);
    double get_width() const { return width_; }
    void set_height(double p_mm);
    double get_height() const { return height_; }
    void set_element_size(double p_mm);
    double get_element_size() const { return element_size_; }
    void set_show_wireframe(bool p_show);
    bool get_show_wireframe() const { return show_wireframe_; }
    void set_skin_color(const Color& p_color);
    Color get_skin_color() const { return skin_color_; }

    // Read-only info for the HUD.
    int get_vertex_count() const { return mesh_.vertex_count(); }
    int get_triangle_count() const { return mesh_.triangle_count(); }
    String get_solver_info() const;

protected:
    static void _bind_methods();

private:
    // Regenerate the solver mesh and rebuild the render mesh from it.
    void rebuild();
    // Create the internal MeshInstance3D and materials the first time they're needed.
    void ensure_render_objects();

    // Default sheet: 100 x 60 mm, 5 mm elements. The trailing underscore marks
    // member variables, a common C++ convention to tell them apart from locals.
    double width_ = 100.0;
    double height_ = 60.0;
    double element_size_ = 5.0;
    bool show_wireframe_ = true;
    Color skin_color_ = Color(0.85, 0.64, 0.53);

    skinsim::TriMesh mesh_;  // the solver's mesh (source of truth)

    // Ref<T> is Godot's reference-counted smart pointer: the object is freed
    // automatically when the last Ref to it goes away (like Python objects).
    Ref<ArrayMesh> render_mesh_;
    Ref<StandardMaterial3D> skin_material_;
    Ref<StandardMaterial3D> wire_material_;

    // A raw pointer is fine here: the MeshInstance3D is our child node, and
    // Godot frees children when the parent is freed.
    MeshInstance3D* mesh_instance_ = nullptr;
};

}  // namespace godot
