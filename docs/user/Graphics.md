# User Handbook - Graphics

Graphical enhancements and visual quality improvements. This doesn't cover texture mods.

## Visual Enhancements

### Anti-aliasing

Enhancements aimed at removing jagged lines/sharp edges and/or masking artifacts.

- **No AA**: Default, provides no anti-aliasing.
- **FXAA**: Fast Anti-Aliasing, an implementation as described on [this blog post](https://web.archive.org/web/20110831051323/http://timothylottes.blogspot.com/2011/03/nvidia-fxaa.html). Generally fast but with some innocuos artifacts.
- **SMAA**: Subpixel Morphological Anti-Aliasing, an implementation as described on [this article](https://web.archive.org/web/20250000000000*/https://www.iryoku.com/smaa/).

### Filters

Various graphical filters exist - each of them aimed at a specific target/image quality preset.

- **Nearest**: Provides no filtering - useful for debugging.
  - **Pros**: Fast, works in any hardware.
  - **Cons**: Less image quality.
- **Bilinear**: Provides the hardware default filtering of the Tegra X1.
  - **Pros**: Fast with acceptable image quality.
- **Bicubic**: Provides a bicubic interpolation using a Catmull-Rom (or hardware-accelerated) implementation.
  - **Pros**: Better image quality with more rounded edges.
- **Zero-Tangent, B-Spline, Mitchell**: Provides bicubic interpolation using the respective matrix weights. They're normally not hardware accelerated unless the device supports the `VK_QCOM_filter_cubic_weights` extension. The matrix weights are those matching [the specification itself](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#VkSamplerCubicWeightsCreateInfoQCOM).
  - **Pros/Cons**: Each of them is a variation of the Bicubic interpolation model with different weights, they offer different methods to fix some artifacts present in Catmull-Rom.
- **Spline-1**: Bicubic interpolation (similar to Mitchell) but with a faster texel fetch method. Generally less blurry than bicubic.
  - **Pros**: Faster than bicubic even without hardware accelerated bicubic.
- **Gaussian**: Whole-area blur, an applied gaussian blur is done to the entire frame.
  - **Pros**: Less edge artifacts.
  - **Cons**: Slow and sometimes blurry.
- **Lanczos**: An implementation using `a = 3` (49 texel fetches). Provides sharper edges but blurrier artifacts.
  - **Pros**: Less edge artifacts and less blurry than gaussian.
  - **Cons**: Slow.
- **ScaleForce**: Experimental texture upscale method, see [ScaleFish](https://github.com/BreadFish64/ScaleFish).
  - **Pros**: Relatively fast.
- **AMD FidelityFX Super Resolution 1 (FSR 1)**: Uses AMD FidelityFX Super Resolution 1 to enhance image quality.
  - **Pros**: Great for upscaling, and offers sharper visual quality.
  - **Cons**: Somewhat slow, and may be offputtingly sharp.
- **AMD FSR 1, slightly sharper (was FSR 3)**: The same FSR 1 upscaling, with a little more sharpening at the same **FSR/SGSR sharpness** value. It is not FSR 3.
- **Area**: Area interpolation (high kernel count).
  - **Pros**: Best for downscaling (internal resolution > display resolution).
  - **Cons**: Costly and slow.
- **MMPX**: Nearest-neighbour filter aimed at providing higher pixel-art quality.
  - **Pros**: Offers decent pixel-art upscaling.
  - **Cons**: Only works for pixel-art.
- **SGSR**: Uses Snapdragon Studios Game Super Resolution to enhance image quality (similar to FSR, but for Adreno devices).
  - **Pros**: Optimized for Adreno devices.
  - **Cons**: Doesn't play nicely with non-Adreno devices.
- **SGSR Edge**: Almost the same pipeline as SGSR, but with improved edge detection.

### Anisotropy values

The anisotropy value is (value game wants + the set value); **Game Default** will use the native anisotropy value as it would be on hardware. **Automatic** sets it according to screen resolution. Turning off anisotropy is not recommended as it can break a myriad of games, however it is provided in the name of flexibility.

Values from x2, x4, x8, x16, x32 up to x64 values are provided. This should be enough to not need to revise those values in my lifetime ever again.

### External

While stock shaders offer a basic subset of options for most users, programs such as [ReShade](https://github.com/crosire/reshade) offer a more flexible experience. In addition to that users can also seek out modifications (mods) for enhancing visual experience (60 FPS mods, HDR, etc).

**Installing ReShade (Windows)**
1. [Download ReShade](https://reshade.me/#download) with add-on support.
 - [ReShade Setup 6.6.2 (Windows 64-bit) with add-on support](https://reshade.me/downloads/ReShade_Setup_6.6.2_Addon.exe)
 - SHA512 checksum: `1f09a73afa160480c13ffdd73cc04b1dc82943dddea58ad3bb9375f26b48c2787d0a85197e46b6fce32a4fd3472465520a3355ed3436241e17fba7ebaff7ffec`.
2. Open ReShade and hit browse, then the folder where `nemyu.exe` is at, hit open, then hit next.
3. Select Vulkan as the rendering API, hit next.
4. In "Select effects to install" screen: hit next don't change anything.
5. In "Select add on" screen: click the box for `Shader Toggler by Otis` ([GitHub](https://github.com/FransBouma/ShaderToggler)) and hit next.

**Using the Shader Toggler**
1. Launch a game, you must see a ReShade pop up afterwards.
2. Progress to a point with a flickering shader.
3. Hit the Home key on keyboard (or change binds if you don't have one).
4. Navigate to the add on tab at the top of the ReShade menu.
5. At the bottom where Shader Toggler is at open the drop down and max out the slider that says "# of fames collected" then select change shaders while staring at the flickering shader.
6. When the Shader Toggler finishes collecting frames in the top left hit Numpad 2 till it turns off the flickering lines.
7. Hit Numpad 3 to add it the group of shaders to turn off and hit done and save all toggle groups.
8. Hit the edit button and select "active at startup" for the shader to be turned off on every game launch.
9. Caps lock to manually turn on and off the shader (default key you can change it with the previous edit button)

## Games with DLSS

Some Switch 2 games upscale their 3D with DLSS, a neural network the console runs on its tensor cores. Of the games Nemyu runs, only Fire Emblem: Fortune's Weave is known to use it; its options are in **Emulation > Configure... > Switch 2**:

- **Use the game's own DLSS**: Runs the game's network, translated for your GPU. It is on by default. Off, a plain upscale stands in for it, which costs much less GPU time but looks softer. The line under the checkbox says what draws the game's DLSS while it runs. Takes effect when a game starts.
- **RCAS sharpness**: How strongly AMD's RCAS sharpens the plain upscale that stands in for the game's DLSS (Vulkan only); 0% turns it off. The game's own network is not sharpened.
- **Use NVIDIA DLSS in place of a game's DLSS (experimental)**: Only in Windows builds made with `ENABLE_NVIDIA_DLSS`. On a GeForce RTX card, NVIDIA DLSS upscales the game's image from its colour, depth and motion vectors. Nemyu does not ship NVIDIA's files: put `sl.interposer.dll`, `sl.common.dll` and `sl.dlss.dll` from NVIDIA Streamline, and `nvngx_dlss.dll` from NVIDIA DLSS, in the `streamline` folder of the Nemyu data folder (see [configuration directories](./Basics.md#configuration-directories)); without `nvngx_dlss.dll`, the newest DLSS the NVIDIA app has downloaded is used. If DLSS can't run, the plain upscale stands in and the log says why. While the box is ticked, a line under it says which of these files were found, with their versions; **Open Folder** opens the folder they go in (creating it), and **Where do these files come from?** opens NVIDIA's Streamline releases on GitHub. When files are missing, **Download from NVIDIA...** takes only those out of NVIDIA's Streamline 2.14.16 release on GitHub, after showing NVIDIA's licence terms for them to accept; each file's checksum and NVIDIA signature are checked before it is saved. Takes effect when a game starts.

## Frame Generation

Nemyu can show generated frames in between the frames a game renders. Frame generation is part of the Vulkan renderer, and its options are in **Emulation > Configure... > Graphics > Frame Generation**:

- **Enable frame generation**: Turns frame generation on. It is off by default.
- **Method**: The method used to create the extra frames.
  - **Built-in optical flow (was FSR 3)**: The default. It needs no extra files.
  - **Lossless Scaling (LSFG)**: Uses the shaders of Lossless Scaling, which Nemyu does not ship. Copy `Lossless.dll` from your own copy of Lossless Scaling into the `lossless` folder of the Nemyu data folder (see [configuration directories](./Basics.md#configuration-directories)).
- **Multiplier**: How many frames are shown for each rendered frame, from 2x to 4x. Only used when **Target frame rate** is 0.
- **Target frame rate**: With the default of 0, every rendered frame gets the full number of generated frames. Set a frame rate (up to 240) to generate only as many frames as are needed to reach it, up to 4x; the multiplier is then not used.

If the selected method can't run (for example `Lossless.dll` is missing, or the GPU lacks a feature it needs), no frames are generated and the log says why.

## Driver specifics

### Mesa environment variable hacks

The software requires a certain version of Vulkan and a certain version of OpenGL to work - otherwise it will refuse to load, this can be easily bypassed by setting an environment variable: `MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460` (OpenGL) and `MESA_VK_VERSION_OVERRIDE=1.3` (Vulkan), for more information see [Environment variables for Mesa](https://web.archive.org/web/20250000000000*/https://docs.mesa3d.org/envvars.html).

### NVIDIA OpenGL environment variables

Unstable multithreaded optimisations are offered by the stock proprietary NVIDIA driver on X11 platforms. Setting `__GL_THREADED_OPTIMIZATIONS` to `1` would enable such optimisations. This mainly benefits the OpenGL backend. For more information see [Environment Variables for X11 NVIDIA](https://web.archive.org/web/20250115162518/https://download.nvidia.com/XFree86/Linux-x86_64/435.17/README/openglenvvariables.html).

### swrast/LLVMpipe crashes under high load

The OpenGL backend would invoke behaviour that would result in swarst/LLVMpipe writing an invalid SSA IR (on old versions of Mesa), and then proceeding to crash. The solution is using a script found in [tools/llvmpipe-run.sh](../../tools/llvmpipe-run.sh).

### HaikuOS compatibility

HaikuOS bundles a Mesa library that doesn't support full core OpenGL 4.6 (required by the emulator). This leads to HaikuOS being one of the few computer platforms where Vulkan is the only available option for users. If OpenGL is desired, Mesa has to be built manually from source. For debugging purposes `lavapipe` is recommended over the GPU driver; there is in-kernel support for NVIDIA cards through.

### Fixes for Windows 10 and above having "Device loss"

Run the following batch script *inside* the folder that contains `nemyu.exe`:
```bat
@echo off
pushd "%~dp0"
if exist "%temp%\FixFullScreen.reg" (
  del %temp%\FixFullScreen.reg
)
set str_path="%cd:\=\\%\\nemyu.exe"
echo Windows Registry Editor Version 5.00 >> %temp%\FixFullScreen.reg
echo. >> %temp%\FixFullScreen.reg
echo [HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers] >> %temp%\FixFullScreen.reg
echo %str_path%="~ DISABLEDXMAXIMIZEDWINDOWEDMODE HIGHDPIAWARE" >> %temp%\FixFullScreen.reg
regedit /s %temp%\FixFullScreen.reg
del %temp%\FixFullScreen.reg
exit /b
```
