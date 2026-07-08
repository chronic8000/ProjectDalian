#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {

struct ObjectTemplate {
  std::string name;
  std::unordered_map<std::string, std::string> properties;
  std::vector<std::string> children;
};

// A placed instance from a level's StaticObjects.con (Object.create ...).
struct ObjectInstance {
  std::string template_name;
  float position[3] = {0.f, 0.f, 0.f};
  float rotation[3] = {0.f, 0.f, 0.f};  // yaw / pitch / roll
  int layer = 0;
};

class ConInterpreter {
public:
  using CommandHandler = std::function<void(const std::string& object, const std::vector<std::string>& args)>;

  ConInterpreter();

  void register_command(const std::string& command, CommandHandler handler);
  bool execute_file(const std::string& path);
  bool execute_script(const std::string& script);
  const std::unordered_map<std::string, ObjectTemplate>& templates() const { return templates_; }
  const std::vector<ObjectInstance>& instances() const { return instances_; }

private:
  std::unordered_map<std::string, CommandHandler> commands_;
  std::unordered_map<std::string, ObjectTemplate> templates_;
  std::vector<ObjectInstance> instances_;
  std::string active_object_;

  void register_builtin_commands();
  void execute_line(const std::string& line);
  static std::vector<std::string> tokenize(const std::string& line);
};

}  // namespace bf2
