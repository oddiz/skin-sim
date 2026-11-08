// Entry point of the GDExtension library.
//
// When Godot loads our DLL it looks up one exported C function by name (the
// "entry symbol", configured in game/bin/skinsim.gdextension). That function
// hands Godot two callbacks: one to register our classes on startup and one to
// clean up on shutdown.

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "skin_sheet.h"

using namespace godot;

// Godot initialises in levels (core, servers, scene, editor). Nodes belong to
// the SCENE level, so that's the only one we act on.
static void initialize_skinsim(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    GDREGISTER_CLASS(SkinSheet);
}

static void uninitialize_skinsim(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

// extern "C" turns off C++ "name mangling" (the compiler normally encodes
// argument types into symbol names), so Godot can find the function by its
// plain name. GDE_EXPORT marks it as visible outside the DLL.
extern "C" {
GDExtensionBool GDE_EXPORT skinsim_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
                                                GDExtensionClassLibraryPtr p_library,
                                                GDExtensionInitialization *r_initialization) {
    GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
    init_obj.register_initializer(initialize_skinsim);
    init_obj.register_terminator(uninitialize_skinsim);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}
