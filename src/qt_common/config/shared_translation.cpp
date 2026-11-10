// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 Torzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shared_translation.h"

#include <map>
#include <memory>
#include <utility>
#include <QCoreApplication>
#include "common/assert.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "common/settings_setting.h"
#include "common/time_zone.h"
#include "qt_common/config/uisettings.h"

namespace ConfigurationShared {

std::unique_ptr<TranslationMap> InitializeTranslations(QObject* parent) {
    std::unique_ptr<TranslationMap> translations = std::make_unique<TranslationMap>();
    const auto& tr = [](const char* text) -> QString { return QCoreApplication::translate("ConfigurationShared", text); };

    // std::map::insert keeps the first entry, so a duplicate would silently lose
#define INSERT(SETTINGS, ID, NAME, TOOLTIP)                                                        \
    do {                                                                                           \
        [[maybe_unused]] const bool inserted_ =                                                    \
            translations                                                                           \
                ->insert(std::pair{SETTINGS::values.ID.Id(), std::pair{(NAME), (TOOLTIP)}})        \
                .second;                                                                           \
        DEBUG_ASSERT(inserted_);                                                                   \
    } while (0)

    // A setting can be ignored by giving it a blank name

    // Applets
    INSERT(Settings, cabinet_applet_mode, tr("Amiibo editor:"),
           tr("Chooses the amiibo settings screen games open; locked while a game runs.\nReal "
              "applet: the firmware's screen, or Nemyu's dialog if your firmware lacks "
              "it.\nCustom frontend: Nemyu's amiibo dialog."));
    INSERT(Settings, controller_applet_mode, tr("Controller configuration:"),
           tr("Chooses what opens when a game asks for controller setup; locked while a game "
              "runs.\nCustom frontend: Nemyu's controller dialog.\nReal applet: the firmware's "
              "screen, or Nemyu's dialog if your firmware lacks it.\nDisabled: no screen; Nemyu "
              "connects controller types the game accepts."));
    INSERT(Settings, data_erase_applet_mode, tr("Data erase:"),
           tr("Chooses the data erase screen a game can open.\nReal applet: the firmware's "
              "screen.\nCustom frontend: Nemyu has none, so the request closes at once, as it "
              "also does without firmware."));
    INSERT(Settings, error_applet_mode, tr("Error:"),
           tr("Chooses how a game's error messages appear; locked while a game runs.\nReal "
              "applet: the firmware's error screen, or Nemyu's message box if your firmware "
              "lacks it.\nCustom frontend: a Nemyu message box with the error code."));
    INSERT(Settings, net_connect_applet_mode, tr("Net connect:"),
           tr("Chooses the network connection screen a game can open.\nReal applet: the "
              "firmware's screen, or Nemyu's stand-in if your firmware lacks it.\nCustom "
              "frontend: a stand-in that never closes on its own, so the game may wait on it."));
    INSERT(Settings, player_select_applet_mode, tr("Player select:"),
           tr("Chooses the screen a game opens to pick a user profile; locked while a game "
              "runs.\nReal applet: the firmware's user picker, or Nemyu's selector if your "
              "firmware lacks it.\nCustom frontend: Nemyu's profile selector."));
    INSERT(Settings, swkbd_applet_mode, tr("Software keyboard:"),
           tr("Chooses the keyboard that opens when a game asks for text; locked while a game "
              "runs.\nCustom frontend: Nemyu's keyboard dialog, which also takes typing from "
              "your PC keyboard.\nReal applet: the firmware's on-screen keyboard, or Nemyu's if "
              "your firmware lacks it."));
    INSERT(Settings, mii_edit_applet_mode, tr("Mii Edit:"),
           tr("Chooses the Mii editor games open; locked while a game runs.\nReal applet: the "
              "firmware's Mii editor, or Nemyu's stand-in if your firmware lacks it.\nCustom "
              "frontend: no editor; a game asking for a new Mii gets a random one, and edits "
              "change nothing."));
    INSERT(Settings, web_applet_mode, tr("Online web:"),
           tr("Chooses how web pages a game opens are shown; locked while a game runs.\nCustom "
              "frontend: opens the link in your PC's default browser.\nReal applet: the "
              "firmware's browser, or Custom frontend if your firmware lacks it."));
    INSERT(Settings, shop_applet_mode, tr("Shop:"),
           tr("Chooses the shop screen a game can open.\nReal applet: the firmware's shop "
              "applet.\nCustom frontend: not implemented; the request closes at once."));
    INSERT(Settings, photo_viewer_applet_mode, tr("Photo viewer:"),
           tr("Chooses the album screen some games open to show your captures; locked while a "
              "game runs.\nReal applet: the firmware's album, or Nemyu's stand-in if your "
              "firmware lacks it.\nCustom frontend: no album; the request closes at once."));
    INSERT(Settings, offline_web_applet_mode, tr("Offline web:"),
           tr("Chooses how pages stored inside a game, like its manual, are shown; locked while "
              "a game runs.\nReal applet: the firmware's browser.\nCustom frontend: this build "
              "has no page viewer, so the page closes at once, as it also does when firmware "
              "lacks the applet."));
    INSERT(Settings, login_share_applet_mode, tr("Login share:"),
           tr("Chooses the account login or share screen a game can open.\nReal applet: the "
              "firmware's applet.\nCustom frontend: not implemented; the request closes at once."));
    INSERT(Settings, wifi_web_auth_applet_mode, tr("Wifi web auth:"),
           tr("Chooses the Wi-Fi sign-in page a game can open.\nReal applet: the firmware's "
              "applet.\nCustom frontend: not implemented; the request closes at once."));
    INSERT(Settings, my_page_applet_mode, tr("My page:"),
           tr("Chooses the My Page screen a game can open.\nReal applet: the firmware's "
              "applet.\nCustom frontend: Nemyu has none, so the request closes at once, as it "
              "also does without firmware."));
    INSERT(Settings, enable_overlay, tr("Enable overlay applet"),
           tr("Runs the firmware's overlay applet alongside the game. Hold Home on the Player 1 "
              "or handheld controller for about a second to open it. Needs installed firmware "
              "and takes effect at the next game boot."));

    // Audio
    INSERT(Settings, sink_id, tr("Audio backend:"),
           tr("Picks the sound library Nemyu plays through; locked while a game runs.\nauto: "
              "cubeb, or SDL3 if cubeb reports high latency.\ncubeb or sdl3: always use that "
              "one.\nnull: no sound at all."));
    INSERT(Settings, audio_output_device_id, tr("Output device:"),
           tr("Picks the speaker or headset for game sound; locked while a game runs. auto uses "
              "your system's default output. The list depends on the audio backend and goes "
              "back to auto when you change backends."));
    INSERT(Settings, audio_input_device_id, tr("Input Device:"),
           tr("Meant to pick the microphone games record from, but neither cubeb nor SDL3 uses "
              "it in this build: games always hear your system's default microphone. Only "
              "matters for games that use a microphone."));
    INSERT(Settings, audio_muted, tr("Mute audio"),
           tr("Silences all game sound. It is saved, so it stays on after a restart, and you "
              "can change it while a game runs."));
    INSERT(Settings, volume, tr("Volume:"),
           tr("Sets game volume from 0 to 200% and can change while a game runs. Past about "
              "105% the boost climbs steeply, to about 6.6 times at 200%, so loud sounds clip "
              "and distort."));
    INSERT(Settings, dump_audio_commands, QString(), QString());
    INSERT(UISettings, mute_when_in_background, tr("Mute when in background"),
           tr("Mutes game sound while you use another app and unmutes it when Nemyu is focused "
              "again. Only acts while a game runs."));

    // Core
    INSERT(Settings, use_multi_core, tr("Multicore CPU emulation"),
           tr("Runs each emulated CPU core on its own PC thread (six cores in this build). When "
              "off, one thread runs every core in turn, which is slower and meant only for "
              "debugging. Cannot change while a game runs."));
    INSERT(Settings, memory_layout_mode, tr("Console memory:"),
           tr("Sets how much RAM the emulated console has. Extra RAM mainly helps Switch 1 "
              "games with mods that need it, such as HD texture packs. Switch 2 games always "
              "get 12 GB, whatever is set here. Cannot change while a game runs."));
    INSERT(Settings, use_speed_limit, QString(), QString());
    INSERT(Settings, current_speed_mode, QString(), QString());
    INSERT(Settings, speed_limit, tr("Speed limit:"),
           tr("Caps emulation at this percent of console speed (100% is normal). Above 100%, "
              "games that tie gameplay to frames run faster; others may only draw more frames. "
              "Unchecked removes the cap; on Vulkan without frame generation, VSync also stops "
              "limiting it."));

    INSERT(Settings, turbo_speed_limit, tr("Turbo speed:"),
           tr("Speed used while Turbo mode is on, as a percent of console speed (200% is "
              "double). Turning Turbo on also enables the speed limit and, on Vulkan without "
              "frame generation, stops VSync from limiting the frame rate."));
    INSERT(Settings, slow_speed_limit, tr("Slow speed:"),
           tr("Speed used while Slow mode is on, as a percent of real console speed (50% is "
              "half). Turning Slow on also enables the speed limit."));

    INSERT(Settings, sync_core_speed, tr("Scale game clock with speed limit"),
           tr("Slows or speeds the clock games read by the active speed percent: half rate at "
              "200%, double at 50%. Meant to let a higher limit raise the frame rate without "
              "speeding up gameplay; results depend on how each game keeps time. Cannot change "
              "while a game runs."));

    // Cpu
    INSERT(Settings, cpu_accuracy, tr("CPU accuracy:"),
           tr("Sets how strictly the game's CPU code is translated.\nAuto: a small fixed set of "
              "speedups.\nAccurate: none of them.\nUnsafe: the speedups ticked "
              "below.\nParanoid: almost no optimizations, the slowest.\nDebugging: the toggles "
              "on Debug > CPU.\nCannot change while a game runs."));
    INSERT(Settings, cpu_backend, tr("CPU backend:"),
           tr("Chooses how game code runs.\nDynarmic: translates it for your CPU.\nNCE: runs it "
              "directly on ARM64 CPUs, falling back to Dynarmic when fastmem is off or the "
              "program can't use it.\nOnly shown on ARM64 Linux builds; cannot change while a "
              "game runs."));

    INSERT(Settings, cpu_clock, tr("CPU clock seen by games:"),
           tr("Makes the clock counter games read run 1.7x (Boost) or 2x (Overclock) faster "
              "than real time. Some games may then lift frame-rate caps; others may run timers "
              "too fast or misbehave. Changing it while a game runs makes that clock jump."));

    INSERT(Settings, use_custom_cpu_ticks, QString(), QString());
    INSERT(Settings, cpu_ticks, tr("Fixed CPU ticks (single-core only):"),
           tr("Advances the game's clock by this fixed number of ticks each time the emulated "
              "CPU reports work, instead of by the work it actually did. Only used when "
              "Multicore CPU emulation is off; in the default multicore mode it is ignored."));

    // Cpu Debug

    // Cpu Unsafe
    INSERT(Settings, cpuopt_unsafe_host_mmu, tr("Enable host MMU emulation (fastmem)"),
           tr("Lets the emulated CPU read and write game memory directly through your PC's "
              "memory hardware (fastmem). When unchecked, every access goes through a slower "
              "software lookup. Applies only with CPU accuracy set to Unsafe; locked while a "
              "game runs."));
    INSERT(Settings, cpuopt_unsafe_unfuse_fma,
           tr("Unfuse FMA (improve performance on CPUs without FMA)"),
           tr("Splits fused multiply-add math into a separate multiply and add on CPUs without "
              "FMA instructions, instead of using a slow exact routine. Faster, but results can "
              "differ slightly; CPUs with FMA ignore it. Applies only with CPU accuracy set to "
              "Unsafe; locked while a game runs."));
    INSERT(Settings, cpuopt_unsafe_reduce_fp_error, tr("Faster FRSQRTE and FRECPE"),
           tr("Computes the reciprocal and reciprocal square root estimate instructions "
              "(FRECPE, FRSQRTE) with your CPU's own fast approximations instead of the "
              "console's exact method. Faster, but results don't match real hardware exactly. "
              "Applies only with CPU accuracy set to Unsafe; locked while a game runs."));
    INSERT(Settings, cpuopt_unsafe_ignore_standard_fpcr,
           tr("Faster ASIMD instructions (32 bits only)"),
           tr("Runs 32-bit vector (ASIMD) floating-point math without the rounding mode it "
              "should use, which is faster but slightly inaccurate. Only 32-bit games use it; "
              "64-bit games ignore it. Applies only with CPU accuracy set to Unsafe."));
    INSERT(Settings, cpuopt_unsafe_inaccurate_nan, tr("Inaccurate NaN handling"),
           tr("Skips exact NaN handling in floating-point math, which is faster but gives wrong "
              "results for some special values. Applies only with CPU accuracy set to Unsafe."));
    INSERT(Settings, cpuopt_unsafe_fastmem_check, tr("Disable address space checks"),
           tr("Skips checking that each fast memory access by a 64-bit game stays inside its "
              "address space. Faster, but a bad address can then reach the emulator's own "
              "memory, which a malicious program could exploit to run code. Applies only with "
              "CPU accuracy set to Unsafe."));
    INSERT(Settings, cpuopt_unsafe_ignore_global_monitor, tr("Ignore global monitor"),
           tr("Skips the global monitor that syncs games' lock and atomic operations across "
              "emulated CPU cores, relying on compare-and-swap alone. Faster, but may cause "
              "deadlocks and other race conditions. Applies only with CPU accuracy set to "
              "Unsafe and host MMU emulation on; locked while a game runs."));

    // Renderer
    INSERT(Settings, renderer_backend, tr("Graphics API:"),
           tr("Selects the graphics API. Only Vulkan supports the mesh shaders some Switch 2 "
              "games use, plus frame generation, post-processing effects and frame pacing. "
              "OpenGL is a fallback (GLASM: NVIDIA only; SPIR-V: experimental); Null draws "
              "nothing. Locked while a game runs."));
    INSERT(Settings, vulkan_device, tr("GPU:"),
           tr("Picks which graphics card runs the game on PCs with more than one, such as "
              "laptops with integrated and dedicated GPUs. Shown only when the graphics API is "
              "Vulkan; locked while a game runs."));
    INSERT(Settings, resolution_setup, tr("Internal resolution:"),
           tr("Renders the game at a multiple of its normal resolution before scaling it to the "
              "window. Higher values look sharper but cost GPU time and video memory; values "
              "below 1X are faster, but they and the other [EXPERIMENTAL] steps can cause "
              "glitches. In Switch 2 games, images that compute shaders write stay at 1X, so the "
              "shown frame can stay at 1X while the cost rises; the status bar then says "
              "\"frame at 1x\". Locked while a game runs."));
    INSERT(Settings, scaling_filter, tr("Upscaling filter:"),
           tr("Chooses how the rendered image is resized to fit the window. AMD FSR 1 and "
              "Snapdragon GSR (Vulkan only) upscale along edges and sharpen, set by FSR/SGSR "
              "sharpness; the others range from blocky Nearest Neighbor (default) to smoother "
              "Bilinear, Bicubic or Lanczos."));
    INSERT(Settings, fsr_sharpening_slider, tr("FSR/SGSR sharpness:"),
           tr("Sets how strongly the AMD FSR 1 and Snapdragon GSR upscaling filters sharpen the "
              "image; a higher percentage is sharper. Other filters ignore it. Can change while "
              "a game runs."));
    INSERT(Settings, game_dlss_network, tr("Use the game's own DLSS"),
           tr("Runs a Switch 2 game's own DLSS (its upscaling network, translated for this GPU). "
              "Turned off, a plain upscale stands in for it, which costs much less GPU time but "
              "looks softer. The upscale also stands in while some of the network's passes "
              "can't run.\nOnly Fire Emblem: Fortune's Weave is known to use it. Takes effect "
              "when a game starts."));
    INSERT(Settings, game_dlss_sharpness, tr("RCAS sharpness:"),
           tr("Strength of RCAS (AMD's contrast-adaptive sharpening) on the upscale that stands "
              "in for a game's DLSS; 0% turns it off. The game's own DLSS is not sharpened. Can "
              "change while a game runs."));
    INSERT(Settings, nvidia_dlss, tr("Use NVIDIA DLSS in place of a game's DLSS (experimental)"),
           tr("Uses NVIDIA DLSS on GeForce RTX cards in place of a Switch 2 game's own DLSS, "
              "with the game's colour, depth and motion vectors. It acts in Fire Emblem: Fortune's "
              "Weave, and in The Legend of Zelda: Ocarina of Time at 1x resolution scale, where it "
              "replaces the game's own upscaler; in other games it does nothing and does not "
              "raise the resolution. Needs NVIDIA's Streamline "
              "files (sl.interposer.dll and its plugins) in the streamline folder of the data "
              "folder, and nvngx_dlss.dll there unless the NVIDIA app has downloaded it; Nemyu "
              "does not ship NVIDIA's files. When DLSS can't run, the plain upscale stands in.\n"
              "Takes effect when a game starts."));
    INSERT(Settings, anti_aliasing, tr("Anti-aliasing:"),
           tr("Smooths jagged edges on the finished game image, at the render resolution. SMAA "
              "gives the best quality; FXAA is lighter and can look steadier at low "
              "resolutions. Can change while a game runs."));
    INSERT(Settings, post_shader_chain, QString(), QString());
    INSERT(Settings, post_shader_preset, QString(), QString());
    INSERT(Settings, post_shader_enabled, tr("Post-processing effects (ReShade, Vulkan only)"),
           tr("Runs the ReShade .fx effects chosen under Configure Effects on every frame, "
              "after anti-aliasing and before the upscaling filter. Does nothing until effects "
              "are added. Vulkan only; can change while a game runs."));
    INSERT(Settings, fullscreen_mode, tr("Fullscreen mode:"),
           tr("Chooses how fullscreen (F11) works. Borderless Windowed covers the screen with a "
              "frameless window, so popups such as the on-screen keyboard appear over the game; "
              "Exclusive Fullscreen uses the system's fullscreen mode, where a popup can take "
              "the focus away. Wayland always uses Exclusive."));
    INSERT(Settings, aspect_ratio, tr("Aspect ratio:"),
           tr("Sets the shape of the picture in the window; the rest is filled with the "
              "background color. Most games draw a 16:9 picture, so other shapes stretch it "
              "unless a mod changes the game. Screenshots use this shape too. Can change while "
              "a game runs."));
    INSERT(Settings, use_disk_shader_cache, tr("Persistent pipeline cache"),
           tr("Saves the shaders a game builds to disk and loads them at the next boot, so they "
              "are not rebuilt during play. When off, every shader is built again each session, "
              "with more stutter. Locked while a game runs."));
    INSERT(Settings, nvdec_emulation, tr("Video decoding:"),
           tr("Decodes in-game videos on the CPU (default) or with the graphics card's video "
              "decoder, which falls back to the CPU when unavailable. No Video Output skips "
              "Switch 1 game videos, leaving them black; Switch 2 games still decode. Locked "
              "while a game runs."));
    INSERT(Settings, accelerate_astc, tr("ASTC decoding:"),
           tr("Chooses what decodes ASTC textures on GPUs without native ASTC support (most PC "
              "cards). GPU (default) uses compute shaders; CPU decodes while the game waits; "
              "CPU Asynchronous decodes in the background, so new textures show black or "
              "see-through briefly. Locked while a game runs."));
    INSERT(Settings, astc_recompression, tr("ASTC recompression:"),
           tr("On GPUs without native ASTC support, keeps ASTC textures uncompressed at full "
              "quality, or as BC3 (a quarter of the VRAM) or BC1 (an eighth) at lower quality. "
              "Switch 2 games get BC3 instead of Uncompressed on cards with 12 GB or less. "
              "Locked while a game runs."));
    INSERT(Settings, frame_pacing_mode, tr("Frame pacing (Vulkan only):"),
           tr("Caps how often Vulkan presents frames at 30, 60, 90 or 120 FPS, spaced evenly; "
              "Auto adds no cap. Applies only while the speed limit is on, and generated frames "
              "are not delayed by it. Can change while a game runs."));
    INSERT(Settings, vram_usage_mode, tr("VRAM usage (Vulkan only):"),
           tr("Sets how much video memory the emulator's caches plan for: Conservative at most "
              "6 GB plus 1 GB per X of internal resolution, Aggressive the driver's budget minus "
              "up to 1 GB (4 and 6 GB on integrated GPUs). Vulkan only; locked while a game "
              "runs."));
    INSERT(Settings, skip_cpu_inner_invalidation,
           tr("Skip cache invalidation on GPU writes (OpenGL only)"),
           tr("Skips refreshing the renderer's caches after memory writes made through the "
              "emulated GPU, saving some CPU time. Only OpenGL reads it; Vulkan always does "
              "this work, so there it has no effect. May cause stale graphics or soft-crashes."));
    INSERT(Settings, vsync_mode, tr("VSync:"),
           tr("FIFO (default) waits for each screen refresh: it never tears but is held to the "
              "refresh rate; Mailbox doesn't tear and can lag less but may drop frames; FIFO "
              "Relaxed and Immediate can tear. With the speed limit off or in Turbo, Vulkan "
              "uses Mailbox for FIFO."));
    INSERT(Settings, bg_red, QString(), QString());
    INSERT(Settings, bg_green, QString(), QString());
    INSERT(Settings, bg_blue, QString(), QString());

    INSERT(Settings, frame_gen, tr("Enable frame generation (Vulkan only)"),
           tr("Inserts frames interpolated between real ones so motion looks smoother. The game "
              "itself runs no faster, and each real frame is shown a little later, adding input "
              "lag. Vulkan only; can change while a game runs."));
    INSERT(Settings, frame_gen_type, tr("Method:"),
           tr("Chooses the interpolation method. Built-in uses the emulator's own optical-flow "
              "shaders and needs no extra files; Lossless Scaling (LSFG) runs the shaders from "
              "your own copy of Lossless.dll. If the chosen method can't run, no frames are "
              "generated. Locked while a game runs."));
    INSERT(Settings, frame_gen_multiplier, tr("Multiplier:"),
           tr("Sets how many frames are shown for each real one: 2 adds one generated frame, 4 "
              "adds three. Generated frames are dropped when showing them would hold back the "
              "real frame rate. Ignored when Target frame rate is above 0."));
    INSERT(Settings, frame_gen_target_rate, tr("Target frame rate:"),
           tr("Adds up to three generated frames per real frame, as needed, to approach this "
              "frame rate, and backs off when that slows the game. 0 turns it off and uses the "
              "fixed multiplier instead. Can change while a game runs."));
    INSERT(Settings, frame_gen_back_off, tr("Back off when the game slows"),
           tr("Pauses generation for a second after back-to-back long frames, and with Target "
              "frame rate, holds back a generated frame for up to a minute when adding it seemed "
              "to slow the game. Off, generation resumes at once and Target frame rate may always "
              "use up to three generated frames, still only as many as the target needs. Can "
              "change while a game runs."));
    INSERT(Settings, frame_gen_flow_scale_auto, tr("Automatic flow scale (LSFG)"),
           tr("Picks the resolution Lossless Scaling uses to estimate motion from how large the "
              "game renders compared with the window, between 25% and 100%. The built-in type "
              "ignores it. Locked while a game runs."));
    INSERT(Settings, frame_gen_flow_scale, tr("Flow scale (LSFG):"),
           tr("Sets the resolution Lossless Scaling uses to estimate motion, as a percent of "
              "the output size. Lower values cost less GPU time but track motion less "
              "precisely. Used only when Automatic flow scale is off; the built-in type ignores "
              "it."));
    INSERT(Settings, frame_gen_queue_target, tr("Frame queue:"),
           tr("Sets how many rounds of frames (a real frame plus its generated ones) may wait "
              "to be shown: 0 allows one, 1 two, 2 three, up to 7 frames in all. More rounds "
              "use more video memory and can add input lag. Locked while a game runs."));
    INSERT(Settings, frame_gen_dump_flow, QString(), QString());

    // Renderer (Advanced Graphics)
    INSERT(Settings, use_asynchronous_gpu_emulation, QString(), QString());

    INSERT(Settings, sync_memory_operations, tr("Wait on GPU sync points"),
           tr("Waits for the GPU to finish at each sync point a game marks in its command lists "
              "before reading further. Only waits while GPU accuracy is Accurate or GPU "
              "fence timing is Balanced or Accurate; each wait stalls the GPU thread."));
    INSERT(Settings, async_presentation, tr("Asynchronous presentation (Vulkan only)"),
           tr("Copies finished frames to the window and presents them on a separate CPU thread, "
              "so the render thread can move on to the next frame. Vulkan only. Cannot change "
              "while a game runs."));
    INSERT(Settings, renderer_force_max_clock, tr("Keep GPU at maximum clocks (Vulkan only)"),
           tr("Keeps a compute job running on the GPU while the game sends it work, so the "
              "driver doesn't lower its clock speed; the extra work draws more power. Vulkan "
              "only; skipped on the Steam Deck and untested drivers, such as macOS's. Locked "
              "while a game runs."));
    INSERT(Settings, max_anisotropy, tr("Anisotropic filtering:"),
           tr("Sharpens textures seen at steep angles by raising the game's own anisotropic "
              "filtering, up to 16x. Cannot change while a game runs.\nAutomatic: raises it "
              "with the resolution scale.\nGame Default: keeps the game's level.\n2x to 16x: "
              "multiplies the game's level."));
    INSERT(Settings, gpu_accuracy, tr("GPU accuracy:"),
           tr("Fast signals GPU work as finished early and skips copying GPU-rendered textures "
              "back to game memory. Accurate waits for the GPU and copies them back, which some "
              "games need but costs frame time. Switch 2 games run in Fast while this is left "
              "at its default."));
    INSERT(Settings, dma_accuracy, tr("DMA accuracy:"),
           tr("Controls how the command lists a game sends to the GPU are read. Safe first "
              "copies back data the GPU changed in that memory; Unsafe skips most of that work "
              "and is faster. Default follows GPU accuracy: Safe with Accurate, Unsafe with "
              "Fast."));
    INSERT(Settings, gpu_fence_behavior, tr("GPU fence timing:"),
           tr("Sets when a game is told its GPU work is done.\nImmediate: right away; "
              "fastest.\nBalanced: once the GPU finishes, and waits after GPU buffer "
              "copies.\nAccurate: Balanced plus extra buffer syncs; slowest.\nDefault: "
              "Immediate with GPU accuracy at Fast, waits for the GPU at Accurate."));
    INSERT(Settings, use_asynchronous_shaders, tr("Asynchronous shader compilation"),
           tr("When a draw needs a shader that is still compiling, skips the draw instead of "
              "waiting, so objects can be missing for a moment instead of the game stuttering. "
              "Tiny draws without depth, like full-screen passes, still wait. Cannot change "
              "while a game runs."));
    INSERT(Settings, gpu_clock, tr("GPU timing seen by games:"),
           tr("Divides the GPU timestamps a game reads by 256 (Boost) or 512 (Overclock), so "
              "its GPU work looks much faster. Games that lower resolution or detail when the "
              "GPU seems busy then stay at full quality. Normal reports real timing."));
    INSERT(Settings, gpu_unswizzle_enabled, tr("Unswizzle 3D textures on the GPU"),
           tr("Rearranges large BCn-compressed 3D textures out of the console's tiled layout "
              "with a GPU compute pass spread over several frames, instead of on the CPU all at "
              "once. Only games with such textures are affected. Cannot change while a game "
              "runs."));
    INSERT(Settings, gpu_unswizzle_texture_size, tr("Texture size threshold:"),
           tr("Sets how large a BCn 3D texture must be before GPU Unswizzle takes it; smaller "
              "ones stay on the CPU path. Only used while GPU Unswizzle is on. Cannot change "
              "while a game runs."));
    INSERT(Settings, gpu_unswizzle_stream_size, tr("Data per frame:"),
           tr("Sets how much of a queued texture is read from game memory each frame, from 4 to "
              "64 MB. Higher values finish textures in fewer frames but add more work to each "
              "one. Only used while GPU Unswizzle is on; cannot change while a game runs."));
    INSERT(Settings, gpu_unswizzle_chunk_size, tr("Slices per dispatch:"),
           tr("Sets how many depth slices of a texture go into one GPU dispatch, from 32 to "
              "512; at most one dispatch runs per frame. Larger batches mean fewer but longer "
              "dispatches. Only used while GPU Unswizzle is on; cannot change while a game runs."));
    INSERT(Settings, native_compute_write_mark, tr("Native compute write mark:"),
           tr("How much of a Switch 2 compute shader's storage buffer is treated as written by "
              "the GPU after each dispatch.\nAuto marks what the shader's stores can reach and "
              "falls back to Full when that cannot be proven.\nFull marks the whole binding (up to "
              "8 MiB), the original behaviour: it can delay the game's own writes to nearby memory "
              "(flickering effects) and copy far more data back from the GPU.\n64 KiB cap marks at "
              "most 64 KiB, which can miss data a large dispatch really wrote."));

    INSERT(Settings, use_vulkan_driver_pipeline_cache, tr("Vulkan driver pipeline cache"),
           tr("Saves the Vulkan driver's compiled pipelines to disk during play and loads them "
              "at the next boot, so they build faster. Ignored on NVIDIA's and AMD's "
              "proprietary drivers, which keep their own cache. Cannot change while a game runs."));
    INSERT(Settings, pipeline_preload, tr("Pipeline preload at boot (Vulkan only):"),
           tr("Which pipelines of the persistent pipeline cache are built while the game "
              "loads.\nUsed pipelines: those the last two sessions used; the others are built "
              "when first needed, which can stutter.\nAll cached pipelines: every one, for a "
              "longer boot and no such stutter. Each takes about 0.45 MB of driver memory and "
              "they are kept while the game runs; on Windows, building and keeping them stops "
              "while less than 8 GB of memory (RAM and page file) is free.\nVulkan only. Cannot "
              "change while a game runs."));
    INSERT(Settings, enable_compute_pipelines,
           tr("Compute pipelines on old Intel drivers (Vulkan only)"),
           tr("Runs compute shaders on the older Intel Windows Vulkan drivers known to crash "
              "with them; with it off they are skipped, and some games need them. Shown only "
              "when such a driver is detected. Cannot change while a game runs."));
    INSERT(Settings, use_reactive_flushing, tr("Reactive flushing"),
           tr("Catches CPU reads of memory the GPU caches and pulls in newer GPU data first, "
              "instead of predicting what to copy back. More accurate, but costs CPU time. "
              "Switch 2 games keep it off unless checked in that game's Properties. Locked "
              "while a game runs."));
    INSERT(Settings, use_video_framerate, tr("Normal speed during video playback"),
           tr("While a game has the console's video decoder open, usually to play a video, runs "
              "at normal speed even if the speed limit is raised or off. Switch 2 games pace "
              "frames on their own path, which does not read it. Cannot change while a game "
              "runs."));
    INSERT(Settings, barrier_feedback_loops, tr("Fix depth feedback loops (Vulkan only)"),
           tr("When a draw reads the depth buffer it is also drawing into, ends the render pass "
              "first so the read sees finished depth values. Can fix transparency effects in "
              "some games; each break costs some GPU time. Vulkan only; cannot change while a "
              "game runs."));
    INSERT(Settings, enable_buffer_history, tr("Frame timing history for games"),
           tr("Records when each frame was queued and shown, and returns that history to games "
              "that ask for it; when off, the request is ignored. Only matters for games that "
              "request it. Takes effect immediately."));
    INSERT(Settings, fix_bloom_effects, tr("Depth-to-color copies (Burnout bloom, Vulkan only)"),
           tr("Copies a game's depth buffer into a color texture when the game converts one "
              "into the other; when off, that copy is skipped for most formats. Added to remove "
              "the bloom blur in Burnout. Vulkan only; cannot change while a game runs."));
    INSERT(Settings, nx2_mesh_shaders, tr("Mesh shader draws"),
           tr("Draws the models Switch 2 games render with task and mesh shaders. Needs a GPU "
              "with VK_EXT_mesh_shader; without it these models are skipped either way. Takes "
              "effect immediately."));
    INSERT(Settings, mkw_frame_rate, tr("Mario Kart World frame rate:"),
           tr("Mario Kart World counts one vblank on most rendered frames, so its simulation "
              "can slow down below 60 FPS.\nDynamic: the game advances two frames when "
              "one runs long, which keeps normal speed down to 30 FPS. Not applied with CPU "
              "clock seen by games other than Normal, Scale game clock with speed limit at a "
              "speed limit other than 100%, or Multicore CPU emulation off. It makes up for a "
              "speed limit below 100% (such as Slow speed): the game keeps normal speed down to a "
              "50% limit and runs at twice a lower one.\nLock 30 FPS: a steady 30 FPS at normal "
              "speed, for PCs that can hold 30.\nOff: the game as released.\nOther games are not "
              "affected. Takes effect at the next launch."));

    // Renderer (Extensions)
    INSERT(Settings, dyna_state, tr("Extended dynamic state:"),
           tr("Sets how much pipeline state the emulator changes per draw through Vulkan's "
              "extended dynamic state extensions, if the GPU supports them. Higher levels need "
              "fewer separate pipelines. After a change, saved pipelines are rebuilt during "
              "play. Cannot change while a game runs."));

    INSERT(Settings, vertex_input_dynamic_state, tr("Vertex input dynamic state"),
           tr("Sets vertex layouts per draw through VK_EXT_vertex_input_dynamic_state instead "
              "of building them into each pipeline. Turned off automatically on older NVIDIA "
              "and Intel drivers, and for Switch 2 games on NVIDIA and Intel drivers. Cannot "
              "change while a game runs."));

    INSERT(Settings, sample_shading, tr("Sample shading (MSAA):"),
           tr("Shades at least this share of each pixel's samples separately, instead of once "
              "per pixel, when a game draws with multisampling (MSAA). Improves quality but "
              "costs GPU time; higher values cost more, and 0% turns it off. Vulkan only; "
              "locked while a game runs."));

    // Renderer (Debug)

    // System
    INSERT(Settings, rng_seed, tr("Fixed RNG seed:"),
           tr("When checked, uses this fixed seed for the random numbers the emulated system "
              "gives games and for where game code is placed in memory, so they repeat every "
              "boot. Unchecked, they differ each boot. Applies at the next game boot."));
    INSERT(Settings, rng_seed_enabled, QString(), QString());
    INSERT(Settings, device_name, tr("Console name:"),
           tr("Sets the console name that games and system software can read. System software "
              "can also change it. Set once for all games; there is no per-game value."));
    INSERT(Settings, program_args, tr("Homebrew arguments:"),
           tr("Command-line arguments passed to homebrew apps when they start; only homebrew "
              "that reads arguments reacts to them. Cannot change while a game runs."));
    INSERT(Settings, custom_rtc, tr("Custom date and time:"),
           tr("When checked, the console reports this date and time instead of your PC's. Nemyu "
              "stores it as an offset from your PC clock, so the console clock keeps running "
              "from the date you pick. Can change while a game runs."));
    INSERT(Settings, custom_rtc_enabled, QString(), QString());
    INSERT(Settings, custom_rtc_offset, tr("Clock offset (seconds):"),
           tr("Seconds added to your PC's clock to get the console time. Picking a custom date "
              "fills it in, and it only applies while Custom date and time is checked."));
    INSERT(Settings, language_index, tr("Language:"),
           tr("Sets the system language games read; a game uses it when it includes that "
              "language. The game list also shows titles and icons in it. Cannot change while a "
              "game runs."));
    INSERT(Settings, region_index, tr("Region:"),
           tr("Sets the console region games read. A warning appears below when this region and "
              "the chosen language don't usually go together. Cannot change while a game runs."));
    INSERT(Settings, time_zone_index, tr("Time zone:"),
           tr("Sets the console's time zone, which games use for local time. Auto follows your "
              "PC's zone, shown in parentheses; Default is GMT. Cannot change while a game runs."));
    INSERT(Settings, sound_index, tr("Sound Output Mode:"),
           tr("Meant to pick mono, stereo or surround output, but nothing in this build reads "
              "it, so changing it has no effect."));
    INSERT(Settings, use_docked_mode, tr("Console mode:"),
           tr("Tells games whether the console is in TV or handheld mode; they pick resolution, "
              "detail and allowed controllers from it. Player 1 set to the Handheld controller "
              "forces Handheld mode. The Controls page shows the same choice; the status bar and "
              "F10 also switch it."));
    INSERT(Settings, current_user, QString(), QString());

    // Controls

    // Data Storage

    // Debugging

    // Debugging Graphics

    // Network

    // Web Service

    // Ui

    // Ui General
    INSERT(UISettings, select_user_on_boot, tr("Prompt for user profile on boot"),
           tr("Shows the profile picker every time you start a game; the profile you pick "
              "becomes the current user. Closing the picker cancels the launch."));
    INSERT(UISettings, pause_when_in_background, tr("Pause when not in focus"),
           tr("Pauses the game when you switch to another app and resumes it when Nemyu gets "
              "focus back. Only acts while a game runs."));
    INSERT(UISettings, confirm_before_stopping, tr("Confirm before stopping emulation:"),
           tr("Decides when Nemyu asks before stopping a game, or before closing while one "
              "runs.\nAlways ask: every time.\nOnly if game specifies not to stop: only when "
              "the game asked not to be closed.\nNever ask: stops at once."));
    INSERT(UISettings, hide_mouse, tr("Hide mouse on inactivity"),
           tr("Hides the mouse cursor over the game after 2.5 seconds without movement; moving "
              "the mouse shows it again. Only while a game runs."));
    INSERT(UISettings, check_for_updates, tr("Check for updates"),
           tr("Checks online for the latest Nemyu release each time Nemyu starts and shows a "
              "dialog if it differs from your version."));

    // Linux
#ifdef __unix__
    INSERT(UISettings, enable_gamemode, tr("Enable Feral GameMode"),
           tr("Asks Feral GameMode, when it is installed, to favor Nemyu while a game runs, "
              "for example with a performance CPU governor. Does nothing without GameMode."));
    INSERT(UISettings, gui_force_x11, tr("Force X11 as graphics backend"),
           tr("Runs Nemyu through X11 (XWayland on Wayland desktops) instead of the "
              "desktop's default. Takes effect at the next start of Nemyu."));
    INSERT(UISettings, gui_hide_backend_warning, QString(), QString());
#else
    // Feral GameMode is only built for __unix__
    INSERT(UISettings, enable_gamemode, QString(), QString());
#endif

    // Ui Debugging

    // Ui Multiplayer

    // Ui Games list

#undef INSERT

    return translations;
}

std::unique_ptr<TipMap> InitializeTips([[maybe_unused]] QObject* parent) {
    std::unique_ptr<TipMap> tips = std::make_unique<TipMap>();
    const auto& tr = [](const char* text) -> QString {
        return QCoreApplication::translate("ConfigurationShared", text);
    };

#define TIP(SETTINGS, ID, TEXT) tips->insert(std::pair{SETTINGS::values.ID.Id(), (TEXT)})

    // Applets
    TIP(Settings, cabinet_applet_mode,
        tr("If a game freezes on its amiibo screen, try the other choice."));
    TIP(Settings, controller_applet_mode,
        tr("Disabled skips the screen, but only from a fresh start: if a game already ran this "
           "session, restart Nemyu first."));
    TIP(Settings, error_applet_mode,
        tr("Custom frontend always shows the code as 2XXX-XXXX, handy for searching or for bug "
           "reports."));
    TIP(Settings, player_select_applet_mode,
        tr("This is separate from Prompt for user profile on boot, which asks before the game "
           "even starts."));
    TIP(Settings, swkbd_applet_mode,
        tr("If a game hangs when its keyboard should appear, try the other choice."));
    TIP(Settings, mii_edit_applet_mode,
        tr("Keep Real applet, with firmware installed, if you want to create or edit Miis "
           "yourself."));
    TIP(Settings, web_applet_mode,
        tr("With Custom frontend the game resumes as soon as the browser opens, and nothing you "
           "do on the page reaches the game."));
    TIP(Settings, photo_viewer_applet_mode,
        tr("The album lists screenshots from the Screenshots Path that keep Nemyu's file names."));
    TIP(Settings, offline_web_applet_mode,
        tr("Real applet with firmware installed is the only way to try a game's manual here; "
           "Custom frontend never shows it."));
    TIP(Settings, enable_overlay,
        tr("The same switch is in View > Enable Overlay Display Applet."));

    // Audio
    TIP(Settings, sink_id,
        tr("If sound crackles, stutters or lags behind the picture, switch between cubeb and "
           "sdl3."));
    TIP(Settings, audio_output_device_id,
        tr("If a game is silent but the volume is up, set this to auto or to the device your PC "
           "is actually using."));
    TIP(Settings, audio_input_device_id,
        tr("To change which microphone games hear, make it the default recording device in your "
           "system's sound settings."));
    TIP(Settings, audio_muted,
        tr("Ctrl+M toggles it during play by default, and the status bar reads VOLUME: MUTE "
           "while it is on."));
    TIP(Settings, volume,
        tr("The status bar volume button and the volume hotkeys (- and = by default) change "
           "this same value, and they also unmute."));
    TIP(Settings, dump_audio_commands,
        tr("Turn it on only while reproducing an audio bug you want to report."));
    TIP(UISettings, mute_when_in_background,
        tr("To stop the game as well, turn on Pause when not in focus on the General page."));

    // Core
    TIP(Settings, memory_layout_mode,
        tr("Raise it only when a mod asks for more; on Windows, the extra RAM is taken only as "
           "the game uses it."));
    TIP(Settings, speed_limit,
        tr("Ctrl+U (default hotkey) toggles the cap while playing; it turns back on whenever a "
           "game stops, even if unchecked here."));
    TIP(Settings, turbo_speed_limit,
        tr("Ctrl+Z (default hotkey) turns Turbo on and off; the status bar shows Turbo while it "
           "is active."));
    TIP(Settings, slow_speed_limit,
        tr("Ctrl+X (default hotkey) turns Slow on and off; the status bar shows Slow while it "
           "is active."));
    TIP(Settings, sync_core_speed,
        tr("Has no effect at 100%. It still applies with the limit unchecked, and toggling "
           "Turbo or Slow makes the game clock jump."));

    // Cpu
    TIP(Settings, cpu_accuracy,
        tr("If a game crashes or glitches, try Accurate to rule out the CPU speedups, then "
           "return to Auto."));
    TIP(Settings, cpu_clock,
        tr("It only changes the time games see, not how fast your PC emulates; go back to "
           "Normal if timers or game speed look wrong."));
    TIP(Settings, cpu_ticks,
        tr("Leave it unchecked unless you are testing single-core timing; high values may make "
           "games hang."));

    // Cpu Unsafe
    TIP(Settings, cpuopt_unsafe_host_mmu,
        tr("Auto and Accurate keep fastmem on too, so untick this only to check whether fastmem "
           "is behind a crash."));
    TIP(Settings, cpuopt_unsafe_unfuse_fma,
        tr("Auto applies this too; pick Accurate if you need exact FMA results on a CPU without "
           "FMA."));
    TIP(Settings, cpuopt_unsafe_reduce_fp_error,
        tr("Auto never applies this one; if a game misbehaves only under Unsafe, try unticking "
           "it."));
    TIP(Settings, cpuopt_unsafe_ignore_standard_fpcr,
        tr("Auto already turns this on for 32-bit games."));
    TIP(Settings, cpuopt_unsafe_inaccurate_nan,
        tr("Auto already uses this for 32-bit games; under Unsafe it covers 64-bit games too."));
    TIP(Settings, cpuopt_unsafe_fastmem_check,
        tr("Auto skips this check too; for homebrew you don't trust, use Accurate or untick "
           "this under Unsafe."));
    TIP(Settings, cpuopt_unsafe_ignore_global_monitor,
        tr("Auto uses this too; if a game hangs, untick it here or switch Accuracy to Accurate "
           "to rule it out."));

    // Renderer
    TIP(Settings, renderer_backend,
        tr("Keep Vulkan; if its startup check fails, the emulator switches to OpenGL GLSL by "
           "itself and greys out this list."));
    TIP(Settings, vulkan_device,
        tr("The list is ordered NVIDIA, AMD, Intel, with dedicated cards first within a brand, "
           "so the top entry is usually the fast card."));
    TIP(Settings, resolution_setup,
        tr("The 720p/1080p labels describe Switch 1 games; below 1X, pair it with the AMD FSR 1 "
           "or Snapdragon GSR upscaling filter."));
    TIP(Settings, scaling_filter,
        tr("Press F8 or click the filter name in the status bar to cycle filters while playing "
           "and compare them."));
    TIP(Settings, fsr_sharpening_slider,
        tr("The 'slightly sharper' FSR 1 option sharpens more than plain FSR 1 at the same "
           "value; lower this if edges look grainy."));
    TIP(Settings, anti_aliasing,
        tr("Click the AA button in the status bar to cycle methods while playing."));
    TIP(Settings, post_shader_enabled,
        tr("Effects run at the render resolution, so they cost more at higher Resolution; "
           "right-click the status bar effects button to apply a preset."));
    TIP(Settings, fullscreen_mode,
        tr("Switch to Borderless if the game drops out of fullscreen whenever a dialog appears."));
    TIP(Settings, use_disk_shader_cache,
        tr("Keep it on; turn it off only to test whether a bad cached shader is causing a "
           "problem."));
    TIP(Settings, nvdec_emulation, tr("Try GPU Video Decoding if videos stutter on a slower CPU."));
    TIP(Settings, accelerate_astc,
        tr("Keep GPU; if textures look corrupted, try CPU to rule out the GPU decoder."));
    TIP(Settings, astc_recompression,
        tr("Prefer BC3 over BC1: BC3 already stores most opaque textures as BC1, so BC1 only "
           "saves more by making soft transparency all-or-nothing."));
    TIP(Settings, frame_pacing_mode,
        tr("Match the game's own frame rate, such as 30 FPS for a 30 FPS game, to even out "
           "frame times; a lower cap holds the game back."));
    TIP(Settings, vram_usage_mode,
        tr("Aggressive only changes anything on integrated GPUs and on cards whose budget "
           "exceeds the Conservative limit; it can slow recording software that needs video "
           "memory."));
    TIP(Settings, skip_cpu_inner_invalidation,
        tr("If a game hangs or shows outdated textures with this on, turn it off first."));
    TIP(Settings, vsync_mode,
        tr("Try Mailbox if FIFO feels laggy; with frame generation on, your choice is kept, "
           "except Immediate, which becomes Mailbox."));
    TIP(Settings, frame_gen,
        tr("If the status bar's presented FPS stays at the game FPS, no frames are generated; "
           "the log may name an unsupported GPU or missing Lossless.dll."));
    TIP(Settings, frame_gen_type,
        tr("For LSFG, create a 'lossless' folder in File > Open Nemyu Folders > Root Data "
           "Folder and copy Lossless.dll there from your Lossless Scaling install."));
    TIP(Settings, frame_gen_multiplier,
        tr("Set Target frame rate instead to let the emulator add up to three frames as needed "
           "to reach your monitor's refresh rate."));
    TIP(Settings, frame_gen_target_rate,
        tr("Set it to your monitor's refresh rate, for example 120 or 144."));
    TIP(Settings, frame_gen_back_off,
        tr("Turn it off if a game whose frame rate swings from scene to scene (Mario Kart World) "
           "never reaches the target while the GPU has room to spare; keep it on if the GPU is "
           "near full load."));
    TIP(Settings, frame_gen_flow_scale,
        tr("Lower it if frame generation itself costs too much GPU time on a high-resolution "
           "display."));
    TIP(Settings, frame_gen_queue_target,
        tr("Keep 0 for the lowest lag; try 1 if generated frames are often skipped."));

    // Renderer (Advanced Graphics)
    TIP(Settings, sync_memory_operations,
        tr("Enable it in a game's Properties rather than globally, so other games don't pay for "
           "the waits."));
    TIP(Settings, async_presentation,
        tr("Frame generation starts this thread on its own while it generates frames, so the "
           "setting mainly matters with frame generation off."));
    TIP(Settings, renderer_force_max_clock,
        tr("On a laptop it can take power from the CPU and make frame times worse; turn it off "
           "if so."));
    TIP(Settings, max_anisotropy,
        tr("At 1x resolution Automatic adds nothing; pick 4x or 16x there if distant ground "
           "textures look blurry."));
    TIP(Settings, gpu_accuracy,
        tr("For a Switch 2 game, pick Accurate in its Properties only to fix a glitch; in Mario "
           "Kart World it cost about 17 ms per frame."));
    TIP(Settings, dma_accuracy,
        tr("Keep Default for Switch 2 games: Safe turns off the second GPU thread they use on "
           "Vulkan."));
    TIP(Settings, gpu_fence_behavior,
        tr("Try Balanced on a glitching game before switching GPU accuracy to Accurate, which "
           "changes much more than fences."));
    TIP(Settings, use_asynchronous_shaders,
        tr("Helps most the first time you play a game, before its pipeline cache has been "
           "filled."));
    TIP(Settings, gpu_clock,
        tr("Boost is the default; set Normal in a game's Properties to test whether a problem "
           "in that game comes from the faked timing."));
    TIP(Settings, gpu_unswizzle_enabled,
        tr("Try it when a game hitches while loading large 3D textures; those textures then "
           "fill in over a few frames."));
    TIP(Settings, gpu_unswizzle_texture_size,
        tr("Lower it to send more textures to the GPU path if loading still hitches."));
    TIP(Settings, gpu_unswizzle_stream_size,
        tr("Raise it if large textures take too long to appear; lower it if frames hitch while "
           "they load."));
    TIP(Settings, gpu_unswizzle_chunk_size,
        tr("Lower it if the GPU driver times out or resets while large textures load."));
    TIP(Settings, use_vulkan_driver_pipeline_cache,
        tr("It needs Persistent pipeline cache turned on; with that off, nothing is saved "
           "or loaded."));
    TIP(Settings, pipeline_preload,
        tr("Choose All cached pipelines if you have 16 GB of RAM or more and a game stutters "
           "when you reach places you have been before; keep Used pipelines on laptops and "
           "GPUs with little memory."));
    TIP(Settings, enable_compute_pipelines,
        tr("Updating the Intel driver is the better fix: newer versions run compute shaders "
           "without this option."));
    TIP(Settings, use_reactive_flushing,
        tr("Turn it on in a game's Properties only if that game's graphics look wrong or out of "
           "date without it."));
    TIP(Settings, use_video_framerate,
        tr("Turn it on if cutscene videos play too fast when you run games unlocked or above "
           "100% speed."));
    TIP(Settings, enable_buffer_history,
        tr("Try it in a game's Properties if its frame pacing is uneven; most games never ask "
           "for this data."));
    TIP(Settings, fix_bloom_effects,
        tr("Turn it on in that game's Properties rather than globally."));
    TIP(Settings, nx2_mesh_shaders,
        tr("Turn it off only to check whether a crash or glitch comes from these draws; the "
           "affected models then disappear."));
    TIP(Settings, mkw_frame_rate,
        tr("Leave it on Dynamic; choose Lock 30 FPS if your PC cannot hold about 40 FPS in "
           "races, and Off only to compare with the game as released."));

    // Renderer (Extensions)
    TIP(Settings, dyna_state,
        tr("If a game shows graphics glitches on your driver, try a lower level; "
           "Level 2 is the default."));
    TIP(Settings, vertex_input_dynamic_state,
        tr("Changing it stops saved pipelines from preloading, so expect shader stutter until "
           "they are rebuilt."));
    TIP(Settings, sample_shading,
        tr("Set it in the Properties of a game that uses MSAA rather than globally; games "
           "without MSAA gain nothing from it."));

    // System
    TIP(Settings, rng_seed,
        tr("Useful for speedruns and testing, though a game's own randomness may still vary. "
           "Enter up to 8 hex digits."));
    TIP(Settings, device_name, tr("Keep it short; the console's name field holds 128 bytes."));
    TIP(Settings, custom_rtc,
        tr("Use it to reach date-based events in games; uncheck it to return to your PC's time."));
    TIP(Settings, custom_rtc_offset,
        tr("Type 86400 to move the clock one day ahead, or -86400 for one day back."));
    TIP(Settings, language_index,
        tr("Brazilian Portuguese, Latin American Spanish and Canadian French pair with the USA "
           "region; British English pairs with Europe or Australia."));
    TIP(Settings, use_docked_mode,
        tr("Handheld is lighter on your PC: Mario Kart World draws 1920x1080 instead of "
           "2560x1440. F10 switches modes while playing."));

    // Ui General
    TIP(UISettings, select_user_on_boot,
        tr("Handy when several people share this PC, each with their own profile."));
    TIP(UISettings, pause_when_in_background,
        tr("Turn off if the game pauses when you click a second monitor, an overlay or a chat "
           "app."));
    TIP(UISettings, confirm_before_stopping,
        tr("Stopping loses unsaved progress, so keep a confirmation if you sometimes hit Stop "
           "(F5 by default) by accident."));

#undef TIP

    return tips;
}

std::unique_ptr<ComboboxTranslationMap> ComboboxEnumeration(QObject* parent) {
    std::unique_ptr<ComboboxTranslationMap> translations =
        std::make_unique<ComboboxTranslationMap>();
    const auto& tr = [](const char* text, const char* context = "") {
        return QCoreApplication::translate("ConfigurationShared", text, context);
    };

#define PAIR(ENUM, VALUE, TRANSLATION) {static_cast<u32>(Settings::ENUM::VALUE), (TRANSLATION)}

    // Intentionally skipping VSyncMode to let the UI fill that one out
    translations->insert({Settings::EnumMetadata<Settings::AppletMode>::Index(),
                          {
                              PAIR(AppletMode, HLE, tr("Custom frontend")),
                              PAIR(AppletMode, LLE, tr("Real applet")),
                              PAIR(AppletMode, Disabled, tr("Disabled")),
                          }});

    translations->insert({Settings::EnumMetadata<Settings::SpirvOptimizeMode>::Index(),
                          {
                              PAIR(SpirvOptimizeMode, Never, tr("Never")),
                              PAIR(SpirvOptimizeMode, OnLoad, tr("On Load")),
                              PAIR(SpirvOptimizeMode, Always, tr("Always")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::AstcDecodeMode>::Index(),
                          {
                              PAIR(AstcDecodeMode, Cpu, tr("CPU")),
                              PAIR(AstcDecodeMode, Gpu, tr("GPU")),
                              PAIR(AstcDecodeMode, CpuAsynchronous, tr("CPU Asynchronous")),
                          }});
    translations->insert(
        {Settings::EnumMetadata<Settings::AstcRecompression>::Index(),
         {
             PAIR(AstcRecompression, Uncompressed, tr("Uncompressed (Best quality)")),
             PAIR(AstcRecompression, Bc1, tr("BC1 (Low quality)")),
             PAIR(AstcRecompression, Bc3, tr("BC3 (Medium quality)")),
         }});
    translations->insert({Settings::EnumMetadata<Settings::FramePacingMode>::Index(),
                          {
                              PAIR(FramePacingMode, Target_Auto, tr("Auto")),
                              PAIR(FramePacingMode, Target_30, tr("30 FPS")),
                              PAIR(FramePacingMode, Target_60, tr("60 FPS")),
                              PAIR(FramePacingMode, Target_90, tr("90 FPS")),
                              PAIR(FramePacingMode, Target_120, tr("120 FPS")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::VramUsageMode>::Index(),
                          {
                              PAIR(VramUsageMode, Conservative, tr("Conservative")),
                              PAIR(VramUsageMode, Aggressive, tr("Aggressive")),
                          }});
    translations->insert(
        {Settings::EnumMetadata<Settings::RendererBackend>::Index(),
         {PAIR(RendererBackend, Vulkan, tr("Vulkan")),
#ifdef HAS_OPENGL
          PAIR(RendererBackend, OpenGL_GLSL, tr("OpenGL GLSL")),
          PAIR(RendererBackend, OpenGL_GLASM, tr("OpenGL GLASM (Assembly Shaders, NVIDIA Only)")),
          PAIR(RendererBackend, OpenGL_SPIRV, tr("OpenGL SPIR-V (Experimental, AMD/Mesa Only)")),
#endif
          PAIR(RendererBackend, Null, tr("Null"))}});
    translations->insert({Settings::EnumMetadata<Settings::GpuAccuracy>::Index(),
                          {
                              PAIR(GpuAccuracy, Low, tr("Fast")),
                              PAIR(GpuAccuracy, High, tr("Accurate")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::NativeComputeWriteMark>::Index(),
                          {
                              PAIR(NativeComputeWriteMark, Auto, tr("Auto (shader extent)")),
                              PAIR(NativeComputeWriteMark, Full, tr("Full binding")),
                              PAIR(NativeComputeWriteMark, Cap64K, tr("64 KiB cap")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::DmaAccuracy>::Index(),
                          {
                              PAIR(DmaAccuracy, Default, tr("Default")),
                              PAIR(DmaAccuracy, Unsafe, tr("Unsafe (fast)")),
                              PAIR(DmaAccuracy, Safe, tr("Safe (stable)")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::NativeComputeWriteMark>::Index(),
                          {
                              PAIR(NativeComputeWriteMark, Auto, tr("Auto (shader extent)")),
                              PAIR(NativeComputeWriteMark, Full, tr("Full binding")),
                              PAIR(NativeComputeWriteMark, Cap64K, tr("64 KiB cap")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::GpuFenceBehavior>::Index(),
                          {
                              PAIR(GpuFenceBehavior, Default, tr("Default")),
                              PAIR(GpuFenceBehavior, Immediate, tr("Immediate")),
                              PAIR(GpuFenceBehavior, Balanced, tr("Balanced")),
                              PAIR(GpuFenceBehavior, Accurate, tr("Accurate")),
                          }});
    translations->insert(
        {Settings::EnumMetadata<Settings::CpuAccuracy>::Index(),
         {
             PAIR(CpuAccuracy, Auto, tr("Auto")),
             PAIR(CpuAccuracy, Accurate, tr("Accurate")),
             PAIR(CpuAccuracy, Unsafe, tr("Unsafe")),
             PAIR(CpuAccuracy, Paranoid, tr("Paranoid (disables most optimizations)")),
             PAIR(CpuAccuracy, Debugging, tr("Debugging")),
         }});
    translations->insert({Settings::EnumMetadata<Settings::CpuBackend>::Index(),
                          {
                              PAIR(CpuBackend, Dynarmic, tr("Dynarmic")),
                              PAIR(CpuBackend, Nce, tr("NCE")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::FullscreenMode>::Index(),
                          {
                              PAIR(FullscreenMode, Borderless, tr("Borderless Windowed")),
                              PAIR(FullscreenMode, Exclusive, tr("Exclusive Fullscreen")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::NvdecEmulation>::Index(),
                          {
                              PAIR(NvdecEmulation, Off, tr("No Video Output")),
                              PAIR(NvdecEmulation, Cpu, tr("CPU Video Decoding (Default)")),
                              PAIR(NvdecEmulation, Gpu, tr("GPU Video Decoding")),
                          }});
    translations->insert(
        {Settings::EnumMetadata<Settings::ResolutionSetup>::Index(),
         {
             PAIR(ResolutionSetup, Res1_4X, tr("0.25X (180p/270p) [EXPERIMENTAL]")),
             PAIR(ResolutionSetup, Res1_2X, tr("0.5X (360p/540p) [EXPERIMENTAL]")),
             PAIR(ResolutionSetup, Res3_4X, tr("0.75X (540p/810p) [EXPERIMENTAL]")),
             PAIR(ResolutionSetup, Res1X, tr("1X (720p/1080p)")),
             PAIR(ResolutionSetup, Res5_4X, tr("1.25X (900p/1350p) [EXPERIMENTAL]")),
             PAIR(ResolutionSetup, Res3_2X, tr("1.5X (1080p/1620p) [EXPERIMENTAL]")),
             PAIR(ResolutionSetup, Res2X, tr("2X (1440p/2160p)")),
             PAIR(ResolutionSetup, Res3X, tr("3X (2160p/3240p)")),
             PAIR(ResolutionSetup, Res4X, tr("4X (2880p/4320p)")),
             PAIR(ResolutionSetup, Res5X, tr("5X (3600p/5400p)")),
             PAIR(ResolutionSetup, Res6X, tr("6X (4320p/6480p)")),
             PAIR(ResolutionSetup, Res7X, tr("7X (5040p/7560p)")),
             PAIR(ResolutionSetup, Res8X, tr("8X (5760p/8640p)")),
         }});
    translations->insert({Settings::EnumMetadata<Settings::FrameGenType>::Index(),
                          {
                              PAIR(FrameGenType, Lsfg, tr("Lossless Scaling (LSFG)")),
                              PAIR(FrameGenType, Fsr3, tr("Built-in optical flow (was FSR 3)")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::MkwFrameRate>::Index(),
                          {
                              PAIR(MkwFrameRate, Off, tr("Off (as released)")),
                              PAIR(MkwFrameRate, Dynamic, tr("Dynamic")),
                              PAIR(MkwFrameRate, Lock30, tr("Lock 30 FPS")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::PipelinePreload>::Index(),
                          {
                              PAIR(PipelinePreload, Used, tr("Used pipelines (recommended)")),
                              PAIR(PipelinePreload, All, tr("All cached pipelines")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::ScalingFilter>::Index(),
                          {
                              PAIR(ScalingFilter, NearestNeighbor, tr("Nearest Neighbor")),
                              PAIR(ScalingFilter, Bilinear, tr("Bilinear")),
                              PAIR(ScalingFilter, Bicubic, tr("Bicubic")),
                              PAIR(ScalingFilter, Gaussian, tr("Gaussian")),
                              PAIR(ScalingFilter, Lanczos, tr("Lanczos")),
                              PAIR(ScalingFilter, ScaleForce, tr("ScaleForce")),
                              PAIR(ScalingFilter, Fsr, tr("AMD FidelityFX Super Resolution 1 (FSR 1)")),
                              PAIR(ScalingFilter, Area, tr("Area")),
                              PAIR(ScalingFilter, Mmpx, tr("MMPX")),
                              PAIR(ScalingFilter, ZeroTangent, tr("Zero-Tangent")),
                              PAIR(ScalingFilter, BSpline, tr("B-Spline")),
                              PAIR(ScalingFilter, Mitchell, tr("Mitchell")),
                              PAIR(ScalingFilter, Spline1, tr("Spline-1")),
                              PAIR(ScalingFilter, Sgsr, tr("Snapdragon Game Super Resolution")),
                              PAIR(ScalingFilter, SgsrEdge, tr("Snapdragon Game Super Resolution EdgeDir")),
                              PAIR(ScalingFilter, Fsr3, tr("AMD FSR 1, slightly sharper (was FSR 3)")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::AntiAliasing>::Index(),
                          {
                              PAIR(AntiAliasing, None, tr("None")),
                              PAIR(AntiAliasing, Fxaa, tr("FXAA")),
                              PAIR(AntiAliasing, Smaa, tr("SMAA")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::AspectRatio>::Index(),
                          {
                              PAIR(AspectRatio, R16_9, tr("Default (16:9)")),
                              PAIR(AspectRatio, R4_3, tr("Force 4:3")),
                              PAIR(AspectRatio, R21_9, tr("Force 21:9")),
                              PAIR(AspectRatio, R16_10, tr("Force 16:10")),
                              PAIR(AspectRatio, Stretch, tr("Stretch to Window")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::AnisotropyMode>::Index(),
                          {
                              PAIR(AnisotropyMode, Automatic, tr("Automatic")),
                              PAIR(AnisotropyMode, Default, tr("Game Default")),
                              PAIR(AnisotropyMode, X2, tr("2x")),
                              PAIR(AnisotropyMode, X4, tr("4x")),
                              PAIR(AnisotropyMode, X8, tr("8x")),
                              PAIR(AnisotropyMode, X16, tr("16x")),
                          }});
    translations->insert(
        {Settings::EnumMetadata<Settings::Language>::Index(),
         {
             PAIR(Language, Japanese, tr("Japanese (日本語)")),
             PAIR(Language, EnglishAmerican, tr("American English")),
             PAIR(Language, French, tr("French (français)")),
             PAIR(Language, German, tr("German (Deutsch)")),
             PAIR(Language, Italian, tr("Italian (italiano)")),
             PAIR(Language, Spanish, tr("Spanish (español)")),
             PAIR(Language, Chinese, tr("Chinese")),
             PAIR(Language, Korean, tr("Korean (한국어)")),
             PAIR(Language, Dutch, tr("Dutch (Nederlands)")),
             PAIR(Language, Portuguese, tr("Portuguese (português)")),
             PAIR(Language, Russian, tr("Russian (Русский)")),
             PAIR(Language, Taiwanese, tr("Taiwanese")),
             PAIR(Language, EnglishBritish, tr("British English")),
             PAIR(Language, FrenchCanadian, tr("Canadian French")),
             PAIR(Language, SpanishLatin, tr("Latin American Spanish")),
             PAIR(Language, ChineseSimplified, tr("Simplified Chinese")),
             PAIR(Language, ChineseTraditional, tr("Traditional Chinese (正體中文)")),
             PAIR(Language, PortugueseBrazilian, tr("Brazilian Portuguese (português do Brasil)")),
             PAIR(Language, Polish, tr("Polish (polska)")),
             PAIR(Language, Thai, tr("Thai (แบบไทย)")),
         }});
    translations->insert({Settings::EnumMetadata<Settings::Region>::Index(),
                          {
                              PAIR(Region, Japan, tr("Japan")),
                              PAIR(Region, Usa, tr("USA")),
                              PAIR(Region, Europe, tr("Europe")),
                              PAIR(Region, Australia, tr("Australia")),
                              PAIR(Region, China, tr("China")),
                              PAIR(Region, Korea, tr("Korea")),
                              PAIR(Region, Taiwan, tr("Taiwan")),
                          }});
    translations->insert(
        {Settings::EnumMetadata<Settings::TimeZone>::Index(),
         {
             {static_cast<u32>(Settings::TimeZone::Auto),
              tr("Auto (%1)", "Auto select time zone")
                  .arg(QString::fromStdString(
                      Settings::GetTimeZoneString(Settings::TimeZone::Auto)))},
             {static_cast<u32>(Settings::TimeZone::Default),
              tr("Default (%1)", "Default time zone")
                  .arg(QString::fromStdString(Common::TimeZone::GetDefaultTimeZone()))},
             PAIR(TimeZone, Cet, tr("CET")),
             PAIR(TimeZone, Cst6Cdt, tr("CST6CDT")),
             PAIR(TimeZone, Cuba, tr("Cuba")),
             PAIR(TimeZone, Eet, tr("EET")),
             PAIR(TimeZone, Egypt, tr("Egypt")),
             PAIR(TimeZone, Eire, tr("Eire")),
             PAIR(TimeZone, Est, tr("EST")),
             PAIR(TimeZone, Est5Edt, tr("EST5EDT")),
             PAIR(TimeZone, Gb, tr("GB")),
             PAIR(TimeZone, GbEire, tr("GB-Eire")),
             PAIR(TimeZone, Gmt, tr("GMT")),
             PAIR(TimeZone, GmtPlusZero, tr("GMT+0")),
             PAIR(TimeZone, GmtMinusZero, tr("GMT-0")),
             PAIR(TimeZone, GmtZero, tr("GMT0")),
             PAIR(TimeZone, Greenwich, tr("Greenwich")),
             PAIR(TimeZone, Hongkong, tr("Hongkong")),
             PAIR(TimeZone, Hst, tr("HST")),
             PAIR(TimeZone, Iceland, tr("Iceland")),
             PAIR(TimeZone, Iran, tr("Iran")),
             PAIR(TimeZone, Israel, tr("Israel")),
             PAIR(TimeZone, Jamaica, tr("Jamaica")),
             PAIR(TimeZone, Japan, tr("Japan")),
             PAIR(TimeZone, Kwajalein, tr("Kwajalein")),
             PAIR(TimeZone, Libya, tr("Libya")),
             PAIR(TimeZone, Met, tr("MET")),
             PAIR(TimeZone, Mst, tr("MST")),
             PAIR(TimeZone, Mst7Mdt, tr("MST7MDT")),
             PAIR(TimeZone, Navajo, tr("Navajo")),
             PAIR(TimeZone, Nz, tr("NZ")),
             PAIR(TimeZone, NzChat, tr("NZ-CHAT")),
             PAIR(TimeZone, Poland, tr("Poland")),
             PAIR(TimeZone, Portugal, tr("Portugal")),
             PAIR(TimeZone, Prc, tr("PRC")),
             PAIR(TimeZone, Pst8Pdt, tr("PST8PDT")),
             PAIR(TimeZone, Roc, tr("ROC")),
             PAIR(TimeZone, Rok, tr("ROK")),
             PAIR(TimeZone, Singapore, tr("Singapore")),
             PAIR(TimeZone, Turkey, tr("Turkey")),
             PAIR(TimeZone, Uct, tr("UCT")),
             PAIR(TimeZone, Universal, tr("Universal")),
             PAIR(TimeZone, Utc, tr("UTC")),
             PAIR(TimeZone, WSu, tr("W-SU")),
             PAIR(TimeZone, Wet, tr("WET")),
             PAIR(TimeZone, Zulu, tr("Zulu")),
         }});
    translations->insert({Settings::EnumMetadata<Settings::AudioMode>::Index(),
                          {
                              PAIR(AudioMode, Mono, tr("Mono")),
                              PAIR(AudioMode, Stereo, tr("Stereo")),
                              PAIR(AudioMode, Surround, tr("Surround")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::MemoryLayout>::Index(),
                          {
                              PAIR(MemoryLayout, Memory_4Gb, tr("4GB DRAM (Default)")),
                              PAIR(MemoryLayout, Memory_6Gb, tr("6GB DRAM (Unsafe)")),
                              PAIR(MemoryLayout, Memory_8Gb, tr("8GB DRAM")),
                              PAIR(MemoryLayout, Memory_10Gb, tr("10GB DRAM (Unsafe)")),
                              PAIR(MemoryLayout, Memory_12Gb, tr("12GB DRAM")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::ConsoleMode>::Index(),
                          {
                              PAIR(ConsoleMode, Docked, tr("TV mode")),
                              PAIR(ConsoleMode, Handheld, tr("Handheld mode")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::CpuClock>::Index(),
                          {
                              PAIR(CpuClock, Normal, tr("Normal")),
                              PAIR(CpuClock, Boost, tr("Boost (1.7x)")),
                              PAIR(CpuClock, Overclock, tr("Overclock (2x)")),
                          }});
    translations->insert(
        {Settings::EnumMetadata<Settings::ConfirmStop>::Index(),
         {
             PAIR(ConfirmStop, Ask_Always, tr("Always ask (Default)")),
             PAIR(ConfirmStop, Ask_Based_On_Game, tr("Only if game specifies not to stop")),
             PAIR(ConfirmStop, Ask_Never, tr("Never ask")),
         }});
    translations->insert({Settings::EnumMetadata<Settings::GpuClock>::Index(),
                          {
                              PAIR(GpuClock, Normal, tr("Normal")),
                              PAIR(GpuClock, Boost, tr("Boost")),
                              PAIR(GpuClock, Overclock, tr("Overclock")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::GpuUnswizzleSize>::Index(),
                          {
                              PAIR(GpuUnswizzleSize, VerySmall, tr("Very Small (16 MB)")),
                              PAIR(GpuUnswizzleSize, Small, tr("Small (32 MB)")),
                              PAIR(GpuUnswizzleSize, Normal, tr("Normal (128 MB)")),
                              PAIR(GpuUnswizzleSize, Large, tr("Large (256 MB)")),
                              PAIR(GpuUnswizzleSize, VeryLarge, tr("Very Large (512 MB)")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::GpuUnswizzle>::Index(),
                          {
                              PAIR(GpuUnswizzle, VeryLow, tr("Very Low (4 MB)")),
                              PAIR(GpuUnswizzle, Low, tr("Low (8 MB)")),
                              PAIR(GpuUnswizzle, Normal, tr("Normal (16 MB)")),
                              PAIR(GpuUnswizzle, Medium, tr("Medium (32 MB)")),
                              PAIR(GpuUnswizzle, High, tr("High (64 MB)")),
                          }});
    translations->insert({Settings::EnumMetadata<Settings::GpuUnswizzleChunk>::Index(),
                          {
                              PAIR(GpuUnswizzleChunk, VeryLow, tr("Very Low (32)")),
                              PAIR(GpuUnswizzleChunk, Low, tr("Low (64)")),
                              PAIR(GpuUnswizzleChunk, Normal, tr("Normal (128)")),
                              PAIR(GpuUnswizzleChunk, Medium, tr("Medium (256)")),
                              PAIR(GpuUnswizzleChunk, High, tr("High (512)")),
                          }});

    translations->insert({Settings::EnumMetadata<Settings::ExtendedDynamicState>::Index(),
                          {
                              PAIR(ExtendedDynamicState, Disabled, tr("Disabled")),
                              PAIR(ExtendedDynamicState, EDS1, tr("Level 1")),
                              PAIR(ExtendedDynamicState, EDS2, tr("Level 2")),
                              PAIR(ExtendedDynamicState, EDS3, tr("Level 3")),
                          }});

    translations->insert({Settings::EnumMetadata<Settings::GameListMode>::Index(),
                          {
                              PAIR(GameListMode, TreeView, tr("Tree View")),
                              PAIR(GameListMode, GridView, tr("Grid View")),
                          }});

#undef PAIR
#undef CTX_PAIR

    return translations;
}
} // namespace ConfigurationShared
