//! Locate a complete JPEG inside a camera DMA buffer.
#![no_std]

use core::ops::Range;

/// Byte range of the first complete JPEG (SOI `FF D8` .. EOI `FF D9` inclusive).
pub fn find_jpeg(buf: &[u8]) -> Option<Range<usize>> {
    let soi = buf.windows(2).position(|w| w == [0xFF, 0xD8])?;
    let after = soi + 2;
    let eoi = buf[after..].windows(2).position(|w| w == [0xFF, 0xD9])?;
    Some(soi..after + eoi + 2)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exact_frame() {
        let b = [0xFF, 0xD8, 1, 2, 3, 0xFF, 0xD9];
        assert_eq!(find_jpeg(&b), Some(0..7));
    }

    #[test]
    fn padding_both_sides() {
        let b = [0, 0, 0xFF, 0xD8, 9, 0xFF, 0xD9, 0, 0];
        assert_eq!(find_jpeg(&b), Some(2..7));
    }

    #[test]
    fn truncated_frame_is_rejected() {
        let b = [0xFF, 0xD8, 1, 2, 3];
        assert_eq!(find_jpeg(&b), None);
    }

    #[test]
    fn no_soi() {
        assert_eq!(find_jpeg(&[1, 2, 0xFF, 0xD9]), None);
    }

    #[test]
    fn empty_and_tiny() {
        assert_eq!(find_jpeg(&[]), None);
        assert_eq!(find_jpeg(&[0xFF]), None);
    }

    #[test]
    fn eoi_before_soi_is_ignored() {
        let b = [0xFF, 0xD9, 0xFF, 0xD8, 7, 0xFF, 0xD9];
        assert_eq!(find_jpeg(&b), Some(2..7));
    }

    #[test]
    fn stuffed_ff00_inside_scan_is_not_eoi() {
        let b = [0xFF, 0xD8, 0xFF, 0x00, 0xD9, 0xFF, 0xD9];
        assert_eq!(find_jpeg(&b), Some(0..7));
    }
}
