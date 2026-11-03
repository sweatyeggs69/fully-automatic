#include "renderer.hpp"
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <cstring>

#include <array>

struct Vertex {
    float x, y;
    float u, v;
};

const std::vector<Vertex> quad_vertices = {
    {-1.0f,  1.0f,  0.0f, 0.0f},
    { 1.0f,  1.0f,  1.0f, 0.0f},
    {-1.0f, -1.0f,  0.0f, 1.0f},
    { 1.0f, -1.0f,  1.0f, 1.0f},
};

