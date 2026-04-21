#pragma once

#include <filesystem>

class App;

std::filesystem::path GetDataPath();
bool LoadState(App& app);
bool SaveState(const App& app);
