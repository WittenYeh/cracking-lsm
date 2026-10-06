// Copyright 2026 Weitang Ye
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstddef>
#include <cstdio>
#include <exception>

namespace cracking_lsm::utils {

/**
 * @brief Reports a fatal error with caller-supplied request context, then terminates.
 * @pre component, operation, and reason are non-null, null-terminated strings.
 *
 * A non-null detail supplies optional exception information. All numeric values are supplied by the
 * caller; reporting does not access component state, construct dynamic strings, or multiply counts.
 * Diagnostic output is best-effort; a write or flush failure must still terminate.
 */
[[noreturn]] inline auto report_fatal_error(const char* component, const char* operation, const char* reason,
    std::size_t count, std::size_t element_bytes, const char* detail = nullptr) noexcept -> void {
    std::fprintf(stderr,
        "cracking-lsm: %s::%s: %s "
        "(count=%zu, element_bytes=%zu)%s%s\n",
        component, operation, reason, count, element_bytes,
        detail == nullptr ? "" : ": ", detail == nullptr ? "" : detail);
    std::fflush(stderr);
    std::terminate();
}

}  // namespace cracking_lsm::utils
