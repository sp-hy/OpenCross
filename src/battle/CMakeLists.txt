# Headless battle core. No rendering, audio or platform dependencies.
add_library(oc_battle STATIC
  rng.h
  rng.cpp
  rules.h
  rules.cpp
  battle.h
  battle.cpp
  versus.h
  versus.cpp
  pac_vm.h
  pac_vm.cpp
  pac_opcodes.inc
)
target_link_libraries(oc_battle PUBLIC oc_core oc_data)
