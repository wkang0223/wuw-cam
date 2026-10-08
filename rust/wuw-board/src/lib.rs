//! WUW CAM pin map: Freenove ESP32-S3-WROOM N16R8 + OV5640 + 2.4" ST7789/HR2046.
//!
//! Mirrors `docs/WIRING.md`. `tools/pincheck.py` fails if these drift from the
//! C++ firmware's `#define`s.
#![no_std]

// Camera (DVP, 8 bit). D0..D7 are the esp32-camera Y2..Y9.
pub const CAM_SIOD: u8 = 4;
pub const CAM_SIOC: u8 = 5;
pub const CAM_VSYNC: u8 = 6;
pub const CAM_HREF: u8 = 7;
pub const CAM_D0: u8 = 11;
pub const CAM_D1: u8 = 9;
pub const CAM_D2: u8 = 8;
pub const CAM_D3: u8 = 10;
pub const CAM_D4: u8 = 12;
pub const CAM_D5: u8 = 18;
pub const CAM_D6: u8 = 17;
pub const CAM_D7: u8 = 16;
pub const CAM_PCLK: u8 = 13;
pub const CAM_XCLK: u8 = 15;

// Onboard microSD, SDMMC 1-bit.
pub const SD_CMD: u8 = 38;
pub const SD_CLK: u8 = 39;
pub const SD_D0: u8 = 40;

// Shutter button to GND, internal pull-up.
pub const BTN: u8 = 14;

// Display + touch on one shared SPI bus (bridges B1-B3 in WIRING.md).
pub const LCD_MISO: u8 = 1;
pub const LCD_MOSI: u8 = 2;
pub const LCD_SCLK: u8 = 41;
pub const LCD_CS: u8 = 21;
pub const LCD_DC: u8 = 47;
pub const LCD_RST: u8 = 20;
pub const LCD_BL: u8 = 48;
pub const TCH_CS: u8 = 42;

/// Every claimed pin with its name, for checks.
#[rustfmt::skip]
pub const ALL: [(&str, u8); 26] = [
    ("CAM_SIOD", CAM_SIOD), ("CAM_SIOC", CAM_SIOC), ("CAM_VSYNC", CAM_VSYNC),
    ("CAM_HREF", CAM_HREF), ("CAM_D0", CAM_D0), ("CAM_D1", CAM_D1),
    ("CAM_D2", CAM_D2), ("CAM_D3", CAM_D3), ("CAM_D4", CAM_D4),
    ("CAM_D5", CAM_D5), ("CAM_D6", CAM_D6), ("CAM_D7", CAM_D7),
    ("CAM_PCLK", CAM_PCLK), ("CAM_XCLK", CAM_XCLK),
    ("SD_CMD", SD_CMD), ("SD_CLK", SD_CLK), ("SD_D0", SD_D0),
    ("BTN", BTN),
    ("LCD_MISO", LCD_MISO), ("LCD_MOSI", LCD_MOSI), ("LCD_SCLK", LCD_SCLK),
    ("LCD_CS", LCD_CS), ("LCD_DC", LCD_DC), ("LCD_RST", LCD_RST),
    ("LCD_BL", LCD_BL), ("TCH_CS", TCH_CS),
];

/// Pins the N16R8 module reserves: SPI flash, octal PSRAM, UART0, nonexistent.
pub fn is_forbidden(pin: u8) -> bool {
    matches!(pin, 22..=37 | 43 | 44)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn no_pin_claimed_twice() {
        for (i, (a, pa)) in ALL.iter().enumerate() {
            for (b, pb) in &ALL[i + 1..] {
                assert_ne!(pa, pb, "{a} and {b} both claim GPIO {pa}");
            }
        }
    }

    #[test]
    fn no_pin_is_reserved() {
        for (name, pin) in ALL {
            assert!(!is_forbidden(pin), "{name} is on reserved GPIO {pin}");
        }
    }

    #[test]
    fn strapping_pins_stay_free() {
        for (name, pin) in ALL {
            assert!(
                ![0, 3, 45, 46].contains(&pin),
                "{name} is on strapping GPIO {pin}"
            );
        }
    }
}
