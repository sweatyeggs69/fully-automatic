#pragma once
#include <cstdint>
#include <string>
namespace fa::rt {
// Guest address of the named kernel data export, allocating and initialising it on first use.
uint32_t kernel_variable(const std::string& name);
}
