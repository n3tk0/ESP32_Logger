//! ESP32 Bootloader Flash Tool
//!
//! Standalone tool — flashes a rollback-enabled bootloader to ESP32 devices.
//! Bootloader binaries are embedded in the executable; no external files needed.
//!
//! Implements the ESP ROM bootloader serial protocol directly:
//! SLIP framing, SYNC, READ_REG, SPI_ATTACH, FLASH_BEGIN/DATA/END.

use std::io::{self, Read, Write};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{bail, Context, Result};
use clap::Parser;

// ═══════════════════════════════════════════════════════════════════
// Embedded bootloader binaries (populated by CI, or empty placeholders)
// ═══════════════════════════════════════════════════════════════════

const BL_ESP32C3: &[u8] = include_bytes!("../../bootloader/esp32c3/bootloader.bin");
const PT_ESP32C3: &[u8] = include_bytes!("../../bootloader/esp32c3/partition-table.bin");
const BL_ESP32: &[u8] = include_bytes!("../../bootloader/esp32/bootloader.bin");
const PT_ESP32: &[u8] = include_bytes!("../../bootloader/esp32/partition-table.bin");

// ═══════════════════════════════════════════════════════════════════
// SLIP framing (RFC 1055)
// ═══════════════════════════════════════════════════════════════════

const SLIP_END: u8 = 0xC0;
const SLIP_ESC: u8 = 0xDB;
const SLIP_ESC_END: u8 = 0xDC;
const SLIP_ESC_ESC: u8 = 0xDD;

fn slip_encode(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len() + 16);
    out.push(SLIP_END);
    for &b in data {
        match b {
            SLIP_END => {
                out.push(SLIP_ESC);
                out.push(SLIP_ESC_END);
            }
            SLIP_ESC => {
                out.push(SLIP_ESC);
                out.push(SLIP_ESC_ESC);
            }
            _ => out.push(b),
        }
    }
    out.push(SLIP_END);
    out
}

fn slip_read_frame(
    port: &mut dyn serialport::SerialPort,
    timeout: Duration,
) -> Result<Vec<u8>> {
    let deadline = Instant::now() + timeout;
    let mut byte = [0u8; 1];
    let mut frame = Vec::with_capacity(256);
    let mut started = false;

    while Instant::now() < deadline {
        match port.read(&mut byte) {
            Ok(1) => match byte[0] {
                SLIP_END => {
                    if started && !frame.is_empty() {
                        return Ok(frame);
                    }
                    started = true;
                    frame.clear();
                }
                SLIP_ESC if started => {
                    // Read escape byte
                    let esc_deadline = Instant::now() + Duration::from_millis(200);
                    loop {
                        match port.read(&mut byte) {
                            Ok(1) => {
                                frame.push(match byte[0] {
                                    SLIP_ESC_END => SLIP_END,
                                    SLIP_ESC_ESC => SLIP_ESC,
                                    other => other,
                                });
                                break;
                            }
                            _ if Instant::now() < esc_deadline => {
                                thread::sleep(Duration::from_millis(1));
                            }
                            _ => bail!("Timeout reading SLIP escape"),
                        }
                    }
                }
                b if started => frame.push(b),
                _ => {} // garbage before frame start
            },
            _ => thread::sleep(Duration::from_millis(1)),
        }
    }
    bail!("SLIP read timeout")
}

// ═══════════════════════════════════════════════════════════════════
// ESP ROM bootloader protocol
// ═══════════════════════════════════════════════════════════════════

const CMD_FLASH_BEGIN: u8 = 0x02;
const CMD_FLASH_DATA: u8 = 0x03;
const CMD_FLASH_END: u8 = 0x04;
const CMD_SYNC: u8 = 0x08;
const CMD_READ_REG: u8 = 0x0A;
const CMD_SPI_ATTACH: u8 = 0x0D;
const CMD_CHANGE_BAUDRATE: u8 = 0x0F;

const CHECKSUM_MAGIC: u8 = 0xEF;
const FLASH_BLOCK: usize = 0x4000; // 16 KB — ROM bootloader block size
const FLASH_TIMEOUT: Duration = Duration::from_secs(30);

// Chip detection: read register at 0x40001000
const CHIP_DETECT_REG: u32 = 0x4000_1000;
const MAGIC_ESP32: u32 = 0x00F0_1D83;
const MAGIC_ESP32C3: [u32; 4] = [0x6921_506F, 0x1B31_506F, 0x4881_606F, 0x4361_606F];

// ── Chip definitions ──────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq)]
enum Chip {
    Esp32,
    Esp32c3,
}

impl Chip {
    fn name(self) -> &'static str {
        match self {
            Chip::Esp32 => "ESP32",
            Chip::Esp32c3 => "ESP32-C3",
        }
    }
    fn bootloader_addr(self) -> u32 {
        match self {
            Chip::Esp32 => 0x1000,   // ESP32 bootloader starts at 0x1000
            Chip::Esp32c3 => 0x0,    // ESP32-C3 bootloader starts at 0x0
        }
    }
    fn partition_addr(self) -> u32 {
        0x8000
    }
    fn bootloader_bin(self) -> &'static [u8] {
        match self {
            Chip::Esp32 => BL_ESP32,
            Chip::Esp32c3 => BL_ESP32C3,
        }
    }
    fn partition_bin(self) -> &'static [u8] {
        match self {
            Chip::Esp32 => PT_ESP32,
            Chip::Esp32c3 => PT_ESP32C3,
        }
    }
}

// ── Protocol helpers ──────────────────────────────────────────────

fn checksum(data: &[u8]) -> u8 {
    data.iter().fold(CHECKSUM_MAGIC, |acc, &b| acc ^ b)
}

fn send_command(
    port: &mut dyn serialport::SerialPort,
    cmd: u8,
    data: &[u8],
    chk: u32,
    timeout: Duration,
) -> Result<u32> {
    // Build packet: [dir=0x00][cmd][data_len:u16 LE][checksum:u32 LE][data...]
    let mut pkt = Vec::with_capacity(8 + data.len());
    pkt.push(0x00); // direction: command
    pkt.push(cmd);
    pkt.extend_from_slice(&(data.len() as u16).to_le_bytes());
    pkt.extend_from_slice(&chk.to_le_bytes());
    pkt.extend_from_slice(data);

    port.write_all(&slip_encode(&pkt))?;
    port.flush()?;

    // Read response — skip stale/unrelated frames
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        let remaining = deadline.saturating_duration_since(Instant::now());
        let resp = match slip_read_frame(port, remaining) {
            Ok(r) => r,
            Err(_) if Instant::now() < deadline => continue,
            Err(e) => return Err(e),
        };

        // Valid response: [dir=0x01][cmd][size:u16][value:u32][...][status][error]
        if resp.len() >= 8 && resp[0] == 0x01 && resp[1] == cmd {
            let value = u32::from_le_bytes([resp[4], resp[5], resp[6], resp[7]]);
            // Check status — last 2 bytes of the body after the 8-byte header
            if resp.len() >= 10 {
                let status = resp[resp.len() - 2];
                if status != 0 {
                    let error = resp[resp.len() - 1];
                    bail!(
                        "Command 0x{cmd:02X} failed (status={status}, error=0x{error:02X})"
                    );
                }
            }
            return Ok(value);
        }
    }
    bail!("Command 0x{cmd:02X} response timeout")
}

// ── High-level operations ─────────────────────────────────────────

fn enter_bootloader(port: &mut dyn serialport::SerialPort) -> Result<()> {
    // Classic DTR/RTS reset sequence (works on most USB-UART boards):
    //   RTS=high (EN low = reset), DTR=low
    //   DTR=high (IO0 low = boot mode), RTS=low (EN high = release reset)
    //   DTR=low (IO0 released — boot mode already latched)
    port.write_data_terminal_ready(false)?;
    port.write_request_to_send(true)?;
    thread::sleep(Duration::from_millis(100));
    port.write_data_terminal_ready(true)?;
    port.write_request_to_send(false)?;
    thread::sleep(Duration::from_millis(50));
    port.write_data_terminal_ready(false)?;
    thread::sleep(Duration::from_millis(50));

    // Drain stale data
    let mut discard = [0u8; 512];
    while port.read(&mut discard).unwrap_or(0) > 0 {}

    Ok(())
}

fn sync(port: &mut dyn serialport::SerialPort) -> Result<()> {
    // SYNC payload: [0x07 0x07 0x12 0x20] + [0x55] × 32
    let mut payload = vec![0x07, 0x07, 0x12, 0x20];
    payload.extend_from_slice(&[0x55; 32]);

    for attempt in 1..=10 {
        // Flush input
        let mut discard = [0u8; 512];
        while port.read(&mut discard).unwrap_or(0) > 0 {}

        if send_command(port, CMD_SYNC, &payload, 0, Duration::from_millis(500)).is_ok() {
            // Drain extra SYNC responses
            thread::sleep(Duration::from_millis(50));
            while slip_read_frame(port, Duration::from_millis(100)).is_ok() {}
            return Ok(());
        }

        if attempt < 10 {
            // On first few failures, try re-entering bootloader
            if attempt == 4 {
                enter_bootloader(port)?;
            }
            thread::sleep(Duration::from_millis(50));
        }
    }
    bail!(
        "Could not sync with bootloader. Make sure the device is connected \
         and in download mode (hold BOOT button during reset on some boards)."
    )
}

fn detect_chip(port: &mut dyn serialport::SerialPort) -> Result<Chip> {
    let data = CHIP_DETECT_REG.to_le_bytes().to_vec();
    let magic = send_command(port, CMD_READ_REG, &data, 0, Duration::from_secs(2))?;

    if magic == MAGIC_ESP32 {
        return Ok(Chip::Esp32);
    }
    if MAGIC_ESP32C3.contains(&magic) {
        return Ok(Chip::Esp32c3);
    }
    bail!(
        "Unknown chip (magic=0x{magic:08X}). \
         Supported: ESP32 (0x{MAGIC_ESP32:08X}), ESP32-C3"
    )
}

fn change_baudrate(
    port: &mut dyn serialport::SerialPort,
    new_baud: u32,
) -> Result<()> {
    let mut data = Vec::new();
    data.extend_from_slice(&new_baud.to_le_bytes());
    data.extend_from_slice(&0u32.to_le_bytes()); // 0 = current baud (auto)
    send_command(port, CMD_CHANGE_BAUDRATE, &data, 0, Duration::from_secs(2))?;
    Ok(())
}

fn spi_attach(port: &mut dyn serialport::SerialPort) -> Result<()> {
    send_command(
        port,
        CMD_SPI_ATTACH,
        &[0u8; 8], // is_hspi=0, is_legacy=0
        0,
        Duration::from_secs(3),
    )?;
    Ok(())
}

fn flash_binary(
    port: &mut dyn serialport::SerialPort,
    bin: &[u8],
    offset: u32,
    label: &str,
) -> Result<()> {
    if bin.is_empty() {
        bail!(
            "{label} binary is empty — bootloader not built yet. \
             Run the GitHub Actions workflow or build with ESP-IDF."
        );
    }

    let num_blocks = (bin.len() + FLASH_BLOCK - 1) / FLASH_BLOCK;
    let erase_size = ((bin.len() + 0xFFF) & !0xFFF) as u32; // round up to 4 KB sector

    // FLASH_BEGIN: [erase_size][num_blocks][block_size][offset]
    let mut begin_data = Vec::new();
    begin_data.extend_from_slice(&erase_size.to_le_bytes());
    begin_data.extend_from_slice(&(num_blocks as u32).to_le_bytes());
    begin_data.extend_from_slice(&(FLASH_BLOCK as u32).to_le_bytes());
    begin_data.extend_from_slice(&offset.to_le_bytes());

    print!("  {label}: erasing {} bytes at 0x{offset:05X}...", bin.len());
    io::stdout().flush().ok();
    send_command(port, CMD_FLASH_BEGIN, &begin_data, 0, FLASH_TIMEOUT)?;
    println!(" OK");

    // FLASH_DATA: [size][seq][0][0][data padded to block_size]
    for (seq, chunk) in bin.chunks(FLASH_BLOCK).enumerate() {
        let mut padded = chunk.to_vec();
        padded.resize(FLASH_BLOCK, 0xFF);

        let chk = checksum(&padded) as u32;

        let mut block_data = Vec::with_capacity(16 + FLASH_BLOCK);
        block_data.extend_from_slice(&(padded.len() as u32).to_le_bytes());
        block_data.extend_from_slice(&(seq as u32).to_le_bytes());
        block_data.extend_from_slice(&0u32.to_le_bytes());
        block_data.extend_from_slice(&0u32.to_le_bytes());
        block_data.extend_from_slice(&padded);

        print!("\r  {label}: block {}/{num_blocks}", seq + 1);
        io::stdout().flush().ok();
        send_command(port, CMD_FLASH_DATA, &block_data, chk, FLASH_TIMEOUT)?;
    }
    println!(" done");
    Ok(())
}

fn flash_end_reboot(port: &mut dyn serialport::SerialPort) -> Result<()> {
    // action=0 means reboot; the device resets immediately so don't wait for response
    let pkt = {
        let data = 0u32.to_le_bytes(); // action = reboot
        let mut p = Vec::with_capacity(12);
        p.push(0x00);
        p.push(CMD_FLASH_END);
        p.extend_from_slice(&(data.len() as u16).to_le_bytes());
        p.extend_from_slice(&0u32.to_le_bytes());
        p.extend_from_slice(&data);
        p
    };
    port.write_all(&slip_encode(&pkt))?;
    port.flush()?;
    Ok(())
}

// ═══════════════════════════════════════════════════════════════════
// CLI
// ═══════════════════════════════════════════════════════════════════

#[derive(Parser)]
#[command(
    name = "flash_bootloader",
    about = "Flash rollback-enabled bootloader to ESP32 devices",
    long_about = "Standalone tool with embedded bootloader binaries.\n\
                  No Python, esptool, or ESP-IDF installation required.\n\n\
                  Supports: ESP32, ESP32-C3 (XIAO ESP32-C3)"
)]
struct Cli {
    /// Serial port (e.g. COM3, /dev/ttyACM0). Auto-detect if omitted.
    #[arg(short, long)]
    port: Option<String>,

    /// Chip type: esp32, esp32c3. Auto-detect if omitted.
    #[arg(short, long)]
    chip: Option<String>,

    /// Flash baud rate (initial sync always at 115200)
    #[arg(short, long, default_value = "460800")]
    baud: u32,

    /// Flash bootloader only, skip partition table
    #[arg(long)]
    bootloader_only: bool,

    /// List available serial ports and exit
    #[arg(long)]
    list_ports: bool,

    /// Skip confirmation prompt
    #[arg(short = 'y', long)]
    yes: bool,
}

fn list_serial_ports() -> Vec<serialport::SerialPortInfo> {
    serialport::available_ports().unwrap_or_default()
}

fn pick_port(ports: &[serialport::SerialPortInfo]) -> Result<String> {
    // Prefer USB ports
    let usb: Vec<_> = ports
        .iter()
        .filter(|p| matches!(p.port_type, serialport::SerialPortType::UsbPort(_)))
        .collect();

    let candidates = if usb.is_empty() { ports } else { &usb };

    match candidates.len() {
        0 => bail!("No serial ports found. Connect the device and try again, or use --port."),
        1 => {
            let name = &candidates[0].port_name;
            println!("Auto-detected port: {name}");
            Ok(name.clone())
        }
        _ => {
            println!("Multiple serial ports found:");
            for (i, p) in candidates.iter().enumerate() {
                let info = match &p.port_type {
                    serialport::SerialPortType::UsbPort(u) => {
                        format!(
                            " — {}",
                            u.product.as_deref().unwrap_or("USB Serial")
                        )
                    }
                    _ => String::new(),
                };
                println!("  [{i}] {}{info}", p.port_name);
            }
            print!("Select port number: ");
            io::stdout().flush()?;
            let mut input = String::new();
            io::stdin().read_line(&mut input)?;
            let idx: usize = input.trim().parse().context("Invalid number")?;
            Ok(candidates
                .get(idx)
                .context("Index out of range")?
                .port_name
                .clone())
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

fn run() -> Result<()> {
    let cli = Cli::parse();

    // ── List ports mode ──
    if cli.list_ports {
        let ports = list_serial_ports();
        if ports.is_empty() {
            println!("No serial ports found.");
        } else {
            println!("Available serial ports:");
            for p in &ports {
                let info = match &p.port_type {
                    serialport::SerialPortType::UsbPort(u) => {
                        format!(
                            " ({})",
                            u.product.as_deref().unwrap_or("USB")
                        )
                    }
                    _ => String::new(),
                };
                println!("  {}{info}", p.port_name);
            }
        }
        return Ok(());
    }

    println!("ESP32 Bootloader Flash Tool v{}", env!("CARGO_PKG_VERSION"));
    println!();

    // ── Resolve port ──
    let port_name = match cli.port {
        Some(p) => p,
        None => pick_port(&list_serial_ports())?,
    };

    // ── Open at 115200 (ROM bootloader default) ──
    let mut port = serialport::new(&port_name, 115200)
        .timeout(Duration::from_millis(100))
        .open()
        .with_context(|| format!("Cannot open {port_name}"))?;

    // ── Enter bootloader & sync ──
    println!("Resetting into bootloader...");
    enter_bootloader(port.as_mut())?;

    print!("Syncing");
    io::stdout().flush().ok();
    sync(port.as_mut())?;
    println!(" OK");

    // ── Detect chip ──
    let chip = match &cli.chip {
        Some(s) => match s.to_lowercase().replace('-', "").as_str() {
            "esp32" => Chip::Esp32,
            "esp32c3" => Chip::Esp32c3,
            other => bail!("Unknown chip '{other}'. Use: esp32, esp32c3"),
        },
        None => {
            print!("Detecting chip... ");
            io::stdout().flush().ok();
            let c = detect_chip(port.as_mut())?;
            println!("{}", c.name());
            c
        }
    };

    // ── Validate embedded binaries ──
    let bl = chip.bootloader_bin();
    let pt = chip.partition_bin();
    if bl.is_empty() {
        bail!(
            "No bootloader binary embedded for {}. \
             Run the GitHub Actions CI to build it.",
            chip.name()
        );
    }

    // ── Show plan & confirm ──
    println!();
    println!("  Chip:         {}", chip.name());
    println!("  Port:         {port_name}");
    println!(
        "  Bootloader:   {} bytes -> 0x{:05X}",
        bl.len(),
        chip.bootloader_addr()
    );
    if !cli.bootloader_only && !pt.is_empty() {
        println!(
            "  Partitions:   {} bytes -> 0x{:05X}",
            pt.len(),
            chip.partition_addr()
        );
    }
    println!();

    if !cli.yes {
        print!("Flash? This overwrites the existing bootloader. [y/N] ");
        io::stdout().flush()?;
        let mut input = String::new();
        io::stdin().read_line(&mut input)?;
        if !matches!(input.trim(), "y" | "Y" | "yes") {
            println!("Aborted.");
            return Ok(());
        }
    }

    // ── Speed up: switch baud rate ──
    if cli.baud > 115200 {
        print!("Switching to {} baud... ", cli.baud);
        io::stdout().flush().ok();
        change_baudrate(port.as_mut(), cli.baud)?;
        // Close and reopen at new baud
        drop(port);
        thread::sleep(Duration::from_millis(100));
        port = serialport::new(&port_name, cli.baud)
            .timeout(Duration::from_millis(100))
            .open()
            .context("Failed to reopen port at new baud rate")?;
        println!("OK");
    }

    // ── Attach SPI flash ──
    spi_attach(port.as_mut())?;

    // ── Flash bootloader ──
    println!();
    flash_binary(port.as_mut(), bl, chip.bootloader_addr(), "Bootloader")?;

    // ── Flash partition table ──
    if !cli.bootloader_only && !pt.is_empty() {
        flash_binary(port.as_mut(), pt, chip.partition_addr(), "Partition table")?;
    }

    // ── Reboot ──
    println!();
    print!("Resetting device... ");
    io::stdout().flush().ok();
    flash_end_reboot(port.as_mut())?;
    println!("OK");

    println!();
    println!("Bootloader with rollback support flashed successfully!");
    println!("Upload your firmware as usual (Arduino IDE / PlatformIO).");
    println!("After the next OTA update, rollback-on-crash will be active.");

    Ok(())
}

fn main() {
    if let Err(e) = run() {
        eprintln!("\nERROR: {e:#}");
        std::process::exit(1);
    }
}
