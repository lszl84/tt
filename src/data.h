#pragma once

#include <filesystem>
#include <ctime>
#include <string>

class App;

std::filesystem::path GetDataPath();
bool LoadState(App& app);
bool SaveState(const App& app);

std::time_t TodayMidnight();
std::string FormatISO(std::time_t t);
std::time_t ParseISO(const std::string& s);
