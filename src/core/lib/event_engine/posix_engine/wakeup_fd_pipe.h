// Copyright 2022 The gRPC Authors
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
#ifndef GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_WAKEUP_FD_PIPE_H
#define GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_WAKEUP_FD_PIPE_H

#include <grpc/support/port_platform.h>

#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/core/lib/event_engine/posix_engine/posix_interface.h"
#include "src/core/lib/event_engine/posix_engine/wakeup_fd_posix.h"

namespace grpc_event_engine::experimental {

class PipeWakeupFd : public WakeupFd {
 public:
  explicit PipeWakeupFd(EventEnginePosixInterface* posix_interface)
      : WakeupFd(), posix_interface_(posix_interface) {}
  ~PipeWakeupFd() override;
  absl::Status ConsumeWakeup() override;
  absl::Status Wakeup() override;
  static absl::StatusOr<std::unique_ptr<WakeupFd>> CreatePipeWakeupFd(
      EventEnginePosixInterface* posix_interface);
  static bool IsSupported();

 private:
  absl::Status Init();
  EventEnginePosixInterface* posix_interface_;
};

}  // namespace grpc_event_engine::experimental

#endif  // GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_WAKEUP_FD_PIPE_H
