#include "ui/BrazenApp.h"
#include "gpu/GpuTestRunner.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <cstdio>

static void GlfwErrorCallback(int error, const char* description) {
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

int main() {
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    // OpenGL 3.3 core, forward-compatible. This is a step up from the
    // bare "3.0, unspecified profile" request this app used before GPU
    // testing existed: the GPU test needs real shader/VAO/FBO support,
    // and requesting an explicit core + forward-compat profile is also
    // what macOS requires to grant anything newer than OpenGL 2.1 at
    // all, so this is a correctness fix for Mac as much as a GPU-test
    // enabler.
    const char* glslVersion = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(1360, 840, "Brazen Benchmark", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    // Pick up the monitor's content scale (e.g. 2.0 on a Retina/4K display
    // at 200%) so text and controls aren't tiny on high-DPI screens.
    float dpiScaleX = 1.0f, dpiScaleY = 1.0f;
    glfwGetWindowContentScale(window, &dpiScaleX, &dpiScaleY);
    float dpiScale = dpiScaleX;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Dear ImGui's built-in default font renders at 13px, which reads as
    // quite small on most modern displays even before DPI is factored in.
    // Bake a bigger base size in, then apply monitor DPI scale on top.
    // (Baking the size into the font, rather than just using
    // io.FontGlobalScale, keeps the glyphs sharp instead of blurring an
    // upscaled bitmap.)
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 16.0f * dpiScale;
    io.Fonts->AddFontDefault(&fontConfig);

    // A second, larger font used only for the "BRAZEN BENCHMARK" title
    // bar so it reads as an actual header rather than same-size text.
    ImFontConfig titleFontConfig;
    titleFontConfig.SizePixels = 24.0f * dpiScale;
    ImFont* titleFont = io.Fonts->AddFontDefault(&titleFontConfig);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ScaleAllSizes(dpiScale * 0.9f); // padding, spacing, widget sizes, etc.

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // GPU test setup happens after the GL context exists but is
    // otherwise independent of ImGui. If shader compilation or context
    // support fails (e.g. a GPU/driver without real OpenGL 3.3 core
    // support), IsSupported() reports false and BrazenApp shows the GPU
    // test as unavailable with the reason, rather than crashing.
    brazen::GpuTestRunner gpuRunner;
    if (!gpuRunner.Init(reinterpret_cast<brazen::GLGetProcAddressFn>(glfwGetProcAddress))) {
        fprintf(stderr, "GPU test unavailable: %s\n", gpuRunner.GetError().c_str());
    } else {
        fprintf(stderr, "GPU test ready.\n");
    }

    brazen::BrazenApp app;
    app.SetTitleFont(titleFont);
    app.SetGpuRunner(&gpuRunner);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Advance the GPU test (if a run is in progress) before drawing
        // the UI, so a result that completes this frame is reflected in
        // the same frame's results table. RunDraw() renders to its own
        // offscreen framebuffer and restores framebuffer 0 before
        // returning, so this never disturbs what ends up on screen.
        brazen::BenchmarkResult gpuResult;
        if (gpuRunner.PollAndAdvance(&gpuResult))
            app.AddExternalResult(gpuResult);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.DrawFrame();

        // BrazenApp doesn't own the window (main.cpp does), so the
        // sidebar's Quit button just raises a flag; main.cpp is
        // responsible for actually telling GLFW to close.
        if (app.WantsQuit())
            glfwSetWindowShouldClose(window, GLFW_TRUE);

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    gpuRunner.Shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
