// Jitter Observatory - ingest probe: an independent process that speaks the ingest
// protocol over a real TCP connection.
// Copyright 2026 Summon Software Labs.

#include <cstdint>
#include <iostream>
#include <string>

#include <jitter/json.hpp>
#include <jitter/render.hpp>
#include <jitter/scenario.hpp>
#include <jitter/text.hpp>
#include <jitter/transport.hpp>
#include <jitter/version.hpp>

namespace {

using namespace jitter;

void print_usage() {
    std::cout <<
        "jitter-ingest-probe - send a deterministic plan over the ingest protocol\n"
        "\n"
        "usage: jitter-ingest-probe --plan FILE --host H --port P\n"
        "\n"
        "exit codes: 0 all batches acknowledged, 2 handshake rejected,\n"
        "            3 a batch was not accepted, 4 transport failure, 5 bad arguments\n";
}

int fail(const std::string& message, int code) {
    std::cerr << "jitter-ingest-probe: " << message << "\n";
    return code;
}

std::string option(int argc, char** argv, const std::string& key, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == key) {
            return argv[i + 1];
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string token = argv[i];
        if (token == "--help" || token == "-h") {
            print_usage();
            return 0;
        }
    }

    const std::string plan_path = option(argc, argv, "--plan", "");
    const std::string host = option(argc, argv, "--host", "127.0.0.1");
    const std::string port_text = option(argc, argv, "--port", "");
    if (plan_path.empty() || port_text.empty()) {
        print_usage();
        return fail("--plan and --port are required", 5);
    }
    const auto port = text::parse_u64(port_text);
    if (!port.has_value() || port.value() == 0 || port.value() > 65535) {
        return fail("port is not a valid TCP port", 5);
    }

    auto plan = read_plan_file(plan_path);
    if (!plan.ok()) {
        return fail(plan.error().to_text(), 5);
    }
    ScenarioPlan effective = plan.value();
    effective.ingest_path = "tcp:" + host + ":" + port_text;

    const std::vector<LatencyBatch> batches = make_batches(effective);

    HelloMessage hello;
    hello.protocol_version = kWireProtocolVersion;
    hello.source = effective.source;
    hello.origin = effective.origin;
    hello.authority = effective.authority;
    hello.agent = std::string(kProductSlug) + "/" + std::string(kVersionString);
    hello.protocol_revision = 1;

    auto client = TcpIngestClient::connect(host, static_cast<std::uint16_t>(port.value()), hello);
    if (!client.ok()) {
        return fail(client.error().to_text(), 4);
    }

    auto ack = client.value().handshake();
    if (!ack.ok()) {
        client.value().close();
        return fail(ack.error().to_text(), 4);
    }
    if (!ack.value().accepted) {
        client.value().close();
        return fail("handshake rejected: " + ack.value().reason, 2);
    }

    std::uint64_t acknowledged = 0;
    std::uint64_t accepted_samples = 0;
    std::uint64_t rejected_samples = 0;
    std::string first_rejection;
    for (const LatencyBatch& batch : batches) {
        auto batch_ack = client.value().send_batch(batch);
        if (!batch_ack.ok()) {
            client.value().close();
            return fail(batch_ack.error().to_text(), 4);
        }
        if (!batch_ack.value().accepted) {
            if (first_rejection.empty()) {
                first_rejection = batch_ack.value().verdict + ":" + batch_ack.value().reason;
            }
            rejected_samples += batch_ack.value().rejected_samples;
            continue;
        }
        ++acknowledged;
        accepted_samples += batch_ack.value().accepted_samples;
    }

    static_cast<void>(client.value().send_bye(batches.size(), acknowledged));
    client.value().close();

    JsonValue report = JsonValue::make_object();
    report.set("plan", JsonValue::make_string(plan_path));
    report.set("host", JsonValue::make_string(host));
    report.set("port", JsonValue::make_u64(port.value()));
    report.set("batches_sent", JsonValue::make_u64(batches.size()));
    report.set("batches_acknowledged", JsonValue::make_u64(acknowledged));
    report.set("samples_accepted", JsonValue::make_u64(accepted_samples));
    report.set("samples_rejected", JsonValue::make_u64(rejected_samples));
    report.set("first_rejection", JsonValue::make_string(first_rejection));
    report.set("bytes_sent", JsonValue::make_u64(client.value().bytes_sent()));
    report.set("bytes_received", JsonValue::make_u64(client.value().bytes_received()));
    std::cout << make_document("ingest_probe_report", std::move(report)).dump_pretty() << "\n";

    if (acknowledged != batches.size()) {
        return 3;
    }
    return 0;
}
