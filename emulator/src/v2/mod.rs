//! The v2 device model, writing bundle format v3.
//!
//! Models what the firmware must do — framed, encrypted capture under an
//! off-card storage root; an NVS bundle counter; atomic sealing; keyless boot
//! recovery; manifest-first sync; local receipt verification; transactional
//! pruning — so each step can be interrupted and the surviving state asserted.
//! ("v2" names the device architecture, not the bundle format.)
//!
//! The point is not to simulate a car. It is to establish that the orderings
//! the specification makes normative actually hold under the failures a car
//! inflicts, before any of it is committed to firmware that lives in a parked
//! vehicle.

// Some of the fault points and recovery fields are not yet asserted on by a
// matrix row: they exist because the specification or the device lifecycle
// defines them, and the later firmware phases add the rows that use them. The
// standing rule is that every phase adds its own rows before it closes.
#![allow(dead_code)]

pub mod fault;
pub mod matrix;
pub mod store;
pub mod sync;
