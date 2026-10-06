//! enginegen: validates `engines/*.yaml` (cairn.engine/v1-draft) and emits the C
//! tables and build identity the firmware compiles in.

pub mod emit;
pub mod expr;
pub mod profile;
pub mod vectors;
