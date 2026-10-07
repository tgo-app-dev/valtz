#include "valtz/models/hardware.h"

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>

#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace valtz::models {

namespace {

std::string
sysctl_str(const char* name)
{
  size_t n = 0;
  if (sysctlbyname(name, nullptr, &n, nullptr, 0) != 0 || n == 0) {
    return {};
  }
  std::string s(n, '\0');
  if (sysctlbyname(name, s.data(), &n, nullptr, 0) != 0) {
    return {};
  }
  while (!s.empty() && s.back() == '\0') {
    s.pop_back();
  }
  return s;
}

template <class T>
T
sysctl_num(const char* name)
{
  T v{};
  size_t n = sizeof(v);
  if (sysctlbyname(name, &v, &n, nullptr, 0) != 0) {
    return T{};
  }
  return v;
}

// The GPU core count is a property of the AGX accelerator service.
std::uint32_t
gpu_core_count()
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceMatching("AGXAccelerator"),
                                   &it) != KERN_SUCCESS) {
    return 0;
  }
  std::uint32_t cores = 0;
  for (io_object_t svc = IOIteratorNext(it); svc; svc = IOIteratorNext(it)) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(
        svc, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0);
    if (v) {
      if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        int n = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &n);
        cores = static_cast<std::uint32_t>(n);
      }
      CFRelease(v);
    }
    IOObjectRelease(svc);
    if (cores) {
      break;
    }
  }
  IOObjectRelease(it);
  return cores;
}

// The Neural Engine's cores, as its driver reports them
// (ANEDevicePropertyNumANECores in the H11ANE service's DeviceProperties);
// summed over the engines there are, 0 when none says.
std::uint32_t
ane_core_count()
{
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceMatching("H11ANEIn"),
                                   &it) != KERN_SUCCESS) {
    return 0;
  }
  std::uint32_t cores = 0;
  for (io_object_t svc = IOIteratorNext(it); svc; svc = IOIteratorNext(it)) {
    CFTypeRef props = IORegistryEntryCreateCFProperty(
        svc, CFSTR("DeviceProperties"), kCFAllocatorDefault, 0);
    if (props) {
      if (CFGetTypeID(props) == CFDictionaryGetTypeID()) {
        auto n = CFDictionaryGetValue(static_cast<CFDictionaryRef>(props),
                                      CFSTR("ANEDevicePropertyNumANECores"));
        int v = 0;
        if (n && CFGetTypeID(n) == CFNumberGetTypeID() &&
            CFNumberGetValue(static_cast<CFNumberRef>(n), kCFNumberIntType,
                             &v) &&
            v > 0) {
          cores += static_cast<std::uint32_t>(v);
        }
      }
      CFRelease(props);
    }
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);
  return cores;
}

}

std::uint32_t
ane_cores_of_chip(std::string_view chip)
{
  const auto m = chip.find(" M");
  if (m == std::string_view::npos) {
    return 0;
  }
  std::uint32_t gen = 0;
  for (auto i = m + 2; i < chip.size() && chip[i] >= '0' && chip[i] <= '9';
       ++i) {
    gen = gen * 10 + static_cast<std::uint32_t>(chip[i] - '0');
  }
  if (gen == 0) {
    return 0;
  }
  const std::uint32_t die = gen >= 6 ? 32 : 16;
  return chip.find("Ultra") != std::string_view::npos ? 2 * die : die;
}

HardwareInfo
probe_hardware()
{
  HardwareInfo hw;
  @autoreleasepool {
    hw.chip = sysctl_str("machdep.cpu.brand_string");
    hw.model_id = sysctl_str("hw.model");
    hw.os_version = sysctl_str("kern.osproductversion");
    hw.ram_bytes = sysctl_num<std::uint64_t>("hw.memsize");
    hw.perf_cores = sysctl_num<std::uint32_t>("hw.perflevel0.physicalcpu");
    hw.efficiency_cores =
        sysctl_num<std::uint32_t>("hw.perflevel1.physicalcpu");
    hw.gpu_cores = gpu_core_count();
    // An Ultra's two dies may report as one engine of one die's cores.
    hw.ane_cores = std::max(ane_core_count(), ane_cores_of_chip(hw.chip));

    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev) {
      hw.gpu_matrix_cores = [dev supportsFamily:MTLGPUFamilyApple10];
      hw.gpu_working_set_bytes = dev.recommendedMaxWorkingSetSize;
    }

    switch ([NSProcessInfo processInfo].thermalState) {
    case NSProcessInfoThermalStateNominal:
      hw.thermal = ThermalState::Nominal;
      break;
    case NSProcessInfoThermalStateFair:
      hw.thermal = ThermalState::Fair;
      break;
    case NSProcessInfoThermalStateSerious:
      hw.thermal = ThermalState::Serious;
      break;
    case NSProcessInfoThermalStateCritical:
      hw.thermal = ThermalState::Critical;
      break;
    }
  }
  if (const char* lim = std::getenv("VALTZ_RAM_LIMIT_GB")) {
    if (auto gb = std::strtoull(lim, nullptr, 10); gb > 0) {
      hw.ram_bytes = gb << 30;
    }
  }
  // An older GPU simulated -- what Favor's presets do on it (models/
  // tuning.cc): its core count, and its matrix cores ("0": none, as
  // before M5).
  if (const char* g = std::getenv("VALTZ_GPU_CORES")) {
    if (auto n = std::strtoul(g, nullptr, 10); n > 0) {
      hw.gpu_cores = static_cast<std::uint32_t>(n);
    }
  }
  if (const char* mc = std::getenv("VALTZ_GPU_MATRIX_CORES")) {
    hw.gpu_matrix_cores = std::string_view(mc) != "0";
  }
  return hw;
}

std::uint64_t
reclaimable_ram_bytes()
{
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&vm), &n) !=
      KERN_SUCCESS) {
    return 0;
  }
  const std::uint64_t page = vm_kernel_page_size;
  std::uint64_t room = (static_cast<std::uint64_t>(vm.free_count) +
                        vm.speculative_count + vm.purgeable_count +
                        vm.external_page_count) * page;
  // A smaller box simulated: what it would have left, the same use made
  // of it as of this one.
  if (const char* lim = std::getenv("VALTZ_RAM_LIMIT_GB")) {
    std::uint64_t total = 0;
    std::size_t len = sizeof(total);
    sysctlbyname("hw.memsize", &total, &len, nullptr, 0);
    const std::uint64_t limit = std::strtoull(lim, nullptr, 10) << 30;
    const std::uint64_t used = total > room ? total - room : 0;
    if (limit > 0 && total > limit) {
      room = limit > used ? limit - used : 0;
    }
  }
  return room;
}

std::uint64_t
used_ram_bytes()
{
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&vm), &n) !=
      KERN_SUCCESS) {
    return 0;
  }
  const std::uint64_t page = vm_kernel_page_size;
  const std::uint64_t app =
      vm.internal_page_count > vm.purgeable_count
          ? static_cast<std::uint64_t>(vm.internal_page_count) -
                vm.purgeable_count
          : 0;
  return (app + vm.wire_count + vm.compressor_page_count) * page;
}

std::uint64_t
free_disk_bytes(const std::filesystem::path& p)
{
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:
                            [NSString stringWithUTF8String:p.c_str()]];
    NSNumber* v = nil;
    // "Important usage" counts purgeable space the system will free on
    // demand, which is what a multi-GB model download can actually use.
    if ([url getResourceValue:&v
                       forKey:NSURLVolumeAvailableCapacityForImportantUsageKey
                        error:nil] &&
        v) {
      return v.unsignedLongLongValue;
    }
  }
  struct statfs st;
  if (statfs(p.c_str(), &st) == 0) {
    return static_cast<std::uint64_t>(st.f_bavail) * st.f_bsize;
  }
  return 0;
}

const char*
to_str(ThermalState t)
{
  switch (t) {
  case ThermalState::Nominal:  return "nominal";
  case ThermalState::Fair:     return "fair";
  case ThermalState::Serious:  return "serious";
  case ThermalState::Critical: return "critical";
  }
  return "?";
}

void
to_json(Json& j, const HardwareInfo& h)
{
  j = {
    {"chip", h.chip},
    {"model_id", h.model_id},
    {"os_version", h.os_version},
    {"ram_gb", h.ram_gb()},
    {"perf_cores", h.perf_cores},
    {"efficiency_cores", h.efficiency_cores},
    {"gpu_cores", h.gpu_cores},
    {"gpu_matrix_cores", h.gpu_matrix_cores},
    {"ane_cores", h.ane_cores},
    {"gpu_working_set_gb",
     static_cast<double>(h.gpu_working_set_bytes) / (1ull << 30)},
    {"thermal", to_str(h.thermal)},
  };
}

}
