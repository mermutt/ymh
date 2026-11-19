#pragma once

namespace ymh {

// Process-wide libcurl initialisation. Safe to call from any thread and from
// any translation unit; the first call runs `curl_global_init` exactly once.
void ensure_curl_global_init();

} // namespace ymh
