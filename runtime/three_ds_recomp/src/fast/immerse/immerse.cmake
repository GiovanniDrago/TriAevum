# iMMERSE module (LICENSE.md in this directory): iMMERSE MXAO by Pascal
# Gilcher, under the original iMMERSE licence, used under the author's explicit
# permission granted exclusively to TriAevum. Included by src/CMakeLists.txt
# only with THREE_DS_RECOMP_ENABLE_IMMERSE (which requires NRI); without it the
# GPL stand-in fast/oot3d/immerse_unavailable.cpp implements the interface.
target_sources(three_ds_recomp_runtime PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/mxao_shader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/mxao_pass.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/mxao_settings_panel.cpp")
triaevum_add_spirv_pack(three_ds_recomp_runtime
    "${CMAKE_CURRENT_LIST_DIR}/mxao_pass_binaries.cpp" "${CMAKE_CURRENT_LIST_DIR}/mxao_pass.spvpack")
target_compile_definitions(three_ds_recomp_runtime PRIVATE ENABLE_OOT3D_IMMERSE)
source_group("immerse" FILES
    "${CMAKE_CURRENT_LIST_DIR}/mxao_shader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/mxao_pass.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/mxao_settings_panel.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/mxao_pass_binaries.cpp")
