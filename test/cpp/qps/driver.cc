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

#include "test/cpp/qps/driver.h"

#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>

#include <cinttypes>
#include <deque>
#include <fstream>
#include <list>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "google/protobuf/timestamp.pb.h"
#include "src/core/util/crash.h"
#include "src/core/util/env.h"
#include "src/core/util/host_port.h"
#include "src/cpp/latent_see/latent_see_client.h"
#include "src/proto/grpc/channelz/v2/latent_see.grpc.pb.h"
#include "src/proto/grpc/testing/worker_service.grpc.pb.h"
#include "test/core/test_util/port.h"
#include "test/core/test_util/test_config.h"
#include "test/cpp/qps/client.h"
#include "test/cpp/qps/histogram.h"
#include "test/cpp/qps/qps_worker.h"
#include "test/cpp/qps/stats.h"
#include "test/cpp/util/test_credentials_provider.h"

using std::deque;
using std::list;
using std::unique_ptr;
using std::vector;

namespace grpc {
namespace testing {
static std::string get_host(const std::string& worker) {
  absl::string_view host;
  absl::string_view port;
  grpc_core::SplitHostPort(worker.c_str(), &host, &port);
  return std::string(host.data(), host.size());
}

static deque<string> get_workers(const string& env_name) {
  deque<string> out;
  auto env = grpc_core::GetEnv(env_name.c_str()).value_or("");
  const char* p = env.c_str();
  if (!env.empty()) {
    for (;;) {
      const char* comma = strchr(p, ',');
      if (comma) {
        out.emplace_back(p, comma);
        p = comma + 1;
      } else {
        out.emplace_back(p);
        break;
      }
    }
  }
  if (out.empty()) {
    LOG(ERROR) << "Environment variable \"" << env_name
               << "\" does not contain a list of QPS "
                  "workers to use. Set it to a comma-separated list of "
                  "hostname:port pairs, starting with hosts that should act as "
                  "servers. E.g. export "
               << env_name
               << "=\"serverhost1:1234,clienthost1:1234,clienthost2:1234\"";
  }
  return out;
}

std::string GetCredType(
    const std::string& worker_addr,
    const std::map<std::string, std::string>& per_worker_credential_types,
    const std::string& credential_type) {
  auto it = per_worker_credential_types.find(worker_addr);
  if (it != per_worker_credential_types.end()) {
    return it->second;
  }
  return credential_type;
}

// helpers for postprocess_scenario_result
static double WallTime(const ClientStats& s) { return s.time_elapsed(); }
static double SystemTime(const ClientStats& s) { return s.time_system(); }
static double UserTime(const ClientStats& s) { return s.time_user(); }
static double CliPollCount(const ClientStats& s) { return s.cq_poll_count(); }
static double SvrPollCount(const ServerStats& s) { return s.cq_poll_count(); }
static double ServerSystemTime(const ServerStats& s) { return s.time_system(); }
static double ServerUserTime(const ServerStats& s) { return s.time_user(); }
static double ServerTotalCpuTime(const ServerStats& s) {
  return s.total_cpu_time();
}
static double ServerIdleCpuTime(const ServerStats& s) {
  return s.idle_cpu_time();
}
static int Cores(int n) { return n; }

static bool IsSuccess(const Status& s) {
  if (s.ok()) return true;
  // Since we shutdown servers and clients at the same time, they both can
  // observe cancellation.  Thus, we consider CANCELLED as good status.
  if (static_cast<StatusCode>(s.error_code()) == StatusCode::CANCELLED) {
    return true;
  }
  // Since we shutdown servers and clients at the same time, server can close
  // the socket before the client attempts to do that, and vice versa.  Thus
  // receiving a "Socket closed" error is fine.
  if (s.error_message() == "Socket closed") return true;
  return false;
}

// Postprocess ScenarioResult and populate result summary.
static void postprocess_scenario_result(ScenarioResult* result) {
  // Get latencies from ScenarioResult latencies histogram and populate to
  // result summary.
  Histogram histogram;
  histogram.MergeProto(result->latencies());
  result->mutable_summary()->set_latency_50(histogram.Percentile(50));
  result->mutable_summary()->set_latency_90(histogram.Percentile(90));
  result->mutable_summary()->set_latency_95(histogram.Percentile(95));
  result->mutable_summary()->set_latency_99(histogram.Percentile(99));
  result->mutable_summary()->set_latency_999(histogram.Percentile(99.9));

  // Calculate qps and cpu load for each client and then aggregate results for
  // all clients
  double qps = 0;
  double client_system_cpu_load = 0, client_user_cpu_load = 0;
  for (int i = 0; i < result->client_stats_size(); i++) {
    auto client_stat = result->client_stats(i);
    qps += client_stat.latencies().count() / client_stat.time_elapsed();
    client_system_cpu_load +=
        client_stat.time_system() / client_stat.time_elapsed();
    client_user_cpu_load +=
        client_stat.time_user() / client_stat.time_elapsed();
  }
  // Calculate cpu load for each server and then aggregate results for all
  // servers
  double server_system_cpu_load = 0, server_user_cpu_load = 0;
  for (int i = 0; i < result->server_stats_size(); i++) {
    auto server_stat = result->server_stats(i);
    server_system_cpu_load +=
        server_stat.time_system() / server_stat.time_elapsed();
    server_user_cpu_load +=
        server_stat.time_user() / server_stat.time_elapsed();
  }
  result->mutable_summary()->set_qps(qps);
  // Populate the percentage of cpu load to result summary.
  result->mutable_summary()->set_server_system_time(100 *
                                                    server_system_cpu_load);
  result->mutable_summary()->set_server_user_time(100 * server_user_cpu_load);
  result->mutable_summary()->set_client_system_time(100 *
                                                    client_system_cpu_load);
  result->mutable_summary()->set_client_user_time(100 * client_user_cpu_load);

  // For Non-linux platform, get_cpu_usage() is not implemented. Thus,
  // ServerTotalCpuTime and ServerIdleCpuTime are both 0.
  if (average(result->server_stats(), ServerTotalCpuTime) == 0) {
    result->mutable_summary()->set_server_cpu_usage(0);
  } else {
    auto server_cpu_usage =
        100 - (100 * average(result->server_stats(), ServerIdleCpuTime) /
               average(result->server_stats(), ServerTotalCpuTime));
    result->mutable_summary()->set_server_cpu_usage(server_cpu_usage);
  }

  // Calculate and populate successful request per second and failed requests
  // per seconds to result summary.
  auto time_estimate = average(result->client_stats(), WallTime);
  if (result->request_results_size() > 0) {
    int64_t successes = 0;
    int64_t failures = 0;
    for (int i = 0; i < result->request_results_size(); i++) {
      const RequestResultCount& rrc = result->request_results(i);
      if (rrc.status_code() == 0) {
        successes += rrc.count();
      } else {
        failures += rrc.count();
      }
    }
    result->mutable_summary()->set_successful_requests_per_second(
        successes / time_estimate);
    result->mutable_summary()->set_failed_requests_per_second(failures /
                                                              time_estimate);
  }

  // Fill in data for other metrics required in result summary
  auto qps_per_server_core = qps / sum(result->server_cores(), Cores);
  result->mutable_summary()->set_qps_per_server_core(qps_per_server_core);
  result->mutable_summary()->set_client_polls_per_request(
      sum(result->client_stats(), CliPollCount) / histogram.Count());
  result->mutable_summary()->set_server_polls_per_request(
      sum(result->server_stats(), SvrPollCount) / histogram.Count());

  auto server_queries_per_cpu_sec =
      histogram.Count() / (sum(result->server_stats(), ServerSystemTime) +
                           sum(result->server_stats(), ServerUserTime));
  auto client_queries_per_cpu_sec =
      histogram.Count() / (sum(result->client_stats(), SystemTime) +
                           sum(result->client_stats(), UserTime));

  result->mutable_summary()->set_server_queries_per_cpu_sec(
      server_queries_per_cpu_sec);
  result->mutable_summary()->set_client_queries_per_cpu_sec(
      client_queries_per_cpu_sec);
}

struct ClientData {
  unique_ptr<channelz::v2::LatentSee::Stub> latent_see_stub;
  unique_ptr<WorkerService::Stub> stub;
  unique_ptr<ClientReaderWriter<ClientArgs, ClientStatus>> stream;
};

struct ServerData {
  unique_ptr<channelz::v2::LatentSee::Stub> latent_see_stub;
  unique_ptr<WorkerService::Stub> stub;
  unique_ptr<ClientReaderWriter<ServerArgs, ServerStatus>> stream;
};

static void FinishClients(const std::vector<ClientData>& clients,
                          const ClientArgs& client_mark) {
  LOG(INFO) << "Finishing clients";
  for (size_t i = 0, i_end = clients.size(); i < i_end; i++) {
    auto client = &clients[i];
    if (!client->stream->Write(client_mark)) {
      grpc_core::Crash(absl::StrFormat("Couldn't write mark to client %zu", i));
    }
    if (!client->stream->WritesDone()) {
      grpc_core::Crash(absl::StrFormat("Failed WritesDone for client %zu", i));
    }
  }
}

static void ReceiveFinalStatusFromClients(
    const std::vector<ClientData>& clients, Histogram& merged_latencies,
    std::unordered_map<int, int64_t>& merged_statuses, ScenarioResult& result) {
  LOG(INFO) << "Receiving final status from clients";
  ClientStatus client_status;
  for (size_t i = 0, i_end = clients.size(); i < i_end; i++) {
    auto client = &clients[i];
    // Read the client final status
    if (client->stream->Read(&client_status)) {
      LOG(INFO) << "Received final status from client " << i;
      const auto& stats = client_status.stats();
      merged_latencies.MergeProto(stats.latencies());
      for (int i = 0; i < stats.request_results_size(); i++) {
        merged_statuses[stats.request_results(i).status_code()] +=
            stats.request_results(i).count();
      }
      result.add_client_stats()->CopyFrom(stats);
      // Check that final status was should be the last message on the client
      // stream.
      // TODO(jtattermusch): note that that waiting for Read to return can take
      // long on some scenarios (e.g. unconstrained streaming_from_server). See
      // https://github.com/grpc/grpc/blob/3bd0cd208ea549760a2daf595f79b91b247fe240/test/cpp/qps/server_async.cc#L176
      // where the shutdown delay pretty much determines the wait here.
      CHECK(!client->stream->Read(&client_status));
    } else {
      grpc_core::Crash(
          absl::StrFormat("Couldn't get final status from client %zu", i));
    }
  }
}

static void ShutdownClients(const std::vector<ClientData>& clients,
                            ScenarioResult& result) {
  LOG(INFO) << "Shutdown clients";
  for (size_t i = 0, i_end = clients.size(); i < i_end; i++) {
    auto client = &clients[i];
    Status s = client->stream->Finish();
    // Since we shutdown servers and clients at the same time, clients can
    // observe cancellation.  Thus, we consider both OK and CANCELLED as good
    // status.
    const bool success = IsSuccess(s);
    result.add_client_success(success);
    if (!success) {
      grpc_core::Crash(absl::StrFormat("Client %zu had an error %s", i,
                                       s.error_message().c_str()));
    }
  }
}

static void FinishServers(const std::vector<ServerData>& servers,
                          const ServerArgs& server_mark) {
  LOG(INFO) << "Finishing servers";
  for (size_t i = 0, i_end = servers.size(); i < i_end; i++) {
    auto server = &servers[i];
    if (!server->stream->Write(server_mark)) {
      grpc_core::Crash(absl::StrFormat("Couldn't write mark to server %zu", i));
    }
    if (!server->stream->WritesDone()) {
      grpc_core::Crash(absl::StrFormat("Failed WritesDone for server %zu", i));
    }
  }
}

static void ReceiveFinalStatusFromServer(const std::vector<ServerData>& servers,
                                         ScenarioResult& result) {
  LOG(INFO) << "Receiving final status from servers";
  ServerStatus server_status;
  for (size_t i = 0, i_end = servers.size(); i < i_end; i++) {
    auto server = &servers[i];
    // Read the server final status
    if (server->stream->Read(&server_status)) {
      LOG(INFO) << "Received final status from server " << i;
      result.add_server_stats()->CopyFrom(server_status.stats());
      result.add_server_cores(server_status.cores());
      // That final status should be the last message on the server stream
      CHECK(!server->stream->Read(&server_status));
    } else {
      grpc_core::Crash(
          absl::StrFormat("Couldn't get final status from server %zu", i));
    }
  }
}

static void ShutdownServers(const std::vector<ServerData>& servers,
                            ScenarioResult& result) {
  LOG(INFO) << "Shutdown servers";
  for (size_t i = 0, i_end = servers.size(); i < i_end; i++) {
    auto server = &servers[i];
    Status s = server->stream->Finish();
    // Since we shutdown servers and clients at the same time, servers can
    // observe cancellation.  Thus, we consider both OK and CANCELLED as good
    // status.
    const bool success = IsSuccess(s);
    result.add_server_success(success);
    if (!success) {
      grpc_core::Crash(absl::StrFormat("Server %zu had an error %s", i,
                                       s.error_message().c_str()));
    }
  }
}

std::vector<grpc::testing::Server*>* g_inproc_servers = nullptr;

std::unique_ptr<ScenarioResult> RunScenario(const RunScenarioOptions& options) {
  if (options.run_inproc) {
    g_inproc_servers = new std::vector<grpc::testing::Server*>;
  }

  // ClientContext allocations (all are destroyed at scope exit)
  list<ClientContext> contexts;
  auto alloc_context = [](list<ClientContext>* contexts) {
    contexts->emplace_back();
    auto context = &contexts->back();
    context->set_wait_for_ready(true);
    return context;
  };

  // To be added to the result, containing the final configuration used for
  // client and config (including host, etc.)
  ClientConfig result_client_config;

  // Get client, server lists; ignore if inproc test
  auto workers =
      (!options.run_inproc) ? get_workers("QPS_WORKERS") : deque<string>();
  ClientConfig client_config = options.client_config;

  // Spawn some local workers if desired
  vector<unique_ptr<QpsWorker>> local_workers;
  for (int i = 0; i < abs(options.spawn_local_worker_count); i++) {
    // act as if we're a new test -- gets a good rng seed
    static bool called_init = false;
    if (!called_init) {
      char args_buf[100];
      strcpy(args_buf, "some-benchmark");
      char* args[] = {args_buf};
      int argc = 1;
      grpc_test_init(&argc, args);
      called_init = true;
    }

    char addr[256];
    // we use port # of -1 to indicate inproc
    int driver_port =
        (!options.run_inproc) ? grpc_pick_unused_port_or_die() : -1;
    local_workers.emplace_back(
        new QpsWorker(driver_port, 0, options.credential_type));
    sprintf(addr, "localhost:%d", driver_port);
    if (options.spawn_local_worker_count < 0) {
      workers.push_front(addr);
    } else {
      workers.push_back(addr);
    }
  }
  CHECK(!workers.empty());

  // if num_clients is set to <=0, do dynamic sizing: all workers
  // except for servers are clients
  size_t num_clients_to_use = options.num_clients;
  if (options.num_clients <= 0) {
    num_clients_to_use = workers.size() - options.num_servers;
  }

  // TODO(ctiller): support running multiple configurations, and binpack
  // client/server pairs
  // to available workers
  CHECK_GE(workers.size(), num_clients_to_use + options.num_servers);

  // Trim to just what we need
  workers.resize(num_clients_to_use + options.num_servers);

  // Start servers
  std::vector<ServerData> servers(options.num_servers);
  std::unordered_map<string, std::deque<int>> hosts_cores;
  ChannelArguments channel_args;

  for (size_t i = 0; i < options.num_servers; i++) {
    LOG(INFO) << "Starting server on " << workers[i] << " (worker #" << i
              << ")";
    std::shared_ptr<Channel> channel;
    if (!options.run_inproc) {
      channel = grpc::CreateTestChannel(
          workers[i],
          GetCredType(workers[i], options.per_worker_credential_types,
                      options.credential_type),
          nullptr /* call creds */, {} /* interceptor creators */);
    } else {
      channel = local_workers[i]->InProcessChannel(channel_args);
    }
    servers[i].stub = WorkerService::NewStub(channel);
    servers[i].latent_see_stub = channelz::v2::LatentSee::NewStub(channel);

    if (options.server_config.core_limit() != 0) {
      grpc_core::Crash("server config core limit is set but ignored by driver");
    }

    ServerArgs args;
    *args.mutable_setup() = options.server_config;
    servers[i].stream = servers[i].stub->RunServer(alloc_context(&contexts));
    if (!servers[i].stream->Write(args)) {
      grpc_core::Crash(
          absl::StrFormat("Could not write args to server %zu", i));
    }
    ServerStatus init_status;
    if (!servers[i].stream->Read(&init_status)) {
      grpc_core::Crash(
          absl::StrFormat("Server %zu did not yield initial status", i));
    }
    if (options.run_inproc) {
      std::string cli_target(INPROC_NAME_PREFIX);
      cli_target += std::to_string(i);
      client_config.add_server_targets(cli_target);
    } else {
      std::string host = get_host(workers[i]);
      std::string cli_target =
          grpc_core::JoinHostPort(host.c_str(), init_status.port());
      client_config.add_server_targets(cli_target.c_str());
    }
  }
  if (!options.qps_server_target_override.empty()) {
    // overriding the qps server target only makes since if there is <= 1
    // servers
    CHECK_LE(options.num_servers, 1u);
    client_config.clear_server_targets();
    client_config.add_server_targets(options.qps_server_target_override);
  }
  client_config.set_median_latency_collection_interval_millis(
      options.median_latency_collection_interval_millis);

  // Targets are all set by now
  result_client_config = client_config;
  // Start clients
  std::vector<ClientData> clients(num_clients_to_use);
  size_t channels_allocated = 0;
  for (size_t i = 0; i < num_clients_to_use; i++) {
    const auto& worker = workers[i + options.num_servers];
    LOG(INFO) << "Starting client on " << worker << " (worker #"
              << i + options.num_servers << ")";
    std::shared_ptr<Channel> channel;
    if (!options.run_inproc) {
      channel = grpc::CreateTestChannel(
          worker,
          GetCredType(worker, options.per_worker_credential_types,
                      options.credential_type),
          nullptr /* call creds */, {} /* interceptor creators */);
    } else {
      channel = local_workers[i + options.num_servers]->InProcessChannel(
          channel_args);
    }
    clients[i].stub = WorkerService::NewStub(channel);
    clients[i].latent_see_stub = channelz::v2::LatentSee::NewStub(channel);
    ClientConfig per_client_config = client_config;

    if (options.client_config.core_limit() != 0) {
      grpc_core::Crash("client config core limit set but ignored");
    }

    // Reduce channel count so that total channels specified is held regardless
    // of the number of clients available
    size_t num_channels =
        (client_config.client_channels() - channels_allocated) /
        (num_clients_to_use - i);
    channels_allocated += num_channels;
    VLOG(2) << "Client " << i << " gets " << num_channels << " channels";
    per_client_config.set_client_channels(num_channels);

    ClientArgs args;
    *args.mutable_setup() = per_client_config;
    clients[i].stream = clients[i].stub->RunClient(alloc_context(&contexts));
    if (!clients[i].stream->Write(args)) {
      grpc_core::Crash(
          absl::StrFormat("Could not write args to client %zu", i));
    }
  }

  for (size_t i = 0; i < num_clients_to_use; i++) {
    ClientStatus init_status;
    if (!clients[i].stream->Read(&init_status)) {
      grpc_core::Crash(
          absl::StrFormat("Client %zu did not yield initial status", i));
    }
  }

  // Send an initial mark: clients can use this to know that everything is ready
  // to start
  LOG(INFO) << "Initiating";
  ServerArgs server_mark;
  server_mark.mutable_mark()->set_reset(true);
  server_mark.mutable_mark()->set_name("warmup");
  ClientArgs client_mark;
  client_mark.mutable_mark()->set_reset(true);
  client_mark.mutable_mark()->set_name("warmup");
  ServerStatus server_status;
  ClientStatus client_status;
  for (size_t i = 0; i < num_clients_to_use; i++) {
    auto client = &clients[i];
    if (!client->stream->Write(client_mark)) {
      grpc_core::Crash(absl::StrFormat("Couldn't write mark to client %zu", i));
    }
  }
  for (size_t i = 0; i < num_clients_to_use; i++) {
    auto client = &clients[i];
    if (!client->stream->Read(&client_status)) {
      grpc_core::Crash(
          absl::StrFormat("Couldn't get status from client %zu", i));
    }
  }

  // Let everything warmup
  LOG(INFO) << "Warming up";
  gpr_timespec start = gpr_now(GPR_CLOCK_REALTIME);
  gpr_sleep_until(gpr_time_add(
      start, gpr_time_from_seconds(options.warmup_seconds, GPR_TIMESPAN)));

  if (options.latent_see_directory.has_value()) {
    LOG(INFO) << "Collecting latent-see";

    client_mark.mutable_mark()->set_name("latent-see");
    server_mark.mutable_mark()->set_name("latent-see");

    for (size_t i = 0; i < options.num_servers; i++) {
      auto server = &servers[i];
      if (!server->stream->Write(server_mark)) {
        grpc_core::Crash(
            absl::StrFormat("Couldn't write mark to server %zu", i));
      }
    }
    for (size_t i = 0; i < num_clients_to_use; i++) {
      auto client = &clients[i];
      if (!client->stream->Write(client_mark)) {
        grpc_core::Crash(
            absl::StrFormat("Couldn't write mark to client %zu", i));
      }
    }
    for (size_t i = 0; i < options.num_servers; i++) {
      auto server = &servers[i];
      if (!server->stream->Read(&server_status)) {
        grpc_core::Crash(
            absl::StrFormat("Couldn't get status from server %zu", i));
      }
    }
    for (size_t i = 0; i < num_clients_to_use; i++) {
      auto client = &clients[i];
      if (!client->stream->Read(&client_status)) {
        grpc_core::Crash(
            absl::StrFormat("Couldn't get status from client %zu", i));
      }
    }

    std::vector<std::thread> threads;
    for (size_t i = 0; i < options.num_servers; i++) {
      threads.emplace_back(std::thread([&options, &servers, i]() {
        std::ofstream out(
            absl::StrCat(*options.latent_see_directory, "/server", i, ".json"));
        grpc_core::latent_see::JsonOutput json_out(out);
        FetchLatentSee(servers[i].latent_see_stub.get(), 1.0, &json_out)
            .IgnoreError();
      }));
    }
    for (size_t i = 0; i < num_clients_to_use; i++) {
      threads.emplace_back(std::thread([&options, &clients, i]() {
        std::ofstream out(
            absl::StrCat(*options.latent_see_directory, "/client", i, ".json"));
        grpc_core::latent_see::JsonOutput json_out(out);
        FetchLatentSee(clients[i].latent_see_stub.get(), 1.0, &json_out)
            .IgnoreError();
      }));
    }
    for (auto& t : threads) {
      t.join();
    }
  }

  // Start a run
  LOG(INFO) << "Starting";

  auto start_time = time(nullptr);

  client_mark.mutable_mark()->set_name("benchmark");
  server_mark.mutable_mark()->set_name("benchmark");

  for (size_t i = 0; i < options.num_servers; i++) {
    auto server = &servers[i];
    if (!server->stream->Write(server_mark)) {
      grpc_core::Crash(absl::StrFormat("Couldn't write mark to server %zu", i));
    }
  }
  for (size_t i = 0; i < num_clients_to_use; i++) {
    auto client = &clients[i];
    if (!client->stream->Write(client_mark)) {
      grpc_core::Crash(absl::StrFormat("Couldn't write mark to client %zu", i));
    }
  }
  for (size_t i = 0; i < options.num_servers; i++) {
    auto server = &servers[i];
    if (!server->stream->Read(&server_status)) {
      grpc_core::Crash(
          absl::StrFormat("Couldn't get status from server %zu", i));
    }
  }
  for (size_t i = 0; i < num_clients_to_use; i++) {
    auto client = &clients[i];
    if (!client->stream->Read(&client_status)) {
      grpc_core::Crash(
          absl::StrFormat("Couldn't get status from client %zu", i));
    }
  }

  // Wait some time
  LOG(INFO) << "Running";
  // Use gpr_sleep_until rather than this_thread::sleep_until to support
  // compilers that don't work with this_thread
  gpr_sleep_until(gpr_time_add(
      start,
      gpr_time_from_seconds(options.warmup_seconds + options.benchmark_seconds,
                            GPR_TIMESPAN)));

  client_mark.mutable_mark()->set_name("done");
  server_mark.mutable_mark()->set_name("done");

  // Finish a run
  std::unique_ptr<ScenarioResult> result(new ScenarioResult);
  Histogram merged_latencies;
  std::unordered_map<int, int64_t> merged_statuses;

  // For the case where clients lead the test such as UNARY and
  // STREAMING_FROM_CLIENT, clients need to finish completely while a server
  // is running to prevent the clients from being stuck while waiting for
  // the result.
  bool client_finish_first =
      (options.client_config.rpc_type() != STREAMING_FROM_SERVER);

  auto end_time = time(nullptr);

  FinishClients(clients, client_mark);

  if (!client_finish_first) {
    FinishServers(servers, server_mark);
  }

  ReceiveFinalStatusFromClients(clients, merged_latencies, merged_statuses,
                                *result);
  ShutdownClients(clients, *result);

  if (client_finish_first) {
    FinishServers(servers, server_mark);
  }

  ReceiveFinalStatusFromServer(servers, *result);
  ShutdownServers(servers, *result);

  delete g_inproc_servers;

  merged_latencies.FillProto(result->mutable_latencies());
  for (std::unordered_map<int, int64_t>::iterator it = merged_statuses.begin();
       it != merged_statuses.end(); ++it) {
    RequestResultCount* rrc = result->add_request_results();
    rrc->set_status_code(it->first);
    rrc->set_count(it->second);
  }

  // Fill in start and end time for the test scenario
  result->mutable_summary()->mutable_start_time()->set_seconds(start_time);
  result->mutable_summary()->mutable_end_time()->set_seconds(end_time);

  postprocess_scenario_result(result.get());
  return result;
}

bool RunQuit(
    const std::string& credential_type,
    const std::map<std::string, std::string>& per_worker_credential_types) {
  // Get client, server lists
  bool result = true;
  auto workers = get_workers("QPS_WORKERS");
  if (workers.empty()) {
    return false;
  }

  for (size_t i = 0; i < workers.size(); i++) {
    auto stub = WorkerService::NewStub(grpc::CreateTestChannel(
        workers[i],
        GetCredType(workers[i], per_worker_credential_types, credential_type),
        nullptr /* call creds */, {} /* interceptor creators */));
    Void phony;
    grpc::ClientContext ctx;
    ctx.set_wait_for_ready(true);
    Status s = stub->QuitWorker(&ctx, phony, &phony);
    if (!s.ok()) {
      LOG(ERROR) << "Worker " << i << " could not be properly quit because "
                 << s.error_message();
      result = false;
    }
  }
  return result;
}

}  // namespace testing
}  // namespace grpc
