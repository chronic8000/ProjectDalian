#include "con_interpreter.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace bf2 {
namespace {

// Parse "x/y/z" (BF2 vector literal) into three floats.
void parse_vec3(const std::string& token, float out[3]) {
  std::size_t start = 0;
  for (int i = 0; i < 3; ++i) {
    const auto slash = token.find('/', start);
    const std::string part = token.substr(start, slash - start);
    try {
      out[i] = part.empty() ? 0.f : std::stof(part);
    } catch (...) {
      out[i] = 0.f;
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
}

}  // namespace

void ConInterpreter::register_builtin_commands() {
  register_command("ObjectTemplate.create", [this](const std::string&, const std::vector<std::string>& args) {
    if (args.empty()) {
      return;
    }
    active_object_ = args[0];
    templates_[active_object_] = ObjectTemplate{active_object_, {}, {}};
  });

  register_command("ObjectTemplate.activeInput", [this](const std::string&, const std::vector<std::string>& args) {
    if (!args.empty()) {
      active_object_ = args[0];
    }
  });

  // .activeSafe <type> <name> and .active <name> select an existing template
  // (creating a shell if it has not been seen yet). Used heavily by .tweak files.
  auto make_active = [this](const std::vector<std::string>& args) {
    if (args.empty()) {
      return;
    }
    const std::string& name = args.size() >= 2 ? args[1] : args[0];
    active_object_ = name;
    if (!templates_.contains(name)) {
      templates_[name] = ObjectTemplate{name, {}, {}};
    }
  };
  register_command("ObjectTemplate.activeSafe", [make_active](const std::string&, const std::vector<std::string>& args) {
    make_active(args);
  });
  register_command("ObjectTemplate.active", [make_active](const std::string&, const std::vector<std::string>& args) {
    make_active(args);
  });

  register_command("ObjectTemplate.addTemplate", [this](const std::string&, const std::vector<std::string>& args) {
    if (active_object_.empty() || args.empty()) {
      return;
    }
    templates_[active_object_].children.push_back(args[0]);
  });

  // Level placement commands (StaticObjects.con).
  register_command("Object.create", [this](const std::string&, const std::vector<std::string>& args) {
    if (args.empty()) {
      return;
    }
    ObjectInstance instance;
    instance.template_name = args[0];
    instances_.push_back(instance);
  });
  register_command("Object.absolutePosition", [this](const std::string&, const std::vector<std::string>& args) {
    if (instances_.empty() || args.empty()) {
      return;
    }
    parse_vec3(args[0], instances_.back().position);
  });
  register_command("Object.rotation", [this](const std::string&, const std::vector<std::string>& args) {
    if (instances_.empty() || args.empty()) {
      return;
    }
    parse_vec3(args[0], instances_.back().rotation);
  });
  register_command("Object.layer", [this](const std::string&, const std::vector<std::string>& args) {
    if (instances_.empty() || args.empty()) {
      return;
    }
    try {
      instances_.back().layer = std::stoi(args[0]);
    } catch (...) {
    }
  });
}

void ConInterpreter::register_command(const std::string& command, CommandHandler handler) {
  commands_[command] = std::move(handler);
}

std::vector<std::string> ConInterpreter::tokenize(const std::string& line) {
  std::vector<std::string> tokens;
  std::string current;
  bool in_quotes = false;
  for (char c : line) {
    if (c == '"') {
      in_quotes = !in_quotes;
      continue;
    }
    if (!in_quotes && (c == ' ' || c == '\t')) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

void ConInterpreter::execute_line(const std::string& raw_line) {
  std::string line = raw_line;
  const auto first = line.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return;
  }
  const auto last = line.find_last_not_of(" \t\r\n");
  line = line.substr(first, last - first + 1);

  if (line.rfind("rem", 0) == 0 || line.front() == ';') {
    return;
  }
  if (line.rfind("run ", 0) == 0) {
    execute_file(line.substr(4));
    return;
  }

  const auto tokens = tokenize(line);
  if (tokens.empty()) {
    return;
  }

  std::string command = tokens[0];
  std::vector<std::string> args;
  if (tokens.size() >= 2 && tokens[0].find('.') == std::string::npos) {
    command = tokens[0] + "." + tokens[1];
    args.assign(tokens.begin() + 2, tokens.end());
  } else {
    args.assign(tokens.begin() + 1, tokens.end());
  }

  if (auto it = commands_.find(command); it != commands_.end()) {
    it->second(active_object_, args);
    return;
  }

  if (!active_object_.empty() && tokens.size() >= 2) {
    std::string value;
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i > 0) {
        value += ' ';
      }
      value += args[i];
    }
    templates_[active_object_].properties[command] = value;
  }
}

bool ConInterpreter::execute_script(const std::string& script) {
  std::istringstream stream(script);
  std::string line;
  bool in_rem_block = false;
  while (std::getline(stream, line)) {
    if (line.rfind("beginRem", 0) == 0) {
      in_rem_block = true;
      continue;
    }
    if (line.rfind("endRem", 0) == 0) {
      in_rem_block = false;
      continue;
    }
    if (in_rem_block) {
      continue;
    }
    execute_line(line);
  }
  return true;
}

bool ConInterpreter::execute_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return execute_script(buffer.str());
}

ConInterpreter::ConInterpreter() { register_builtin_commands(); }

}  // namespace bf2
