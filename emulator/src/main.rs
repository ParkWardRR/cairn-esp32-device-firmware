//! Cairn device emulator.
//!
//! `conformance` checks this crate's independent implementation of bundle
//! format v3 against the committed vectors; `fault-matrix` runs the
//! fault-injection property matrix.
mod conformance;
mod format;
mod v2;

use std::path::PathBuf;

use clap::Parser;

#[derive(Parser)]
#[command(name = "cairn-emulator", about = "Cairn device emulator")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(clap::Subcommand)]
enum Command {
    /// Check this implementation of bundle format v3 against the committed
    /// conformance vectors.
    ///
    /// The vectors are bytes and expected verdicts, not code, so neither
    /// implementation can quietly drag the other along. A disagreement about a
    /// single byte fails the run.
    Conformance {
        /// Directory holding the vectors (default: $CAIRN_CONTRACTS/format/v3/vectors).
        #[arg(long)]
        vectors: Option<PathBuf>,

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

        #[command(flatten)]
        identity: IdentityArgs,
    },
}

/// The simulated device's provisioning. Defaults are public test values that
/// protect nothing; the protocol rows need the values a server actually holds,
/// because intake refuses a device whose storage root it has not escrowed and
/// an assignment it did not issue.
#[derive(clap::Args)]
struct IdentityArgs {
    /// Storage root K_root, 64 hex characters. Default: the hex of
    /// "cairn-emulator-public-test-root!" — escrow it with
    /// `cairn-server -enroll-root`.
    #[arg(long)]
    root_key: Option<String>,

    /// storage_key_version of --root-key.
    #[arg(long, default_value_t = 1)]
    key_version: u32,

    /// Vehicle id, 32 hex characters, as issued by the server.
    #[arg(long)]
    vehicle_id: Option<String>,

    /// Assignment id, 32 hex characters, as issued by `cairn-admin assign`.
    #[arg(long)]
    assignment_id: Option<String>,
}

impl IdentityArgs {
    fn resolve(&self) -> Result<v2::matrix::IdentityConfig, String> {
        let mut id = v2::matrix::IdentityConfig {
            storage_key_version: self.key_version,
            ..Default::default()
        };
        if let Some(h) = &self.root_key {
            id.root_key = format::unhex_array(h).ok_or("--root-key must be 64 hex characters")?;
        }
        if let Some(h) = &self.vehicle_id {
            id.vehicle_id =
                format::unhex_array(h).ok_or("--vehicle-id must be 32 hex characters")?;
        }
        if let Some(h) = &self.assignment_id {
            id.assignment_id =
                format::unhex_array(h).ok_or("--assignment-id must be 32 hex characters")?;
        }
        Ok(id)
    }
}

fn main() {
    let cli = Cli::parse();

    match cli.command {
        Command::Conformance { vectors, verbose } => {
            run_conformance(&vectors.unwrap_or_else(|| contracts_dir().join("format/v3/vectors")), verbose)
        }
        Command::FaultMatrix {
            work_dir,
            server,
            seed,
            chunk_size,
            verbose,
            identity,
        } => {
            let identity = identity.resolve().unwrap_or_else(|e| {
                eprintln!("{e}");
                std::process::exit(2);
            });
            run_fault_matrix(work_dir, server, seed, chunk_size, verbose, identity)
        }
    }
}

/// The Cairn contracts directory (the one containing format/, sync/, ble/): $CAIRN_CONTRACTS,
/// else the nearest ancestor of the working directory with contracts/format (the monorepo or
/// the front door), else one with .contracts/contracts/format (a fetched copy).
fn contracts_dir() -> PathBuf {
    if let Ok(d) = std::env::var("CAIRN_CONTRACTS") {
        if !d.is_empty() {
            return PathBuf::from(d);
        }
    }
    let wd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let mut dir = Some(wd.as_path());
    while let Some(d) = dir {
        for rel in ["contracts", ".contracts/contracts"] {
            if d.join(rel).join("format").is_dir() {
                return d.join(rel);
            }
        }
        dir = d.parent();
    }
    wd.join(".contracts/contracts")
}

/// Check the format implementation against the committed vectors.
fn run_conformance(vectors: &PathBuf, verbose: bool) {
    let report = match conformance::run(vectors) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("cannot read vectors at {}: {e}", vectors.display());
            eprintln!(
                "set CAIRN_CONTRACTS to the contracts directory, or generate them with the server's `go run ./cmd/mkvectors`"
            );
            std::process::exit(2);
        }
    };

    if verbose {
        for line in &report.passes {
            eprintln!("  pass  {line}");
        }
    }
    // There is no skip list: the runner fails a vector it does not recognise,
    // because a skipped vector looks exactly like a passing one on a clean run.
    for failure in &report.failures {
        eprintln!("  FAIL  {failure}");
    }

    eprintln!();
    if report.ok() {
        eprintln!(
            "conformance: {}/{} vectors pass ({} structural and {} keyed segment verdicts) \
             — this implementation agrees with the specification",
            report.passed, report.checked, report.structural_verdicts, report.keyed_verdicts
        );
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
    identity: v2::matrix::IdentityConfig,
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
        identity,
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

