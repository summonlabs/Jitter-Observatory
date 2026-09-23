// Jitter Observatory - real TCP ingest from an independent operating system process.
// Copyright 2026 Summon Software Labs.
//
// The ingest transport is REAL: a real loopback TCP connection between two real
// processes, framed by the versioned protocol in wire.hpp. The latency values carried
// over it are SYNTHETIC, and the runtime makes no claim about switching hardware,
// RDMA, InfiniBand, NVLink or multi-host fabrics anywhere in this file.
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <jitter/codec.hpp>
#include <jitter/engine.hpp>
#include <jitter/scenario.hpp>
#include <jitter/transport.hpp>

#include "support/fixtures.hpp"
#include "support/harness.hpp"
#include "support/process.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

std::string transport_directory() { return scratch_directory("transport"); }

}  // namespace

JITTER_TEST(transport, an_independent_process_ingests_over_real_tcp) {
    const std::string probe = environment_value("JITTER_PROBE_EXECUTABLE");
    REQUIRE(!probe.empty());

    ScenarioPlan plan;
    plan.samples = 256;
    plan.batch_size = 32;
    plan.seed = 424242;
    plan.spike_every = 64;

    EngineConfig config = fixture_config(512, 512);
    Observatory observatory(config);
    REQUIRE_OK(observatory.initialize());
    auto registered = register_scenario(observatory, plan, "transport/loopback");
    REQUIRE_OK(registered);
    ScenarioPlan effective = registered.value();

    const std::string plan_path = transport_directory() + "/probe-plan.txt";
    REQUIRE_OK(write_plan_file(plan_path, effective));

    IngestServerConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.port = 0;
    const IngestContext context = [&]() {
        IngestContext built;
        built.now_utc_ns = effective.start_utc_ns +
                           static_cast<std::int64_t>(effective.samples) * effective.interval_ns +
                           effective.receive_delay_ns;
        return built;
    }();
    TcpIngestServer server(observatory, server_config, [context]() { return context; });
    REQUIRE_OK(server.start());
    CHECK(server.listening());
    CHECK(server.port() != 0);

    const std::string report_path = transport_directory() + "/probe-report.txt";
    const std::string command = quote_argument(probe) + " --plan " + quote_argument(plan_path) +
                                " --host 127.0.0.1 --port " + std::to_string(server.port()) +
                                " > " + quote_argument(report_path) + " 2>&1";

    // The server accepts on this thread while the probe runs as a separate process; the
    // test blocks on the child's completion and on the server's own bookkeeping rather
    // than on any timer.
    std::thread server_thread([&server]() { static_cast<void>(server.serve_once()); });
    const int exit_code = run_command(command);
    server_thread.join();

    const std::string report = read_text_file(report_path);
    CHECK_EQ(exit_code, 0);
    CHECK_EQ(find_json_number(report, "batches_acknowledged"), std::string("8"));
    CHECK_EQ(find_json_number(report, "samples_accepted"), std::string("256"));
    CHECK_EQ(find_json_number(report, "samples_rejected"), std::string("0"));
    CHECK_EQ(find_json_number(report, "bytes_sent").empty(), false);
    CHECK_EQ(find_json_number(report, "bytes_received").empty(), false);

    CHECK_EQ(server.stats().connections_accepted, std::uint64_t{1});
    CHECK_EQ(server.stats().connections_completed, std::uint64_t{1});
    CHECK_EQ(server.stats().hellos_accepted, std::uint64_t{1});
    CHECK_EQ(server.stats().hellos_rejected, std::uint64_t{0});
    CHECK_EQ(server.stats().frames_received, std::uint64_t{10});  // hello + 8 batches + bye
    CHECK_EQ(server.stats().frames_rejected, std::uint64_t{0});
    CHECK_EQ(server.stats().batches_accepted, std::uint64_t{8});
    CHECK_EQ(server.stats().samples_accepted, std::uint64_t{256});
    CHECK(server.stats().bytes_received > 0);
    CHECK(server.stats().bytes_sent > 0);

    // The engine received the samples over the network, and the provenance says so.
    const EngineCounters counters = observatory.counters();
    CHECK_EQ(counters.batches_accepted, std::uint64_t{8});
    CHECK_EQ(counters.samples_stored, std::uint64_t{256});

    SummaryRequest request;
    request.series = effective.series;
    request.path = effective.path;
    request.generation = effective.generation;
    request.generation_ordinal = effective.ordinal;
    request.window = config.window;
    request.policy = config.policy;
    request.metrics = config.metrics;
    request.now_utc_ns = context.now_utc_ns;

    auto summary = observatory.summarize(request, context, false);
    REQUIRE_OK(summary);
    CHECK_EQ(summary.value().summary.selected, std::uint64_t{256});
    CHECK_EQ(summary.value().summary.evidence.fresh, std::uint64_t{256});
    CHECK_EQ(summary.value().summary.evidence.synthetic_origin, std::uint64_t{256});
    CHECK_EQ(summary.value().summary.evidence.real_origin, std::uint64_t{0});

    // Identical samples delivered over a real socket produce exactly the same summary
    // as the in-process path.
    EngineConfig local_config = config;
    Observatory local(local_config);
    REQUIRE_OK(local.initialize());
    auto local_plan = register_scenario(local, plan, "transport/loopback");
    REQUIRE_OK(local_plan);
    REQUIRE_OK(ingest_plan(local, local_plan.value(), context));
    auto local_summary = local.summarize(request, context, false);
    REQUIRE_OK(local_summary);
    CHECK_EQ(local_summary.value().summary.id, summary.value().summary.id);
    CHECK_EQ(local_summary.value().summary.metrics.size(), summary.value().summary.metrics.size());
    for (const MetricValue& value : summary.value().summary.metrics) {
        const MetricValue* other = local_summary.value().summary.find(value.id);
        REQUIRE(other != nullptr);
        CHECK_EQ(value.value, other->value);
    }
}

JITTER_TEST(transport, a_probe_from_an_unregistered_source_is_refused) {
    const std::string probe = environment_value("JITTER_PROBE_EXECUTABLE");
    REQUIRE(!probe.empty());

    ScenarioPlan plan;
    plan.samples = 16;
    plan.batch_size = 8;

    EngineConfig config = fixture_config();
    Observatory observatory(config);
    REQUIRE_OK(observatory.initialize());
    auto registered = register_scenario(observatory, plan, "transport/unregistered");
    REQUIRE_OK(registered);
    ScenarioPlan effective = registered.value();
    // The probe will claim a source the server never registered.
    effective.source = synthetic_source_id("unregistered-probe-source");

    const std::string plan_path = transport_directory() + "/unregistered-plan.txt";
    REQUIRE_OK(write_plan_file(plan_path, effective));

    IngestServerConfig server_config;
    server_config.port = 0;
    IngestContext context;
    context.now_utc_ns = effective.start_utc_ns + 1000000;
    TcpIngestServer server(observatory, server_config, [context]() { return context; });
    REQUIRE_OK(server.start());

    const std::string report_path = transport_directory() + "/unregistered-report.txt";
    const std::string command = quote_argument(probe) + " --plan " + quote_argument(plan_path) +
                                " --host 127.0.0.1 --port " + std::to_string(server.port()) +
                                " > " + quote_argument(report_path) + " 2>&1";
    std::thread server_thread([&server]() { static_cast<void>(server.serve_once()); });
    const int exit_code = run_command(command);
    server_thread.join();

    // The engine refuses to ingest from a source it does not know, and the probe reports
    // a batch failure rather than pretending to succeed.
    CHECK(exit_code != 0);
    const EngineCounters counters = observatory.counters();
    CHECK_EQ(counters.samples_stored, std::uint64_t{0});
    CHECK_EQ(server.stats().samples_accepted, std::uint64_t{0});
    CHECK_EQ(server.stats().batches_rejected, std::uint64_t{2});
    CHECK(server.stats().frames_received >= std::uint64_t{3});
    const std::string report = read_text_file(report_path);
    CHECK(report.find("not_found") != std::string::npos);
}

JITTER_TEST(transport, malformed_frames_are_rejected_without_touching_the_engine) {
    ScenarioPlan plan;
    plan.samples = 8;
    plan.batch_size = 8;
    EngineConfig config = fixture_config();
    Observatory observatory(config);
    REQUIRE_OK(observatory.initialize());
    auto registered = register_scenario(observatory, plan, "transport/malformed");
    REQUIRE_OK(registered);
    const ScenarioPlan effective = registered.value();

    IngestServerConfig server_config;
    server_config.port = 0;
    IngestContext context;
    context.now_utc_ns = effective.start_utc_ns + 1000000;
    TcpIngestServer server(observatory, server_config, [context]() { return context; });
    REQUIRE_OK(server.start());

    std::thread server_thread([&server]() { static_cast<void>(server.serve_once()); });

    // A well framed message of a type a client must never send first is refused, and the
    // engine never sees anything.
    auto handle = platform::connect_tcp("127.0.0.1", server.port());
    REQUIRE_OK(handle);
    FrameStream stream(handle.value());
    ErrorMessage message;
    message.code = "test";
    message.message = "unexpected first message";
    ByteWriter writer;
    write_error(writer, message);
    REQUIRE_OK(stream.write_frame(WireMessageType::Error, writer.data()));

    auto response = stream.read_frame();
    REQUIRE_OK(response);
    CHECK_EQ(response.value().type, WireMessageType::Error);
    ByteReader reader(response.value().payload);
    auto parsed = read_error(reader);
    REQUIRE_OK(parsed);
    CHECK_EQ(parsed.value().code, std::string("protocol_violation"));
    platform::close_socket(handle.value());
    server_thread.join();

    CHECK_EQ(server.stats().frames_rejected, std::uint64_t{1});
    CHECK_EQ(server.stats().frames_received, std::uint64_t{1});
    CHECK_EQ(observatory.counters().samples_stored, std::uint64_t{0});
}

JITTER_TEST(transport, the_probe_reports_a_plan_that_does_not_match_its_arguments) {
    const std::string probe = environment_value("JITTER_PROBE_EXECUTABLE");
    REQUIRE(!probe.empty());
    const std::string report_path = transport_directory() + "/probe-usage.txt";
    const int exit_code = run_command(quote_argument(probe) + " --host 127.0.0.1 > " +
                                      quote_argument(report_path) + " 2>&1");
    CHECK_EQ(exit_code, 5);
    const std::string report = read_text_file(report_path);
    CHECK(report.find("--plan and --port are required") != std::string::npos);
}

JITTER_TEST(transport, frames_are_framed_and_checksummed_on_a_real_connection) {
    ScenarioPlan plan;
    plan.samples = 16;
    plan.batch_size = 4;
    EngineConfig config = fixture_config();
    Observatory observatory(config);
    REQUIRE_OK(observatory.initialize());
    auto registered = register_scenario(observatory, plan, "transport/frames");
    REQUIRE_OK(registered);
    const ScenarioPlan effective = registered.value();
    const std::vector<LatencyBatch> batches = make_batches(effective);

    IngestServerConfig server_config;
    server_config.port = 0;
    IngestContext context;
    context.now_utc_ns = effective.start_utc_ns +
                         static_cast<std::int64_t>(effective.samples) * effective.interval_ns +
                         effective.receive_delay_ns;
    TcpIngestServer server(observatory, server_config, [context]() { return context; });
    REQUIRE_OK(server.start());

    std::thread server_thread([&server]() { static_cast<void>(server.serve_once()); });

    HelloMessage hello;
    hello.source = effective.source;
    hello.origin = effective.origin;
    hello.authority = effective.authority;
    auto client = TcpIngestClient::connect("127.0.0.1", server.port(), hello);
    REQUIRE_OK(client);
    auto ack = client.value().handshake();
    REQUIRE_OK(ack);
    CHECK(ack.value().accepted);
    CHECK_EQ(ack.value().max_frame_bytes, Limits::kMaxWireFrameBytes);

    std::uint64_t accepted = 0;
    for (const LatencyBatch& batch : batches) {
        auto batch_ack = client.value().send_batch(batch);
        REQUIRE_OK(batch_ack);
        CHECK(batch_ack.value().accepted);
        CHECK_EQ(batch_ack.value().batch, batch.content);
        accepted += batch_ack.value().accepted_samples;
    }
    CHECK_EQ(accepted, std::uint64_t{16});
    REQUIRE_OK(client.value().send_bye(batches.size(), batches.size()));
    client.value().close();
    server_thread.join();

    CHECK_EQ(observatory.counters().samples_stored, std::uint64_t{16});
    CHECK(server.stats().bytes_received > 0);
    CHECK(server.stats().bytes_sent > 0);
}
