// HelloSkin: the M0 "hello world" node.
//
// GDExtension lets a C++ class appear in Godot as if it were built in: you can
// add it to a scene from the "Add Node" dialog, and GDScript can call its
// methods. This layer stays thin: it forwards to the solver library and holds
// no physics.
#pragma once

#include <godot_cpp/classes/node.hpp>

namespace godot {

class HelloSkin : public Node {
    // GDCLASS is a godot-cpp macro that generates the boilerplate Godot needs
    // to know this class exists: its name, its parent class, and so on.
    GDCLASS(HelloSkin, Node)

protected:
    // Godot calls this once at registration so we can tell it which methods
    // and properties scripts are allowed to see.
    static void _bind_methods();

public:
    // Called by Godot when the node enters the scene tree. The leading
    // underscore marks a "virtual" callback that the engine invokes for us.
    // `override` asks the compiler to check the parent class really has it.
    void _ready() override;

    // Callable from GDScript as `$HelloSkin.get_solver_info()`.
    String get_solver_info() const;
};

}  // namespace godot
