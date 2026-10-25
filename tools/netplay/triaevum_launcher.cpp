// Minimal two-instance launcher for TriAevum.
//
// This is deliberately a supervisor, not an in-game menu: it selects the
// controller before any guest starts, gives each player its own save/config
// roots, and owns the child processes. It does not add multiplayer sync yet.
#include <SDL2/SDL.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <ctime>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Controller {
    int Index = -1;
    std::string Name;
    std::string Guid;
    std::string Path;
    std::string Serial;
    bool SteamVirtual = false;
};

struct Options {
    std::string Mode = "single";
    std::string Package;
    std::string Runtime;
    std::string Data;
    std::string Root;
    std::string LaunchProfile;
    std::string GameConfig;
    std::string ControlsConfig;
    std::string SaveSeed;
    int P1 = -1;
    int P2 = -1;
    int WarmupSeconds = 12;
    int DurationSeconds = 0;
    int NetplayPort = 27888;
    std::string NetplayHostAddress = "127.0.0.1";
    std::string CacheSource;
    bool DryRun = false;
    bool NoSaveCopy = false;
};

std::vector<pid_t> gChildren;

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

void Usage() {
    std::cout
        << "Usage: triaevum_launcher --package DIR --runtime FILE --data DIR\n"
        << "       [--mode single|multiplayer] [--p1 INDEX] [--p2 INDEX]\n"
        << "       [--root DIR] [--warmup SECONDS] [--duration SECONDS]\n"
        << "       [--netplay-port PORT] [--netplay-connect-address HOST]\n"
        << "       [--cache-source DIR]\n"
        << "       [--dry-run] [--no-save-copy]\n";
}

std::optional<std::string> ArgValue(int& i, int argc, char** argv) {
    if (i + 1 >= argc) return std::nullopt;
    return std::string(argv[++i]);
}

std::vector<Controller> EnumerateControllers() {
    std::vector<Controller> result;
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        std::cerr << "SDL controller initialization failed: " << SDL_GetError() << '\n';
        return result;
    }
    const int count = SDL_NumJoysticks();
    for (int index = 0; index < count; ++index) {
        if (SDL_IsGameController(index) != SDL_TRUE) continue;
        SDL_GameController* controller = SDL_GameControllerOpen(index);
        if (controller == nullptr) continue;
        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
        char guid[33]{};
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
        Controller info;
        info.Index = index;
        info.Name = SDL_GameControllerName(controller) ? SDL_GameControllerName(controller) : "gamepad";
        info.Guid = guid;
#if SDL_VERSION_ATLEAST(2, 24, 0)
        info.Path = SDL_JoystickPath(joystick) ? SDL_JoystickPath(joystick) : "";
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
        if (const char* serial = SDL_GameControllerGetSerial(controller);
            serial != nullptr) {
            info.Serial = serial;
        }
#endif
        info.SteamVirtual = Lower(info.Name).find("steam virtual") != std::string::npos;
        result.push_back(std::move(info));
        SDL_GameControllerClose(controller);
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    return result;
}

std::vector<Controller> UsableControllers() {
    auto all = EnumerateControllers();
    std::vector<Controller> physical;
    for (const auto& controller : all)
        if (!controller.SteamVirtual) physical.push_back(controller);
    return physical.empty() ? all : physical;
}

void PrintControllers(const std::vector<Controller>& controllers) {
    std::cout << "Controllers:\n";
    for (size_t i = 0; i < controllers.size(); ++i) {
        const auto& c = controllers[i];
        std::cout << "  [" << i << "] " << c.Name << "  guid=" << c.Guid
                  << "  path=" << c.Path;
        if (!c.Serial.empty()) std::cout << "  serial=" << c.Serial;
        std::cout << '\n';
    }
}

int ReadChoice(const std::string& prompt, int count) {
    while (true) {
        std::cout << prompt;
        int value = -1;
        if (!(std::cin >> value) || value < 0 || value >= count) {
            std::cin.clear();
            std::cin.ignore(4096, '\n');
            std::cout << "Invalid choice. Enter 0.." << (count - 1) << ".\n";
            continue;
        }
        return value;
    }
}

bool CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return false;
    fs::remove_all(to, ec);
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool CopyFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) return false;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool WriteControlsConfig(const fs::path& source, const fs::path& destination,
                         const Controller& controller) {
    std::ifstream input(source);
    if (!input) return false;
    json document = json::parse(input, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return false;
    document["profile"] = "controller";
    document["devices"]["controller"] = true;
    document["devices"]["keyboard"] = false;
    document["devices"]["mouse"] = false;
    document["capture_mouse"] = false;
    document["controller_guid"] = controller.Guid;
    document["controller_serial"] = controller.Serial;
    document["controller_path"] = controller.Path;
    std::ofstream output(destination);
    if (!output) return false;
    output << document.dump(2) << '\n';
    return output.good();
}

bool WaitForLog(const fs::path& path, std::string_view needle,
                int timeoutSeconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(std::max(1, timeoutSeconds));
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream input(path);
        if (input) {
            std::string line;
            while (std::getline(input, line)) {
                if (line.find(needle) != std::string::npos) return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&time, &local);
    std::array<char, 32> text{};
    std::strftime(text.data(), text.size(), "%Y%m%d-%H%M%S", &local);
    return text.data();
}

struct Instance {
    std::string Name;
    fs::path Root;
    fs::path SaveData;
    fs::path Config;
    fs::path Controls;
    fs::path Output;
    fs::path Log;
    fs::path Cache;
    Controller Pad;
    pid_t Pid = -1;
};

bool PrepareInstance(Instance& instance, const Options& options,
                     const Controller& controller, bool copySave) {
    std::error_code ec;
    fs::create_directories(instance.Root, ec);
    if (ec) return false;
    instance.SaveData = instance.Root / "savedata";
    instance.Config = instance.Root / "oot3d_native_game.json";
    instance.Controls = instance.Root / "oot3d_controls.json";
    instance.Output = instance.Root / "runtime.json";
    instance.Log = instance.Root / "run.log";
    instance.Pad = controller;

    if (copySave && !options.NoSaveCopy) {
        CopyTree(fs::path(options.SaveSeed), instance.SaveData);
    } else {
        fs::create_directories(instance.SaveData, ec);
    }
    CopyFile(fs::path(options.GameConfig), instance.Config);
    if (!WriteControlsConfig(fs::path(options.ControlsConfig), instance.Controls,
                             controller)) {
        std::cerr << "Could not write controls config for " << instance.Name << '\n';
        return false;
    }
    return true;
}

pid_t Spawn(const Instance& instance, const Options& options,
            const fs::path& cacheDirectory) {
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (chdir(options.Package.c_str()) != 0) _exit(127);
        setenv("XDG_DATA_HOME", (instance.Root / "xdg").c_str(), 1);
        setenv("TRIAEVUM_RENDERER_CACHE_DIR", cacheDirectory.c_str(), 1);
        setenv("SDL_VIDEO_CENTERED", "0", 1);
        setenv("SDL_VIDEO_WINDOW_POS", instance.Name == "p1" ? "0,80" : "700,80", 1);
        const int log = open(instance.Log.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            close(log);
        }
        std::vector<std::string> args{
            options.Runtime,
            "--launch-profile", options.LaunchProfile,
            "--config", instance.Config.string(),
            "--controls-config", instance.Controls.string(),
            "--save-data", instance.SaveData.string(),
            "--output", instance.Output.string(),
        };
        if (options.Mode == "multiplayer") {
            if (instance.Name == "p1") {
                args.push_back("--netplay-host");
                args.push_back(std::to_string(options.NetplayPort));
            } else {
                args.push_back("--netplay-connect");
                args.push_back(options.NetplayHostAddress + ":" +
                               std::to_string(options.NetplayPort));
            }
            args.push_back("--netplay-player");
            args.push_back(instance.Name == "p1" ? "1" : "2");
            args.push_back("--netplay-probe");
            args.push_back((instance.Root / "netplay.jsonl").string());
        }
        if (options.DurationSeconds > 0) {
            args.push_back("--max-seconds");
            args.push_back(std::to_string(options.DurationSeconds));
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}

void StopChildren() {
    for (pid_t pid : gChildren) {
        if (pid > 0) kill(pid, SIGTERM);
    }
    for (pid_t pid : gChildren) {
        if (pid > 0) waitpid(pid, nullptr, 0);
    }
    gChildren.clear();
}

void SignalHandler(int) {
    StopChildren();
    _exit(0);
}

void PrintPlan(const Options& options, const std::vector<Controller>& controllers,
               const std::vector<Instance>& instances) {
    json plan{
        {"schema", "triaevum.launcher.plan.v1"},
        {"mode", options.Mode},
        {"package", options.Package},
        {"runtime", options.Runtime},
        {"controllers", json::array()},
        {"instances", json::array()},
    };
    for (const auto& c : controllers) {
        plan["controllers"].push_back({{"index", c.Index},
                                       {"name", c.Name},
                                       {"guid", c.Guid},
                                       {"path", c.Path},
                                       {"serial", c.Serial}});
    }
    for (const auto& i : instances) {
        plan["instances"].push_back({{"name", i.Name},
                                     {"netplay_player", i.Name == "p1" ? 1 : 2},
                                     {"root", i.Root.string()},
                                     {"save_data", i.SaveData.string()},
                                     {"config", i.Config.string()},
                                     {"controls", i.Controls.string()},
                                     {"controller_path", i.Pad.Path}});
    }
    std::cout << plan.dump(2) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string& out) {
            auto v = ArgValue(i, argc, argv);
            if (!v) return false;
            out = *v;
            return true;
        };
        if (arg == "--package") {
            if (!value(options.Package)) { Usage(); return 2; }
        } else if (arg == "--runtime") {
            if (!value(options.Runtime)) { Usage(); return 2; }
        } else if (arg == "--data") {
            if (!value(options.Data)) { Usage(); return 2; }
        } else if (arg == "--mode") {
            if (!value(options.Mode)) { Usage(); return 2; }
        } else if (arg == "--root") {
            if (!value(options.Root)) { Usage(); return 2; }
        } else if (arg == "--launch-profile") {
            if (!value(options.LaunchProfile)) { Usage(); return 2; }
        } else if (arg == "--game-config") {
            if (!value(options.GameConfig)) { Usage(); return 2; }
        } else if (arg == "--controls-config") {
            if (!value(options.ControlsConfig)) { Usage(); return 2; }
        } else if (arg == "--save-seed") {
            if (!value(options.SaveSeed)) { Usage(); return 2; }
        } else if (arg == "--p1") {
            std::string v;
            if (!value(v)) { Usage(); return 2; }
            options.P1 = std::stoi(v);
        } else if (arg == "--p2") {
            std::string v;
            if (!value(v)) { Usage(); return 2; }
            options.P2 = std::stoi(v);
        } else if (arg == "--warmup") {
            std::string v;
            if (!value(v)) { Usage(); return 2; }
            options.WarmupSeconds = std::max(0, std::stoi(v));
        } else if (arg == "--netplay-port") {
            std::string v;
            if (!value(v)) { Usage(); return 2; }
            options.NetplayPort = std::stoi(v);
        } else if (arg == "--netplay-connect-address") {
            if (!value(options.NetplayHostAddress)) { Usage(); return 2; }
        } else if (arg == "--cache-source") {
            if (!value(options.CacheSource)) { Usage(); return 2; }
        } else if (arg == "--duration") {
            std::string v;
            if (!value(v)) { Usage(); return 2; }
            options.DurationSeconds = std::max(0, std::stoi(v));
        } else if (arg == "--dry-run") {
            options.DryRun = true;
        } else if (arg == "--no-save-copy") {
            options.NoSaveCopy = true;
        } else if (arg == "--help" || arg == "-h") {
            Usage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            Usage();
            return 2;
        }
    }
    if (options.Package.empty() || options.Runtime.empty() || options.Data.empty()) {
        Usage();
        return 2;
    }
    if (options.LaunchProfile.empty()) {
        options.LaunchProfile = (fs::path(options.Package) / "TriAevum.launch.json").string();
    }
    if (options.GameConfig.empty()) {
        options.GameConfig = (fs::path(options.Data) / "config" / "oot3d_native_game.json").string();
    }
    if (options.ControlsConfig.empty()) {
        options.ControlsConfig = (fs::path(options.Data) / "config" / "oot3d_controls.json").string();
    }
    if (options.SaveSeed.empty()) {
        options.SaveSeed = (fs::path(options.Data) / "savedata").string();
    }
    const std::string mode = Lower(options.Mode);
    if (mode != "single" && mode != "multiplayer") {
        std::cerr << "--mode must be single or multiplayer\n";
        return 2;
    }
    options.Mode = mode;

    auto controllers = UsableControllers();
    if (controllers.empty() && mode == "multiplayer") {
        std::cerr << "No usable game controllers found.\n";
        return 1;
    }
    if (mode == "multiplayer" && controllers.size() < 2) {
        std::cerr << "Multiplayer requires two connected game controllers.\n";
        return 1;
    }

    std::vector<Instance> instances;
    if (mode == "single") {
        std::cerr << "Single-player launch uses the normal game data and does not create a second instance.\n";
        Instance one;
        one.Name = "single";
        one.Root = fs::path(options.Data);
        one.SaveData = fs::path(options.SaveSeed);
        one.Config = fs::path(options.GameConfig);
        one.Controls = fs::path(options.ControlsConfig);
        one.Output = one.Root / "runtime.json";
        one.Log = one.Root / "launcher-single.log";
        one.Pad = controllers.empty() ? Controller{} : controllers.front();
        instances.push_back(std::move(one));
    } else {
        if (options.P1 < 0 || options.P2 < 0) {
            PrintControllers(controllers);
            options.P1 = ReadChoice("P1 controller index: ", static_cast<int>(controllers.size()));
            do {
                options.P2 = ReadChoice("P2 controller index: ", static_cast<int>(controllers.size()));
            } while (options.P2 == options.P1);
        }
        if (options.P1 < 0 || options.P1 >= static_cast<int>(controllers.size()) ||
            options.P2 < 0 || options.P2 >= static_cast<int>(controllers.size()) ||
            options.P1 == options.P2) {
            std::cerr << "Invalid controller assignment.\n";
            return 2;
        }
        const fs::path root = fs::path(options.Root.empty()
            ? fs::path("netplay-runs") / Timestamp()
            : fs::path(options.Root));
        Instance p1;
        p1.Name = "p1";
        p1.Root = root / "p1";
        Instance p2;
        p2.Name = "p2";
        p2.Root = root / "p2";
        if (!PrepareInstance(p1, options, controllers[options.P1], true) ||
            !PrepareInstance(p2, options, controllers[options.P2], true)) {
            return 1;
        }
        instances.push_back(std::move(p1));
        instances.push_back(std::move(p2));
    }

    const fs::path cacheSource = options.CacheSource.empty()
        ? fs::path(options.Data) / "cache" / "renderer"
        : fs::path(options.CacheSource);
    for (auto& instance : instances) {
        const fs::path cache = mode == "multiplayer"
            ? instance.Root / "cache"
            : cacheSource;
        if (mode == "multiplayer") {
            fs::create_directories(cache);
            CopyTree(cacheSource, cache);
        }
        instance.Cache = cache;
    }

    PrintPlan(options, controllers, instances);
    if (options.DryRun) return 0;

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    if (mode == "single") {
        Instance& one = instances.front();
        const pid_t pid = Spawn(one, options, one.Cache);
        if (pid < 0) {
            std::cerr << "Could not launch single-player process.\n";
            return 1;
        }
        gChildren.push_back(pid);
        std::cout << "Launched single-player pid " << pid << " (log: " << one.Log << ")\n";
        waitpid(pid, nullptr, 0);
        gChildren.clear();
        return 0;
    }

    std::cout << "Launching p1 (" << instances[0].Pad.Name << ") first.\n";
    instances[0].Pid = Spawn(instances[0], options, instances[0].Cache);
    if (instances[0].Pid < 0) return 1;
    gChildren.push_back(instances[0].Pid);
    if (!WaitForLog(instances[0].Log, "TopScreen source-port layout", 120)) {
        std::cout << "p1 readiness marker not seen; falling back to "
                  << options.WarmupSeconds << "s warm-up.\n";
        std::this_thread::sleep_for(std::chrono::seconds(options.WarmupSeconds));
    }
    std::cout << "Launching p2 (" << instances[1].Pad.Name << ").\n";
    instances[1].Pid = Spawn(instances[1], options, instances[1].Cache);
    if (instances[1].Pid < 0) {
        StopChildren();
        return 1;
    }
    gChildren.push_back(instances[1].Pid);

    for (;;) {
        int status = 0;
        const pid_t finished = wait(&status);
        if (finished < 0) break;
        std::cout << "Instance pid " << finished << " exited with status " << status << '\n';
        gChildren.erase(std::remove(gChildren.begin(), gChildren.end(), finished),
                        gChildren.end());
        if (gChildren.empty()) break;
    }
    return 0;
}
