#include "skin_sheet.h"

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

Vector3 to_godot(const Eigen::Vector2d& p, real_t lift = 0.0f) {
    return Vector3(static_cast<real_t>(p.x()), lift, static_cast<real_t>(-p.y()));
}

}  // namespace

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
    ClassDB::bind_method(D_METHOD("set_pin_boundary", "pin"), &SkinSheet::set_pin_boundary);
    ClassDB::bind_method(D_METHOD("get_pin_boundary"), &SkinSheet::get_pin_boundary);
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

    ADD_GROUP("Grab", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grab_radius", PROPERTY_HINT_RANGE, "0,10,0.1,suffix:mm"),
                 "set_grab_radius", "get_grab_radius");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grab_stiffness", PROPERTY_HINT_RANGE,
                              "0.1,100,0.1,exp,suffix:N/mm"),
                 "set_grab_stiffness", "get_grab_stiffness");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_pull_force", PROPERTY_HINT_RANGE,
                              "0.1,50,0.1,exp,suffix:N"),
                 "set_max_pull_force", "get_max_pull_force");

    ADD_GROUP("Solver", "");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "iterations_per_frame", PROPERTY_HINT_RANGE, "1,50,1"),
                 "set_iterations_per_frame", "get_iterations_per_frame");

    ADD_GROUP("Display", "");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_wireframe"), "set_show_wireframe", "get_show_wireframe");
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "skin_color"), "set_skin_color", "get_skin_color");

    ADD_SIGNAL(MethodInfo("mesh_rebuilt"));
}

void SkinSheet::_ready() {
    rebuild();
    set_process(true);  // ask Godot to call _process() every frame
}

void SkinSheet::_process(double /*p_delta*/) {
    if (!sim_) return;
    // Measure solver time so the HUD can show whether we fit in the frame budget
    // (16.7 ms at 60 frames per second, shared with rendering).
    const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
    last_step_ = sim_->step(iterations_per_frame_);
    last_step_ms_ = static_cast<double>(Time::get_singleton()->get_ticks_usec() - t0) / 1000.0;
    if (last_step_.iterations > 0) update_render_mesh();  // only re-upload when something moved
}

// ---------------------------------------------------------------------------
// Property setters
// ---------------------------------------------------------------------------
// While a scene loads, Godot calls setters before the node enters the tree;
// _ready() builds everything once afterwards, so skip work until then.

void SkinSheet::set_width(double p_mm) {
    width_ = p_mm;
    if (is_inside_tree()) rebuild();
}

void SkinSheet::set_height(double p_mm) {
    height_ = p_mm;
    if (is_inside_tree()) rebuild();
}

void SkinSheet::set_element_size(double p_mm) {
    element_size_ = p_mm;
    if (is_inside_tree()) rebuild();
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

void SkinSheet::set_pin_boundary(bool p_pin) {
    params_.pin_boundary = p_pin;
    push_params();
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
    if (skin_material_.is_valid()) skin_material_->set_albedo(skin_color_);
}

void SkinSheet::push_params() {
    if (!sim_) return;
    sim_->set_params(params_);
    sim_->set_grab_params(grab_params_);
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
    return sim_ && sim_->grab(p_id, world_to_sheet(p_world_point));
}

void SkinSheet::move_grab(int p_id, const Vector3& p_world_point) {
    if (sim_) sim_->move_grab(p_id, world_to_sheet(p_world_point));
}

void SkinSheet::release_grab(int p_id) {
    if (sim_) sim_->release_grab(p_id);
}

void SkinSheet::release_all_grabs() {
    if (sim_) sim_->release_all_grabs();
}

void SkinSheet::reset_simulation() {
    if (!sim_) return;
    sim_->reset();
    update_render_mesh();
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------

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
    d["grabs"] = sim_ ? sim_->grab_count() : 0;
    d["min_area_ratio"] = sim_ ? sim_->min_area_ratio() : 1.0;
    d["max_area_ratio"] = sim_ ? sim_->max_area_ratio() : 1.0;
    return d;
}

Array SkinSheet::get_grab_info() const {
    Array out;
    if (!sim_) return out;
    for (const skinsim::GrabInfo& g : sim_->grab_info()) {
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

    wire_material_.instantiate();
    wire_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    wire_material_->set_albedo(Color(0.35, 0.2, 0.17));

    // Internal child: hidden from the Scene dock and never saved into the .tscn,
    // so saving the scene doesn't write the whole generated mesh into it.
    mesh_instance_ = memnew(MeshInstance3D);  // memnew = Godot's `new` for engine objects
    mesh_instance_->set_mesh(render_mesh_);
    add_child(mesh_instance_, false, INTERNAL_MODE_FRONT);
}

void SkinSheet::rebuild() {
    ensure_render_objects();
    mesh_ = skinsim::make_rectangle_mesh(width_, height_, element_size_);
    edges_ = skinsim::unique_edges(mesh_);
    sim_ = std::make_unique<skinsim::Simulation>(mesh_);  // old simulation is deleted here
    push_params();
    last_step_ = skinsim::StepInfo{};
    update_render_mesh();
    emit_signal("mesh_rebuilt");
}

void SkinSheet::update_render_mesh() {
    if (!sim_ || render_mesh_.is_null()) return;

    // Rebuilding the surfaces from scratch each frame is the simplest correct
    // approach and is fast enough for a few thousand vertices. If profiling
    // ever shows otherwise, Godot can update just the vertex buffer in place.
    const Eigen::Map<const Eigen::Matrix2Xd> x = sim_->positions();
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
        render_mesh_->surface_set_material(1, wire_material_);
    }
}

}  // namespace godot
