#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits.h>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char* kDefaultRemoteConfigDir = "/breton_pilot/modules/drivers/camera/config";
const char* kDefaultRemoteLogDir = "/breton_data/logs/camera";
const char* kDefaultVehicleUser = "nvidia";

struct AppPaths {
  fs::path tool_root;
  fs::path runtime_dir;
  fs::path cache_dir;
  fs::path upload_dir;
  fs::path vehicle_config_path;
  fs::path import_template_path;
  fs::path default_local_log_dir;
};

struct CameraRule {
  std::string camera_name;
  std::string file_name;
  bool allow_resolution_change;
};

struct CameraState {
  int bitrate = 0;
  int width = 0;
  int height = 0;
};

struct CameraUpdate {
  int bitrate = 0;
  std::optional<int> width;
  std::optional<int> height;
};

enum class PlanMode {
  Fixed,
  CurrentValues,
};

struct PlanRequest {
  PlanMode mode = PlanMode::Fixed;
  std::map<std::string, CameraUpdate> fixed_updates;
};

struct VehicleInfo {
  std::string car_id;
  std::string ip;
  std::string password;
};

struct ConnectionConfig {
  std::string jump_host_user;
  std::string jump_host_ip;
  std::string jump_host_password;
  std::string vehicle_user = kDefaultVehicleUser;
  std::string default_vehicle_password;
  fs::path local_log_dir;
  std::string remote_config_dir = kDefaultRemoteConfigDir;
  std::string remote_log_dir = kDefaultRemoteLogDir;
  std::map<std::string, VehicleInfo> vehicles;
};

struct TimestampInfo {
  std::string compact;
  std::string readable;
};

struct RemoteEndpoint {
  std::string host;
  int port = 22;
  std::optional<fs::path> control_socket;
};

struct CachedCameraFile {
  CameraRule rule;
  fs::path cached_path;
  std::string original_content;
  CameraState state;
};

struct VehicleSnapshot {
  VehicleInfo vehicle;
  std::map<std::string, CachedCameraFile> cameras;
};

struct PreparedUpload {
  CameraRule rule;
  CameraState before;
  CameraUpdate after;
  fs::path local_path;
};

struct ActiveTunnel {
  fs::path control_socket;
  std::string jump_host_user;
  std::string jump_host_ip;
};

std::optional<ActiveTunnel> g_active_tunnel;

std::string Trim(const std::string& input) {
  const auto begin = input.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = input.find_last_not_of(" \t\r\n");
  return input.substr(begin, end - begin + 1);
}

std::string ShellEscapeSingleQuoted(const std::string& value) {
  std::string escaped = "'";
  for (char ch : value) {
    if (ch == '\'') {
      escaped += "'\\''";
    } else {
      escaped += ch;
    }
  }
  escaped += "'";
  return escaped;
}

void EnsureDirectory(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    throw std::runtime_error("failed to create directory: " + path.string());
  }
}

std::string ReadTextFile(const fs::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to open file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void WriteTextFile(const fs::path& path, const std::string& content) {
  EnsureDirectory(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  output << content;
}

bool ReplaceField(std::string& content, const std::string& key, int value) {
  const std::string token = key + ":";
  const auto pos = content.find(token);
  if (pos == std::string::npos) {
    return false;
  }
  const auto line_end = content.find('\n', pos);
  const auto value_begin = content.find_first_of("0123456789-", pos + token.size());
  if (value_begin == std::string::npos || (line_end != std::string::npos && value_begin > line_end)) {
    return false;
  }
  const auto value_end = content.find_first_not_of("0123456789-", value_begin);
  content.replace(value_begin, value_end - value_begin, std::to_string(value));
  return true;
}

int ParseFieldValue(const std::string& content, const std::string& key) {
  const std::string token = key + ":";
  const auto pos = content.find(token);
  if (pos == std::string::npos) {
    throw std::runtime_error("missing field: " + key);
  }
  const auto line_end = content.find('\n', pos);
  const auto value_begin = content.find_first_of("0123456789-", pos + token.size());
  if (value_begin == std::string::npos || (line_end != std::string::npos && value_begin > line_end)) {
    throw std::runtime_error("invalid field value: " + key);
  }
  const auto value_end = content.find_first_not_of("0123456789-", value_begin);
  return std::stoi(content.substr(value_begin, value_end - value_begin));
}

CameraState ParseCameraState(const std::string& content) {
  CameraState state;
  state.bitrate = ParseFieldValue(content, "stream_bitrate");
  state.width = ParseFieldValue(content, "stream_width");
  state.height = ParseFieldValue(content, "stream_height");
  return state;
}

void ApplyUpdateToContent(std::string& content, const CameraRule& rule, const CameraUpdate& update) {
  if (!ReplaceField(content, "stream_bitrate", update.bitrate)) {
    throw std::runtime_error("failed to update stream_bitrate for " + rule.camera_name);
  }
  if (rule.allow_resolution_change) {
    if (!update.width || !update.height) {
      throw std::runtime_error("missing resolution update for " + rule.camera_name);
    }
    if (!ReplaceField(content, "stream_width", *update.width)) {
      throw std::runtime_error("failed to update stream_width for " + rule.camera_name);
    }
    if (!ReplaceField(content, "stream_height", *update.height)) {
      throw std::runtime_error("failed to update stream_height for " + rule.camera_name);
    }
  }
}

fs::path ReadExecutablePath() {
  std::vector<char> buffer(PATH_MAX, '\0');
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    throw std::runtime_error("failed to read executable path");
  }
  buffer[static_cast<std::size_t>(length)] = '\0';
  return fs::path(buffer.data());
}

AppPaths ResolveAppPaths() {
  const fs::path exe_path = ReadExecutablePath();
  fs::path tool_root = exe_path.parent_path();
  if (tool_root.filename() == "build") {
    tool_root = tool_root.parent_path();
  }

  AppPaths paths;
  paths.tool_root = tool_root;
  paths.runtime_dir = tool_root / "runtime";
  paths.cache_dir = paths.runtime_dir / "cache";
  paths.upload_dir = paths.runtime_dir / "upload";
  paths.vehicle_config_path = tool_root / "vehicles.conf";
  paths.import_template_path = tool_root / "camera_import_template.json";
  paths.default_local_log_dir = tool_root / "logs";
  return paths;
}

fs::path ResolveConfiguredPath(const fs::path& tool_root,
                              const std::string& configured_value,
                              const fs::path& fallback) {
  if (configured_value.empty()) {
    return fallback;
  }
  const fs::path configured_path(configured_value);
  if (configured_path.is_absolute()) {
    return configured_path;
  }
  return tool_root / configured_path;
}

std::string BuildAuthPrefix(const std::string& password) {
  if (password.empty()) {
    return "";
  }
  return "sshpass -p " + ShellEscapeSingleQuoted(password) + " ";
}

int RunCommand(const std::string& command) {
  return std::system(command.c_str());
}

void ThrowIfCommandFailed(const std::string& command, const std::string& error_message) {
  const int code = RunCommand(command);
  if (code != 0) {
    throw std::runtime_error(error_message);
  }
}

int FindAvailableLocalPort() {
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    throw std::runtime_error("failed to create socket for local port allocation");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(socket_fd);
    throw std::runtime_error("failed to bind local port");
  }

  socklen_t addr_len = sizeof(addr);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
    close(socket_fd);
    throw std::runtime_error("failed to query local port");
  }

  const int port = ntohs(addr.sin_port);
  close(socket_fd);
  return port;
}

void CleanupActiveTunnel() {
  if (!g_active_tunnel) {
    return;
  }

  const auto& tunnel = *g_active_tunnel;
  const std::string command =
      "ssh -S " + ShellEscapeSingleQuoted(tunnel.control_socket.string()) +
      " -O exit " + tunnel.jump_host_user + "@" + tunnel.jump_host_ip + " >/dev/null 2>&1";
  RunCommand(command);
  std::error_code ec;
  fs::remove(tunnel.control_socket, ec);
  g_active_tunnel.reset();
}

void SignalHandler(int) {
  CleanupActiveTunnel();
  std::_Exit(1);
}

void SetupSignalHandlers() {
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
}

TimestampInfo CurrentTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm{};
  localtime_r(&now_time, &local_tm);

  std::ostringstream compact;
  compact << std::put_time(&local_tm, "%Y%m%d%H%M%S");
  std::ostringstream readable;
  readable << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
  return {compact.str(), readable.str()};
}

ConnectionConfig LoadConnectionConfig(const AppPaths& paths) {
  ConnectionConfig config;
  config.local_log_dir = paths.default_local_log_dir;

  std::ifstream input(paths.vehicle_config_path);
  if (!input) {
    throw std::runtime_error("missing vehicle config: " + paths.vehicle_config_path.string());
  }

  std::string line;
  while (std::getline(input, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }

    const std::string key = Trim(line.substr(0, separator));
    const std::string value = Trim(line.substr(separator + 1));
    if (key == "jump_host_user") {
      config.jump_host_user = value;
    } else if (key == "jump_host_ip") {
      config.jump_host_ip = value;
    } else if (key == "jump_host_password") {
      config.jump_host_password = value;
    } else if (key == "vehicle_user") {
      if (!value.empty()) {
        config.vehicle_user = value;
      }
    } else if (key == "default_vehicle_password") {
      config.default_vehicle_password = value;
    } else if (key == "local_log_dir") {
      config.local_log_dir = ResolveConfiguredPath(paths.tool_root, value, paths.default_local_log_dir);
    } else if (key == "remote_config_dir") {
      if (!value.empty()) {
        config.remote_config_dir = value;
      }
    } else if (key == "remote_log_dir") {
      if (!value.empty()) {
        config.remote_log_dir = value;
      }
    } else {
      VehicleInfo vehicle;
      vehicle.car_id = key;
      const auto comma = value.find(',');
      if (comma == std::string::npos) {
        vehicle.ip = Trim(value);
        vehicle.password = config.default_vehicle_password;
      } else {
        vehicle.ip = Trim(value.substr(0, comma));
        vehicle.password = Trim(value.substr(comma + 1));
        if (vehicle.password.empty()) {
          vehicle.password = config.default_vehicle_password;
        }
      }
      if (vehicle.ip.empty()) {
        throw std::runtime_error("vehicle ip is empty for " + key);
      }
      config.vehicles[key] = vehicle;
    }
  }

  if (config.vehicles.empty()) {
    throw std::runtime_error("no vehicles configured in vehicles.conf");
  }
  if (!config.jump_host_ip.empty() && config.jump_host_user.empty()) {
    throw std::runtime_error("jump_host_user is required when jump_host_ip is set");
  }
  return config;
}

std::vector<CameraRule> BuildCameraRules() {
  return {
      {"camera_front_left", "camera_front_left.pb.txt", true},
      {"camera_ir_left", "camera_ir_left.pb.txt", false},
      {"camera_surround_left_rear", "camera_surround_left_rear.pb.txt", true},
      {"camera_surround_right_rear", "camera_surround_right_rear.pb.txt", true},
      {"camera_rear_left", "camera_rear_left.pb.txt", true},
  };
}

void PrintVehicles(const ConnectionConfig& config) {
  std::cout << "可选车号:\n";
  int index = 1;
  for (const auto& [car_id, vehicle] : config.vehicles) {
    std::cout << "  " << index++ << ". " << car_id << " -> " << vehicle.ip << "\n";
  }
}

std::vector<std::string> ParseVehicleSelection(const std::string& input,
                                               const ConnectionConfig& config) {
  const std::string trimmed = Trim(input);
  if (trimmed == "all") {
    std::vector<std::string> all;
    for (const auto& [car_id, vehicle] : config.vehicles) {
      all.push_back(car_id);
    }
    return all;
  }

  std::string normalized = trimmed;
  std::replace(normalized.begin(), normalized.end(), ',', ' ');
  std::stringstream stream(normalized);
  std::string token;
  std::vector<std::string> selected;
  std::set<std::string> seen;
  while (stream >> token) {
    if (config.vehicles.find(token) == config.vehicles.end()) {
      throw std::runtime_error("unknown car id: " + token);
    }
    if (seen.insert(token).second) {
      selected.push_back(token);
    }
  }

  if (selected.empty()) {
    throw std::runtime_error("no vehicles selected");
  }
  return selected;
}

void PrintCameraChoices(const std::vector<CameraRule>& rules) {
  std::cout << "可修改摄像头:\n";
  for (std::size_t index = 0; index < rules.size(); ++index) {
    std::cout << "  " << index + 1 << ". " << rules[index].camera_name;
    if (!rules[index].allow_resolution_change) {
      std::cout << " (仅支持修改 bitrate)";
    }
    std::cout << "\n";
  }
}

std::vector<CameraRule> ParseCameraSelection(const std::string& input,
                                             const std::vector<CameraRule>& rules) {
  const std::string trimmed = Trim(input);
  if (trimmed == "all") {
    return rules;
  }

  std::map<std::string, CameraRule> by_name;
  for (const auto& rule : rules) {
    by_name[rule.camera_name] = rule;
  }

  std::string normalized = trimmed;
  std::replace(normalized.begin(), normalized.end(), ',', ' ');
  std::stringstream stream(normalized);
  std::string token;
  std::vector<CameraRule> selected;
  std::set<std::string> seen;
  while (stream >> token) {
    const bool numeric = std::all_of(token.begin(), token.end(), [](unsigned char ch) {
      return std::isdigit(ch) != 0;
    });
    if (numeric) {
      const int index = std::stoi(token);
      if (index < 1 || index > static_cast<int>(rules.size())) {
        throw std::runtime_error("invalid camera index: " + token);
      }
      const auto& rule = rules[static_cast<std::size_t>(index - 1)];
      if (seen.insert(rule.camera_name).second) {
        selected.push_back(rule);
      }
      continue;
    }

    const auto found = by_name.find(token);
    if (found == by_name.end()) {
      throw std::runtime_error("unknown camera: " + token);
    }
    if (seen.insert(found->first).second) {
      selected.push_back(found->second);
    }
  }

  if (selected.empty()) {
    throw std::runtime_error("no cameras selected");
  }
  return selected;
}

int AskChoice(const std::string& prompt, int min_value, int max_value) {
  while (true) {
    std::cout << prompt;
    std::string line;
    if (!std::getline(std::cin, line)) {
      throw std::runtime_error("input ended");
    }
    line = Trim(line);
    if (line.empty()) {
      continue;
    }
    try {
      const int value = std::stoi(line);
      if (value >= min_value && value <= max_value) {
        return value;
      }
    } catch (const std::exception&) {
    }
    std::cout << "输入无效，请重新输入。\n";
  }
}

int AskInt(const std::string& prompt, int min_value, int max_value) {
  while (true) {
    std::cout << prompt;
    std::string line;
    if (!std::getline(std::cin, line)) {
      throw std::runtime_error("input ended");
    }
    line = Trim(line);
    try {
      const int value = std::stoi(line);
      if (value >= min_value && value <= max_value) {
        return value;
      }
    } catch (const std::exception&) {
    }
    std::cout << "输入无效，请重新输入。\n";
  }
}

RemoteEndpoint OpenRemoteEndpoint(const AppPaths& paths,
                                  const ConnectionConfig& config,
                                  const VehicleInfo& vehicle) {
  if (config.jump_host_ip.empty()) {
    return {vehicle.ip, 22, std::nullopt};
  }

  EnsureDirectory(paths.runtime_dir);
  const int local_port = FindAvailableLocalPort();
  const fs::path control_socket = paths.runtime_dir / ("jump_" + vehicle.car_id + ".sock");
  std::error_code ec;
  fs::remove(control_socket, ec);

  const std::string command =
      BuildAuthPrefix(config.jump_host_password) +
      "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "
      "-o ExitOnForwardFailure=yes -M -f -N -S " +
      ShellEscapeSingleQuoted(control_socket.string()) + " -L 127.0.0.1:" +
      std::to_string(local_port) + ":" + vehicle.ip + ":22 " +
      config.jump_host_user + "@" + config.jump_host_ip;
  ThrowIfCommandFailed(command, "failed to open jump-host tunnel for vehicle " + vehicle.car_id);

  g_active_tunnel = ActiveTunnel{control_socket, config.jump_host_user, config.jump_host_ip};
  return {"127.0.0.1", local_port, control_socket};
}

void CloseRemoteEndpoint(const RemoteEndpoint& endpoint) {
  if (!endpoint.control_socket) {
    return;
  }
  CleanupActiveTunnel();
}

void DownloadRemoteFile(const ConnectionConfig& config,
                        const VehicleInfo& vehicle,
                        const RemoteEndpoint& endpoint,
                        const std::string& remote_path,
                        const fs::path& local_path) {
  EnsureDirectory(local_path.parent_path());
  const std::string command =
      BuildAuthPrefix(vehicle.password) + "scp -P " + std::to_string(endpoint.port) +
      " -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null " +
      config.vehicle_user + "@" + endpoint.host + ":" + remote_path + " " +
      ShellEscapeSingleQuoted(local_path.string());
  ThrowIfCommandFailed(command, "failed to download remote file: " + remote_path);
}

void UploadRemoteFile(const ConnectionConfig& config,
                      const VehicleInfo& vehicle,
                      const RemoteEndpoint& endpoint,
                      const fs::path& local_path,
                      const std::string& remote_path) {
  const std::string command =
      BuildAuthPrefix(vehicle.password) + "scp -P " + std::to_string(endpoint.port) +
      " -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null " +
      ShellEscapeSingleQuoted(local_path.string()) + " " + config.vehicle_user + "@" +
      endpoint.host + ":" + remote_path;
  ThrowIfCommandFailed(command, "failed to upload remote file: " + remote_path);
}

void RunRemoteCommand(const ConnectionConfig& config,
                      const VehicleInfo& vehicle,
                      const RemoteEndpoint& endpoint,
                      const std::string& remote_command) {
  const std::string command =
      BuildAuthPrefix(vehicle.password) + "ssh -p " + std::to_string(endpoint.port) +
      " -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null " +
      config.vehicle_user + "@" + endpoint.host + " " +
      ShellEscapeSingleQuoted(remote_command);
  ThrowIfCommandFailed(command, "failed to run remote command for vehicle " + vehicle.car_id);
}

VehicleSnapshot FetchVehicleSnapshot(const AppPaths& paths,
                                     const ConnectionConfig& config,
                                     const VehicleInfo& vehicle,
                                     const std::vector<CameraRule>& rules) {
  VehicleSnapshot snapshot;
  snapshot.vehicle = vehicle;

  const RemoteEndpoint endpoint = OpenRemoteEndpoint(paths, config, vehicle);
  try {
    for (const auto& rule : rules) {
      const fs::path local_path = paths.cache_dir / vehicle.car_id / rule.file_name;
      const std::string remote_path = config.remote_config_dir + "/" + rule.file_name;
      DownloadRemoteFile(config, vehicle, endpoint, remote_path, local_path);

      CachedCameraFile cached;
      cached.rule = rule;
      cached.cached_path = local_path;
      cached.original_content = ReadTextFile(local_path);
      cached.state = ParseCameraState(cached.original_content);
      snapshot.cameras[rule.camera_name] = cached;
    }
  } catch (...) {
    CloseRemoteEndpoint(endpoint);
    throw;
  }
  CloseRemoteEndpoint(endpoint);
  return snapshot;
}

void PrintVehicleSnapshot(const VehicleSnapshot& snapshot, const std::vector<CameraRule>& rules) {
  std::cout << "车辆 " << snapshot.vehicle.car_id << " 当前配置:\n";
  for (const auto& rule : rules) {
    const auto found = snapshot.cameras.find(rule.camera_name);
    if (found == snapshot.cameras.end()) {
      continue;
    }
    const auto& state = found->second.state;
    std::cout << "  " << rule.camera_name << ": bitrate=" << state.bitrate
              << ", resolution=" << state.width << "x" << state.height;
    if (!rule.allow_resolution_change) {
      std::cout << " (仅支持修改 bitrate)";
    }
    std::cout << "\n";
  }
}

PlanRequest BuildDefault720Plan(const std::vector<CameraRule>& rules) {
  PlanRequest request;
  request.mode = PlanMode::Fixed;
  for (const auto& rule : rules) {
    if (rule.camera_name == "camera_ir_left") {
      request.fixed_updates[rule.camera_name] = CameraUpdate{500000, std::nullopt, std::nullopt};
    } else if (rule.camera_name == "camera_front_left" || rule.camera_name == "camera_rear_left") {
      request.fixed_updates[rule.camera_name] = CameraUpdate{1500000, 1280, 720};
    } else if (rule.camera_name == "camera_surround_left_rear" ||
               rule.camera_name == "camera_surround_right_rear") {
      request.fixed_updates[rule.camera_name] = CameraUpdate{1200000, 960, 768};
    }
  }
  return request;
}

PlanRequest BuildCurrentValuePlan() {
  PlanRequest request;
  request.mode = PlanMode::CurrentValues;
  return request;
}

CameraUpdate BuildResolutionUpdate(const CameraRule& rule) {
  if (!rule.allow_resolution_change) {
    const int bitrate = AskInt("请输入 camera_ir_left 的 bitrate: ", 1, 10000000);
    return CameraUpdate{bitrate, std::nullopt, std::nullopt};
  }

  std::cout << "为 " << rule.camera_name << " 选择分辨率:\n";
  std::cout << "  1. 1080P\n";
  std::cout << "  2. 720P\n";
  std::cout << "  3. 640P\n";
  std::cout << "  4. 480P\n";
  std::cout << "  5. 320P\n";
  const int choice = AskChoice("请输入选项: ", 1, 5);

  if (rule.camera_name == "camera_front_left" || rule.camera_name == "camera_rear_left") {
    switch (choice) {
      case 1:
        return CameraUpdate{3000000, 1920, 1080};
      case 2:
        return CameraUpdate{1500000, 1280, 720};
      case 3:
        return CameraUpdate{800000, 640, 480};
      case 4:
        return CameraUpdate{500000, 640, 480};
      case 5:
        return CameraUpdate{300000, 320, 240};
      default:
        break;
    }
  }

  if (rule.camera_name == "camera_surround_left_rear" || rule.camera_name == "camera_surround_right_rear") {
    switch (choice) {
      case 1:
        return CameraUpdate{3000000, 1920, 1536};
      case 2:
        return CameraUpdate{1200000, 960, 768};
      case 3:
        return CameraUpdate{700000, 640, 512};
      case 4:
        return CameraUpdate{400000, 450, 360};
      case 5:
        return CameraUpdate{250000, 320, 256};
      default:
        break;
    }
  }

  throw std::runtime_error("unsupported resolution option");
}

std::optional<int> ParseJsonIntField(const std::string& object, const std::string& field) {
  const std::string token = "\"" + field + "\"";
  const auto field_pos = object.find(token);
  if (field_pos == std::string::npos) {
    return std::nullopt;
  }
  const auto colon_pos = object.find(':', field_pos + token.size());
  if (colon_pos == std::string::npos) {
    return std::nullopt;
  }
  const auto value_begin = object.find_first_of("0123456789-", colon_pos + 1);
  if (value_begin == std::string::npos) {
    return std::nullopt;
  }
  const auto value_end = object.find_first_not_of("0123456789-", value_begin);
  return std::stoi(object.substr(value_begin, value_end - value_begin));
}

std::string ExtractJsonObject(const std::string& json, const std::string& key) {
  const std::string token = "\"" + key + "\"";
  const auto key_pos = json.find(token);
  if (key_pos == std::string::npos) {
    return "";
  }
  const auto object_begin = json.find('{', key_pos + token.size());
  if (object_begin == std::string::npos) {
    return "";
  }

  int depth = 0;
  for (std::size_t index = object_begin; index < json.size(); ++index) {
    if (json[index] == '{') {
      ++depth;
    } else if (json[index] == '}') {
      --depth;
      if (depth == 0) {
        return json.substr(object_begin, index - object_begin + 1);
      }
    }
  }
  return "";
}

PlanRequest LoadPlanFromJson(const std::vector<CameraRule>& rules, const fs::path& json_path) {
  const std::string json = ReadTextFile(json_path);
  PlanRequest request;
  request.mode = PlanMode::Fixed;

  for (const auto& rule : rules) {
    const std::string object = ExtractJsonObject(json, rule.camera_name);
    if (object.empty()) {
      throw std::runtime_error("json missing camera object: " + rule.camera_name);
    }

    auto bitrate = ParseJsonIntField(object, "stream_bitrate");
    if (!bitrate) {
      throw std::runtime_error("json missing stream_bitrate for " + rule.camera_name);
    }

    CameraUpdate update;
    update.bitrate = *bitrate;
    if (rule.allow_resolution_change) {
      auto width = ParseJsonIntField(object, "stream_width");
      auto height = ParseJsonIntField(object, "stream_height");
      if (!width || !height) {
        throw std::runtime_error("json missing resolution for " + rule.camera_name);
      }
      update.width = *width;
      update.height = *height;
    }
    request.fixed_updates[rule.camera_name] = update;
  }
  return request;
}

PlanRequest BuildInteractivePlan(const std::vector<CameraRule>& rules, const AppPaths& paths) {
  std::cout << "参数修改选项:\n";
  std::cout << "  1. 默认用720P\n";
  std::cout << "  2. 默认用现有值\n";
  std::cout << "  3. 选择分辨率\n";
  std::cout << "  4. 使用本地json直接导入\n";
  const int mode = AskChoice("请输入选项: ", 1, 4);

  if (mode == 1) {
    return BuildDefault720Plan(rules);
  }
  if (mode == 2) {
    return BuildCurrentValuePlan();
  }
  if (mode == 4) {
    std::cout << "默认导入文件: " << paths.import_template_path << "\n";
    std::cout << "如需其他文件，请输入完整路径；直接回车使用默认文件: ";
    std::string path_line;
    std::getline(std::cin, path_line);
    path_line = Trim(path_line);
    const fs::path json_path = path_line.empty() ? paths.import_template_path : fs::path(path_line);
    return LoadPlanFromJson(rules, json_path);
  }

  PlanRequest request;
  request.mode = PlanMode::Fixed;
  for (const auto& rule : rules) {
    request.fixed_updates[rule.camera_name] = BuildResolutionUpdate(rule);
  }
  return request;
}

CameraUpdate ResolveUpdate(const PlanRequest& request,
                           const CameraRule& rule,
                           const CameraState& before) {
  if (request.mode == PlanMode::CurrentValues) {
    CameraUpdate update;
    update.bitrate = before.bitrate;
    if (rule.allow_resolution_change) {
      update.width = before.width;
      update.height = before.height;
    }
    return update;
  }

  const auto found = request.fixed_updates.find(rule.camera_name);
  if (found == request.fixed_updates.end()) {
    throw std::runtime_error("missing planned update for " + rule.camera_name);
  }
  return found->second;
}

std::vector<PreparedUpload> PrepareUploads(const AppPaths& paths,
                                           const VehicleSnapshot& snapshot,
                                           const std::vector<CameraRule>& selected_rules,
                                           const PlanRequest& request) {
  std::vector<PreparedUpload> uploads;
  for (const auto& rule : selected_rules) {
    const auto found = snapshot.cameras.find(rule.camera_name);
    if (found == snapshot.cameras.end()) {
      throw std::runtime_error("snapshot missing camera: " + rule.camera_name);
    }

    const auto& cached = found->second;
    const CameraUpdate update = ResolveUpdate(request, rule, cached.state);
    std::string content = cached.original_content;
    ApplyUpdateToContent(content, rule, update);

    const fs::path upload_path = paths.upload_dir / snapshot.vehicle.car_id / rule.file_name;
    WriteTextFile(upload_path, content);
    uploads.push_back({rule, cached.state, update, upload_path});
  }
  return uploads;
}

void PrintPlannedChanges(const std::string& car_id, const std::vector<PreparedUpload>& uploads) {
  std::cout << "车辆 " << car_id << " 将应用以下参数:\n";
  for (const auto& upload : uploads) {
    std::cout << "  " << upload.rule.camera_name << ": bitrate=" << upload.after.bitrate;
    if (upload.rule.allow_resolution_change && upload.after.width && upload.after.height) {
      std::cout << ", resolution=" << *upload.after.width << "x" << *upload.after.height;
    } else {
      std::cout << ", resolution=保持当前值";
    }
    std::cout << "\n";
  }
}

std::string BuildLogContent(const std::string& car_id,
                            const TimestampInfo& ts,
                            const std::vector<PreparedUpload>& uploads,
                            const std::string& result,
                            const std::string& error_message) {
  std::ostringstream output;
  output << "car_id: " << car_id << "\n";
  output << "modify_time: " << ts.readable << "\n";
  output << "result: " << result << "\n";
  if (!error_message.empty()) {
    output << "error: " << error_message << "\n";
  }

  for (const auto& upload : uploads) {
    output << "camera: " << upload.rule.camera_name << "\n";
    output << "before_bitrate: " << upload.before.bitrate << "\n";
    output << "after_bitrate: " << upload.after.bitrate << "\n";
    output << "before_resolution: " << upload.before.width << "x" << upload.before.height << "\n";
    if (upload.rule.allow_resolution_change && upload.after.width && upload.after.height) {
      output << "after_resolution: " << *upload.after.width << "x" << *upload.after.height << "\n";
    } else {
      output << "after_resolution: " << upload.before.width << "x" << upload.before.height << "\n";
    }
  }
  return output.str();
}

void ProcessVehicle(const AppPaths& paths,
                    const ConnectionConfig& config,
                    const VehicleSnapshot& snapshot,
                    const std::vector<PreparedUpload>& uploads) {
  EnsureDirectory(config.local_log_dir);

  const TimestampInfo timestamp = CurrentTimestamp();
  const std::string log_name = timestamp.compact + "_" + snapshot.vehicle.car_id + ".log";
  const fs::path local_log_path = config.local_log_dir / log_name;
  const std::string success_log = BuildLogContent(snapshot.vehicle.car_id, timestamp, uploads, "success", "");

  std::optional<RemoteEndpoint> endpoint;
  try {
    endpoint.emplace(OpenRemoteEndpoint(paths, config, snapshot.vehicle));
    RunRemoteCommand(config, snapshot.vehicle, *endpoint,
                     "mkdir -p " + config.remote_config_dir + " " + config.remote_log_dir);
    for (const auto& upload : uploads) {
      const std::string remote_path = config.remote_config_dir + "/" + upload.rule.file_name;
      UploadRemoteFile(config, snapshot.vehicle, *endpoint, upload.local_path, remote_path);
    }

    WriteTextFile(local_log_path, success_log);
    UploadRemoteFile(config, snapshot.vehicle, *endpoint, local_log_path,
                     config.remote_log_dir + "/" + log_name);
  } catch (const std::exception& ex) {
    const std::string failure_log =
        BuildLogContent(snapshot.vehicle.car_id, timestamp, uploads, "failed", ex.what());
    WriteTextFile(local_log_path, failure_log);
    if (endpoint) {
      try {
        RunRemoteCommand(config, snapshot.vehicle, *endpoint,
                         "mkdir -p " + config.remote_log_dir);
        UploadRemoteFile(config, snapshot.vehicle, *endpoint, local_log_path,
                         config.remote_log_dir + "/" + log_name);
      } catch (const std::exception&) {
      }
      CloseRemoteEndpoint(*endpoint);
    }
    throw;
  }

  if (endpoint) {
    CloseRemoteEndpoint(*endpoint);
  }
}

}  // namespace

int main() {
  try {
    SetupSignalHandlers();
    const AppPaths paths = ResolveAppPaths();
    const ConnectionConfig config = LoadConnectionConfig(paths);
    const std::vector<CameraRule> all_rules = BuildCameraRules();

    PrintVehicles(config);
    std::cout << "请输入车号，多个可用空格或逗号分隔，输入 all 表示全选: ";
    std::string selection_line;
    std::getline(std::cin, selection_line);
    const std::vector<std::string> selected_cars = ParseVehicleSelection(selection_line, config);

    std::vector<VehicleSnapshot> snapshots;
    for (const auto& car_id : selected_cars) {
      const auto vehicle_it = config.vehicles.find(car_id);
      if (vehicle_it == config.vehicles.end()) {
        continue;
      }
      try {
        std::cout << "读取车辆 " << car_id << " 当前配置...\n";
        VehicleSnapshot snapshot = FetchVehicleSnapshot(paths, config, vehicle_it->second, all_rules);
        PrintVehicleSnapshot(snapshot, all_rules);
        snapshots.push_back(std::move(snapshot));
      } catch (const std::exception& ex) {
        std::cerr << "车辆 " << car_id << " 配置读取失败: " << ex.what() << "\n";
      }
    }

    if (snapshots.empty()) {
      throw std::runtime_error("没有可处理的车辆");
    }

    PrintCameraChoices(all_rules);
    std::cout << "请输入要修改的摄像头编号或名称，多个可用空格或逗号分隔，输入 all 表示全选: ";
    std::string camera_selection_line;
    std::getline(std::cin, camera_selection_line);
    const std::vector<CameraRule> selected_rules = ParseCameraSelection(camera_selection_line, all_rules);

    std::cout << "配置模式:\n";
    std::cout << "  1. 一台一台配\n";
    std::cout << "  2. 选中的一起配\n";
    const int apply_mode = AskChoice("请输入选项: ", 1, 2);

    if (apply_mode == 2) {
      const PlanRequest request = BuildInteractivePlan(selected_rules, paths);
      for (const auto& snapshot : snapshots) {
        try {
          const auto uploads = PrepareUploads(paths, snapshot, selected_rules, request);
          PrintPlannedChanges(snapshot.vehicle.car_id, uploads);
          ProcessVehicle(paths, config, snapshot, uploads);
          std::cout << "车辆 " << snapshot.vehicle.car_id << " 处理完成。\n";
        } catch (const std::exception& ex) {
          std::cerr << "车辆 " << snapshot.vehicle.car_id << " 处理失败: " << ex.what() << "\n";
        }
      }
    } else {
      for (const auto& snapshot : snapshots) {
        try {
          std::cout << "当前车辆: " << snapshot.vehicle.car_id << "\n";
          const PlanRequest request = BuildInteractivePlan(selected_rules, paths);
          const auto uploads = PrepareUploads(paths, snapshot, selected_rules, request);
          PrintPlannedChanges(snapshot.vehicle.car_id, uploads);
          ProcessVehicle(paths, config, snapshot, uploads);
          std::cout << "车辆 " << snapshot.vehicle.car_id << " 处理完成。\n";
        } catch (const std::exception& ex) {
          std::cerr << "车辆 " << snapshot.vehicle.car_id << " 处理失败: " << ex.what() << "\n";
        }
      }
    }

    std::cout << "全部处理完成。\n";
    return 0;
  } catch (const std::exception& ex) {
    CleanupActiveTunnel();
    std::cerr << "错误: " << ex.what() << "\n";
    return 1;
  }
}
