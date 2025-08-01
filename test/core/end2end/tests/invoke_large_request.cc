//
//
// Copyright 2015 gRPC authors.
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
//
//

#include <grpc/impl/channel_arg_names.h>
#include <grpc/status.h>
#include <string.h>

#include <memory>

#include "gtest/gtest.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/slice/slice.h"
#include "src/core/util/time.h"
#include "test/core/end2end/end2end_tests.h"

namespace grpc_core {
namespace {

CORE_END2END_TEST(Http2SingleHopTests, InvokeLargeRequest) {
  const size_t kMessageSize = 10 * 1024 * 1024;
  auto send_from_client = RandomSlice(kMessageSize);
  auto send_from_server = RandomSlice(kMessageSize);
  // TODO(b/424667351): Not using the default server args since the default ping
  // timeout is too aggressive for this test under UBSAN.
  InitServer(
      ChannelArgs().Set(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, kMessageSize));
  InitClient(
      ChannelArgs().Set(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, kMessageSize));
  auto c = NewClientCall("/foo").Timeout(Duration::Minutes(5)).Create();
  IncomingStatusOnClient server_status;
  IncomingMetadata server_initial_metadata;
  IncomingMessage server_message;
  c.NewBatch(1)
      .SendInitialMetadata({})
      .SendMessage(send_from_client.Ref())
      .SendCloseFromClient()
      .RecvInitialMetadata(server_initial_metadata)
      .RecvMessage(server_message)
      .RecvStatusOnClient(server_status);
  auto s = RequestCall(101);
  Expect(101, true);
  Step(Duration::Minutes(1));
  IncomingMessage client_message;
  s.NewBatch(102).SendInitialMetadata({}).RecvMessage(client_message);
  Expect(102, true);
  Step(Duration::Minutes(1));
  IncomingCloseOnServer client_close;
  s.NewBatch(103)
      .SendStatusFromServer(GRPC_STATUS_UNIMPLEMENTED, "xyz", {})
      .SendMessage(send_from_server.Ref())
      .RecvCloseOnServer(client_close);
  Expect(103, true);
  Expect(1, true);
  Step(Duration::Minutes(1));
  EXPECT_EQ(server_status.status(), GRPC_STATUS_UNIMPLEMENTED);
  EXPECT_EQ(server_status.message(), "xyz");
  EXPECT_EQ(s.method(), "/foo");
  EXPECT_FALSE(client_close.was_cancelled());
  EXPECT_EQ(client_message.payload(), send_from_client);
  EXPECT_EQ(server_message.payload(), send_from_server);
  // TODO(b/424667351): Using an explicit shutdown with a larger timeout to
  // avoid failing on graceful shutdown.
  ShutdownServerAndNotify(104);
  Expect(104, true);
  Step(Duration::Minutes(1));
}

}  // namespace
}  // namespace grpc_core
