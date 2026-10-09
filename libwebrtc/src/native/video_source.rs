// Copyright 2025 LiveKit, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use std::{
    sync::{Arc, Weak},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use cxx::SharedPtr;
use livekit_runtime::interval;
use parking_lot::Mutex;
use webrtc_sys::{video_frame as vf_sys, video_frame::ffi::VideoRotation, video_track as vt_sys};

use crate::{
    video_frame::{I420Buffer, VideoBuffer, VideoFrame},
    video_source::VideoResolution,
};

impl From<vt_sys::ffi::VideoResolution> for VideoResolution {
    fn from(res: vt_sys::ffi::VideoResolution) -> Self {
        Self { width: res.width, height: res.height }
    }
}

impl From<VideoResolution> for vt_sys::ffi::VideoResolution {
    fn from(res: VideoResolution) -> Self {
        Self { width: res.width, height: res.height }
    }
}

#[derive(Clone)]
pub struct NativeVideoSource {
    sys_handle: SharedPtr<vt_sys::ffi::VideoTrackSource>,
    inner: Arc<Mutex<VideoSourceInner>>,
}

struct VideoSourceInner {
    captured_frames: usize,
}

async fn startup_keepalive(
    resolution: VideoResolution,
    state: Weak<Mutex<VideoSourceInner>>,
    sys_handle: SharedPtr<vt_sys::ffi::VideoTrackSource>,
) {
    // The task must not own the source: an abandoned share may never capture
    // its first frame. Backports #1271/#1400 while retaining our submission lock.
    if !state.upgrade().is_some_and(|state| state.lock().captured_frames == 0) {
        return;
    }
    let i420 = I420Buffer::new_black(resolution.width, resolution.height);
    let mut interval = interval(Duration::from_millis(100));
    loop {
        interval.tick().await;
        let Some(state) = state.upgrade() else { break };
        let inner = state.lock();
        if inner.captured_frames > 0 {
            break;
        }

        // Serialize the check and submission with capture_frame, so a black
        // keepalive cannot land after the first real picture.
        let mut builder = vf_sys::ffi::new_video_frame_builder();
        builder.pin_mut().set_rotation(VideoRotation::VideoRotation0);
        builder.pin_mut().set_video_frame_buffer(i420.as_ref().sys_handle());
        let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();
        builder.pin_mut().set_timestamp_us(now.as_micros() as i64);
        sys_handle.on_captured_frame(&builder.pin_mut().build());
    }
}

impl NativeVideoSource {
    pub fn new(resolution: VideoResolution, is_screencast: bool) -> NativeVideoSource {
        let source = Self {
            sys_handle: vt_sys::ffi::new_video_track_source(
                &vt_sys::ffi::VideoResolution::from(resolution.clone()),
                is_screencast,
            ),
            inner: Arc::new(Mutex::new(VideoSourceInner { captured_frames: 0 })),
        };

        livekit_runtime::spawn(startup_keepalive(
            resolution,
            Arc::downgrade(&source.inner),
            source.sys_handle.clone(),
        ));

        source
    }

    pub fn sys_handle(&self) -> SharedPtr<vt_sys::ffi::VideoTrackSource> {
        self.sys_handle.clone()
    }

    /// Returns `false` when WebRTC rejected the frame: its own adaptation
    /// (framerate or resolution) dropped it before the encoder ever saw it.
    /// Callers use this to tell WebRTC-side drops apart from encoder-side
    /// ones - without it, a starving stream and an adapting one look the same.
    pub fn capture_frame<T: AsRef<dyn VideoBuffer>>(&self, frame: &VideoFrame<T>) -> bool {
        let mut inner = self.inner.lock();
        inner.captured_frames += 1;

        let mut builder = vf_sys::ffi::new_video_frame_builder();
        builder.pin_mut().set_rotation(frame.rotation.into());
        builder.pin_mut().set_video_frame_buffer(frame.buffer.as_ref().sys_handle());

        if frame.timestamp_us == 0 {
            // If the timestamp is set to 0, default to now
            let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();
            builder.pin_mut().set_timestamp_us(now.as_micros() as i64);
        } else {
            builder.pin_mut().set_timestamp_us(frame.timestamp_us);
        }

        self.sys_handle.on_captured_frame(&builder.pin_mut().build())
    }

    pub fn video_resolution(&self) -> VideoResolution {
        self.sys_handle.video_resolution().into()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn startup_black_frame_initializes_all_planes_including_odd_sizes() {
        for (width, height) in [(2, 2), (17, 9), (320, 180)] {
            let buffer = I420Buffer::new_black(width, height);
            let (y, u, v) = buffer.data();
            assert!(y.iter().all(|&p| p == 0));
            assert!(u.iter().chain(v).all(|&p| p == 128));
            assert_eq!(y.len(), (width * height) as usize);
            assert_eq!(u.len(), (((width + 1) / 2) * ((height + 1) / 2)) as usize);
        }
    }

    #[tokio::test]
    async fn dropping_a_never_started_source_releases_capture_state() {
        let source = NativeVideoSource::new(VideoResolution { width: 16, height: 16 }, true);
        let state = Arc::downgrade(&source.inner);
        tokio::task::yield_now().await;
        drop(source);
        assert!(state.upgrade().is_none());
    }

    #[tokio::test]
    async fn startup_task_finishes_after_capture_or_owner_drop() {
        for captured_frames in [0, 1] {
            let resolution = VideoResolution { width: 16, height: 16 };
            let state = Arc::new(Mutex::new(VideoSourceInner { captured_frames }));
            let handle = vt_sys::ffi::new_video_track_source(&resolution.clone().into(), true);
            let task = startup_keepalive(resolution, Arc::downgrade(&state), handle);
            if captured_frames == 0 {
                drop(state);
            }
            tokio::time::timeout(Duration::from_secs(1), task)
                .await
                .expect("startup keepalive retained a finished source");
        }
    }
}
