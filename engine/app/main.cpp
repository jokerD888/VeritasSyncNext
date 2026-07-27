#include "engine/storage/database.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Usage() {
  std::cout << "Usage: veritassync-engine --headless --db <path> [--init-task <id> --mode <one_way|bidirectional> --role <source|target|peer> --root <path>]\n";
}
}
int main(int argc, char** argv) {
  try {
    bool headless = false;
    std::string db_path;
    std::string task_id, mode, role, root;
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help") { Usage(); return 0; }
      if (argument == "--headless") { headless = true; continue; }
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + argument);
      const std::string value = argv[++i];
      if (argument == "--db") db_path = value;
      else if (argument == "--init-task") task_id = value;
      else if (argument == "--mode") mode = value;
      else if (argument == "--role") role = value;
      else if (argument == "--root") root = value;
      else throw std::invalid_argument("unknown option: " + argument);
    }
    if (!headless || db_path.empty()) { Usage(); return 2; }
    veritassync::storage::Database database(db_path);
    database.ApplyMigrations();
    if (!task_id.empty()) {
      if (mode.empty() || role.empty() || root.empty()) throw std::invalid_argument("--init-task requires --mode, --role, and --root");
      database.CreateTask({task_id, mode, role, root});
      std::cout << "Created task " << task_id << " in schema v" << database.SchemaVersion() << "\n";
    } else {
      std::cout << "VeritasSyncNext headless engine ready; schema v" << database.SchemaVersion() << "\n";
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << "error: " << error.what() << "\n"; return 1; }
}
