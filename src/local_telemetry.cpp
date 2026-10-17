#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "local_telemetry.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>
#include <winioctl.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
std::string lowercase(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
class WindowsCpu {
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER frequency_ = nullptr, zones_ = nullptr;
    bool preciseZones_ = true, hasTimes_ = false;
    local_telemetry::CpuTimes previous_{};
    std::uint32_t cores_ = 0, logical_ = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    static bool valid(DWORD status) { return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA; }
public:
    WindowsCpu() {
        DWORD size = 0; GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
        if (size && size < 1024 * 1024) {
            std::vector<unsigned char> buffer(size);
            if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &size)) {
                for (std::size_t at = 0; at + 8 <= size;) {
                    const auto* item = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
                    if (item->Size < 8 || item->Size > size - at) break;
                    if (item->Relationship == RelationProcessorCore) ++cores_;
                    at += item->Size;
                }
            }
        }
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return;
        PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\Processor Frequency", 0, &frequency_);
        if (PdhAddEnglishCounterW(query_, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &zones_) != ERROR_SUCCESS) {
            preciseZones_ = false;
            PdhAddEnglishCounterW(query_, L"\\Thermal Zone Information(*)\\Temperature", 0, &zones_);
        }
    }
    ~WindowsCpu() { if (query_) PdhCloseQuery(query_); }
    void sample(TelemetrySnapshot& out) {
        out.cpuPhysicalCores = cores_; out.cpuLogicalProcessors = logical_;
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            auto ticks = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
            const local_telemetry::CpuTimes current{ticks(idle), ticks(kernel), ticks(user)};
            if (hasTimes_) out.cpuUsagePercent = local_telemetry::cpuUsage(previous_, current);
            previous_ = current; hasTimes_ = true;
        } else hasTimes_ = false;
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return;
        PDH_FMT_COUNTERVALUE value{};
        if (frequency_ && PdhGetFormattedCounterValue(frequency_, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS &&
            valid(value.CStatus) && std::isfinite(value.doubleValue) && value.doubleValue > 0 && value.doubleValue <= 10000)
            out.cpuClockMHz = value.doubleValue;
        if (!zones_) return;
        DWORD bytes = 0, count = 0;
        constexpr DWORD temperatureFormat = PDH_FMT_DOUBLE | PDH_FMT_NOSCALE;
        if (PdhGetFormattedCounterArrayW(zones_, temperatureFormat, &bytes, &count, nullptr) != PDH_MORE_DATA ||
            !bytes || bytes > 1024 * 1024) return;
        std::vector<unsigned char> buffer(bytes);
        auto* values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(zones_, temperatureFormat, &bytes, &count, values) != ERROR_SUCCESS) return;
        for (DWORD i = 0; i < count; ++i) if (valid(values[i].FmtValue.CStatus)) {
            const double celsius = local_telemetry::thermalZoneCelsius(values[i].FmtValue.doubleValue, preciseZones_);
            if (celsius >= 0) out.thermalZoneTemperature = std::max(out.thermalZoneTemperature, celsius);
        }
    }
};
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), count, nullptr, nullptr);
    return out;
}
std::uint64_t firmwareMemoryRate() {
    constexpr DWORD rsmb = 0x52534D42;
    const UINT size = GetSystemFirmwareTable(rsmb, 0, nullptr, 0);
    if (size < 8 || size > 16 * 1024 * 1024) return 0;
    std::vector<unsigned char> bytes(size);
    if (GetSystemFirmwareTable(rsmb, 0, bytes.data(), size) != size) return 0;
    DWORD length = 0; std::memcpy(&length, bytes.data() + 4, 4);
    if (length > size - 8) return 0;
    return local_telemetry::configuredMemoryRate(std::span(bytes).subspan(8, length));
}
std::string computerName() {
    wchar_t name[256]{}; DWORD size = 256;
    return GetComputerNameExW(ComputerNameDnsHostname, name, &size) ? utf8(name) : "This PC";
}
std::string cpuName() {
    wchar_t name[512]{}; DWORD size = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS) return {};
    return utf8(name);
}
std::wstring systemRoot() {
    wchar_t windows[MAX_PATH]{}, root[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, MAX_PATH) || !GetVolumePathNameW(windows, root, MAX_PATH)) return {};
    return root;
}
struct PageUsage { std::uint64_t used = 0, total = 0; };
BOOL CALLBACK pageFile(LPVOID context, PENUM_PAGE_FILE_INFORMATION info, LPCWSTR) {
    auto& value = *static_cast<PageUsage*>(context);
    value.used += info->TotalInUse; value.total += info->TotalSize;
    return TRUE;
}

// Windows SDK-compatible read-only storage temperature descriptor.
struct TemperatureInfo {
    WORD index; SHORT temperature, over, under;
    BYTE overChangeable, underChangeable, eventGenerated, reserved;
    DWORD reserved1;
};
struct TemperatureDescriptor {
    DWORD version, size; SHORT critical, warning; WORD count; BYTE reserved[2]; DWORD reserved1[2];
    TemperatureInfo temperatures[1];
};
HANDLE openTemperatureDisk(const std::wstring& root) {
    if (root.size() < 2 || root[1] != L':') return INVALID_HANDLE_VALUE;
    const auto volume = L"\\\\.\\" + root.substr(0, 2);
    HANDLE handle = CreateFileW(volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return handle;
    alignas(VOLUME_DISK_EXTENTS) unsigned char buffer[1024]{}; DWORD returned = 0;
    const bool ok = DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
        buffer, sizeof(buffer), &returned, nullptr) != FALSE;
    CloseHandle(handle);
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer);
    if (!ok || returned < sizeof(VOLUME_DISK_EXTENTS) || extents->NumberOfDiskExtents != 1) return INVALID_HANDLE_VALUE;
    const auto path = L"\\\\.\\PhysicalDrive" + std::to_wstring(extents->Extents[0].DiskNumber);
    return CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}
double diskTemperature(HANDLE disk) {
    if (disk == INVALID_HANDLE_VALUE) return -1;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceTemperatureProperty; query.QueryType = PropertyStandardQuery;
    alignas(TemperatureDescriptor) unsigned char bytes[1024]{}; DWORD size = 0;
    if (!DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), bytes, sizeof(bytes), &size, nullptr) ||
        size < sizeof(TemperatureDescriptor)) return -1;
    const auto& data = *reinterpret_cast<const TemperatureDescriptor*>(bytes);
    const std::size_t available = (size - offsetof(TemperatureDescriptor, temperatures)) / sizeof(TemperatureInfo);
    if (!data.count || data.count > available) return -1;
    // Index zero is the composite/device temperature, not an optional hot spot.
    for (std::size_t i = 0; i < data.count; ++i) {
        const auto* temp = reinterpret_cast<const TemperatureInfo*>(bytes + offsetof(TemperatureDescriptor, temperatures));
        if (temp[i].index == 0 && temp[i].temperature >= 0 && temp[i].temperature <= 150) return temp[i].temperature;
    }
    return -1;
}

class Nvidia {
    HMODULE module_ = nullptr;
    bool initialized_ = false;
    template<class T> T proc(const char* name) const { return reinterpret_cast<T>(GetProcAddress(module_, name)); }
public:
    Nvidia() {
        module_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) {
            wchar_t program[MAX_PATH]{};
            const DWORD size = GetEnvironmentVariableW(L"ProgramW6432", program, MAX_PATH);
            if (size && size < MAX_PATH) {
                const auto path = std::wstring(program) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
                module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
        }
        if (module_) { const auto init = proc<int(*)()>("nvmlInit_v2"); initialized_ = init && init() == 0; }
    }
    ~Nvidia() {
        if (initialized_) if (auto shutdown = proc<int(*)()>("nvmlShutdown")) shutdown();
        if (module_) FreeLibrary(module_);
    }
    void sample(TelemetrySnapshot& snapshot) const {
        if (!initialized_) return;
        const auto count = proc<int(*)(unsigned*)>("nvmlDeviceGetCount_v2");
        const auto handle = proc<int(*)(unsigned, void**)>("nvmlDeviceGetHandleByIndex_v2");
        unsigned devices = 0;
        if (!count || !handle || count(&devices)) return;
        for (unsigned i = 0; i < std::min(devices, 16u); ++i) {
            void* device = nullptr; if (handle(i, &device)) continue;
            GpuMetrics gpu;
            char name[128]{};
            if (auto fn = proc<int(*)(void*, char*, unsigned)>("nvmlDeviceGetName"); fn && !fn(device, name, sizeof(name))) gpu.name = name;
            unsigned value = 0;
            if (auto fn = proc<int(*)(void*, unsigned, unsigned*)>("nvmlDeviceGetTemperature"); fn && !fn(device, 0, &value) && value <= 150) gpu.temperature = value;
            if (auto fn = proc<int(*)(void*, unsigned*)>("nvmlDeviceGetFanSpeed"); fn && !fn(device, &value) && value <= 100) gpu.fanPercent = value;
            if (auto fn = proc<int(*)(void*, unsigned*)>("nvmlDeviceGetPowerUsage"); fn && !fn(device, &value) && value <= 10000000) gpu.powerWatts = value / 1000.0;
            if (auto fn = proc<int(*)(void*, unsigned, unsigned*)>("nvmlDeviceGetClockInfo"); fn && !fn(device, 2, &value) && value <= 100000) gpu.memoryClockMHz = value;
            struct Memory { unsigned long long total, free, used; } memory{};
            if (auto fn = proc<int(*)(void*, Memory*)>("nvmlDeviceGetMemoryInfo"); fn && !fn(device, &memory) && memory.used <= memory.total) {
                gpu.memoryUsed = memory.used; gpu.memoryTotal = memory.total;
            }
            snapshot.gpus.push_back(std::move(gpu));
        }
    }
};

class Sensors {
    bool com_ = false;
    IWbemLocator* locator_ = nullptr;
    IWbemServices* service_ = nullptr;
    std::chrono::steady_clock::time_point attempted_{};
    void connect() {
        if (!locator_ || service_) return;
        const auto now = std::chrono::steady_clock::now();
        if (attempted_.time_since_epoch().count() && now - attempted_ < std::chrono::seconds(30)) return;
        attempted_ = now;
        for (const auto* path : {L"ROOT\\LibreHardwareMonitor", L"ROOT\\OpenHardwareMonitor"}) {
            BSTR name = SysAllocString(path);
            const auto result = locator_->ConnectServer(name, nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &service_);
            SysFreeString(name);
            if (SUCCEEDED(result)) break;
        }
        if (service_) CoSetProxyBlanket(service_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    }
    static std::string text(IWbemClassObject* item, const wchar_t* key) {
        VARIANT value; VariantInit(&value); std::string out;
        if (SUCCEEDED(item->Get(key, 0, &value, nullptr, nullptr)) && value.vt == VT_BSTR && value.bstrVal) out = utf8(value.bstrVal);
        VariantClear(&value); return out;
    }
public:
    Sensors() {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (!com_) return;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&locator_)))) return;
        connect();
    }
    ~Sensors() { if (service_) service_->Release(); if (locator_) locator_->Release(); if (com_) CoUninitialize(); }
    void sample(TelemetrySnapshot& snapshot) {
        connect();
        if (!service_) return;
        BSTR language = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT Name,Identifier,SensorType,Value FROM Sensor WHERE SensorType='Temperature' OR SensorType='Power'");
        IEnumWbemClassObject* rows = nullptr;
        const auto result = service_->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows);
        SysFreeString(language); SysFreeString(query);
        if (FAILED(result) || !rows) { service_->Release(); service_ = nullptr; attempted_ = std::chrono::steady_clock::now(); return; }
        std::vector<local_telemetry::CpuSensorReading> readings;
        for (int i = 0; i < 128; ++i) {
            IWbemClassObject* item = nullptr; ULONG count = 0;
            if (rows->Next(100, 1, &item, &count) != WBEM_S_NO_ERROR || !count) break;
            const auto name = text(item, L"Name"), id = text(item, L"Identifier"), type = text(item, L"SensorType");
            VARIANT value, number; VariantInit(&value); VariantInit(&number);
            double reading = -1;
            if (SUCCEEDED(item->Get(L"Value", 0, &value, nullptr, nullptr)) && value.vt != VT_EMPTY && value.vt != VT_NULL &&
                SUCCEEDED(VariantChangeType(&number, &value, 0, VT_R8))) reading = number.dblVal;
            VariantClear(&number); VariantClear(&value); item->Release();
            if (!std::isfinite(reading) || reading < 0) continue;
            readings.push_back({name, id, type, reading});
            if ((id.starts_with("/nvme/") || id.starts_with("/ssd/")) && type == "Temperature" && reading <= 150 && snapshot.ssdTemperature < 0)
                snapshot.ssdTemperature = reading;
        }
        rows->Release();
        local_telemetry::applyCpuSensors(snapshot, readings);
    }
};
}

double local_telemetry::cpuUsage(CpuTimes previous, CpuTimes current) {
    if (current.idle < previous.idle || current.kernel < previous.kernel || current.user < previous.user) return -1;
    const double idle = static_cast<double>(current.idle - previous.idle);
    const double total = static_cast<double>(current.kernel - previous.kernel) + static_cast<double>(current.user - previous.user);
    if (total <= 0 || idle > total) return -1;
    return std::clamp(100.0 * (total - idle) / total, 0.0, 100.0);
}

double local_telemetry::thermalZoneCelsius(double raw, bool highPrecision) {
    if (!std::isfinite(raw)) return -1;
    const double result = raw / (highPrecision ? 10.0 : 1.0) - 273.15;
    return result >= 0 && result <= 150 ? result : -1;
}

void local_telemetry::applyCpuSensors(TelemetrySnapshot& snapshot, std::span<const CpuSensorReading> readings) {
    int priority = 0;
    for (const auto& item : readings) {
        const auto id = lowercase(item.identifier), name = lowercase(item.name), type = lowercase(item.type);
        if ((!id.starts_with("/amdcpu/") && !id.starts_with("/intelcpu/")) ||
            !std::isfinite(item.value) || item.value < 0) continue;
        if (type == "power" && item.value <= 10000 && name.find("package") != std::string::npos &&
            name.find("limit") == std::string::npos && name.find("tdp") == std::string::npos)
            snapshot.packagePowerWatts = std::max(snapshot.packagePowerWatts, item.value);
        if (type != "temperature" || item.value > 150) continue;
        if (name.find("ccd") != std::string::npos) {
            if (name.find("average") == std::string::npos && name.find("max") == std::string::npos)
                snapshot.chipletTemperatures.push_back({item.name, item.value});
            continue;
        }
        const int rank = name.find("tdie") != std::string::npos ? 4 :
            name.find("tctl") != std::string::npos ? 3 : name.find("package") != std::string::npos ? 2 : 0;
        if (rank) {
            if (rank > priority) snapshot.packageTemperature = item.value;
            else if (rank == priority) snapshot.packageTemperature = std::max(snapshot.packageTemperature, item.value);
            priority = std::max(priority, rank);
        } else if (name.find("core") != std::string::npos &&
            std::any_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); }))
            snapshot.coreTemperatures.push_back({item.name, item.value});
    }
    if (!priority) for (const auto& core : snapshot.coreTemperatures)
        snapshot.packageTemperature = std::max(snapshot.packageTemperature, core.value);
}

std::uint64_t local_telemetry::configuredMemoryRate(std::span<const unsigned char> bytes) {
    std::uint64_t minimum = 0;
    std::size_t offset = 0;
    while (offset + 4 <= bytes.size()) {
        const auto type = bytes[offset], size = bytes[offset + 1];
        if (size < 4 || size > bytes.size() - offset) break;
        auto u16 = [&](std::size_t at) { return unsigned(bytes[offset + at]) | unsigned(bytes[offset + at + 1]) << 8; };
        if (type == 17 && size >= 34 && u16(12) != 0) {
            std::uint64_t rate = u16(32);
            if (rate == 0xFFFF && size >= 92) {
                rate = 0; for (unsigned i = 0; i < 4; ++i) rate |= std::uint64_t(bytes[offset + 88 + i]) << (i * 8);
            }
            if (rate > 0 && rate <= 100000 && rate != 0xFFFF) minimum = minimum ? std::min(minimum, rate) : rate;
        }
        std::size_t end = offset + size;
        while (end + 1 < bytes.size() && (bytes[end] || bytes[end + 1])) ++end;
        if (end + 1 >= bytes.size()) break;
        offset = end + 2;
        if (type == 127) break;
    }
    return minimum;
}

struct LocalTelemetry::Impl {
    std::string host = computerName(), cpu = cpuName();
    std::wstring root = systemRoot();
    std::uint64_t memoryRate = firmwareMemoryRate();
    HANDLE disk = openTemperatureDisk(root);
    WindowsCpu cpuMetrics;
    Nvidia nvidia;
    Sensors sensors;
    std::chrono::steady_clock::time_point previous{};
    std::uint64_t luid = 0, rx = 0, tx = 0;
    ~Impl() { if (disk != INVALID_HANDLE_VALUE) CloseHandle(disk); }

    void network(TelemetrySnapshot& out) {
        MIB_IF_TABLE2* table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR) { previous = {}; return; }
        MIB_IF_ROW2* best = nullptr;
        MIB_IPFORWARD_TABLE2* routes = nullptr;
        std::uint64_t metric = std::numeric_limits<std::uint64_t>::max();
        if (GetIpForwardTable2(AF_UNSPEC, &routes) == NO_ERROR) {
            for (ULONG r = 0; r < routes->NumEntries; ++r) {
                const auto& route = routes->Table[r];
                if (route.DestinationPrefix.PrefixLength != 0) continue;
                MIB_IPINTERFACE_ROW ip{}; ip.Family = route.DestinationPrefix.Prefix.si_family; ip.InterfaceLuid = route.InterfaceLuid;
                if (GetIpInterfaceEntry(&ip) != NO_ERROR) continue;
                const auto cost = std::uint64_t(route.Metric) + ip.Metric;
                for (ULONG i = 0; i < table->NumEntries; ++i) {
                    auto& row = table->Table[i];
                    if (row.InterfaceLuid.Value == route.InterfaceLuid.Value && row.OperStatus == IfOperStatusUp &&
                        row.InterfaceAndOperStatusFlags.HardwareInterface && cost < metric) { best = &row; metric = cost; }
                }
            }
            FreeMibTable(routes);
        }
        if (!best) for (ULONG i = 0; i < table->NumEntries; ++i) {
            auto& row = table->Table[i];
            if (row.OperStatus == IfOperStatusUp && row.InterfaceAndOperStatusFlags.HardwareInterface &&
                row.Type != IF_TYPE_SOFTWARE_LOOPBACK && (!best || row.TransmitLinkSpeed > best->TransmitLinkSpeed)) best = &row;
        }
        const auto now = std::chrono::steady_clock::now();
        if (best) {
            out.networkLinkMbps = best->TransmitLinkSpeed / 1000000;
            const double seconds = std::chrono::duration<double>(now - previous).count();
            if (previous.time_since_epoch().count() && luid == best->InterfaceLuid.Value && seconds > 0 && best->InOctets >= rx && best->OutOctets >= tx) {
                out.networkDown = static_cast<std::uint64_t>((best->InOctets - rx) / seconds);
                out.networkUp = static_cast<std::uint64_t>((best->OutOctets - tx) / seconds);
            }
            luid = best->InterfaceLuid.Value; rx = best->InOctets; tx = best->OutOctets; previous = now;
        } else previous = {};
        FreeMibTable(table);
    }
};

LocalTelemetry::LocalTelemetry() : impl_(std::make_unique<Impl>()) {}
LocalTelemetry::~LocalTelemetry() = default;
TelemetrySnapshot LocalTelemetry::sample() {
    TelemetrySnapshot out;
    out.connected = true; out.status = "Local"; out.hostName = impl_->host;
    out.cpuName = impl_->cpu;
    out.cpuPowerStrategy = impl_->cpu.find("AMD") != std::string::npos ? "amd-windows-sensors" : "windows-sensors";
    out.sampledAt = std::chrono::system_clock::now(); out.memoryRateMTs = impl_->memoryRate;
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory)) { out.memoryTotal = memory.ullTotalPhys; out.memoryUsed = memory.ullTotalPhys - memory.ullAvailPhys; }
    ULARGE_INTEGER available{}, total{}, free{};
    if (!impl_->root.empty() && GetDiskFreeSpaceExW(impl_->root.c_str(), &available, &total, &free)) {
        out.diskTotal = total.QuadPart; out.diskUsed = total.QuadPart - free.QuadPart;
    }
    PageUsage page; SYSTEM_INFO system{}; GetSystemInfo(&system);
    if (EnumPageFilesW(pageFile, &page)) {
        out.swapAvailable = true; out.swapUsed = page.used * system.dwPageSize; out.swapTotal = page.total * system.dwPageSize;
    }
    out.ssdTemperature = diskTemperature(impl_->disk);
    impl_->cpuMetrics.sample(out); impl_->network(out); impl_->nvidia.sample(out); impl_->sensors.sample(out);
    return out;
}
