#include <windows.h>

#include "memory.hpp"

#include <cstring>

namespace {

constexpr wchar_t POLARIS_MODULE[] = L"Polaris-Win64-Shipping.exe";

template <typename T>
T read_at(std::uintptr_t addr) {
    if (!addr) return T{};
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(T));
    return value;
}

template <typename T>
void write_at(std::uintptr_t addr, T value) {
    if (!addr) return;
    std::memcpy(reinterpret_cast<void*>(addr), &value, sizeof(T));
}

}  // namespace

std::uintptr_t opendojo::memory::polaris_base() {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(POLARIS_MODULE));
}

std::uint64_t opendojo::memory::read_u64(std::uintptr_t addr) { return read_at<std::uint64_t>(addr); }
std::uint32_t opendojo::memory::read_u32(std::uintptr_t addr) { return read_at<std::uint32_t>(addr); }
std::uint16_t opendojo::memory::read_u16(std::uintptr_t addr) { return read_at<std::uint16_t>(addr); }
std::uint8_t  opendojo::memory::read_u8 (std::uintptr_t addr) { return read_at<std::uint8_t >(addr); }

void opendojo::memory::write_u64(std::uintptr_t addr, std::uint64_t v) { write_at(addr, v); }
void opendojo::memory::write_u32(std::uintptr_t addr, std::uint32_t v) { write_at(addr, v); }
void opendojo::memory::write_u16(std::uintptr_t addr, std::uint16_t v) { write_at(addr, v); }
void opendojo::memory::write_u8 (std::uintptr_t addr, std::uint8_t  v) { write_at(addr, v); }

void opendojo::memory::read_bytes(std::uintptr_t addr, void* out, std::size_t n) {
    if (!addr || !out || !n) return;
    std::memcpy(out, reinterpret_cast<const void*>(addr), n);
}

void opendojo::memory::write_bytes(std::uintptr_t addr, const void* src, std::size_t n) {
    if (!addr || !src || !n) return;
    std::memcpy(reinterpret_cast<void*>(addr), src, n);
}
