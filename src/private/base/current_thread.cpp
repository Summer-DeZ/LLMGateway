#include "base/current_thread.hpp"

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>

namespace gw::CurrentThread {

namespace thread_local_data {
thread_local char t_tid_string[32] = "0";
thread_local char t_name[32] = "?";
thread_local int t_tid_cached = 0;
}  // namespace thread_local_data

pid_t tid() {
    if (thread_local_data::t_tid_cached == 0) {
        thread_local_data::t_tid_cached = static_cast<int>(::syscall(SYS_gettid));
        std::snprintf(thread_local_data::t_tid_string, sizeof(thread_local_data::t_tid_string), "%d",
                      thread_local_data::t_tid_cached);
    }
    return static_cast<pid_t>(thread_local_data::t_tid_cached);
}

const char* tidString() {
    tid();
    return thread_local_data::t_tid_string;
}

const char* name() {
    return thread_local_data::t_name;
}

void setName(const char* name) {
    std::snprintf(thread_local_data::t_name, sizeof(thread_local_data::t_name), "%s", name);
}

void setName(const std::string& name) {
    setName(name.c_str());
}

}  // namespace gw::CurrentThread
