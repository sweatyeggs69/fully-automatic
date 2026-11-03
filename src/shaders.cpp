#include <iostream>
#include <vector>
#include <string>

// --- FullAuto Native: Shaders ---
// These are the GLSL shaders for the initial "Boot" screen.

const char* vertexShaderSource = R"(
#version 450

layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec2 inTexCoord;

out vec2 outTexCoord;

void main() {
    gl_Position = vec4(inPosition, 1.0);
    outTexCoord = inTexCoord;
}
)
";

const char* fragmentShaderSource = R"(
#version 450

layout (location = 0) in vec2 inTexCoord;
layout (location = 0) out vec4 outColor;

uniform sampler2D texSampler;

void main() {
    outColor = texture(texSampler, inTexCoord);
}
)
";

int main() {
    std::cout << "Shader source files generated successfully." << std::endl;
    std::cout << "Next step: Integrating these into the Vulkan Pipeline in renderer.cpp." << std::endl;
    return 0;
}
