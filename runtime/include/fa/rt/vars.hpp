#pragma once
#include <cstdint>
#include <string>
namespace fa::rt {
// True if the export name is a data export (kVariable in the XenonUtils tables).
bool is_kernel_variable(const std::string& name);
// Guest address of the named kernel data export, allocating and initialising it on first use.
uint32_t kernel_variable(const std::string& name);
// Guest interrupt time in 100 ns units since the first call (same base as KeTimeStampBundle +0).
uint64_t interrupt_time_100ns();
}
