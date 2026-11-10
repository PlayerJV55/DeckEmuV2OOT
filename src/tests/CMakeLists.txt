# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    video_core/virtual_range_cache.cpp
    video_core/framebuffer_limits.cpp
    video_core/spirv_min_max_denorms.cpp
    video_core/pipeline_cache_file.cpp
    video_core/build_priority.cpp
    video_core/shader_cache_v2_reader.cpp
    video_core/shader_object_pool.cpp
    video_core/spirv_inline_control.cpp
    video_core/vulkan_overlay_filter.cpp
    video_core/storage_write_extent.cpp
    common/bit_field.cpp
    common/bit_util.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/core_pinning.cpp
    common/event.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/multi_level_page_table.cpp
    common/nx2_profiles.cpp
    common/page_table.cpp
    common/param_package.cpp
    common/queue_lifetime.cpp
    common/range_map.cpp
    common/range_set_move.cpp
    common/range_set_subtract.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/sparse_large_vector.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    core/builtin_patches.cpp
    core/core_timing.cpp
    core/gpu_dirty_memory.cpp
    core/nso_compatibility.cpp
    core/nup_writer.cpp
    core/nup_read_bench.cpp
    core/nup_zstd_read.cpp
    core/internal_network/network.cpp
    core/nx2_gpu_memory_registry.cpp
    core/vfs_vector.cpp
    video_core/memory_tracker.cpp
    video_core/gpu_written_pages.cpp
    video_core/native_qmd.cpp
    video_core/fast_draw.cpp
    video_core/back_split.cpp
    video_core/h264_packet.cpp
    video_core/msaa_upload_copy.cpp
    video_core/descriptor_table_growth.cpp
    video_core/evicted_pointer_slots.cpp
    video_core/ampere_returns.cpp
    video_core/ampere_native_formats.cpp
    video_core/ampere_async_copy.cpp
    video_core/ampere_bindless_surfaces.cpp
    video_core/ampere_branch_modifiers.cpp
    video_core/ampere_compute_lowering.cpp
    video_core/ampere_cyberpunk.cpp
    video_core/ampere_program_scan.cpp
    video_core/descriptor_payload_growth.cpp
    video_core/ampere_f2i.cpp
    video_core/ampere_fast_fusion.cpp
    video_core/ampere_fast_fusion_logged.cpp
    video_core/ampere_geometry.cpp
    video_core/ampere_geometry_capture.cpp
    video_core/ampere_geometry_exit.cpp
    video_core/ampere_geometry_lowering.cpp
    video_core/ampere_guarded_ulea.cpp
    video_core/ampere_i2f.cpp
    video_core/ampere_ldc.cpp
    video_core/ampere_logged_numeric.cpp
    video_core/ampere_logged_textures.cpp
    video_core/ampere_lowering.cpp
    video_core/ampere_matrix.cpp
    video_core/ampere_ported_conversions.cpp
    video_core/ampere_ported_half_compare.cpp
    video_core/ampere_ported_textures.cpp
    video_core/ampere_ported_uniform_vote.cpp
    video_core/ampere_selevet_modes.cpp
    video_core/ampere_selevet_spills.cpp
    video_core/ampere_starfox_test10.cpp
    video_core/ampere_tennis_i2f.cpp
    video_core/ampere_tennis_tessellation.cpp
    video_core/ampere_trap_guards.cpp
    video_core/ampere_uniform_guards.cpp
    video_core/ampere_uniform_immediate_shift.cpp
    video_core/ampere_vote.cpp
    video_core/ampere_yoshi_compute.cpp
    video_core/ampere_yoshi_title.cpp
    video_core/fast_fusion_byte_store.cpp
    video_core/fast_fusion_surface_store.cpp
    video_core/global_address_tracking.cpp
    video_core/global_memory_coherence.cpp
    video_core/maxwell_numeric.cpp
    video_core/maxwell_permute_lod.cpp
    video_core/maxwell_shr.cpp
    video_core/mesh_shaders.cpp
    video_core/nvn_storage_descriptor.cpp
    video_core/nx2_clean_pages.cpp
    video_core/nx2_semaphore_regions.cpp
    video_core/nx2_written_pages.cpp
    video_core/oot_visual_workaround.cpp
    video_core/query_copy.cpp
    video_core/report_time.cpp
    video_core/selected_write_bound.cpp
    video_core/shader_attributes.cpp
    video_core/spirv_builtin_capabilities.cpp
    video_core/spirv_cooperative_matrix.cpp
    video_core/spirv_image_gather.cpp
    video_core/spirv_fragment_shuffle.cpp
    video_core/spirv_software_matrix.cpp
    video_core/texture_unswizzle.cpp
    video_core/storage_buffer_control_flow.cpp
    video_core/storage_buffer_pointer_tracking.cpp
    video_core/syncpoint_manager.cpp
    video_core/texture_depth_float.cpp
    video_core/texture_descriptor_bounds.cpp
    video_core/texture_handles.cpp
    video_core/texture_layout.cpp
    video_core/turing_macro_effects.cpp
    video_core/upload_order.cpp
    video_core/usage_tracker.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)

if (ENABLE_UPDATE_CHECKER)
    target_sources(tests PRIVATE common/update_checker.cpp ../frontend_common/update_checker.cpp)
    target_link_libraries(tests PRIVATE httplib::httplib nlohmann_json::nlohmann_json)
endif()

# Catch2 checks compare unsigned results with integer literals.
if (MSVC)
    target_compile_options(tests PRIVATE /wd4388 /wd4389)
endif()

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()
