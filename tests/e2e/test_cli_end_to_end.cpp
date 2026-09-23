// Jitter Observatory - end to end command line tooling.
// Copyright 2026 Summon Software Labs.
//
// The command line tool is exercised as a real child process, and its canonical JSON
// output is what the assertions read.
#include <filesystem>
#include <string>

#include <jitter/engine.hpp>
#include <jitter/scenario.hpp>

#include "support/harness.hpp"
#include "support/process.hpp"

using namespace jitter;
using namespace jitter::test;

namespace {

std::string directory() { return scratch_directory("cli"); }

struct CliRun {
    int exit_code = -1;
    std::string output;
};

CliRun run_cli(const std::string& arguments, const std::string& capture_name) {
    const std::string executable = environment_value("JITTER_CTL_EXECUTABLE");
    const std::string capture = directory() + "/" + capture_name;
    CliRun run;
    if (executable.empty()) {
        return run;
    }
    run.exit_code = run_command(quote_argument(executable) + " " + arguments + " > " +
                                quote_argument(capture) + " 2>&1");
    run.output = read_text_file(capture);
    return run;
}

}  // namespace

JITTER_TEST(cli, version_and_metric_catalog_are_available_without_a_store) {
    const CliRun version = run_cli("version", "version.json");
    CHECK_EQ(version.exit_code, 0);
    CHECK(version.output.find("jitter-observatory") != std::string::npos);
    CHECK(version.output.find("1.0.0") != std::string::npos);

    const CliRun metrics = run_cli("metrics", "metrics.json");
    CHECK_EQ(metrics.exit_code, 0);
    CHECK(metrics.output.find("jitter.absolute_delta.mean") != std::string::npos);
    CHECK(metrics.output.find("jitter.variance.sample") != std::string::npos);
    CHECK(metrics.output.find("ns^2") != std::string::npos);

    const CliRun usage = run_cli("", "usage.txt");
    CHECK_EQ(usage.exit_code, 2);
    CHECK(usage.output.find("usage: jitterctl") != std::string::npos);
}

JITTER_TEST(cli, the_demo_reports_a_synthetic_scenario_and_writes_a_store) {
    const std::string store = directory() + "/demo.jostore";
    std::error_code error;
    std::filesystem::remove(store, error);

    const CliRun demo = run_cli("demo --store " + quote_argument(store) +
                                    " --samples 256 --seed 7 --spike-every 32",
                                "demo.json");
    CHECK_EQ(demo.exit_code, 0);
    CHECK_EQ(find_json_string(demo.output, "evidence_origin"), std::string("synthetic"));
    const std::string series = find_json_string(demo.output, "series");
    const std::string generation = find_json_string(demo.output, "generation");
    CHECK(!series.empty());
    CHECK(!generation.empty());
    CHECK(std::filesystem::exists(store));
    CHECK(std::filesystem::file_size(store) > 0);
    CHECK(demo.output.find("\"level\": \"unstable\"") != std::string::npos);

    const CliRun verify = run_cli("verify --store " + quote_argument(store), "verify.json");
    CHECK_EQ(verify.exit_code, 0);
    CHECK(verify.output.find("\"clean\": true") != std::string::npos);

    const CliRun catalog = run_cli("catalog --store " + quote_argument(store), "catalog.json");
    CHECK_EQ(catalog.exit_code, 0);
    CHECK(catalog.output.find("metric_definitions") != std::string::npos);
    CHECK(catalog.output.find("clock_domains") != std::string::npos);

    // Without an explicit admission the restored evidence is reported as stale: the
    // tool demonstrates the conservative default rather than hiding it.
    const CliRun conservative =
        run_cli("summarize --store " + quote_argument(store) + " --series " + series,
                "summarize-conservative.json");
    CHECK_EQ(conservative.exit_code, 0);
    CHECK(conservative.output.find("\"level\":\"stale\"") != std::string::npos);
    CHECK(conservative.output.find("restored_evidence_is_not_admitted_as_current") !=
          std::string::npos);

    const CliRun admitted = run_cli("summarize --store " + quote_argument(store) + " --series " +
                                        series + " --admit-persisted",
                                    "summarize-admitted.json");
    CHECK_EQ(admitted.exit_code, 0);
    CHECK(admitted.output.find("\"level\":\"unstable\"") != std::string::npos);
    CHECK(admitted.output.find("\"fresh\":256") != std::string::npos);

    const CliRun explanation =
        run_cli("explain --store " + quote_argument(store) + " --series " + series +
                    " --admit-persisted",
                "explain.json");
    CHECK_EQ(explanation.exit_code, 0);
    CHECK(explanation.output.find("boundary.authority") != std::string::npos);
    CHECK(explanation.output.find("jitter.absolute_delta.p95") != std::string::npos);

    const CliRun history = run_cli(
        "history --store " + quote_argument(store) + " --series " + series, "history.json");
    CHECK_EQ(history.exit_code, 0);
    CHECK(history.output.find("\"segments\"") != std::string::npos);
    CHECK(history.output.find("\"ordinal\":1") != std::string::npos);

    const CliRun attribution =
        run_cli("attribute --store " + quote_argument(store) + " --series " + series +
                    " --admit-persisted",
                "attribute.json");
    CHECK_EQ(attribution.exit_code, 0);
    CHECK(attribution.output.find("limitation_note") != std::string::npos);
    CHECK(attribution.output.find("\"complete\":false") != std::string::npos);

    const CliRun baseline = run_cli("baseline --store " + quote_argument(store) + " --series " +
                                        series + " --name e2e --admit-persisted",
                                    "baseline.json");
    CHECK_EQ(baseline.exit_code, 0);
    const std::string baseline_id = find_json_string(baseline.output, "id");
    CHECK(!baseline_id.empty());

    const CliRun comparison =
        run_cli("compare --store " + quote_argument(store) + " --series " + series +
                    " --baseline " + baseline_id + " --admit-persisted",
                "compare.json");
    CHECK_EQ(comparison.exit_code, 0);
    CHECK(comparison.output.find("\"overall\":\"comparable\"") != std::string::npos);
    CHECK(comparison.output.find("\"delta\":") != std::string::npos);

    // An unknown series is an explicit, non-zero failure.
    const CliRun unknown =
        run_cli("summarize --store " + quote_argument(store) + " --series " +
                    std::string(32, 'a'),
                "summarize-unknown.json");
    CHECK_EQ(unknown.exit_code, 4);

    // A corrupt store is detected, not silently repaired.
    std::filesystem::resize_file(store, std::filesystem::file_size(store) - 4);
    const CliRun damaged = run_cli("verify --store " + quote_argument(store), "verify-damaged.json");
    CHECK_EQ(damaged.exit_code, 3);
    CHECK(damaged.output.find("\"clean\": false") != std::string::npos);
}

JITTER_TEST(cli, the_server_ingests_over_the_real_protocol_and_persists_the_result) {
    const std::string executable = environment_value("JITTER_CTL_EXECUTABLE");
    REQUIRE(!executable.empty());

    const std::string store = directory() + "/serve.jostore";
    const std::string server_log = directory() + "/server.json";
    std::error_code error;
    std::filesystem::remove(store, error);
    // A stale log from an earlier run would publish a port nobody is listening on, so it
    // is removed before the server starts.
    std::filesystem::remove(server_log, error);

    EngineConfig config;
    config.policy = default_instability_policy();
    config.boot_id = 1;
    config.metrics = default_metric_set();
    WindowPolicy window;
    window.kind = WindowKind::Count;
    window.count = 64;
    window.capacity = 64;
    config.window = window;

    Observatory observatory(config);
    REQUIRE_OK(observatory.initialize());
    ScenarioPlan plan;
    plan.samples = 64;
    plan.batch_size = 16;
    auto registered = register_scenario(observatory, plan, "cli/forward");
    REQUIRE_OK(registered);
    const ScenarioPlan effective = registered.value();
    REQUIRE_OK(observatory.save(store, true, IngestContext{}));

    const std::string server_command = quote_argument(executable) + " serve --store " +
                                       quote_argument(store) + " --port 0 --connections 1 > " +
                                       quote_argument(server_log) + " 2>&1";

    std::thread server_thread([&]() { static_cast<void>(run_command(server_command)); });

    // The tool publishes the port it bound on its first output line. Waiting for that
    // line is the whole synchronisation: no timer is involved.
    std::string port;
    for (int attempt = 0; attempt < 20000 && port.empty(); ++attempt) {
        const std::string log = read_text_file(server_log);
        const std::size_t marker = log.find("listening on 127.0.0.1:");
        if (marker != std::string::npos) {
            const std::size_t digits = marker + std::string("listening on 127.0.0.1:").size();
            std::size_t end = digits;
            while (end < log.size() && log[end] >= '0' && log[end] <= '9') {
                ++end;
            }
            if (end > digits) {
                port = log.substr(digits, end - digits);
            }
        }
    }
    if (port.empty()) {
        server_thread.join();
        CHECK(false);
        return;
    }

    // The connection is made from this process so that a failure is reported instead of
    // leaving a server waiting for a client that never arrives.
    const std::vector<LatencyBatch> batches = make_batches(effective);
    HelloMessage hello;
    hello.source = effective.source;
    hello.origin = effective.origin;
    hello.authority = effective.authority;
    auto client = TcpIngestClient::connect("127.0.0.1", static_cast<std::uint16_t>(std::stoi(port)),
                                           hello);
    REQUIRE_OK(client);
    auto ack = client.value().handshake();
    REQUIRE_OK(ack);
    CHECK(ack.value().accepted);
    std::uint64_t accepted = 0;
    for (const LatencyBatch& batch : batches) {
        auto batch_ack = client.value().send_batch(batch);
        REQUIRE_OK(batch_ack);
        CHECK(batch_ack.value().accepted);
        accepted += batch_ack.value().accepted_samples;
    }
    CHECK_EQ(accepted, std::uint64_t{64});
    REQUIRE_OK(client.value().send_bye(batches.size(), batches.size()));
    client.value().close();
    server_thread.join();

    const std::string server_output = read_text_file(server_log);
    CHECK_EQ(find_json_number(server_output, "connections_accepted"), std::string("1"));
    CHECK_EQ(find_json_number(server_output, "batches_accepted"), std::string("4"));
    CHECK_EQ(find_json_number(server_output, "samples_accepted"), std::string("64"));
    CHECK_EQ(find_json_number(server_output, "frames_rejected"), std::string("0"));

    // The tool persisted what it received, and the store still verifies.
    const CliRun verify = run_cli("verify --store " + quote_argument(store), "serve-verify.json");
    CHECK_EQ(verify.exit_code, 0);
    CHECK_EQ(find_json_string(verify.output, "clean"), std::string("true"));

    const CliRun history = run_cli("history --store " + quote_argument(store) + " --series " +
                                       effective.series.hex() + " --admit-persisted",
                                   "serve-history.json");
    CHECK_EQ(history.exit_code, 0);
    CHECK_EQ(find_json_number(history.output, "retained_observations"), std::string("64"));
}
