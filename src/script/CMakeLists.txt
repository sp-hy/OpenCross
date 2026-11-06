# Field script module: parser, disassembler and VM over the original room event bytecode.
add_library(oc_script STATIC
  field_opcodes.inc
  field_script.h
  field_script.cpp
  field_host.h
  field_host.cpp
  field_vm.h
  field_vm.cpp
  field_vm_ext.cpp
  field_vm_extension.cpp
  field_vm_ops_core.cpp
  field_vm_ops_game.cpp
  field_vm_ops_scene.cpp
  mod_ui.h
  mod_ui.cpp
)
target_link_libraries(oc_script PUBLIC oc_core)
