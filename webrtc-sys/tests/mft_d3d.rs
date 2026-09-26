// Runs the production MFT encoder against every adapter on this machine that
// has a hardware H.264 MFT, with texture input offered on it (vendor
// allowlist lifted). Needs real hardware, so it lives behind a feature:
//   cargo test -p webrtc-sys --features mft-selftest --test mft_d3d -- --nocapture
#![cfg(all(feature = "mft-selftest", target_os = "windows"))]

use webrtc_sys::mft_selftest::ffi::mft_d3d_selftest;

const CPU_FRAMES: u32 = 5;
const TEXTURE_FRAMES: u32 = 55;

#[test]
fn texture_frames_are_encoded_on_the_gpu_and_read_back_exactly() {
    let mut adapters = 0;
    for ordinal in 0..8 {
        let run = mft_d3d_selftest(ordinal, 1280, 720, CPU_FRAMES, TEXTURE_FRAMES);
        if run.mft_count == 0 {
            break;
        }
        adapters += 1;
        println!("{run:#?}");
        let who = &run.encoder_name;
        assert_eq!(run.init_result, 0, "{who}: encoder init");
        assert_eq!(run.d3d_stage, 10, "{who}: texture input setup stopped at stage {}", run.d3d_stage);
        assert!(run.native_handle, "{who}: must take native frames while texture input is on");
        assert_eq!(run.active_luid, run.adapter_luid, "{who}: advertised on the wrong adapter");
        assert_eq!(run.first_error, 0, "{who}: an Encode call failed");
        // The first frames were uploaded from memory, every later one was a
        // texture copy; nothing fell back to memory on the way.
        assert_eq!(run.memory_frames, u64::from(CPU_FRAMES), "{who}");
        assert!(
            run.texture_frames + run.dropped == u64::from(TEXTURE_FRAMES) && run.dropped <= 2,
            "{who}: texture frames {} dropped {}",
            run.texture_frames,
            run.dropped
        );
        assert_ne!(run.flags & 2048, 0, "{who}: no texture frame reached the MFT");
        // An async MFT still holds its last few inputs when the loop stops.
        assert!(
            run.encoded_frames + 4 >= CPU_FRAMES + TEXTURE_FRAMES,
            "{who}: encoded {}",
            run.encoded_frames
        );
        assert!(run.key_frames >= 1, "{who}");
        assert_eq!(run.active_after_release, 0, "{who}: texture input must be withdrawn on release");
        // A texture-mode runtime failure hands the share back to the classic
        // hardware encoder, not to software, and texture input stays off for
        // that adapter afterwards.
        assert_eq!(run.fallback_error, 0, "{who}: the failure reached WebRTC as a fallback");
        assert_eq!(run.fallback_stage, 12, "{who}: stage after the forced failure");
        // E_FAIL, what the forced failure reports.
        assert_eq!(run.fallback_hr, 0x8000_4005, "{who}: hr after the forced failure");
        assert!(!run.fallback_native_handle, "{who}: still claims native frames");
        assert_eq!(run.fallback_active_luid, 0, "{who}: still advertises texture input");
        assert!(run.fallback_latched, "{who}: adapter not latched as failed");
        assert!(run.fallback_memory_frames >= 15, "{who}: frames after the takeover {}", run.fallback_memory_frames);
        assert!(run.fallback_encoded >= 12, "{who}: encoded after the takeover {}", run.fallback_encoded);
        assert_eq!(run.latched_init, 0, "{who}: second encoder init");
        assert!(!run.latched_native_handle, "{who}: second encoder retried texture input");
        assert_eq!(run.latched_stage, 12, "{who}: second encoder stage");
        assert_eq!(run.latched_hr, run.fallback_hr, "{who}: second encoder lost the failure's hr");
        assert!(run.readback_ok, "{who}: ToI420 read-back did not reproduce the texture");
        assert!(run.crop_ok, "{who}: CropAndScale did not read back and scale");
        assert!(run.validation_ok, "{who}: Create() accepted a texture it must refuse");
    }
    assert!(adapters > 0, "no adapter with a hardware H.264 MFT");
}
