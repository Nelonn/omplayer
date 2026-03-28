#include "media_player.hpp"
#include "ui.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlgpu3.h>
#include <imgui.h>

#include <memory>

struct AppState {
  SDL_Window* window = nullptr;
  SDL_GPUDevice* device = nullptr;
  MediaPlayer player;
  std::unique_ptr<PlayerUI> ui;
};

auto SDL_AppInit(void **appstate, int argc, char *argv[]) -> SDL_AppResult {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return SDL_APP_FAILURE;

  auto* state = new AppState();
  *appstate = state;

  state->window = SDL_CreateWindow(
      "OMPlayer", 160 * 4, 90 * 4,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!state->window) return SDL_APP_FAILURE;

  state->device = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
  if (!state->device) return SDL_APP_FAILURE;

  if (!SDL_ClaimWindowForGPUDevice(state->device, state->window)) {
    SDL_Log("Error: SDL_ClaimWindowForGPUDevice(): %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }
  SDL_SetGPUSwapchainParameters(state->device, state->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

  // Initialize ImGui
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsDark();

  ImGui_ImplSDL3_InitForSDLGPU(state->window);

  ImGui_ImplSDLGPU3_InitInfo init_info = {};
  init_info.Device = state->device;
  init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(state->device, state->window);
  init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
  init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
  ImGui_ImplSDLGPU3_Init(&init_info);

  state->player.setDevice(state->device);
  state->ui = std::make_unique<PlayerUI>(state->player);

  if (argc > 1) {
    state->player.play(argv[1]);
  }

  return SDL_APP_CONTINUE;
}

auto SDL_AppEvent(void* appstate, SDL_Event* event) -> SDL_AppResult {
  auto* state = static_cast<AppState*>(appstate);
  ImGui_ImplSDL3_ProcessEvent(event);
  if (!state->ui->handleEvent(*event)) return SDL_APP_SUCCESS;
  return SDL_APP_CONTINUE;
}

auto SDL_AppIterate(void* appstate) -> SDL_AppResult {
  auto* state = static_cast<AppState*>(appstate);

  state->player.tickVideo();

  ImGui_ImplSDLGPU3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  state->ui->render(state->window);

  ImGui::Render();
  ImDrawData* draw_data = ImGui::GetDrawData();

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(state->device);
  if (!cmd) return SDL_APP_CONTINUE;

  SDL_GPUTexture* swapchain_tex;
  if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, state->window, &swapchain_tex, nullptr, nullptr)) {
    if (swapchain_tex) {
      ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);

      SDL_GPUColorTargetInfo color_target = {};
      color_target.texture = swapchain_tex;
      color_target.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
      color_target.load_op = SDL_GPU_LOADOP_CLEAR;
      color_target.store_op = SDL_GPU_STOREOP_STORE;

      SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, nullptr);

      ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, render_pass);

      SDL_EndGPURenderPass(render_pass);
    }
  }

  SDL_SubmitGPUCommandBuffer(cmd);

  return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult /*result*/) {
  auto* state = static_cast<AppState*>(appstate);
  if (state) {
    state->player.stop();
    SDL_WaitForGPUIdle(state->device);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(state->device, state->window);
    SDL_DestroyGPUDevice(state->device);
    SDL_DestroyWindow(state->window);
    delete state;
  }
  SDL_Quit();
}
