//! The formula expression language of cairn.engine/v1-draft: lexer, parser, static
//! analysis, bytecode compiler and a reference evaluator.
//!
//! The language is integer-only and has no loops, no calls out and no state, so
//! evaluation always terminates and cannot execute anything. Every value is an
//! int32 and every operation must land in int32 or the evaluation fails, which makes
//! the result identical in C, Go, Swift and TypeScript. See engines/SPEC.draft.md.
//!
//! The compiler refuses, at build time, any expression that could divide by zero or
//! overflow for some input byte values (interval analysis over A..D in 0..=255), so
//! the runtime errors below are reachable only through hand-written or corrupt
//! bytecode. The C evaluator still checks them, because it is the last line.

use std::fmt;

pub const MAX_STACK: usize = 16;
pub const MAX_SOURCE_LEN: usize = 256;
pub const MAX_DEPTH: usize = 32;

// Opcodes. These numbers are part of the draft contract; the bytecode vectors pin them.
pub const OP_PUSH_U8: u8 = 0x01;
pub const OP_PUSH_U16: u8 = 0x02;
pub const OP_PUSH_I32: u8 = 0x03;
pub const OP_PUSH_S8: u8 = 0x04;
pub const OP_LOAD_A: u8 = 0x10; // ..= 0x13 for A..D
pub const OP_ADD: u8 = 0x20;
pub const OP_SUB: u8 = 0x21;
pub const OP_MUL: u8 = 0x22;
pub const OP_DIV: u8 = 0x23;
pub const OP_MOD: u8 = 0x24;
pub const OP_NEG: u8 = 0x25;
pub const OP_AND: u8 = 0x26;
pub const OP_OR: u8 = 0x27;
pub const OP_SHL: u8 = 0x28;
pub const OP_SHR: u8 = 0x29;
pub const OP_MIN: u8 = 0x30;
pub const OP_MAX: u8 = 0x31;
pub const OP_CLAMP: u8 = 0x32;

const I32_MIN: i64 = i32::MIN as i64;
const I32_MAX: i64 = i32::MAX as i64;

#[derive(Debug, Clone, PartialEq)]
pub struct ExprError(pub String);

impl fmt::Display for ExprError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for ExprError {}

fn err<T>(msg: impl Into<String>) -> Result<T, ExprError> {
    Err(ExprError(msg.into()))
}

// ── lexer ───────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
enum Tok {
    Int(i64),
    Var(u8),
    Func(Func),
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Amp,
    Pipe,
    Shl,
    Shr,
    LParen,
    RParen,
    Comma,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Func {
    Min,
    Max,
    Clamp,
}

fn lex(src: &str) -> Result<Vec<(Tok, usize)>, ExprError> {
    if src.len() > MAX_SOURCE_LEN {
        return err(format!("expression longer than {MAX_SOURCE_LEN} characters"));
    }
    if !src.is_ascii() {
        return err("expression must be ASCII");
    }
    let b = src.as_bytes();
    let mut i = 0;
    let mut out = Vec::new();
    while i < b.len() {
        let c = b[i];
        let at = i;
        match c {
            b' ' | b'\t' => {
                i += 1;
            }
            b'+' => {
                out.push((Tok::Plus, at));
                i += 1;
            }
            b'-' => {
                out.push((Tok::Minus, at));
                i += 1;
            }
            b'*' => {
                out.push((Tok::Star, at));
                i += 1;
            }
            b'/' => {
                out.push((Tok::Slash, at));
                i += 1;
            }
            b'%' => {
                out.push((Tok::Percent, at));
                i += 1;
            }
            b'&' => {
                out.push((Tok::Amp, at));
                i += 1;
            }
            b'|' => {
                out.push((Tok::Pipe, at));
                i += 1;
            }
            b'(' => {
                out.push((Tok::LParen, at));
                i += 1;
            }
            b')' => {
                out.push((Tok::RParen, at));
                i += 1;
            }
            b',' => {
                out.push((Tok::Comma, at));
                i += 1;
            }
            b'<' | b'>' => {
                if i + 1 < b.len() && b[i + 1] == c {
                    out.push((if c == b'<' { Tok::Shl } else { Tok::Shr }, at));
                    i += 2;
                } else {
                    return err(format!("column {}: only the shifts << and >> are operators; comparisons do not exist", at + 1));
                }
            }
            b'0'..=b'9' => {
                let (radix, start) = if c == b'0' && i + 1 < b.len() && (b[i + 1] == b'x' || b[i + 1] == b'X') {
                    (16, i + 2)
                } else {
                    (10, i)
                };
                let mut j = start;
                while j < b.len() && (b[j] as char).is_digit(radix) {
                    j += 1;
                }
                if j == start {
                    return err(format!("column {}: malformed number", at + 1));
                }
                if j < b.len() && (b[j].is_ascii_alphanumeric() || b[j] == b'.' || b[j] == b'_') {
                    return err(format!("column {}: malformed number (integers only, no suffixes or fractions)", at + 1));
                }
                let v = i64::from_str_radix(&src[start..j], radix)
                    .map_err(|_| ExprError(format!("column {}: number too large", at + 1)))?;
                if v > I32_MAX {
                    return err(format!("column {}: literal {} exceeds int32", at + 1, v));
                }
                out.push((Tok::Int(v), at));
                i = j;
            }
            b'A'..=b'D' if !(i + 1 < b.len() && b[i + 1].is_ascii_alphanumeric()) => {
                out.push((Tok::Var(c - b'A'), at));
                i += 1;
            }
            b'a'..=b'z' | b'A'..=b'Z' | b'_' => {
                let mut j = i;
                while j < b.len() && (b[j].is_ascii_alphanumeric() || b[j] == b'_') {
                    j += 1;
                }
                let word = &src[i..j];
                let f = match word {
                    "min" => Func::Min,
                    "max" => Func::Max,
                    "clamp" => Func::Clamp,
                    _ => {
                        return err(format!(
                            "column {}: unknown name `{}` (variables are A B C D; functions are min, max, clamp)",
                            at + 1,
                            word
                        ))
                    }
                };
                out.push((Tok::Func(f), at));
                i = j;
            }
            _ => return err(format!("column {}: unexpected character `{}`", at + 1, c as char)),
        }
    }
    Ok(out)
}

// ── AST and parser ──────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    And,
    Or,
    Shl,
    Shr,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Ast {
    Lit(i64),
    Var(u8),
    Neg(Box<Ast>),
    Bin(BinOp, Box<Ast>, Box<Ast>),
    Call(Func, Vec<Ast>),
}

struct Parser {
    toks: Vec<(Tok, usize)>,
    pos: usize,
    depth: usize,
}

impl Parser {
    fn peek(&self) -> Option<&Tok> {
        self.toks.get(self.pos).map(|t| &t.0)
    }
    fn col(&self) -> usize {
        self.toks.get(self.pos).map(|t| t.1 + 1).unwrap_or(0)
    }
    fn bump(&mut self) -> Option<Tok> {
        let t = self.toks.get(self.pos).map(|t| t.0.clone());
        self.pos += 1;
        t
    }
    fn enter(&mut self) -> Result<(), ExprError> {
        self.depth += 1;
        if self.depth > MAX_DEPTH {
            return err(format!("expression nested deeper than {MAX_DEPTH}"));
        }
        Ok(())
    }

    // Precedence follows C, lowest first: |  &  << >>  + -  * / %  unary-
    fn or(&mut self) -> Result<Ast, ExprError> {
        self.enter()?;
        let mut l = self.and()?;
        while self.peek() == Some(&Tok::Pipe) {
            self.bump();
            let r = self.and()?;
            l = Ast::Bin(BinOp::Or, Box::new(l), Box::new(r));
        }
        self.depth -= 1;
        Ok(l)
    }
    fn and(&mut self) -> Result<Ast, ExprError> {
        let mut l = self.shift()?;
        while self.peek() == Some(&Tok::Amp) {
            self.bump();
            let r = self.shift()?;
            l = Ast::Bin(BinOp::And, Box::new(l), Box::new(r));
        }
        Ok(l)
    }
    fn shift(&mut self) -> Result<Ast, ExprError> {
        let mut l = self.add()?;
        loop {
            let op = match self.peek() {
                Some(Tok::Shl) => BinOp::Shl,
                Some(Tok::Shr) => BinOp::Shr,
                _ => break,
            };
            self.bump();
            let r = self.add()?;
            l = Ast::Bin(op, Box::new(l), Box::new(r));
        }
        Ok(l)
    }
    fn add(&mut self) -> Result<Ast, ExprError> {
        let mut l = self.mul()?;
        loop {
            let op = match self.peek() {
                Some(Tok::Plus) => BinOp::Add,
                Some(Tok::Minus) => BinOp::Sub,
                _ => break,
            };
            self.bump();
            let r = self.mul()?;
            l = Ast::Bin(op, Box::new(l), Box::new(r));
        }
        Ok(l)
    }
    fn mul(&mut self) -> Result<Ast, ExprError> {
        let mut l = self.unary()?;
        loop {
            let op = match self.peek() {
                Some(Tok::Star) => BinOp::Mul,
                Some(Tok::Slash) => BinOp::Div,
                Some(Tok::Percent) => BinOp::Mod,
                _ => break,
            };
            self.bump();
            let r = self.unary()?;
            l = Ast::Bin(op, Box::new(l), Box::new(r));
        }
        Ok(l)
    }
    fn unary(&mut self) -> Result<Ast, ExprError> {
        if self.peek() == Some(&Tok::Minus) {
            self.enter()?;
            self.bump();
            let inner = self.unary()?;
            self.depth -= 1;
            // A negated literal is a literal, so -127 costs one push, not two ops.
            return Ok(match inner {
                Ast::Lit(n) => Ast::Lit(-n),
                other => Ast::Neg(Box::new(other)),
            });
        }
        self.primary()
    }
    fn primary(&mut self) -> Result<Ast, ExprError> {
        let col = self.col();
        match self.bump() {
            Some(Tok::Int(n)) => Ok(Ast::Lit(n)),
            Some(Tok::Var(v)) => Ok(Ast::Var(v)),
            Some(Tok::LParen) => {
                let e = self.or()?;
                match self.bump() {
                    Some(Tok::RParen) => Ok(e),
                    _ => err(format!("column {}: expected `)`", self.col())),
                }
            }
            Some(Tok::Func(f)) => {
                if self.bump() != Some(Tok::LParen) {
                    return err(format!("column {col}: a function name must be followed by `(`"));
                }
                let want = match f {
                    Func::Min | Func::Max => 2,
                    Func::Clamp => 3,
                };
                let mut args = Vec::new();
                loop {
                    args.push(self.or()?);
                    match self.bump() {
                        Some(Tok::Comma) => continue,
                        Some(Tok::RParen) => break,
                        _ => return err(format!("column {}: expected `,` or `)`", self.col())),
                    }
                }
                if args.len() != want {
                    return err(format!("column {col}: {:?} takes {} arguments, got {}", f, want, args.len()));
                }
                Ok(Ast::Call(f, args))
            }
            Some(t) => err(format!("column {col}: unexpected {t:?}")),
            None => err("expression ends early"),
        }
    }
}

pub fn parse(src: &str) -> Result<Ast, ExprError> {
    let toks = lex(src)?;
    if toks.is_empty() {
        return err("empty expression");
    }
    let mut p = Parser { toks, pos: 0, depth: 0 };
    let ast = p.or()?;
    if p.pos < p.toks.len() {
        return err(format!("column {}: unexpected trailing input", p.col()));
    }
    Ok(ast)
}

// ── static analysis ─────────────────────────────────────────────────────────

pub type Iv = (i64, i64);

fn fit(iv: Iv, what: &str) -> Result<Iv, ExprError> {
    if iv.0 < I32_MIN || iv.1 > I32_MAX {
        return err(format!("{what} can leave int32 range for some input bytes ({}..{})", iv.0, iv.1));
    }
    Ok(iv)
}

fn corners(a: Iv, b: Iv, f: impl Fn(i64, i64) -> i64) -> Iv {
    let v = [f(a.0, b.0), f(a.0, b.1), f(a.1, b.0), f(a.1, b.1)];
    (*v.iter().min().unwrap(), *v.iter().max().unwrap())
}

/// Smallest `2^k - 1` that is `>= x` (x >= 0).
fn mask_up(x: i64) -> i64 {
    let mut m = 0i64;
    while m < x {
        m = (m << 1) | 1;
    }
    m
}

/// The range of values the expression can produce when each of the first `nbytes`
/// variables ranges over 0..=255 and the rest are 0. Errors if some input could
/// divide by zero, shift by a non-constant or out-of-range count, or overflow int32.
pub fn interval(ast: &Ast, nbytes: usize) -> Result<Iv, ExprError> {
    match ast {
        Ast::Lit(n) => Ok((*n, *n)),
        Ast::Var(v) => {
            if (*v as usize) >= nbytes {
                err(format!(
                    "formula uses {} but the PID returns {} data byte(s)",
                    (b'A' + v) as char,
                    nbytes
                ))
            } else {
                Ok((0, 255))
            }
        }
        Ast::Neg(a) => {
            let a = interval(a, nbytes)?;
            fit((-a.1, -a.0), "negation")
        }
        Ast::Call(f, args) => {
            let v: Result<Vec<Iv>, _> = args.iter().map(|a| interval(a, nbytes)).collect();
            let v = v?;
            Ok(match f {
                Func::Min => (v[0].0.min(v[1].0), v[0].1.min(v[1].1)),
                Func::Max => (v[0].0.max(v[1].0), v[0].1.max(v[1].1)),
                Func::Clamp => {
                    // clamp(x, lo, hi) is min(max(x, lo), hi)
                    let m = (v[0].0.max(v[1].0), v[0].1.max(v[1].1));
                    (m.0.min(v[2].0), m.1.min(v[2].1))
                }
            })
        }
        Ast::Bin(op, l, r) => {
            let a = interval(l, nbytes)?;
            let b = interval(r, nbytes)?;
            match op {
                BinOp::Add => fit((a.0 + b.0, a.1 + b.1), "addition"),
                BinOp::Sub => fit((a.0 - b.1, a.1 - b.0), "subtraction"),
                BinOp::Mul => fit(corners(a, b, |x, y| x * y), "multiplication"),
                BinOp::Div => {
                    if b.0 <= 0 && b.1 >= 0 {
                        return err("a divisor can be zero for some input bytes (guard it, e.g. max(B, 1))");
                    }
                    fit(corners(a, b, |x, y| x / y), "division")
                }
                BinOp::Mod => {
                    if b.0 <= 0 && b.1 >= 0 {
                        return err("a modulus can be zero for some input bytes (guard it, e.g. max(B, 1))");
                    }
                    let m = b.0.abs().max(b.1.abs()) - 1;
                    Ok(if a.0 >= 0 {
                        (0, a.1.min(m))
                    } else if a.1 <= 0 {
                        ((-m).max(a.0), 0)
                    } else {
                        ((-m).max(a.0), a.1.min(m))
                    })
                }
                BinOp::And => Ok(if a.0 >= 0 && b.0 >= 0 {
                    (0, a.1.min(b.1))
                } else if b.0 >= 0 {
                    (0, b.1)
                } else if a.0 >= 0 {
                    (0, a.1)
                } else {
                    (I32_MIN, I32_MAX)
                }),
                BinOp::Or => Ok(if a.0 >= 0 && b.0 >= 0 {
                    (a.0.max(b.0), mask_up(a.1.max(b.1)))
                } else {
                    (I32_MIN, I32_MAX)
                }),
                BinOp::Shl | BinOp::Shr => {
                    if b.0 != b.1 || !(0..=31).contains(&b.0) {
                        return err("a shift count must be a constant in 0..=31");
                    }
                    let n = b.0 as u32;
                    if *op == BinOp::Shl {
                        fit((a.0 << n, a.1 << n), "left shift")
                    } else {
                        Ok((a.0 >> n, a.1 >> n))
                    }
                }
            }
        }
    }
}

// ── bytecode ────────────────────────────────────────────────────────────────

fn emit_push(code: &mut Vec<u8>, n: i64) {
    if (0..=255).contains(&n) {
        code.extend([OP_PUSH_U8, n as u8]);
    } else if (-128..0).contains(&n) {
        code.extend([OP_PUSH_S8, (n as i8) as u8]);
    } else if (256..=65535).contains(&n) {
        code.extend([OP_PUSH_U16, (n & 0xff) as u8, (n >> 8) as u8]);
    } else {
        code.push(OP_PUSH_I32);
        code.extend((n as i32).to_le_bytes());
    }
}

fn gen(ast: &Ast, code: &mut Vec<u8>, depth: &mut usize, max: &mut usize) {
    let push = |d: &mut usize, m: &mut usize| {
        *d += 1;
        if *d > *m {
            *m = *d;
        }
    };
    match ast {
        Ast::Lit(n) => {
            emit_push(code, *n);
            push(depth, max);
        }
        Ast::Var(v) => {
            code.push(OP_LOAD_A + v);
            push(depth, max);
        }
        Ast::Neg(a) => {
            gen(a, code, depth, max);
            code.push(OP_NEG);
        }
        Ast::Bin(op, l, r) => {
            gen(l, code, depth, max);
            gen(r, code, depth, max);
            code.push(match op {
                BinOp::Add => OP_ADD,
                BinOp::Sub => OP_SUB,
                BinOp::Mul => OP_MUL,
                BinOp::Div => OP_DIV,
                BinOp::Mod => OP_MOD,
                BinOp::And => OP_AND,
                BinOp::Or => OP_OR,
                BinOp::Shl => OP_SHL,
                BinOp::Shr => OP_SHR,
            });
            *depth -= 1;
        }
        Ast::Call(f, args) => {
            for a in args {
                gen(a, code, depth, max);
            }
            code.push(match f {
                Func::Min => OP_MIN,
                Func::Max => OP_MAX,
                Func::Clamp => OP_CLAMP,
            });
            *depth -= args.len() - 1;
        }
    }
}

#[derive(Debug)]
pub struct Compiled {
    pub code: Vec<u8>,
    pub range: Iv,
}

/// Parse, statically check and compile `src` for a PID returning `nbytes` data bytes.
pub fn compile(src: &str, nbytes: usize) -> Result<Compiled, ExprError> {
    let ast = parse(src)?;
    let range = interval(&ast, nbytes)?;
    let mut code = Vec::new();
    let (mut depth, mut max) = (0usize, 0usize);
    gen(&ast, &mut code, &mut depth, &mut max);
    if max > MAX_STACK {
        return err(format!("expression needs a stack of {max}, the limit is {MAX_STACK}"));
    }
    if code.len() > 255 {
        return err("compiled formula longer than 255 bytes");
    }
    Ok(Compiled { code, range })
}

// ── reference evaluator ─────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EvalError {
    Trunc,
    BadOp,
    Stack,
    DivZero,
    Overflow,
    Shift,
    Result,
}

impl EvalError {
    /// The names the vector files and the C evaluator use.
    pub fn name(self) -> &'static str {
        match self {
            EvalError::Trunc => "ERR_TRUNC",
            EvalError::BadOp => "ERR_BAD_OP",
            EvalError::Stack => "ERR_STACK",
            EvalError::DivZero => "ERR_DIV_ZERO",
            EvalError::Overflow => "ERR_OVERFLOW",
            EvalError::Shift => "ERR_SHIFT",
            EvalError::Result => "ERR_RESULT",
        }
    }
}

fn chk(v: i64) -> Result<i64, EvalError> {
    if (I32_MIN..=I32_MAX).contains(&v) {
        Ok(v)
    } else {
        Err(EvalError::Overflow)
    }
}

pub fn eval(code: &[u8], input: [u8; 4]) -> Result<i32, EvalError> {
    let mut st = [0i64; MAX_STACK];
    let mut sp = 0usize;
    let mut pc = 0usize;

    macro_rules! push {
        ($v:expr) => {{
            if sp >= MAX_STACK {
                return Err(EvalError::Stack);
            }
            st[sp] = $v;
            sp += 1;
        }};
    }
    macro_rules! pop {
        () => {{
            if sp == 0 {
                return Err(EvalError::Stack);
            }
            sp -= 1;
            st[sp]
        }};
    }

    while pc < code.len() {
        let op = code[pc];
        pc += 1;
        match op {
            OP_PUSH_U8 => {
                let b = *code.get(pc).ok_or(EvalError::Trunc)?;
                pc += 1;
                push!(b as i64);
            }
            OP_PUSH_S8 => {
                let b = *code.get(pc).ok_or(EvalError::Trunc)?;
                pc += 1;
                push!((b as i8) as i64);
            }
            OP_PUSH_U16 => {
                if pc + 2 > code.len() {
                    return Err(EvalError::Trunc);
                }
                push!(u16::from_le_bytes([code[pc], code[pc + 1]]) as i64);
                pc += 2;
            }
            OP_PUSH_I32 => {
                if pc + 4 > code.len() {
                    return Err(EvalError::Trunc);
                }
                push!(i32::from_le_bytes([code[pc], code[pc + 1], code[pc + 2], code[pc + 3]]) as i64);
                pc += 4;
            }
            0x10..=0x13 => push!(input[(op - OP_LOAD_A) as usize] as i64),
            OP_NEG => {
                let a = pop!();
                push!(chk(-a)?);
            }
            OP_ADD | OP_SUB | OP_MUL | OP_DIV | OP_MOD | OP_AND | OP_OR | OP_SHL | OP_SHR | OP_MIN | OP_MAX => {
                let b = pop!();
                let a = pop!();
                let r = match op {
                    OP_ADD => chk(a + b)?,
                    OP_SUB => chk(a - b)?,
                    OP_MUL => chk(a * b)?,
                    OP_DIV => {
                        if b == 0 {
                            return Err(EvalError::DivZero);
                        }
                        chk(a / b)?
                    }
                    OP_MOD => {
                        if b == 0 {
                            return Err(EvalError::DivZero);
                        }
                        a % b
                    }
                    OP_AND => a & b,
                    OP_OR => a | b,
                    OP_SHL => {
                        if !(0..=31).contains(&b) {
                            return Err(EvalError::Shift);
                        }
                        chk(a << b)?
                    }
                    OP_SHR => {
                        if !(0..=31).contains(&b) {
                            return Err(EvalError::Shift);
                        }
                        a >> b
                    }
                    OP_MIN => a.min(b),
                    _ => a.max(b),
                };
                push!(r);
            }
            OP_CLAMP => {
                let hi = pop!();
                let lo = pop!();
                let x = pop!();
                push!(x.max(lo).min(hi));
            }
            _ => return Err(EvalError::BadOp),
        }
    }
    if sp != 1 {
        return Err(EvalError::Result);
    }
    Ok(st[0] as i32)
}

pub fn to_hex(code: &[u8]) -> String {
    code.iter().map(|b| format!("{b:02x}")).collect::<Vec<_>>().join("")
}

pub fn from_hex(s: &str) -> Result<Vec<u8>, ExprError> {
    let s: String = s.chars().filter(|c| !c.is_whitespace()).collect();
    if s.len() % 2 != 0 {
        return err("odd number of hex digits");
    }
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).map_err(|_| ExprError("bad hex digit".into())))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(src: &str, n: usize, inp: [u8; 4]) -> i32 {
        let c = compile(src, n).unwrap_or_else(|e| panic!("{src}: {e}"));
        eval(&c.code, inp).unwrap()
    }

    #[test]
    fn precedence_follows_c() {
        assert_eq!(run("1+2*3", 1, [0; 4]), 7);
        assert_eq!(run("8>>1+1", 1, [0; 4]), 2); // + binds tighter than >>
        assert_eq!(run("6&3|8", 1, [0; 4]), 10);
        assert_eq!(run("-7/2", 1, [0; 4]), -3); // truncates toward zero
        assert_eq!(run("-7%3", 1, [0; 4]), -1); // sign follows the dividend
        assert_eq!(run("(A*256+B)/4", 2, [1, 2, 0, 0]), 64);
    }

    #[test]
    fn rejects_unsafe_expressions() {
        assert!(compile("A/B", 2).unwrap_err().0.contains("zero"));
        assert!(compile("A/max(B,1)", 2).is_ok());
        assert!(compile("A/0", 1).is_err());
        assert!(compile("C", 2).unwrap_err().0.contains("data byte"));
        assert!(compile("A*A*A*A*A", 1).unwrap_err().0.contains("int32"));
        assert!(compile("A<<B", 2).unwrap_err().0.contains("shift"));
        assert!(compile("A<<32", 1).is_err());
        assert!(compile("A==1", 1).is_err());
        assert!(compile("A.5", 1).is_err());
        assert!(compile("sqrt(A)", 1).is_err());
        assert!(compile("clamp(A,1)", 1).is_err());
        assert!(compile("", 1).is_err());
        assert!(compile("A+", 1).is_err());
        assert!(compile("(A", 1).is_err());
        assert!(compile("A B", 1).is_err());
        assert!(compile("99999999999", 1).is_err());
    }

    /// The static interval must contain every value the expression really takes.
    #[test]
    fn interval_is_sound_exhaustively() {
        let cases: &[(&str, usize)] = &[
            ("(A*256+B)/4", 2),
            ("(A-128)*100/128", 1),
            ("A/2-64", 1),
            ("clamp(A-40,-127,127)", 1),
            ("(A*256+B)*10000/32768", 2),
            ("((A*256+B)/100)*100", 2),
            ("A%7-3", 1),
            ("(A&15)|(B&240)", 2),
            ("A-B", 2),
            ("A*B", 2),
            ("A>>2", 1),
            ("(A<<3)-B", 2),
            ("A/(B%5+1)", 2),
            ("-A/3", 1),
            ("(A-100)/(B+1)", 2),
            ("(A-100)%(B+1)", 2),
            ("max(A,B)-min(A,B)", 2),
        ];
        for (src, n) in cases {
            let c = compile(src, *n).unwrap_or_else(|e| panic!("{src}: {e}"));
            let (lo, hi) = c.range;
            let bmax = if *n > 1 { 255u32 } else { 0 };
            for a in 0..=255u32 {
                for b in 0..=bmax {
                    let v = eval(&c.code, [a as u8, b as u8, 0, 0]).unwrap() as i64;
                    assert!(v >= lo && v <= hi, "{src}: {v} outside {lo}..{hi} at A={a} B={b}");
                }
            }
        }
    }

    #[test]
    fn runtime_checks_hold_for_hostile_bytecode() {
        assert_eq!(eval(&[OP_PUSH_U8], [0; 4]), Err(EvalError::Trunc));
        assert_eq!(eval(&[OP_ADD], [0; 4]), Err(EvalError::Stack));
        assert_eq!(eval(&[0xEE], [0; 4]), Err(EvalError::BadOp));
        assert_eq!(eval(&[], [0; 4]), Err(EvalError::Result));
        assert_eq!(eval(&[OP_PUSH_U8, 1, OP_PUSH_U8, 0, OP_DIV], [0; 4]), Err(EvalError::DivZero));
        assert_eq!(eval(&[OP_PUSH_U8, 1, OP_PUSH_U8, 32, OP_SHL], [0; 4]), Err(EvalError::Shift));
        let mut deep = vec![];
        for _ in 0..=MAX_STACK {
            deep.extend([OP_PUSH_U8, 1]);
        }
        assert_eq!(eval(&deep, [0; 4]), Err(EvalError::Stack));
        let big = [OP_PUSH_I32, 0xff, 0xff, 0xff, 0x7f, OP_PUSH_U8, 2, OP_MUL];
        assert_eq!(eval(&big, [0; 4]), Err(EvalError::Overflow));
    }
}
