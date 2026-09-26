use std::path::Path;

use rand::rngs::StdRng;
use rand::SeedableRng;
use sha2::{Digest, Sha256};

use crate::config::UPLOAD_CHUNK_SIZE;

pub struct NetworkEmulator {
    rng: StdRng,
    server_url: String,
    device_id: String,
    connected: bool,
    force_drop: bool,
    drop_after_bytes: i64,
}

impl NetworkEmulator {
    pub fn new(rng_seed: u64, server_url: &str, device_id: &str) -> Self {
        Self {
            rng: StdRng::seed_from_u64(rng_seed),
            server_url: server_url.trim_end_matches('/').to_string(),
            device_id: device_id.to_string(),
            connected: false,
            force_drop: false,
            drop_after_bytes: -1,
        }
    }

    pub fn is_connected(&self) -> bool {
        self.connected && !self.force_drop
    }

    pub fn scan_for_home(&self) -> bool {
        true
    }

    pub fn connect(&mut self) -> bool {
        self.connected = true;
        self.force_drop = false;
        true
    }

    pub fn disconnect(&mut self) {
        self.connected = false;
    }

    pub fn force_drop_after(&mut self, byte_offset: i64) {
        self.drop_after_bytes = byte_offset;
        self.force_drop = false;
    }

    pub fn force_drop_now(&mut self) {
        self.force_drop = true;
        self.connected = false;
    }

    pub fn set_force_drop(&mut self, v: bool) {
        self.force_drop = v;
    }

    pub fn set_drop_after_bytes(&mut self, v: i64) {
        self.drop_after_bytes = v;
    }

    pub fn upload_bundle(
        &mut self,
        trip_id: &str,
        bundle_dir: &Path,
        log_fn: &dyn Fn(&str),
    ) -> bool {
        if self.server_url.is_empty() {
            log_fn(&format!(
                "[NET] No server configured -- skipping upload for {trip_id}"
            ));
            return true;
        }

        let samples_path = bundle_dir.join("samples.bin");
        if !samples_path.exists() {
            log_fn(&format!(
                "[NET] samples.bin not found in {}",
                bundle_dir.display()
            ));
            return false;
        }

        let data = match std::fs::read(&samples_path) {
            Ok(d) => d,
            Err(e) => {
                log_fn(&format!("[NET] Failed to read samples.bin: {e}"));
                return false;
            }
        };

        let content_hash = {
            let mut hasher = Sha256::new();
            hasher.update(&data);
            format!("{:x}", hasher.finalize())
        };

        let init_body = serde_json::json!({
            "device_id": self.device_id,
            "trip_id": trip_id,
            "content_hash": content_hash,
            "size": data.len(),
        });

        let init_url = format!("{}/api/v1/upload/init", self.server_url);
        let resp = match ureq::post(&init_url)
            .set("Content-Type", "application/json")
            .send_string(&init_body.to_string())
        {
            Ok(r) => r,
            Err(e) => {
                log_fn(&format!("[NET] Upload failed for {trip_id}: {e}"));
                return false;
            }
        };

        let resp_body: serde_json::Value = match resp.into_json() {
            Ok(v) => v,
            Err(e) => {
                log_fn(&format!("[NET] Upload failed for {trip_id}: {e}"));
                return false;
            }
        };

        let upload_id = match resp_body["upload_id"].as_str() {
            Some(id) => id.to_string(),
            None => {
                log_fn(&format!("[NET] Upload failed for {trip_id}: no upload_id"));
                return false;
            }
        };

        let resume_offset = resp_body
            .get("resume_offset")
            .and_then(|v| v.as_i64())
            .unwrap_or(0);

        if resume_offset == -1 {
            log_fn(&format!(
                "[NET] Trip {trip_id} already uploaded (server confirms)"
            ));
            return true;
        }

        log_fn(&format!(
            "[NET] Init OK: upload_id={upload_id}, resume={resume_offset}, size={}",
            data.len()
        ));

        let mut offset = resume_offset as usize;
        while offset < data.len() {
            if !self.is_connected() {
                log_fn(&format!("[NET] Wi-Fi dropped at offset {offset}"));
                return false;
            }

            if self.drop_after_bytes >= 0 && offset as i64 >= self.drop_after_bytes {
                log_fn(&format!(
                    "[NET] Simulated Wi-Fi drop at offset {offset}"
                ));
                self.force_drop = true;
                self.connected = false;
                return false;
            }

            let end = (offset + UPLOAD_CHUNK_SIZE).min(data.len());
            let chunk = &data[offset..end];

            let chunk_url = format!(
                "{}/api/v1/upload/{}/chunk",
                self.server_url, upload_id
            );
            let result = ureq::put(&chunk_url)
                .set("Content-Type", "application/octet-stream")
                .set("X-Upload-Offset", &offset.to_string())
                .send_bytes(chunk);

            if let Err(e) = result {
                log_fn(&format!("[NET] Upload failed for {trip_id}: {e}"));
                return false;
            }

            offset += chunk.len();
            let pct = ((offset as f64 / data.len() as f64) * 100.0).min(100.0) as u32;
            log_fn(&format!(
                "[NET] Chunk: {offset}/{} ({pct}%)",
                data.len()
            ));
        }

        let fin_body = serde_json::json!({
            "content_hash": content_hash,
        });

        let fin_url = format!(
            "{}/api/v1/upload/{}/finalize",
            self.server_url, upload_id
        );
        let fin_result = ureq::post(&fin_url)
            .set("Content-Type", "application/json")
            .send_string(&fin_body.to_string());

        match fin_result {
            Ok(resp) => {
                let receipt: serde_json::Value = resp.into_json().unwrap_or_default();
                let receipt_id = receipt
                    .get("receipt_id")
                    .and_then(|v| v.as_str())
                    .unwrap_or("n/a");
                log_fn(&format!(
                    "[NET] Finalized trip {trip_id}, receipt={receipt_id}"
                ));
                true
            }
            Err(e) => {
                log_fn(&format!("[NET] Upload failed for {trip_id}: {e}"));
                false
            }
        }
    }
}
