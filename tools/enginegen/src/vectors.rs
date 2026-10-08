//! The expression-language vector file (contracts/engine/v1/vectors/expr.txt).
//!
//! Line kinds, `;`-separated, `#` starts a comment:
//!
//!   E ; nbytes ; expression ; A B C D ; expected ; bytecode-hex
//!       compile for a PID of `nbytes` data bytes; the bytecode must equal the last
//!       column exactly (so another implementation's compiler can be held to it) and
//!       evaluating it on the inputs must give `expected`.
//!   X ; nbytes ; expression ; text
//!       compilation must fail, and the message must contain `text`.
//!   B ; bytecode-hex ; A B C D ; expected | ERR_*
//!       evaluate raw bytecode: the cases an evaluator meets only from a corrupt or
//!       hostile image, since the compiler never produces them.
//!
//! The C evaluator runs the E and B lines (test/host/engine_test.c); this tool is the
//! independent second implementation that produced and checks them.

use crate::expr::{compile, eval, from_hex, to_hex};

fn inputs(s: &str) -> Result<[u8; 4], String> {
    let mut v = [0u8; 4];
    let parts: Vec<&str> = s.split_whitespace().collect();
    if parts.len() > 4 {
        return Err("more than four inputs".into());
    }
    for (i, p) in parts.iter().enumerate() {
        v[i] = p.parse::<u8>().map_err(|_| format!("input `{p}` is not a byte"))?;
    }
    Ok(v)
}

/// Check every line; with `update`, fill in the bytecode column of E lines and return
/// the rewritten text.
pub fn run(text: &str, update: bool) -> Result<(String, usize), String> {
    let mut out = String::new();
    let mut errs = Vec::new();
    let mut n = 0;
    for (ln, line) in text.lines().enumerate() {
        let ln = ln + 1;
        let t = line.trim();
        if t.is_empty() || t.starts_with('#') {
            out.push_str(line);
            out.push('\n');
            continue;
        }
        let f: Vec<&str> = t.split(';').map(str::trim).collect();
        let mut keep = line.to_string();
        match f[0] {
            "E" if f.len() == 6 => {
                n += 1;
                let nb: usize = f[1].parse().map_err(|_| format!("line {ln}: bad nbytes"))?;
                let inp = inputs(f[3]).map_err(|e| format!("line {ln}: {e}"))?;
                match compile(f[2], nb) {
                    Err(e) => errs.push(format!("line {ln}: `{}` does not compile: {e}", f[2])),
                    Ok(c) => {
                        let hex = to_hex(&c.code);
                        if update {
                            keep = format!("E ; {} ; {} ; {} ; {} ; {}", f[1], f[2], f[3], f[4], hex);
                        } else if f[5] != hex {
                            errs.push(format!("line {ln}: `{}` compiles to {hex}, the file says {}", f[2], f[5]));
                        }
                        let want: i64 = f[4].parse().map_err(|_| format!("line {ln}: expected is not an integer"))?;
                        match eval(&c.code, inp) {
                            Ok(v) if v as i64 == want => {}
                            other => errs.push(format!("line {ln}: `{}` on {:?} gave {other:?}, expected {want}", f[2], inp)),
                        }
                    }
                }
            }
            "X" if f.len() == 4 => {
                n += 1;
                let nb: usize = f[1].parse().map_err(|_| format!("line {ln}: bad nbytes"))?;
                match compile(f[2], nb) {
                    Ok(_) => errs.push(format!("line {ln}: `{}` compiled but must be rejected", f[2])),
                    Err(e) if e.0.contains(f[3]) => {}
                    Err(e) => errs.push(format!("line {ln}: `{}` rejected with `{e}`, expected it to mention `{}`", f[2], f[3])),
                }
            }
            "B" if f.len() == 4 => {
                n += 1;
                let code = from_hex(f[1]).map_err(|e| format!("line {ln}: {e}"))?;
                let inp = inputs(f[2]).map_err(|e| format!("line {ln}: {e}"))?;
                let got = match eval(&code, inp) {
                    Ok(v) => v.to_string(),
                    Err(e) => e.name().to_string(),
                };
                if got != f[3] {
                    errs.push(format!("line {ln}: bytecode {} gave {got}, expected {}", f[1], f[3]));
                }
            }
            _ => errs.push(format!("line {ln}: not a valid vector line: {t}")),
        }
        out.push_str(&keep);
        out.push('\n');
    }
    if errs.is_empty() {
        Ok((out, n))
    } else {
        Err(errs.join("\n"))
    }
}
