// Runs the production MFT encoder through live bitrate changes on every
// hardware H.264 MFT Windows lists on this machine, from system memory. Needs
// real hardware, so it lives behind a feature:
//   cargo test -p webrtc-sys --features mft-selftest --test mft_rate -- --nocapture
#![cfg(all(feature = "mft-selftest", target_os = "windows"))]

use webrtc_sys::mft_selftest::ffi::{mft_rate_selftest, MftRatePhase, MftRateSelfTest};

/// MftDiagCounters flags.
const VBR: u32 = 131072;
const REBUILDS: u32 = 262144;

/// A 1440p60 share's ceiling.
const CEILING_KBPS: u32 = 22000;

fn report(run: &MftRateSelfTest, what: &str) {
    println!("== {} [{what}] init {}", run.encoder_name, run.init_result);
    for p in &run.phases {
        println!(
            "{:<10}{:>6} kbps @{:>2}  stage {:>2}->{:>2}  flags {:#07x}->{:#07x}  rate {}/{} -> {}/{}  cap {}/{} -> {}/{}  frames {:>3} keys {:>2}  late avg {:>6}  delta max {:>6}  key max {:>6}  encode max {:>3} ms  total {}",
            p.name,
            p.target_kbps,
            p.fps,
            p.stage,
            p.end_stage,
            p.flags,
            p.end_flags,
            p.rate_asked,
            p.rate_reported,
            p.end_rate_asked,
            p.end_rate_reported,
            p.hrd_asked,
            p.hrd_reported,
            p.end_hrd_asked,
            p.end_hrd_reported,
            p.frames,
            p.key_frames,
            p.late_avg_bytes,
            p.delta_max_bytes,
            p.key_max_bytes,
            p.encode_max_ms,
            p.total_bytes,
        );
    }
}

/// What a frame may cost at `kbps` when the encoder budgets 60 frames a
/// second.
fn budget(kbps: u32) -> u32 {
    kbps * 1000 / 8 / 60
}

fn check(run: &MftRateSelfTest, what: &str) {
    let who = format!("{} [{what}]", run.encoder_name);
    assert_eq!(run.init_result, 0, "{who}: encoder init");
    let phase = |name: &str| -> &MftRatePhase { run.phases.iter().find(|p| p.name == name).unwrap() };
    for p in &run.phases {
        assert_eq!(p.first_error, 0, "{who} {}: an Encode call failed", p.name);
        assert_eq!(p.end_encoder, run.encoder_name, "{who} {}: another encoder took over", p.name);
        assert!(p.frames + 12 >= 28, "{who} {}: only {} frames came out", p.name, p.frames);
        assert_eq!(p.end_stage, 10, "{who} {}: the stage it ended at", p.name);
    }

    // Content that always wants more bits costs what the target allows, up
    // and down. This is the whole point: an encoder left at its opening rate
    // spends about 2 KB a frame through all of it.
    let at = |name: &str, kbps: u32| {
        let got = phase(name).late_avg_bytes;
        let want = budget(kbps);
        assert!(
            got * 10 >= want * 6 && got * 10 <= want * 14,
            "{who} {name}: frames of {got} bytes, {want} at the target"
        );
    };
    at("up-8M", 8000);
    at("up-20M", 20000);
    at("down-2M", 2000);
    at("back-8M", 8000);

    // A cap is never under two frames of the rate in force: that is what
    // held a share to its opening bitrate.
    for p in &run.phases {
        if p.end_hrd_asked == 0 {
            continue;
        }
        let cap = if p.end_hrd_reported != 0 { p.end_hrd_reported } else { p.end_hrd_asked };
        let rate_kbps = p.end_rate_asked / 1000;
        assert!(
            cap + cap / 10 >= 2 * budget(rate_kbps),
            "{who} {}: a cap of {cap} bytes under {rate_kbps} kbps",
            p.name
        );
    }

    // Moving the target every 100 ms neither breaks the encoder nor turns
    // every change into a keyframe: the periodic ones a couple of seconds
    // apart, and a rebuild's no more than once a second, where a keyframe per
    // change is one frame in six.
    let wobble = phase("wobble-8M");
    assert!(
        wobble.key_frames <= wobble.frames / 20 + 1,
        "{who}: {} keyframes in {} frames of wobble",
        wobble.key_frames,
        wobble.frames
    );
}

#[test]
fn frame_sizes_follow_the_live_target() {
    let mut found = 0;
    for candidate in 0..4 {
        let run = mft_rate_selftest(candidate, false, CEILING_KBPS, 1, 1920, 1080);
        if run.init_result != 0 {
            break;
        }
        found += 1;
        report(&run, "as detected");
        check(&run, "as detected");
        let intel = run.encoder_name.contains("Intel");
        if intel {
            // Live under VBR, with no cap of ours.
            let last = run.phases.last().unwrap();
            assert_ne!(last.end_flags & VBR, 0, "Intel: not in VBR");
            assert_eq!(last.end_flags & REBUILDS, 0, "Intel: rates went through rebuilds");
            assert_eq!(last.end_hrd_asked, 0, "Intel: a cap was set");
            // Its own buffer keeps a keyframe near the rate at the low end.
            let low = run.phases.iter().find(|p| p.name == "down-2M").unwrap();
            assert!(low.key_max_bytes <= 12 * budget(2000), "Intel: a {} byte keyframe at 2 Mbps", low.key_max_bytes);
        }

        // The same MFT as an unknown vendor's: CBR and a cap sized for the
        // ceiling. Whatever it does with a live rate, the target is
        // followed, by opening the encoder again if it has to be.
        let other = mft_rate_selftest(candidate, true, CEILING_KBPS, 3, 1920, 1080);
        report(&other, "as an unknown vendor");
        check(&other, "as an unknown vendor");
        if intel {
            // Intel ignores a live rate under CBR.
            let last = other.phases.last().unwrap();
            assert_ne!(last.end_flags & REBUILDS, 0, "Intel under CBR: no rebuilds");
            continue;
        }

        // And opened under a ceiling the targets then climb past, so the
        // cap has to grow: live where the MFT resizes it, by opening again
        // where it does not.
        let low = mft_rate_selftest(candidate, true, 4000, 3, 1920, 1080);
        report(&low, "a 4 Mbps ceiling");
        check(&low, "a 4 Mbps ceiling");
    }
    assert!(found > 0, "no hardware H.264 MFT");
}
