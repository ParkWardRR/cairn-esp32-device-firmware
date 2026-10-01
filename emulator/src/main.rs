//! Cairn device emulator.
//!
//! The v2 rebuild is in progress. `conformance` checks this crate's
//! independent implementation of bundle format v2 against the committed
//! vectors; `scenario` still drives the legacy v1 capture path, which is
//! retired as the v2 device lands.

mod conformance;
mod format;
mod v2;

// Legacy v1 capture path. Retired once the v2 device replaces it.
mod config;
mod device;
mod driving;
mod math;
mod network;
mod scenarios;
mod sensors;
mod storage;
mod types;

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;

use chrono::Utc;
use clap::Parser;
use rand::SeedableRng;
use rand::rngs::StdRng;

use device::DeviceEmulator;
use scenarios::{SCENARIO_NAMES, get_scenario};
use types::ScenarioResult;

#[derive(Parser)]
#[command(
    name = "cairn-emulator",
    about = "Cairn Freematics ONE+ Model B device emulator",
    after_help = format!(
        "Available scenarios:\n{}",
        SCENARIO_NAMES.iter()
            .map(|n| format!("  {n}"))
            .collect::<Vec<_>>()
            .join("\n")
    )
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(clap::Subcommand)]
enum Command {
    /// Check this implementation of bundle format v2 against the committed
    /// conformance vectors.
    ///
    /// The vectors are bytes and expected verdicts, not code, so neither
    /// implementation can quietly drag the other along. A disagreement about a
    /// single byte fails the run.
    Conformance {
        /// Directory holding the vectors.
        #[arg(long, default_value = "../fixtures/format-v2")]
        vectors: PathBuf,

        /// Print every vector, not only failures.
        #[arg(long)]
        verbose: bool,
    },

    /// Run the fault-injection property matrix.
    ///
    /// Each row states a property the architecture claims, arms a fault that
    /// would violate it, and asserts what survived. Local durability rows need
    /// no server; protocol rows are skipped unless --server is given.
    FaultMatrix {
        /// Working directory for simulated device storage.
        #[arg(long, default_value = "output/fault-matrix")]
        work_dir: PathBuf,

        /// Ingest server base URL. Protocol rows are skipped when absent.
        #[arg(long, default_value = "")]
        server: String,

        /// Seed, printed on failure so a run reproduces exactly.
        #[arg(long, default_value_t = 1)]
        seed: u64,

        /// Transfer chunk size in bytes.
        #[arg(long, default_value_t = 4096)]
        chunk_size: usize,

        #[arg(long)]
        verbose: bool,
    },

    /// Run a legacy v1 capture scenario.
    Scenario(ScenarioArgs),
}

#[derive(clap::Args)]
struct ScenarioArgs {
    #[arg(long, help = "Scenario to run")]
    scenario: Option<String>,

    #[arg(long, help = "Run all built-in scenarios sequentially")]
    all_scenarios: bool,

    #[arg(long, default_value = "", help = "Ingest server URL")]
    server: String,

    #[arg(long, default_value = "cairn-emulator-01", help = "Device identifier")]
    device_id: String,

    #[arg(long, default_value_t = 10.0, help = "Time speedup factor")]
    speedup: f64,

    #[arg(long, default_value_t = 1, help = "Number of concurrent devices")]
    devices: u32,

    #[arg(long, help = "Print all log lines to stderr")]
    verbose: bool,

    #[arg(long, help = "Output directory")]
    output: Option<String>,

    #[arg(long, help = "Random seed for reproducibility")]
    seed: Option<u64>,
}

fn run_single(
    device_id: &str,
    scenario_name: &str,
    server: &str,
    output_dir: &PathBuf,
    speedup: f64,
    verbose: bool,
    seed: Option<u64>,
) -> ScenarioResult {
    let dev_output = output_dir.join(device_id);
    std::fs::create_dir_all(&dev_output).ok();

    let rng = match seed {
        Some(s) => StdRng::seed_from_u64(s),
        None => StdRng::from_entropy(),
    };

    let mut device = DeviceEmulator::new(device_id, rng, &dev_output, server, speedup, verbose);

    let mut scenario = get_scenario(scenario_name).unwrap_or_else(|| {
        eprintln!("Unknown scenario: {scenario_name}");
        std::process::exit(1);
    });

    device.run_scenario(scenario.as_mut())
}

fn run_multi_device(
    scenario_name: &str,
    device_count: u32,
    server: &str,
    output_dir: &PathBuf,
    speedup: f64,
    verbose: bool,
    seed: Option<u64>,
) -> Vec<ScenarioResult> {
    let mut handles = Vec::new();

    for i in 0..device_count {
        let dev_id = format!("cairn-emulator-{:02}", i + 1);
        let dev_seed = seed.map(|s| s + i as u64);
        let server = server.to_string();
        let output_dir = output_dir.clone();
        let scenario_name = scenario_name.to_string();

        let handle = thread::spawn(move || {
            run_single(
                &dev_id,
                &scenario_name,
                &server,
                &output_dir,
                speedup,
                verbose,
                dev_seed,
            )
        });
        handles.push(handle);
    }

    handles.into_iter().filter_map(|h| h.join().ok()).collect()
}

fn main() {
    let cli = Cli::parse();

    match cli.command {
        Command::Conformance { vectors, verbose } => run_conformance(&vectors, verbose),
        Command::FaultMatrix {
            work_dir,
            server,
            seed,
            chunk_size,
            verbose,
        } => run_fault_matrix(work_dir, server, seed, chunk_size, verbose),
        Command::Scenario(args) => run_scenarios(args),
    }
}

/// Check the format implementation against the committed vectors.
fn run_conformance(vectors: &PathBuf, verbose: bool) {
    let report = match conformance::run(vectors) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("cannot read vectors at {}: {e}", vectors.display());
            eprintln!(
                "generate them with: cd ../server && go run ./cmd/mkvectors -out ../fixtures/format-v2"
            );
            std::process::exit(2);
        }
    };

    // Always listed, not only when verbose. A skipped vector is not a passing
    // vector, and hiding the difference on a clean run is how a gap goes
    // unnoticed.
    for name in &report.skipped {
        eprintln!("  skip  {name}");
    }
    for failure in &report.failures {
        eprintln!("  FAIL  {failure}");
    }

    eprintln!();
    if report.ok() {
        if report.skipped.is_empty() {
            eprintln!(
                "conformance: {}/{} vectors pass — this implementation agrees with the specification",
                report.passed, report.checked
            );
        } else {
            eprintln!(
                "conformance: {}/{} vectors pass, {} skipped — this implementation \
                 agrees with the specification on everything it implements",
                report.passed,
                report.checked,
                report.skipped.len()
            );
        }
        std::process::exit(0);
    }

    eprintln!(
        "conformance: {}/{} vectors pass, {} FAILED",
        report.passed,
        report.checked,
        report.failures.len()
    );
    if report.checked == 0 {
        eprintln!("no vectors were checked");
    }
    std::process::exit(1);
}

/// Run the fault-injection matrix.
fn run_fault_matrix(
    work_dir: PathBuf,
    server: String,
    seed: u64,
    chunk_size: usize,
    verbose: bool,
) {
    if let Err(e) = v2::matrix::prepare(&work_dir) {
        eprintln!("cannot prepare {}: {e}", work_dir.display());
        std::process::exit(2);
    }

    let cfg = v2::matrix::Config {
        work_dir,
        server,
        seed,
        chunk_size,
        verbose,
    };

    let report = match v2::matrix::run(&cfg) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("matrix run failed: {e}");
            std::process::exit(2);
        }
    };

    eprintln!();
    for row in &report.rows {
        let mark = if row.skipped {
            "skip"
        } else if row.passed {
            "pass"
        } else {
            "FAIL"
        };
        eprintln!("  {mark}  {}", row.family);
        eprintln!("        {}", row.property);
        if verbose || !row.passed || row.skipped {
            eprintln!("        {}", row.detail);
        }
    }

    eprintln!();
    eprintln!(
        "fault matrix: {} passed, {} failed, {} skipped",
        report.passed(),
        report.failed(),
        report.skipped()
    );

    if !report.ok() {
        eprintln!("reproduce with --seed {}", cfg.seed);
        std::process::exit(1);
    }
}

fn run_scenarios(cli: ScenarioArgs) {
    let output_dir = match &cli.output {
        Some(p) => PathBuf::from(p),
        None => {
            let exe_dir = std::env::current_dir().unwrap_or_default();
            exe_dir.join("output")
        }
    };
    std::fs::create_dir_all(&output_dir).ok();

    let scenario_names: Vec<String> = if cli.all_scenarios {
        SCENARIO_NAMES.iter().map(|s| s.to_string()).collect()
    } else if let Some(ref name) = cli.scenario {
        vec![name.clone()]
    } else {
        eprintln!("Specify --scenario or --all-scenarios");
        std::process::exit(1);
    };

    let shutdown = Arc::new(AtomicBool::new(false));
    {
        let shutdown = shutdown.clone();
        ctrlc_set_handler(move || {
            eprintln!("\nShutting down gracefully...");
            shutdown.store(true, Ordering::SeqCst);
        });
    }

    let mut all_results: Vec<ScenarioResult> = Vec::new();
    let mut exit_code = 0;

    for scenario_name in &scenario_names {
        if shutdown.load(Ordering::SeqCst) {
            break;
        }

        let desc = get_scenario(scenario_name)
            .map(|s| s.description().to_string())
            .unwrap_or_default();

        eprintln!("\n{}", "=".repeat(60));
        eprintln!("  Scenario: {scenario_name}");
        eprintln!("  {desc}");
        eprintln!("{}", "=".repeat(60));

        if cli.devices > 1 || scenario_name == "multi_device" {
            let count = if scenario_name == "multi_device" {
                cli.devices.max(3)
            } else {
                cli.devices
            };
            let results = run_multi_device(
                scenario_name,
                count,
                &cli.server,
                &output_dir,
                cli.speedup,
                cli.verbose,
                cli.seed,
            );
            all_results.extend(results);
        } else {
            let result = run_single(
                &cli.device_id,
                scenario_name,
                &cli.server,
                &output_dir,
                cli.speedup,
                cli.verbose,
                cli.seed,
            );
            all_results.push(result);
        }

        for r in &all_results {
            if !r.errors.is_empty() && !r.expects_errors {
                exit_code = 1;
            }
        }
    }

    let summary = serde_json::json!({
        "emulator_version": "1.0.0-rust",
        "timestamp": Utc::now().to_rfc3339(),
        "speedup": cli.speedup,
        "scenarios_run": all_results.len(),
        "results": all_results,
        "total_trips": all_results.iter().map(|r| r.trips_generated).sum::<usize>(),
        "total_gnss_samples": all_results.iter().map(|r| r.total_gnss_samples).sum::<usize>(),
        "total_imu_summaries": all_results.iter().map(|r| r.total_imu_summaries).sum::<usize>(),
        "total_uploads": all_results.iter().map(|r| r.trips_uploaded).sum::<usize>(),
        "total_errors": all_results.iter().map(|r| r.errors.len()).sum::<usize>(),
        "output_dir": output_dir.display().to_string(),
    });

    println!("{}", serde_json::to_string_pretty(&summary).unwrap());

    std::process::exit(exit_code);
}

fn ctrlc_set_handler<F: Fn() + Send + 'static>(handler: F) {
    use std::sync::Once;
    static INIT: Once = Once::new();
    // Store the handler in a static; only one handler is supported.
    static mut HANDLER: Option<Box<dyn Fn() + Send>> = None;

    INIT.call_once(|| {
        // SAFETY: only called once, before the signal can fire
        unsafe {
            HANDLER = Some(Box::new(handler));
            libc::signal(
                libc::SIGINT,
                signal_trampoline as *const () as libc::sighandler_t,
            );
            libc::signal(
                libc::SIGTERM,
                signal_trampoline as *const () as libc::sighandler_t,
            );
        }
    });

    extern "C" fn signal_trampoline(_: libc::c_int) {
        // SAFETY: HANDLER is set once before the signal is registered
        unsafe {
            if let Some(ref h) = HANDLER {
                h();
            }
        }
    }
}
