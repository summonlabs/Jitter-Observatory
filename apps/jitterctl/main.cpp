// Jitter Observatory - jitterctl command line tool.
// Copyright 2026 Summon Software Labs.
//
// Every subcommand either reads an existing store, writes one, or both. The tool never
// fabricates a claim: a synthetic store is labelled synthetic throughout, and a
// command that cannot answer says so with an explicit reason code.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <jitter/engine.hpp>
#include <jitter/json.hpp>
#include <jitter/persistence.hpp>
#include <jitter/render.hpp>
#include <jitter/scenario.hpp>
#include <jitter/text.hpp>
#include <jitter/transport.hpp>
#include <jitter/version.hpp>

namespace {

using namespace jitter;

struct Options {
    std::map<std::string, std::string> values;
    std::vector<std::string> flags;

    bool has(const std::string& key) const { return values.find(key) != values.end(); }
    bool flag(const std::string& key) const {
        for (const std::string& item : flags) {
            if (item == key) {
                return true;
            }
        }
        return false;
    }
    std::string get(const std::string& key, const std::string& fallback = {}) const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    std::int64_t number(const std::string& key, std::int64_t fallback) const {
        const auto it = values.find(key);
        if (it == values.end()) {
            return fallback;
        }
        const auto parsed = text::parse_i64(it->second);
        return parsed.has_value() ? parsed.value() : fallback;
    }
};

void print_usage() {
    std::cout <<
        "jitterctl - Jitter Observatory command line tool\n"
        "\n"
        "usage: jitterctl <command> [options]\n"
        "\n"
        "commands:\n"
        "  version                                             print version information\n"
        "  metrics                                             list metric definitions\n"
        "  demo      --store F [--samples N] [--seed N]        build a synthetic store and report it\n"
        "            [--spike-every N] [--incomparable]\n"
        "  catalog   --store F                                 print the declared catalog\n"
        "  summarize --store F --series HEX [--now N]          summarize the current window\n"
        "  explain   --store F --series HEX [--now N]          explain the current classification\n"
        "  baseline  --store F --series HEX --name TEXT        capture a named baseline\n"
        "  compare   --store F --series HEX --baseline HEX     compare against a baseline\n"
        "  history   --store F --series HEX --path HEX         route generation segmented history\n"
        "  attribute --store F --series HEX [--metric NAME]    per hop timing attribution\n"
        "  verify    --store F                                 verify store integrity\n"
        "  serve     --store F --port N [--connections N]      accept ingest connections\n"
        "options:\n"
        "  --pretty            indent JSON output\n"
        "  --now N             explicit current instant in UTC nanoseconds\n"
        "  --admit-persisted   admit persisted evidence as current; off by default, so a\n"
        "                      restored store reports its evidence as stale on purpose\n";
}

int fail(const std::string& message, int code = 1) {
    std::cerr << "jitterctl: " << message << "\n";
    return code;
}

Result<Options> parse(int argc, char** argv, std::string& command) {
    Options options;
    if (argc < 2) {
        return Result<Options>::fail(ErrorCode::InvalidArgument, "no command given");
    }
    command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string token = argv[i];
        if (text::starts_with(token, "--")) {
            const std::string key = token.substr(2);
            if (i + 1 < argc && !text::starts_with(std::string(argv[i + 1]), "--")) {
                options.values[key] = argv[i + 1];
                ++i;
            } else {
                options.flags.push_back(key);
            }
        } else {
            return Result<Options>::fail(ErrorCode::InvalidArgument, "unexpected positional argument", token);
        }
    }
    return options;
}

EngineConfig default_config(std::uint64_t boot_id, bool admit_persisted) {
    EngineConfig config;
    config.policy = default_instability_policy();
    config.boot_id = boot_id;
    // The library default withholds persisted evidence from current classifications.
    // The inspection tool keeps that default unless the operator asks otherwise.
    config.admit_persisted_evidence = admit_persisted;
    config.metrics = default_metric_set();
    WindowPolicy window;
    window.kind = WindowKind::Count;
    window.count = 256;
    window.capacity = 512;
    config.window = window;
    return config;
}

Status load_store_if_present(Observatory& observatory, const std::string& path) {
    std::ifstream probe(path, std::ios::binary);
    if (!probe.is_open()) {
        return Status::success();
    }
    probe.close();
    RestoreOptions options;
    options.repair_truncated_tail = false;
    options.admit_persisted_evidence = true;
    auto restored = observatory.restore(path, options);
    if (!restored.ok()) {
        return restored.error();
    }
    return Status::success();
}

std::string series_from_options(const Options& options) { return options.get("series"); }

int command_demo(Observatory& observatory, const Options& options) {
    const std::string store = options.get("store");
    if (store.empty()) {
        return fail("demo requires --store");
    }
    ScenarioPlan plan;
    plan.samples = static_cast<std::uint64_t>(options.number("samples", 512));
    plan.seed = static_cast<std::uint64_t>(options.number("seed", 12345));
    plan.batch_size = static_cast<std::uint64_t>(options.number("batch", 16));
    plan.spike_every = options.number("spike-every", 0);
    plan.incomparable_second_hop = options.flag("incomparable");
    plan.ingest_path = "in_process:synthetic";

    auto registered = register_scenario(observatory, plan, "synthetic/forward");
    if (!registered.ok()) {
        return fail(registered.error().to_text());
    }
    const ScenarioPlan effective = registered.value();
    const std::int64_t now = effective.start_utc_ns +
                             static_cast<std::int64_t>(effective.samples) * effective.interval_ns +
                             effective.receive_delay_ns;
    IngestContext context;
    context.now_utc_ns = now;
    const Status ingested = ingest_plan(observatory, effective, context);
    if (!ingested.ok()) {
        return fail(ingested.error().to_text());
    }

    SummaryRequest request;
    request.series = effective.series;
    request.path = effective.path;
    request.generation = effective.generation;
    request.generation_ordinal = effective.ordinal;
    request.window = observatory.config().window;
    request.policy = observatory.config().policy;
    request.metrics = observatory.config().metrics;
    request.now_utc_ns = now;
    request.current_generation = true;

    auto summary = observatory.summary_document(request, context);
    if (!summary.ok()) {
        return fail(summary.error().to_text());
    }
    const Status saved = observatory.save(store, true, context);
    if (!saved.ok()) {
        return fail(saved.error().to_text());
    }

    JsonValue document = JsonValue::make_object();
    document.set("store", JsonValue::make_string(store));
    document.set("evidence_origin", JsonValue::make_string(to_string(effective.origin)));
    document.set("series", JsonValue::make_string(effective.series.hex()));
    document.set("path", JsonValue::make_string(effective.path.hex()));
    document.set("generation", JsonValue::make_string(effective.generation.hex()));
    document.set("samples", JsonValue::make_u64(effective.samples));
    document.set("summary", std::move(summary.value()));
    std::cout << document.dump_pretty() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string command;
    auto parsed = parse(argc, argv, command);
    if (!parsed.ok()) {
        print_usage();
        return fail(parsed.error().message, 2);
    }
    const Options options = parsed.value();

    if (command == "help" || command == "--help") {
        print_usage();
        return 0;
    }
    if (command == "version") {
        JsonValue document = make_document("version", render_version());
        std::cout << document.dump_pretty() << "\n";
        return 0;
    }
    if (command == "metrics") {
        JsonValue definitions = JsonValue::make_array();
        for (const MetricDefinition& definition : metric_registry().definitions()) {
            definitions.push(render_metric_definition(definition));
        }
        JsonValue document = make_document("metric_catalog", std::move(definitions));
        std::cout << (options.flag("pretty") ? document.dump_pretty() : document.dump()) << "\n";
        return 0;
    }

    const std::string store = options.get("store");
    if (command != "demo" && store.empty()) {
        return fail(command + " requires --store");
    }

    EngineConfig config = default_config(options.number("boot", 1), options.flag("admit-persisted"));
    Observatory observatory(config);
    const Status initialized = observatory.initialize();
    if (!initialized.ok()) {
        return fail(initialized.error().to_text());
    }

    if (command == "demo") {
        return command_demo(observatory, options);
    }

    if (command == "verify") {
        auto result = load_store(store);
        if (!result.ok()) {
            return fail(result.error().to_text());
        }
        JsonValue document = JsonValue::make_object();
        document.set("store", JsonValue::make_string(store));
        document.set("clean", JsonValue::make_bool(result.value().recovery.clean));
        document.set("records", JsonValue::make_u64(result.value().recovery.records_recovered));
        document.set("detail", JsonValue::make_string(result.value().recovery.describe()));
        document.set("format_version", JsonValue::make_u64(result.value().header.format_version));
        document.set("store_epoch", JsonValue::make_u64(result.value().header.store_epoch));
        document.set("boot_id", JsonValue::make_u64(result.value().header.boot_id));
        std::cout << make_document("store_verification", std::move(document)).dump_pretty() << "\n";
        return result.value().recovery.clean ? 0 : 3;
    }

    const Status loaded = load_store_if_present(observatory, store);
    if (!loaded.ok()) {
        return fail(loaded.error().to_text());
    }

    IngestContext context;
    // "now" is never read from the ambient clock: it is either supplied explicitly or
    // derived from the newest evidence the runtime already holds.
    if (options.has("now")) {
        context.now_utc_ns = options.number("now", 0);
    } else {
        const auto newest = observatory.newest_received_utc_ns();
        context.now_utc_ns = newest.has_value() ? newest.value() + 1 : 0;
    }

    if (command == "catalog") {
        auto document = observatory.catalog_document();
        if (!document.ok()) {
            return fail(document.error().to_text());
        }
        std::cout << (options.flag("pretty") ? document.value().dump_pretty()
                                             : document.value().dump())
                  << "\n";
        return 0;
    }

    if (command == "serve") {
        IngestServerConfig server_config;
        server_config.port = static_cast<std::uint16_t>(options.number("port", 0));
        auto server = std::make_unique<TcpIngestServer>(
            observatory, server_config, [context]() { return context; });
        const Status started = server->start();
        if (!started.ok()) {
            return fail(started.error().to_text());
        }
        std::cout << "listening on " << server_config.bind_address << ":" << server->port() << "\n";
        std::cout.flush();
        const std::uint64_t connections =
            static_cast<std::uint64_t>(options.number("connections", 1));
        const Status served = server->serve(connections);
        if (!served.ok()) {
            return fail(served.error().to_text());
        }
        const Status saved = observatory.save(store, true, context);
        if (!saved.ok()) {
            return fail(saved.error().to_text());
        }
        JsonValue document = JsonValue::make_object();
        document.set("connections_accepted", JsonValue::make_u64(server->stats().connections_accepted));
        document.set("connections_completed", JsonValue::make_u64(server->stats().connections_completed));
        document.set("hellos_accepted", JsonValue::make_u64(server->stats().hellos_accepted));
        document.set("hellos_rejected", JsonValue::make_u64(server->stats().hellos_rejected));
        document.set("batches_accepted", JsonValue::make_u64(server->stats().batches_accepted));
        document.set("batches_rejected", JsonValue::make_u64(server->stats().batches_rejected));
        document.set("samples_accepted", JsonValue::make_u64(server->stats().samples_accepted));
        document.set("samples_rejected", JsonValue::make_u64(server->stats().samples_rejected));
        document.set("frames_rejected", JsonValue::make_u64(server->stats().frames_rejected));
        document.set("bytes_received", JsonValue::make_u64(server->stats().bytes_received));
        document.set("last_error", JsonValue::make_string(server->stats().last_error));
        std::cout << make_document("ingest_session", std::move(document)).dump_pretty() << "\n";
        return 0;
    }

    SeriesId series;
    if (!series_from_options(options).empty()) {
        auto parsed_series = SeriesId::parse(series_from_options(options));
        if (!parsed_series.ok()) {
            return fail(parsed_series.error().to_text());
        }
        series = parsed_series.value();
    } else if (command != "catalog") {
        return fail(command + " requires --series");
    }

    const SeriesDescriptor* descriptor = observatory.series_catalog().find(series);
    if (descriptor == nullptr) {
        return fail("unknown series identity", 4);
    }
    const PathGeneration* generation = observatory.paths().current(descriptor->path);
    if (generation == nullptr) {
        return fail("series path has no generation", 4);
    }

    SummaryRequest request;
    request.series = series;
    request.path = descriptor->path;
    request.generation = generation->id;
    request.generation_ordinal = generation->ordinal;
    request.window = config.window;
    request.policy = config.policy;
    request.metrics = config.metrics;
    request.now_utc_ns = context.now_utc_ns;
    request.current_generation = generation->is_open();

    if (command == "summarize") {
        auto document = observatory.export_summary(request, context, options.flag("pretty"));
        if (!document.ok()) {
            return fail(document.error().to_text(), 5);
        }
        std::cout << document.value() << "\n";
        return 0;
    }
    if (command == "explain") {
        auto explanation = observatory.explain(request, context);
        if (!explanation.ok()) {
            return fail(explanation.error().to_text(), 5);
        }
        JsonValue document = make_document("explanation", render_explanation(explanation.value()));
        std::cout << (options.flag("pretty") ? document.dump_pretty() : document.dump()) << "\n";
        return 0;
    }
    if (command == "baseline") {
        const std::string name = options.get("name", "baseline");
        auto baseline = observatory.capture_baseline(name, request, context, "captured by jitterctl");
        if (!baseline.ok()) {
            return fail(baseline.error().to_text(), 5);
        }
        const Status saved = observatory.save(store, true, context);
        if (!saved.ok()) {
            return fail(saved.error().to_text());
        }
        JsonValue document = make_document("baseline", render_baseline(baseline.value()));
        std::cout << (options.flag("pretty") ? document.dump_pretty() : document.dump()) << "\n";
        return 0;
    }
    if (command == "compare") {
        auto baseline_id = BaselineId::parse(options.get("baseline"));
        if (!baseline_id.ok()) {
            return fail(baseline_id.error().to_text(), 2);
        }
        CompareRequest compare_request;
        compare_request.current = request;
        compare_request.baseline = baseline_id.value();
        auto document = observatory.export_comparison(compare_request, context, options.flag("pretty"));
        if (!document.ok()) {
            return fail(document.error().to_text(), 5);
        }
        std::cout << document.value() << "\n";
        return 0;
    }
    if (command == "history") {
        HistoryRequest history_request;
        history_request.series = series;
        history_request.path = descriptor->path;
        auto document = observatory.export_history(history_request, options.flag("pretty"));
        if (!document.ok()) {
            return fail(document.error().to_text(), 5);
        }
        std::cout << document.value() << "\n";
        return 0;
    }
    if (command == "attribute") {
        AttributionRequest attribution_request;
        attribution_request.series = series;
        attribution_request.min_arrivals = 4;
        const std::string metric_name = options.get("metric", "jitter.absolute_delta.mean");
        const MetricDefinition* definition = metric_registry().find_by_name(metric_name);
        if (definition == nullptr) {
            return fail("unknown metric name: " + metric_name, 2);
        }
        attribution_request.metric = definition->key;
        auto document = observatory.export_attribution(attribution_request, context, options.flag("pretty"));
        if (!document.ok()) {
            return fail(document.error().to_text(), 5);
        }
        std::cout << document.value() << "\n";
        return 0;
    }
    print_usage();
    return fail("unknown command: " + command, 2);
}
