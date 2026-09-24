#pragma once

#include <sys/types.h>

#include <string>

namespace gw::CurrentThread {

pid_t tid();
const char* tidString();
const char* name();
void setName(const char* name);
void setName(const std::string& name);

}  // namespace gw::CurrentThread
