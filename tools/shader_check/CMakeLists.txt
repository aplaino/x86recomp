# recomp_shader_check: compiled shaders through the bytecode translator
# and the Metal backend's compile path, offline (dev tool).
add_executable(recomp_shader_check main.mm)
set_source_files_properties(main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-std=c++20")
target_link_libraries(recomp_shader_check PRIVATE recomp_gfx recomp_hlsl "-framework Metal"
                      "-framework Foundation")
