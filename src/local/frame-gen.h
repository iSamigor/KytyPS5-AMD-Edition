#ifndef EMULATOR_SRC_LOCAL_FRAME_GEN_H_
#define EMULATOR_SRC_LOCAL_FRAME_GEN_H_
// DLSS Frame Generation through NVIDIA Streamline (KYTY_FRAMEGEN=<generated frames, 1..5>), for
// Demon's Souls PPSA01341 01.007.000 and 01.005.000. The inputs come from the game's own passes, found by frame
// capture (docs/RE-DEMONS-SOULS.md): the G-buffer vertex shaders' constants hold the camera
// (world->clip matrix, the previous frame's, and the TAA jitter), the TAA dispatch reads the depth
// and motion-vector images. The renderer thread gathers them per guest frame; at the guest flip
// they are copied next to the presenter's frame; the present thread hands them to Streamline and
// presents through it (sl.interposer.dll is the process' Vulkan loader, see Initialize()).
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace Libs::Graphics {
class CommandBuffer;
class TextureCache;
namespace FrameGen {

// The TAA compute shader, and the G-buffer vertex shaders sharing the constant layout below (flattened SRT
// words), of 01.007.000 and of 01.005.000: the same shaders recompiled (KYTY_SHADER_DUMP: the same resources,
// the vertex shaders the same translated program; the TAA images are what is read from its dispatch).
constexpr std::array<uint64_t, 2> TaaShaders {0xa44e580f4f0cfb2bULL, 0x934a6e1197b99148ULL};
constexpr std::array<uint64_t, 6> GbufferVertexShaders {0xf4d2e78cd1769fc7ULL, 0x08e16923875aa8e9ULL, 0x0ffcbf08be26e74eULL,
                                                        0x1d69b5d27198446aULL, 0x5931e753b757f3bcULL, 0xa97f610824942162ULL};
inline bool IsTaaShader(uint64_t hash) {
	return std::find(TaaShaders.begin(), TaaShaders.end(), hash) != TaaShaders.end();
}
inline bool IsGbufferVertexShader(uint64_t hash) {
	return std::find(GbufferVertexShaders.begin(), GbufferVertexShaders.end(), hash) != GbufferVertexShaders.end();
}
constexpr uint32_t SrtViewProj     = 29; // 16 floats, row-major, clip = M * world
constexpr uint32_t SrtPrevViewProj = 45; // 12 floats: the previous frame's rows x, y and w
constexpr uint32_t SrtJitter       = 57; // 2 floats: clip-space offset (NDC units)

struct Camera {
	std::array<float, 16> view_proj {};
	std::array<float, 16> prev_view_proj {};
	std::array<float, 2>  jitter_ndc {};
	bool                  valid = false;
};

// One presented frame's inputs, owned by the presenter's frame.
struct FrameInputs {
	VulkanImage   depth, motion;
	vk::ImageView depth_view = nullptr, motion_view = nullptr;
	vk::Extent2D  render_extent {};
	Camera        camera;
	uint64_t      guest_frame = 0;
	bool          motion_attachment = false; // the motion copy is a color attachment too
	bool          valid       = false;
	// KYTY_FRAMEGEN_BACKEND=fsr: a copy of the game's interface canvas (its alpha: where the HUD is), when known.
	VulkanImage   ui;
	vk::ImageView ui_view  = nullptr;
	bool          ui_valid = false;
};

// Process start, before the window: loads Streamline when KYTY_FRAMEGEN is set (it then becomes
// SDL's Vulkan loader, and so the proxy of instance, device and swapchain calls).
void Initialize();
// After Initialize: Streamline is loaded (device creation then adds what it relies on but does
// not add itself, VK_KHR_present_id for its VK_NV_low_latency2).
[[nodiscard]] bool Requested();
// Before the device is created: why Streamline says DLSS-G cannot run with this GPU, driver and Windows (e.g.
// hardware-accelerated GPU scheduling turned off), logged; empty when it can, or when frame generation is off. A refused
// device's error repeats it.
[[nodiscard]] std::string SupportProblem(vk::PhysicalDevice physical_device);
// A refused device's advice when this process runs without bottom-up ASLR (issue #45: forcing it on for the emulator in
// Windows' Exploit protection let the driver make the device); empty otherwise.
[[nodiscard]] std::string RefusedDeviceAdvice();
// After the device exists: checks that DLSS-G is available on it.
void OnDevice(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device);
// Right after the dispatcher is loaded for the device (before the recording layer copies it):
// the command-buffer calls Streamline intercepts only for its command-list state tracking
// (disabled here) go straight to the driver.
void BypassCommandHooks(vk::Device device);
[[nodiscard]] bool Enabled();

// Present thread, around vkQueuePresentKHR of the image showing `inputs` (null: a frame
// without inputs, e.g. a menu). `backbuffer` is the swapchain extent, `region` the part the
// game image covers.
void BeforePresent(const FrameInputs* inputs, vk::Extent2D backbuffer, vk::Rect2D region,
                   vk::Format format, uint32_t image_count);
void AfterPresent();
// Present thread: the swapchain is made again (DLSS-G off until frames with inputs turn it on again).
void SwapchainChanging();
// Any thread: a frame with inputs was left unshown (the presenter kept a newer one): the next one resets DLSS-G's history.
void NoteDroppedFrame();
// Present thread: the frames DLSS-G showed per presented frame (1 without generation), as last
// queried (about once a second), for the KYTY_FPS_HUD panel.
[[nodiscard]] uint32_t PresentedFrames();
// KYTY_FPS_HUD: the panel's rectangle, normalized to the game image. Its motion vectors are
// zeroed so generated frames keep the panel in place.
void SetStaticRect(float x0, float y0, float x1, float y1);

// KYTY_FRAMEGEN_BACKEND=fsr: AMD FidelityFX FSR 3.1 frame generation instead of Streamline's DLSS-G, any GPU: one
// generated frame per frame (2x), from the same inputs. amd_fidelityfx_vk.dll (AMD's signed runtime, next to the
// emulator) makes the window's swapchain (its frame generation swapchain: it interpolates and paces the presents on
// queues of its own, GraphicContext::fsr_*), so the swapchain's calls go through it.
// After Initialize: FSR asked for and its runtime loaded (device creation then adds its queues).
[[nodiscard]] bool UsesFsr();
// The video memory FSR takes beside the game's (FidelityFX's allocations, outside the emulator's allocator, and its
// HUD images): what the emulator leaves it when it sizes its use of the budget (small_video_memory).
[[nodiscard]] uint64_t FsrVideoMemoryReserve();
// After the device's queues: FSR is ready when its queues were made.
void OnFsrQueues(GraphicContext& graphics);
// The swapchain is FSR's (Enabled() with the FSR backend).
[[nodiscard]] bool OwnsSwapchain();
// The swapchain calls, FSR's in place of Vulkan's (OwnsSwapchain()). Present thread.
[[nodiscard]] vk::Result CreateSwapchain(GraphicContext& graphics, const vk::SwapchainCreateInfoKHR& info,
                                         vk::SwapchainKHR* swapchain);
void                     DestroySwapchain(GraphicContext& graphics, vk::SwapchainKHR swapchain);
[[nodiscard]] vk::Result GetSwapchainImages(vk::Device device, vk::SwapchainKHR swapchain, uint32_t* count, vk::Image* images);
[[nodiscard]] vk::Result AcquireNextImage(vk::Device device, vk::SwapchainKHR swapchain, uint64_t timeout,
                                          vk::Semaphore semaphore, uint32_t* index);
[[nodiscard]] vk::Result QueuePresent(vk::Queue queue, const vk::PresentInfoKHR& present);
// Present thread, recording the frame's present commands (before its submit): FSR's configuration for the frame
// and its prepare pass on `inputs` (null: no inputs, generation off). `region`: the part of the backbuffer the game
// image covers.
void RecordPrepare(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs, vk::Rect2D region);
// Present thread: a frame without inputs while FSR generates (an idle refresh, a menu's first frames) is not
// presented, up to a few in a row; then generation turns off and they are.
[[nodiscard]] bool SkipsFrame(const FrameInputs* inputs);
// GPU thread (an interface draw, hud-anchor.h): the game's 3840x2160 interface canvas, composited over the 3D into its
// output; FSR is told where the HUD is from its alpha, so that generated frames show the HUD of the game's frame
// instead of moving it with the scene.
void NoteUiCanvas(uint64_t address);
// Present thread, in the frame's present commands right after the game image is blitted into `backbuffer`
// (transfer-destination layout, left so): FSR's HUD-less image of the frame, the backbuffer with the HUD darkened
// (FSR then takes the HUD's pixels from the game's frame). Before the system overlay: that counts as HUD too.
void RecordHudless(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs, vk::Image backbuffer,
                   vk::Extent2D extent, vk::Rect2D region);

// Renderer thread (Kyty.Gpu).
// The guest frame ran the TAA pass: the game's 3D view (play, or a cutscene in the engine), not a movie, a menu or a
// loading screen; with or without frame generation, by the normal or the table way. The guest flip takes it
// (Presenter::PrepareFrame): above 60 Hz the other frames keep 60 (videoOut.cpp).
void NoteScene();
[[nodiscard]] bool TakeScene();
// The current guest frame ran the TAA pass so far, or the last one flipped did (HudAnchor: the game queues a frame's
// interface before the flip of the one before).
[[nodiscard]] bool SceneNoted();
// The guest flips taken so far (TakeScene).
[[nodiscard]] uint64_t GuestFlips();
// The current guest frame has no camera yet: its next G-buffer draw goes the normal way (the table way's draws have
// no constants on the CPU), whose hook reads it.
[[nodiscard]] bool WantsCamera();
void OnDraw(uint64_t vs_hash, std::span<const uint32_t> flattened_srt);
void OnDispatch(uint64_t cs_hash, ImageId depth, uint64_t depth_address, ImageId motion,
                uint64_t motion_address);
// At the guest flip, in the frame's command buffer: copy the gathered images into `inputs`.
void PrepareInputs(GraphicContext& graphics, TextureCache& cache, CommandBuffer& command,
                   FrameInputs& inputs);
void DestroyInputs(GraphicContext& graphics, FrameInputs& inputs);

} // namespace FrameGen
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_LOCAL_FRAME_GEN_H_
