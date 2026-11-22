#pragma once
/// @file system_telemetry.hpp
/// @brief Sovereign C++20 Linux Kernel Telemetry, Process Table & Desktop App Grouping Engine.

#include "nisaba/backend_os/signal.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sysmon {

struct MemoryMetrics {
    uint64_t total = 0;
    uint64_t used = 0;
    uint64_t available = 0;
    uint64_t cache = 0;
    double percent = 0.0;
    uint64_t swapTotal = 0;
    uint64_t swapUsed = 0;
    double swapPercent = 0.0;
};

struct NetworkInterface {
    std::string name;
    std::string state;
    std::string address;
    double download = 0.0;
    double upload = 0.0;
    uint64_t received = 0;
    uint64_t sent = 0;
    bool physical = false;
};

struct Disk {
    std::string name;
    std::string model;
    double read = 0.0;
    double write = 0.0;
    uint64_t size = 0;
};

struct Volume {
    std::string mount;
    std::string device;
    std::string type;
    uint64_t total = 0;
    uint64_t available = 0;
    uint64_t used = 0;
};

struct Gpu {
    std::string name;
    std::string driver;
    bool active = false;
    bool hasBusy = false;
    int busy = 0;
    bool hasTemperature = false;
    double temperature = -1.0;
    bool hasFrequency = false;
    double frequency = 0.0;
    bool hasVram = false;
    uint64_t vramUsed = 0;
    uint64_t vramTotal = 0;
};

struct Processor {
    int number = 0;
    double usage = 0.0;
};

struct Metrics {
    MemoryMetrics memory;

    double cpuPercent = 0.0;
    std::vector<Processor> cores;
    std::string cpuName;
    int processCount = 0;

    double download = 0.0;
    double upload = 0.0;
    uint64_t received = 0;
    uint64_t sent = 0;
    std::vector<NetworkInterface> interfaces;

    double diskRead = 0.0;
    double diskWrite = 0.0;
    std::vector<Disk> disks;
    std::vector<Volume> volumes;

    std::vector<Gpu> gpus;
    bool gpuAvailable = false;
    std::string gpuName;
    bool gpuHasBusy = false;
    bool gpuHasTemperature = false;
    double gpuPercent = 0.0;
    double gpuTemperature = 0.0;
};

struct ProcessRecord {
    int pid = 0;
    int parentPid = 0;
    uint32_t uid = 0;
    std::string name;
    std::string displayName;
    std::string iconName;
    std::string user;
    std::string command;
    std::string executable;
    std::string program;
    std::string scopeId;
    std::string helperType;
    std::string appId;
    std::string state;
    int threads = 0;
    int nice = 0;
    uint64_t startTicks = 0;
    uint64_t cpuTicks = 0;
    uint64_t rss = 0;
    uint64_t memory = 0;
    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;
    uint64_t pss = 0;
    int64_t pssTime = 0;
    bool pssAvailable = false;
    bool ioAvailable = false;
    bool kernelThread = false;
    bool scopeSettled = false;
    double cpu = 0.0;
    double readRate = 0.0;
    double writeRate = 0.0;
    std::vector<ProcessRecord> members;
};

struct ProcessExtras {
    int openFiles = -1;
    std::string unit;
};

struct GpuHistory {
    std::vector<double> busy;
    std::vector<double> temperature;
};

struct History {
    std::vector<int64_t> timestamps;
    std::vector<double> cpuPercent;
    std::vector<double> memoryPercent;
    std::vector<double> download;
    std::vector<double> upload;
    std::vector<double> diskRead;
    std::vector<double> diskWrite;
    std::vector<double> gpuPercent;
    std::vector<double> gpuTemperature;
    // One series pair per detected GPU, in Metrics::gpus order.
    std::vector<GpuHistory> gpus;
};

struct Preferences {
    int interval = 1000;
    int scope = 1;
    std::string sortColumn = "cpu";
    bool sortAscending = false;

    static Preferences load(const std::string& path);
    void save(const std::string& path) const;
    static std::string defaultPath();
};

class ProcessList {
public:
    ProcessList();

    void apply(std::vector<ProcessRecord> processes);
    const ProcessRecord* find(int pid) const;
    const std::vector<ProcessRecord>& processes() const noexcept { return processes_; }

    const std::string& search() const noexcept { return search_; }
    void setSearch(std::string query);

    bool allUsers() const noexcept { return all_users_; }
    void setAllUsers(bool all_users);

    const std::string& sortColumn() const noexcept { return sort_col_; }
    bool ascending() const noexcept { return ascending_; }

    void sortBy(const std::string& column);
    void setSorting(const std::string& column, bool ascending);

    size_t count() const noexcept { return visible_indices_.size(); }
    const ProcessRecord* at(size_t row) const;
    int indexOf(int pid) const;

    static std::string programName(const ProcessRecord& proc);

    nisaba::backend_os::Signal<> changed;
    nisaba::backend_os::Signal<> sortingChanged;

private:
    bool matchesFilter(const ProcessRecord& proc) const;
    bool compareRows(const ProcessRecord& a, const ProcessRecord& b) const;
    void rebuildView();

    std::vector<ProcessRecord> processes_;
    std::vector<size_t> visible_indices_;
    std::string search_;
    bool all_users_{false};
    std::string sort_col_{"cpu"};
    bool ascending_{false};
    uint32_t current_uid_{0};
};

struct DesktopAppInfo {
    std::string id;
    std::string name;
    std::string icon;
    bool noDisplay = false;
    bool hidden = false;
};

class TelemetryCollector;

struct TelemetryFrame {
    bool ok = false;
    bool is_warmup = false;
    std::string err_msg;
    Metrics metrics;
    std::vector<ProcessRecord> processes;
};

class Monitor {
public:
    explicit Monitor(std::string config_path = Preferences::defaultPath());
    ~Monitor();

    void tick();

    const Metrics& metrics() const noexcept { return metrics_; }
    const History& history() const noexcept { return history_; }
    ProcessList& processes() noexcept { return processes_; }
    ProcessList& applications() noexcept { return applications_; }

    int scope() const noexcept { return scope_; }
    void setScope(int scope);

    bool ready() const noexcept { return ready_; }
    const std::string& error() const noexcept { return error_; }

    int interval() const noexcept { return interval_ms_; }
    void setInterval(int interval_ms);
    int historyDuration() const noexcept { return 60; }

    const ProcessRecord* process(int pid, uint64_t start_ticks) const;
    const ProcessRecord* application(int pid, uint64_t start_ticks) const;
    static bool canManage(const ProcessRecord& proc);

    std::string endProcess(int pid, uint64_t start_ticks, bool force);
    std::string endApplication(int pid, uint64_t start_ticks, bool force);
    static std::string signalProcess(int pid, uint64_t start_ticks, bool force);

    void sortBy(const std::string& column);

    static int64_t systemBootTime();
    static ProcessExtras inspectProcessExtras(int pid, uint64_t start_ticks);
    static std::string describeProcessState(std::string_view code);
    static std::string describeHelperRole(std::string_view role);

    static std::string formatBytes(double bytes);
    static std::string formatRate(double bytes_per_sec);
    static std::string formatPercent(double pct);
    static std::string formatTemperature(double celsius);
    static std::string formatDuration(int64_t seconds);

    nisaba::backend_os::Signal<> updated;
    nisaba::backend_os::Signal<> intervalChanged;
    nisaba::backend_os::Signal<> scopeChanged;

private:
    struct CachedAppIdentity {
        std::string id;
        std::string name;
        std::string icon;
        uint64_t last_sample = 0;
    };

    void persistConfig() const;
    void dispatchSample();
    void ingestFrame(TelemetryFrame frame);
    void tagProcessApp(ProcessRecord& proc);
    std::vector<ProcessRecord> aggregateApplications(const std::vector<ProcessRecord>& all_procs);

    std::string config_path_;
    ProcessList processes_;
    ProcessList applications_;
    int scope_{1};

    std::unordered_map<std::string, CachedAppIdentity> app_by_id_;
    std::unordered_map<std::string, CachedAppIdentity> app_by_prog_;
    uint64_t sample_seq_{0};

    std::shared_ptr<TelemetryCollector> collector_;
    std::future<TelemetryFrame> frame_future_;
    bool frame_in_flight_{false};

    std::future<std::vector<Volume>> vol_future_;
    bool vol_in_flight_{false};
    std::vector<Volume> cached_volumes_;

    int interval_ms_{1000};
    std::chrono::steady_clock::time_point next_due_{};
    bool warmup_pending_{false};
    std::chrono::steady_clock::time_point warmup_due_{};

    Metrics metrics_;
    History history_;
    std::chrono::steady_clock::time_point epoch_{std::chrono::steady_clock::now()};
    std::string error_;
    bool ready_{false};
};

} // namespace sysmon
