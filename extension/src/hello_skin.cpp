#include "hello_skin.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "skinsim/info.hpp"  // from the Godot-free solver library

namespace godot {

void HelloSkin::_bind_methods() {
    // D_METHOD gives the name GDScript will use; &HelloSkin::... is a pointer
    // to the C++ member function that Godot should call.
    ClassDB::bind_method(D_METHOD("get_solver_info"), &HelloSkin::get_solver_info);
}

String HelloSkin::get_solver_info() const {
    // std::string (C++ standard library) -> godot::String (Godot's own type).
    // The boundary between solver and Godot is where conversions like this live.
    return String(skinsim::build_info().c_str());
}

void HelloSkin::_ready() {
    // GDExtension classes run inside the editor too (unlike plain scripts,
    // which need @tool). That is why opening the scene prints this message.
    const bool in_editor = Engine::get_singleton()->is_editor_hint();
    UtilityFunctions::print("Hello from C++! [", in_editor ? "editor" : "game", "] ",
                            get_solver_info());
}

}  // namespace godot
