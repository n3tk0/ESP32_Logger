use std::path::Path;

/// Ensure bootloader binary files exist so `include_bytes!` doesn't fail.
/// CI populates real binaries; local builds get empty placeholders with a warning.
fn main() {
    let bins = [
        "../bootloader/esp32c3/bootloader.bin",
        "../bootloader/esp32c3/partition-table.bin",
        "../bootloader/esp32/bootloader.bin",
        "../bootloader/esp32/partition-table.bin",
    ];

    let manifest_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();

    for bin in &bins {
        let full = format!("{manifest_dir}/{bin}");
        let path = Path::new(&full);

        println!("cargo:rerun-if-changed={full}");

        if !path.exists() {
            if let Some(parent) = path.parent() {
                std::fs::create_dir_all(parent).ok();
            }
            std::fs::write(path, b"").ok();
            println!(
                "cargo:warning=Created empty placeholder: {bin} — \
                 run CI or build bootloader with ESP-IDF to get real binaries"
            );
        } else if std::fs::metadata(path).map(|m| m.len()).unwrap_or(0) == 0 {
            println!(
                "cargo:warning={bin} is empty — \
                 flash tool will refuse to flash placeholder binaries"
            );
        }
    }
}
