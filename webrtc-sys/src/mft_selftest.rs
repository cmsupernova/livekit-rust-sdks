// Hardware test of MFT texture input, built only with the `mft-selftest`
// feature on Windows. See tests/mft_d3d.rs.

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    /// What one run of the real MFT encoder saw with texture input offered on
    /// one adapter that has a hardware H.264 MFT.
    #[derive(Debug)]
    pub struct MftD3dSelfTest {
        pub mft_count: u32,
        pub adapter_luid: u64,
        pub vendor_id: u32,
        pub encoder_name: String,
        pub init_result: i32,
        pub d3d_stage: u32,
        pub d3d_hr: u32,
        pub mft_stage: u32,
        pub mft_hr: u32,
        pub flags: u32,
        pub native_handle: bool,
        pub active_luid: u64,
        pub first_error: i32,
        pub encoded_frames: u32,
        pub key_frames: u32,
        pub texture_frames: u64,
        pub memory_frames: u64,
        pub dropped: u64,
        pub readback_ok: bool,
        pub active_after_release: u64,
    }

    unsafe extern "C++" {
        include!("livekit/mft_d3d_selftest.h");

        /// Encodes `cpu_frames` system-memory NV12 frames, then
        /// `texture_frames` keyed-mutex NV12 textures, through the real MFT
        /// encoder on the `adapter_ordinal`-th adapter with a hardware H.264
        /// MFT (mft_count 0 when there is none), and reads one texture back
        /// through ToI420().
        fn mft_d3d_selftest(
            adapter_ordinal: u32,
            width: u32,
            height: u32,
            cpu_frames: u32,
            texture_frames: u32,
        ) -> MftD3dSelfTest;
    }
}
