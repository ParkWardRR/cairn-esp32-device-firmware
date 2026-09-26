use std::collections::HashMap;
use std::path::{Path, PathBuf};

use crate::config::MIN_FREE_SD_MB;

pub struct StorageEmulator {
    output_dir: PathBuf,
    capacity_bytes: u64,
    pub used_bytes: u64,
    trip_dirs: HashMap<String, PathBuf>,
}

impl StorageEmulator {
    pub fn new(output_dir: &Path, capacity_mb: u64) -> Self {
        let output = output_dir.to_path_buf();
        std::fs::create_dir_all(&output).ok();
        Self {
            output_dir: output,
            capacity_bytes: capacity_mb * 1024 * 1024,
            used_bytes: 0,
            trip_dirs: HashMap::new(),
        }
    }

    pub fn free_mb(&self) -> u64 {
        (self.capacity_bytes - self.used_bytes) / (1024 * 1024)
    }

    pub fn is_full(&self) -> bool {
        self.free_mb() < MIN_FREE_SD_MB as u64
    }

    pub fn set_nearly_full(&mut self, free_mb: u64) {
        self.used_bytes = self.capacity_bytes - free_mb * 1024 * 1024;
    }

    pub fn open_trip(&mut self, trip_id: &str) -> PathBuf {
        let trip_dir = self.output_dir.join(trip_id);
        std::fs::create_dir_all(&trip_dir).ok();
        self.trip_dirs.insert(trip_id.to_string(), trip_dir.clone());
        trip_dir
    }

    pub fn write_file(&mut self, trip_id: &str, filename: &str, data: &[u8]) -> bool {
        if self.used_bytes + data.len() as u64 > self.capacity_bytes {
            return false;
        }
        let trip_dir = match self.trip_dirs.get(trip_id) {
            Some(d) => d,
            None => return false,
        };
        let path = trip_dir.join(filename);
        if std::fs::write(&path, data).is_err() {
            return false;
        }
        self.used_bytes += data.len() as u64;
        true
    }

    pub fn prune_trip(&mut self, trip_id: &str) {
        if let Some(trip_dir) = self.trip_dirs.remove(trip_id) {
            if trip_dir.exists() {
                if let Ok(entries) = std::fs::read_dir(&trip_dir) {
                    for entry in entries.flatten() {
                        if let Ok(meta) = entry.metadata() {
                            let sz = meta.len();
                            let _ = std::fs::remove_file(entry.path());
                            self.used_bytes = self.used_bytes.saturating_sub(sz);
                        }
                    }
                }
                let _ = std::fs::remove_dir(&trip_dir);
            }
        }
    }

    pub fn get_trip_dir(&self, trip_id: &str) -> Option<&PathBuf> {
        self.trip_dirs.get(trip_id)
    }
}
