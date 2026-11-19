#include "ymh/llm/curl_init.hpp"

#include <mutex>

#include <curl/curl.h>

namespace ymh {
namespace {

std::once_flag g_curl_init;

} // namespace

void ensure_curl_global_init() {
    std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace ymh
