#include <GL/glew.h>
#include <SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "engine/anim/skinning.hpp"
#include "engine/core/resource_manager.hpp"
#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/texture_cache.hpp"

namespace {

std::string default_bf2_archive() {
  return R"(C:\Program Files (x86)\Battlefield2\mods\bf2\Objects_client.zip)";
}

bool ends_with(const std::string& s, const std::string& suffix) {
  if (s.size() < suffix.size()) return false;
  return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
                    [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

}  // namespace

int main(int argc, char** argv) {
  std::string archive_path = argc > 1 ? argv[1] : default_bf2_archive();
  std::string selected_mesh = "develop/cube/meshes/cube.staticmesh";

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
    return 1;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

  SDL_Window* window =
      SDL_CreateWindow("BF2 Respawn Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280,
                       720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
  if (!window) {
    std::cerr << "Window creation failed: " << SDL_GetError() << '\n';
    return 1;
  }

  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK) {
    std::cerr << "GLEW init failed\n";
    return 1;
  }
  SDL_GL_SetSwapInterval(1);

  bf2::Renderer renderer;
  if (!renderer.initialize(window)) {
    std::cerr << "Renderer init failed\n";
    return 1;
  }

  ImGui::CreateContext();
  ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
  ImGui_ImplOpenGL3_Init("#version 330");

  bf2::ResourceManager resources;
  if (std::filesystem::exists(archive_path)) {
    resources.archives().mount(archive_path, "Objects");
  }

  bf2::TextureCache textures(resources, renderer);
  bf2::GpuMesh gpu_mesh{};
  bf2::GpuTexturedMesh gpu_textured{};
  bf2::GpuSkinnedMesh gpu_skinned{};
  bool mesh_loaded = false;
  bool textured_loaded = false;
  bool skinned_loaded = false;
  bool use_textures = true;
  float yaw = 0.f;
  float pitch = 20.f;
  float distance = 4.f;
  bool running = true;

  // Skinned-mesh state.
  bf2::Mesh skinned_source;
  bf2::Skeleton skeleton;
  bf2::AnimationClip clip;
  bool has_clip = false;
  int geom_index = 0;
  int frame = 0;
  bool playing = true;
  char skeleton_path[256] = "soldiers/Common/Animations/3p_setup.ske";
  char animation_path[256] = "soldiers/Common/Animations/3P/3p_runForward.baf";
  std::string skinned_status;

  auto reload_mesh = [&]() {
    if (mesh_loaded) {
      renderer.destroy_mesh(gpu_mesh);
      mesh_loaded = false;
    }
    if (textured_loaded) {
      renderer.destroy_textured(gpu_textured);
      textured_loaded = false;
    }
    if (skinned_loaded) {
      renderer.destroy_skinned(gpu_skinned);
      skinned_loaded = false;
    }
    try {
      const auto mesh = resources.load_mesh(selected_mesh);
      if (use_textures) {
        const auto data = bf2::MeshLoader::extract_textured(mesh);
        gpu_textured = renderer.upload_textured(data);
        for (std::size_t i = 0; i < gpu_textured.submeshes.size() && i < data.submeshes.size();
             ++i) {
          gpu_textured.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map);
          gpu_textured.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map);
        }
        textured_loaded = gpu_textured.vao != 0;
      }
      if (!textured_loaded) {
        const auto extracted = bf2::MeshLoader::extract_geometry(mesh);
        gpu_mesh = renderer.upload_mesh(extracted);
        mesh_loaded = true;
      }
    } catch (const std::exception& ex) {
      std::cerr << "Failed to load mesh: " << ex.what() << '\n';
    }
  };

  auto reload_skinned = [&]() {
    if (skinned_loaded) {
      renderer.destroy_skinned(gpu_skinned);
      skinned_loaded = false;
    }
    if (mesh_loaded) {
      renderer.destroy_mesh(gpu_mesh);
      mesh_loaded = false;
    }
    if (textured_loaded) {
      renderer.destroy_textured(gpu_textured);
      textured_loaded = false;
    }
    has_clip = false;
    skinned_status.clear();
    try {
      const auto mesh_bytes = resources.read_bytes(selected_mesh);
      const auto ske_bytes = resources.read_bytes(skeleton_path);
      if (!mesh_bytes || !ske_bytes) {
        skinned_status = "mesh or skeleton not found";
        return;
      }
      skinned_source = bf2::MeshLoader::load_from_memory(*mesh_bytes, bf2::MeshKind::Skinned);
      skeleton = bf2::SkeletonLoader::load_from_memory(*ske_bytes);
      if (animation_path[0] != '\0') {
        if (const auto baf = resources.read_bytes(animation_path)) {
          clip = bf2::AnimationLoader::load_from_memory(*baf);
          has_clip = true;
        }
      }
      geom_index = std::clamp(geom_index, 0,
                              static_cast<int>(skinned_source.geometries.size()) - 1);
      const auto geometry =
          bf2::extract_skinned(skinned_source, skeleton, static_cast<std::size_t>(geom_index), 0);
      gpu_skinned = renderer.upload_skinned(geometry);
      skinned_loaded = gpu_skinned.vao != 0;
      frame = 0;
      skinned_status = std::to_string(geometry.vertices.size()) + " verts, " +
                       std::to_string(skeleton.nodes.size()) + " bones" +
                       (has_clip ? ", clip " + std::to_string(clip.frame_count) + "f" : "");
    } catch (const std::exception& ex) {
      skinned_status = std::string("error: ") + ex.what();
      std::cerr << "Failed to load skinned mesh: " << ex.what() << '\n';
    }
  };

  auto load_selected = [&]() {
    if (ends_with(selected_mesh, ".skinnedmesh")) {
      reload_skinned();
    } else {
      reload_mesh();
    }
  };

  load_selected();

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) {
        running = false;
      }
      if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        renderer.set_viewport(event.window.data1, event.window.data2);
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Archive Browser");
    ImGui::Text("Archive: %s", archive_path.c_str());
    ImGui::TextWrapped("Selected: %s", selected_mesh.c_str());
    if (ImGui::Button("Reload")) {
      load_selected();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Textures", &use_textures)) {
      load_selected();
    }

    static char filter[128] = "skinnedmesh";
    ImGui::InputText("Filter", filter, sizeof(filter));
    const auto entries = resources.archives().list();
    ImGui::BeginChild("entries", ImVec2(0, 220), true);
    for (const auto& entry : entries) {
      if (entry.find(filter) == std::string::npos) {
        continue;
      }
      if (ImGui::Selectable(entry.c_str(), entry == selected_mesh)) {
        selected_mesh = entry;
        load_selected();
      }
    }
    ImGui::EndChild();

    if (ends_with(selected_mesh, ".skinnedmesh")) {
      ImGui::Separator();
      ImGui::Text("Skinning (GPU)");
      ImGui::InputText("Skeleton", skeleton_path, sizeof(skeleton_path));
      ImGui::InputText("Animation", animation_path, sizeof(animation_path));
      if (!skinned_source.geometries.empty()) {
        const int max_geom = static_cast<int>(skinned_source.geometries.size()) - 1;
        if (ImGui::SliderInt("Geometry", &geom_index, 0, max_geom)) {
          reload_skinned();
        }
      }
      if (ImGui::Button("Apply")) {
        reload_skinned();
      }
      ImGui::SameLine();
      ImGui::Checkbox("Play", &playing);
      if (has_clip && clip.frame_count > 0) {
        ImGui::SliderInt("Frame", &frame, 0, clip.frame_count - 1);
      }
      if (!skinned_status.empty()) {
        ImGui::TextWrapped("%s", skinned_status.c_str());
      }
    }
    ImGui::End();

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    renderer.set_viewport(width, height);
    renderer.begin_frame(0.08f, 0.1f, 0.12f);

    if (mesh_loaded || textured_loaded || skinned_loaded) {
      yaw += 0.25f;
      const float cam_dist = skinned_loaded ? 3.2f : distance;
      const float target_y = skinned_loaded ? 1.0f : 0.0f;
      const glm::vec3 target(0.f, target_y, 0.f);
      const glm::vec3 eye =
          target + glm::vec3(cam_dist * std::cos(glm::radians(pitch)) * std::sin(glm::radians(yaw)),
                             cam_dist * std::sin(glm::radians(pitch)),
                             cam_dist * std::cos(glm::radians(pitch)) * std::cos(glm::radians(yaw)));
      const auto view = glm::lookAt(eye, target, glm::vec3(0.f, 1.f, 0.f));
      const auto projection =
          glm::perspective(glm::radians(60.f), static_cast<float>(width) / std::max(height, 1),
                           0.1f, 100.f);
      const auto mvp = projection * view;

      if (skinned_loaded) {
        if (playing && has_clip && clip.frame_count > 0) {
          frame = (frame + 1) % clip.frame_count;
        }
        const bf2::AnimationClip* clip_ptr = has_clip ? &clip : nullptr;
        const auto palette = bf2::compute_skin_palette(
            skinned_source, skeleton, clip_ptr, frame, static_cast<std::size_t>(geom_index), 0);
        if (!palette.empty()) {
          renderer.draw_skinned(gpu_skinned, &mvp[0][0], &palette[0][0][0],
                                static_cast<int>(palette.size()));
        }
      } else if (textured_loaded) {
        renderer.draw_textured(gpu_textured, &mvp[0][0]);
      } else {
        renderer.draw_mesh(gpu_mesh, &mvp[0][0]);
      }
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    renderer.end_frame();
    SDL_GL_SwapWindow(window);
  }

  if (mesh_loaded) {
    renderer.destroy_mesh(gpu_mesh);
  }
  if (textured_loaded) {
    renderer.destroy_textured(gpu_textured);
  }
  if (skinned_loaded) {
    renderer.destroy_skinned(gpu_skinned);
  }
  textures.clear();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  renderer.shutdown();
  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
