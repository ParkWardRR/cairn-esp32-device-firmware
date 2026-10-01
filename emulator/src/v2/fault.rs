//! Fault injection.
//!
//! The architecture's value lives at failure boundaries, not on the happy path,
//! so the harness interrupts the device at named points and then asserts what
//! survived. Every point corresponds to a real failure a car inflicts: key-off
//! mid-write, Wi-Fi dropping mid-upload, a reboot between earning a receipt and
//! acting on it.
//!
//! Interruptions are modelled as an error returned at the chosen point rather
//! than as a process abort, because the device's durable state is what matters
//! and that is already on disk by then. A "reboot" is simply constructing a new
//! device over the same directory and running recovery.

use std::fmt;

/// A named point at which the device can be interrupted.
///
/// The names are deliberately concrete about *when* relative to durability: the
/// difference between `BeforeManifestSync` and `AfterManifestSync` is the whole
/// question of whether a sealed bundle survives.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FaultPoint {
    /// Power cut immediately after the Nth frame was written but before the
    /// segment was flushed. The frame may or may not be durable.
    AfterFrameWrite(usize),

    /// Power cut after the segment was flushed and synced, so every frame
    /// written so far is durable.
    AfterSegmentSync,

    /// Power cut after only part of a frame reached the medium: `drop_bytes` of
    /// the frame's tail never landed.
    ///
    /// This is the torn-tail case — the most likely real failure, and the one
    /// unframed records cannot distinguish from valid data.
    MidFrameWrite { frame: usize, drop_bytes: usize },

    /// Power cut after the manifest was written to its temporary file but before
    /// it was synced.
    BeforeManifestSync,

    /// Power cut after the manifest was synced but before the sealing rename.
    AfterManifestSync,

    /// Power cut after the sealing rename, so the bundle is sealed.
    AfterSealRename,

    /// Network loss before the manifest was offered.
    BeforeOffer,

    /// Network loss after the offer succeeded but before any chunk was sent.
    AfterOffer,

    /// Network loss before sending chunk N.
    BeforeChunk(u32),

    /// Network loss after chunk N was accepted.
    AfterChunk(u32),

    /// Corrupt chunk N in transit. The server must reject it, and a retry must
    /// succeed without operator action.
    CorruptChunkInTransit(u32),

    /// Network loss before commit, with every chunk already delivered.
    BeforeCommit,

    /// The server committed and issued a receipt, but the response never
    /// reached the device. The device must retry and get the same receipt.
    ReceiptLostInTransit,

    /// Reboot after the receipt was verified but before the prune journal
    /// recorded an intent.
    AfterReceiptVerify,

    /// Reboot after `prune_intent` was journalled but before any payload was
    /// deleted.
    AfterPruneIntent,

    /// Reboot after the payload was deleted but before `prune_complete` was
    /// journalled. The dangerous window, and the reason pruning is
    /// transactional.
    AfterPayloadDelete,
}

impl FaultPoint {
    /// A short stable label, used in reports and as part of a reproduction
    /// command.
    pub fn label(&self) -> String {
        match self {
            Self::AfterFrameWrite(n) => format!("after-frame-write:{n}"),
            Self::AfterSegmentSync => "after-segment-sync".into(),
            Self::MidFrameWrite { frame, drop_bytes } => {
                format!("mid-frame-write:{frame}:{drop_bytes}")
            }
            Self::BeforeManifestSync => "before-manifest-sync".into(),
            Self::AfterManifestSync => "after-manifest-sync".into(),
            Self::AfterSealRename => "after-seal-rename".into(),
            Self::BeforeOffer => "before-offer".into(),
            Self::AfterOffer => "after-offer".into(),
            Self::BeforeChunk(n) => format!("before-chunk:{n}"),
            Self::AfterChunk(n) => format!("after-chunk:{n}"),
            Self::CorruptChunkInTransit(n) => format!("corrupt-chunk:{n}"),
            Self::BeforeCommit => "before-commit".into(),
            Self::ReceiptLostInTransit => "receipt-lost-in-transit".into(),
            Self::AfterReceiptVerify => "after-receipt-verify".into(),
            Self::AfterPruneIntent => "after-prune-intent".into(),
            Self::AfterPayloadDelete => "after-payload-delete".into(),
        }
    }

    /// Whether this fault models losing power, as opposed to losing the network.
    ///
    /// The distinction matters for what the harness asserts: a power cut tests
    /// local durability and recovery, a network fault tests the protocol's
    /// ability to resume.
    pub fn is_power_loss(&self) -> bool {
        matches!(
            self,
            Self::AfterFrameWrite(_)
                | Self::AfterSegmentSync
                | Self::MidFrameWrite { .. }
                | Self::BeforeManifestSync
                | Self::AfterManifestSync
                | Self::AfterSealRename
                | Self::AfterReceiptVerify
                | Self::AfterPruneIntent
                | Self::AfterPayloadDelete
        )
    }
}

impl fmt::Display for FaultPoint {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.label())
    }
}

/// What the device should do when it reaches the armed point.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FaultAction {
    /// Stop as if power were lost or the network vanished.
    Interrupt,
    /// Corrupt the data in flight but keep going, so the far side must detect it.
    Corrupt,
}

/// An armed fault.
#[derive(Debug, Clone, Copy)]
pub struct FaultPlan {
    pub point: FaultPoint,
    pub action: FaultAction,
    /// The seed that produced this plan, printed on failure so a run reproduces.
    pub seed: u64,
}

impl FaultPlan {
    pub fn interrupt(point: FaultPoint) -> Self {
        Self {
            point,
            action: FaultAction::Interrupt,
            seed: 0,
        }
    }

    pub fn corrupt(point: FaultPoint) -> Self {
        Self {
            point,
            action: FaultAction::Corrupt,
            seed: 0,
        }
    }

    pub fn with_seed(mut self, seed: u64) -> Self {
        self.seed = seed;
        self
    }
}

/// Signals that an armed fault fired. Not an error in the usual sense — it is
/// the expected outcome of a test.
#[derive(Debug, Clone)]
pub struct Interrupted {
    pub point: FaultPoint,
}

impl fmt::Display for Interrupted {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "interrupted at {}", self.point)
    }
}

impl std::error::Error for Interrupted {}

/// Tracks an armed fault and reports whether it fired.
#[derive(Debug, Default)]
pub struct Injector {
    plan: Option<FaultPlan>,
    fired: bool,
}

impl Injector {
    /// An injector that never fires — the happy path.
    pub fn none() -> Self {
        Self {
            plan: None,
            fired: false,
        }
    }

    pub fn armed(plan: FaultPlan) -> Self {
        Self {
            plan: Some(plan),
            fired: false,
        }
    }

    pub fn plan(&self) -> Option<FaultPlan> {
        self.plan
    }

    pub fn fired(&self) -> bool {
        self.fired
    }

    /// Whether the armed point matches, marking it fired if so.
    pub fn should_fire(&mut self, point: FaultPoint) -> Option<FaultAction> {
        match self.plan {
            Some(plan) if plan.point == point && !self.fired => {
                self.fired = true;
                Some(plan.action)
            }
            _ => None,
        }
    }

    /// Fire as an interruption if the point matches.
    pub fn check(&mut self, point: FaultPoint) -> std::result::Result<(), Interrupted> {
        match self.should_fire(point) {
            Some(FaultAction::Interrupt) => Err(Interrupted { point }),
            _ => Ok(()),
        }
    }

    /// Whether the armed point requests corruption here.
    pub fn should_corrupt(&mut self, point: FaultPoint) -> bool {
        matches!(self.should_fire(point), Some(FaultAction::Corrupt))
    }
}
