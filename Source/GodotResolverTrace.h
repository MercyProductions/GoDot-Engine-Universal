#pragma once

#include <cstdint>

void GodotResolver_Clear();
void GodotResolver_Record(const char* symbolName, const char* sourceLabel, void* address);
int GodotResolver_WriteReport(const wchar_t* path);
