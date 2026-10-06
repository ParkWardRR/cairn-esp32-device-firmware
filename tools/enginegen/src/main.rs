use std::path::PathBuf;
use std::process::ExitCode;

use enginegen::{emit, expr, profile, vectors};

const USAGE: &str = "\
enginegen: engine profiles (engines/*.yaml) to C tables

  enginegen validate [--dir engines]
      Validate every profile. Exit 1, with every problem listed, if any is invalid.
  enginegen gen --engines all|a,b [--dir engines] --out FILE
      Validate every profile, then write the C header for the selection. The file is
      only rewritten if its content changes.
  enginegen list [--dir engines]
      Print the engine ids.
  enginegen identity --engines all|a,b [--dir engines]
      Print the build identity string for a selection.
  enginegen vectors --file FILE [--update]
      Check the expression-language vectors; --update fills in the bytecode column.
  enginegen eval NBYTES 'EXPR' [A B C D]
      Compile an expression and evaluate it.
";

struct Args {
    rest: Vec<String>,
}

impl Args {
    fn flag(&mut self, name: &str) -> Option<String> {
        let i = self.rest.iter().position(|a| a == name)?;
        self.rest.remove(i);
        if i < self.rest.len() {
            Some(self.rest.remove(i))
        } else {
            None
        }
    }
    fn switch(&mut self, name: &str) -> bool {
        match self.rest.iter().position(|a| a == name) {
            Some(i) => {
                self.rest.remove(i);
                true
            }
            None => false,
        }
    }
}

fn run() -> Result<(), String> {
    let mut argv: Vec<String> = std::env::args().skip(1).collect();
    if argv.is_empty() {
        return Err(USAGE.into());
    }
    let cmd = argv.remove(0);
    let mut a = Args { rest: argv };
    let dir = PathBuf::from(a.flag("--dir").unwrap_or_else(|| "engines".into()));

    match cmd.as_str() {
        "validate" => {
            let all = profile::load_dir(&dir)?;
            for c in &all {
                println!("ok  {} v{} {}", c.profile.engine_id, c.profile.version, profile::hex(&c.sha256)[..16].to_string());
            }
            Ok(())
        }
        "list" => {
            for c in profile::load_dir(&dir)? {
                println!("{}", c.profile.engine_id);
            }
            Ok(())
        }
        "gen" => {
            let spec = a.flag("--engines").ok_or("gen needs --engines all|a,b")?;
            let out = a.flag("--out").ok_or("gen needs --out FILE")?;
            let all = profile::load_dir(&dir)?;
            let sel = profile::select(&all, &spec)?;
            let text = emit::header(&all, &sel);
            if std::fs::read_to_string(&out).ok().as_deref() != Some(text.as_str()) {
                if let Some(p) = std::path::Path::new(&out).parent() {
                    std::fs::create_dir_all(p).map_err(|e| format!("{}: {e}", p.display()))?;
                }
                std::fs::write(&out, &text).map_err(|e| format!("{out}: {e}"))?;
            }
            println!("enginegen: {}", emit::identity(&sel).string);
            Ok(())
        }
        "identity" => {
            let spec = a.flag("--engines").ok_or("identity needs --engines all|a,b")?;
            let all = profile::load_dir(&dir)?;
            let sel = profile::select(&all, &spec)?;
            println!("{}", emit::identity(&sel).string);
            Ok(())
        }
        "vectors" => {
            let file = a.flag("--file").ok_or("vectors needs --file FILE")?;
            let update = a.switch("--update");
            let text = std::fs::read_to_string(&file).map_err(|e| format!("{file}: {e}"))?;
            let (out, n) = vectors::run(&text, update)?;
            if update && out != text {
                std::fs::write(&file, out).map_err(|e| format!("{file}: {e}"))?;
            }
            println!("{n} vectors ok");
            Ok(())
        }
        "eval" => {
            if a.rest.len() < 2 {
                return Err(USAGE.into());
            }
            let nb: usize = a.rest[0].parse().map_err(|_| "NBYTES must be 1..4")?;
            let c = expr::compile(&a.rest[1], nb).map_err(|e| e.to_string())?;
            let mut inp = [0u8; 4];
            for (i, v) in a.rest[2..].iter().take(4).enumerate() {
                inp[i] = v.parse().map_err(|_| format!("`{v}` is not a byte"))?;
            }
            println!("bytecode {}", expr::to_hex(&c.code));
            println!("range    {}..{}", c.range.0, c.range.1);
            match expr::eval(&c.code, inp) {
                Ok(v) => println!("value    {v}"),
                Err(e) => return Err(e.name().into()),
            }
            Ok(())
        }
        "-h" | "--help" | "help" => {
            print!("{USAGE}");
            Ok(())
        }
        other => Err(format!("unknown command `{other}`\n{USAGE}")),
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("enginegen: error: {e}");
            ExitCode::FAILURE
        }
    }
}
