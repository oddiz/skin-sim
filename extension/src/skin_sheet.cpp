#include "skin_sheet.h"

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

// The coordinate mapping described in skin_sheet.h, in one place.
Vector3 to_godot(const Eigen::Vector2d& p, real_t lift = 0.0f) {
    return Vector3(static_cast<real_t>(p.x()), lift, static_cast<real_t>(-p.y()));
}

}  // namespace

void SkinSheet::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_width", "mm"), &SkinSheet::set_width);
    ClassDB::bind_method(D_METHOD("get_width"), &SkinSheet::get_width);
    ClassDB::bind_method(D_METHOD("set_height", "mm"), &SkinSheet::set_height);
    ClassDB::bind_method(D_METHOD("get_height"), &SkinSheet::get_height);
    ClassDB::bind_method(D_METHOD("set_element_size", "mm"), &SkinSheet::set_element_size);
    ClassDB::bind_method(D_METHOD("get_element_size"), &SkinSheet::get_element_size);
    ClassDB::bind_method(D_METHOD("set_show_wireframe", "show"), &SkinSheet::set_show_wireframe);
    ClassDB::bind_method(D_METHOD("get_show_wireframe"), &SkinSheet::get_show_wireframe);
    ClassDB::bind_method(D_METHOD("set_skin_color", "color"), &SkinSheet::set_skin_color);
    ClassDB::bind_method(D_METHOD("get_skin_color"), &SkinSheet::get_skin_color);
    ClassDB::bind_method(D_METHOD("get_vertex_count"), &SkinSheet::get_vertex_count);
    ClassDB::bind_method(D_METHOD("get_triangle_count"), &SkinSheet::get_triangle_count);
    ClassDB::bind_method(D_METHOD("get_solver_info"), &SkinSheet::get_solver_info);

    // ADD_PROPERTY ties a name to a setter/getter pair. The hint string
    // "min,max,step,suffix:mm" gives the Inspector a slider with units.
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "width", PROPERTY_HINT_RANGE, "5,300,0.5,suffix:mm"),
                 "set_width", "get_width");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "5,300,0.5,suffix:mm"),
                 "set_height", "get_height");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "element_size", PROPERTY_HINT_RANGE, "0.5,50,0.1,suffix:mm"),
                 "set_element_size", "get_element_size");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_wireframe"), "set_show_wireframe", "get_show_wireframe");
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "skin_color"), "set_skin_color", "get_skin_color");

    // Emitted after every rebuild so UI (e.g. the HUD) can refresh.
    ADD_SIGNAL(MethodInfo("mesh_rebuilt"));
}

void SkinSheet::_ready() {
    rebuild();
}

// Setters store the value and rebuild only if we're already in the scene. While a
// scene file is loading, Godot calls setters *before* the node enters the tree;
// rebuilding then would be wasted work, since _ready() will do it once anyway.
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

void SkinSheet::set_show_wireframe(bool p_show) {
    show_wireframe_ = p_show;
    if (is_inside_tree()) rebuild();
}

void SkinSheet::set_skin_color(const Color& p_color) {
    skin_color_ = p_color;
    if (skin_material_.is_valid()) skin_material_->set_albedo(skin_color_);
}

String SkinSheet::get_solver_info() const {
    return String(skinsim::build_info().c_str());
}

void SkinSheet::ensure_render_objects() {
    if (mesh_instance_ != nullptr) return;

    render_mesh_.instantiate();

    skin_material_.instantiate();
    skin_material_->set_albedo(skin_color_);
    skin_material_->set_roughness(0.85);

    wire_material_.instantiate();
    wire_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    wire_material_->set_albedo(Color(0.35, 0.2, 0.17));

    // The MeshInstance3D is added as an *internal* child: it is hidden from the
    // Scene dock and never saved into the .tscn file. Otherwise saving the scene
    // in the editor would write the entire generated mesh into main.tscn.
    mesh_instance_ = memnew(MeshInstance3D);  // memnew = Godot's `new` for engine objects
    mesh_instance_->set_mesh(render_mesh_);
    add_child(mesh_instance_, false, INTERNAL_MODE_FRONT);
}

void SkinSheet::rebuild() {
    ensure_render_objects();

    mesh_ = skinsim::make_rectangle_mesh(width_, height_, element_size_);
    const int n_verts = mesh_.vertex_count();
    const int n_tris = mesh_.triangle_count();

    // --- Surface 0: the skin itself --------------------------------------
    PackedVector3Array vertices;
    PackedVector3Array normals;
    vertices.resize(n_verts);
    normals.resize(n_verts);
    // ptrw() ("pointer, writable") gives direct access to the array's memory.
    // Much faster than calling set(i, ...) per element on big meshes.
    Vector3* vw = vertices.ptrw();
    Vector3* nw = normals.ptrw();
    for (int i = 0; i < n_verts; ++i) {
        vw[i] = to_godot(mesh_.rest_positions.col(i));
        nw[i] = Vector3(0, 1, 0);  // flat sheet: every normal points up
    }

    PackedInt32Array indices;
    indices.resize(3 * n_tris);
    int32_t* iw = indices.ptrw();
    for (int t = 0; t < n_tris; ++t) {
        const skinsim::Triangle& tri = mesh_.triangles[t];
        // The solver lists triangles counter-clockwise. Godot treats *clockwise*
        // (as seen by the camera) as the front face and by default skips drawing
        // back faces ("back-face culling"). Swapping two indices flips the order.
        iw[3 * t + 0] = tri[0];
        iw[3 * t + 1] = tri[2];
        iw[3 * t + 2] = tri[1];
    }

    // An ArrayMesh surface is described by an Array with one slot per kind of
    // data (positions, normals, colours, UVs, indices...). Unused slots stay null.
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = vertices;
    arrays[Mesh::ARRAY_NORMAL] = normals;
    arrays[Mesh::ARRAY_INDEX] = indices;

    render_mesh_->clear_surfaces();
    render_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
    render_mesh_->surface_set_material(0, skin_material_);

    // --- Surface 1: wireframe (each unique edge as a line segment) -----------
    if (show_wireframe_) {
        const std::vector<skinsim::Edge> edges = skinsim::unique_edges(mesh_);

        PackedVector3Array line_vertices;
        line_vertices.resize(n_verts);
        Vector3* lw = line_vertices.ptrw();
        for (int i = 0; i < n_verts; ++i) {
            lw[i] = to_godot(mesh_.rest_positions.col(i), kWireLift);
        }

        PackedInt32Array line_indices;
        line_indices.resize(2 * static_cast<int64_t>(edges.size()));
        int32_t* liw = line_indices.ptrw();
        for (size_t e = 0; e < edges.size(); ++e) {
            liw[2 * e + 0] = edges[e][0];
            liw[2 * e + 1] = edges[e][1];
        }

        Array line_arrays;
        line_arrays.resize(Mesh::ARRAY_MAX);
        line_arrays[Mesh::ARRAY_VERTEX] = line_vertices;
        line_arrays[Mesh::ARRAY_INDEX] = line_indices;
        render_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, line_arrays);
        render_mesh_->surface_set_material(1, wire_material_);
    }

    emit_signal("mesh_rebuilt");
}

}  // namespace godot
