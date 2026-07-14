# LVGL and ESP-SR Concurrency Demo

This standalone example makes the rendering effect of the Matter touchscreen controller's audio frontend visible without Matter, networking, playback, encoding, or a cloud-agent backend. ESP Board Manager owns display, touch, microphone, and PSRAM policy. LVGL runs on core 1; AFE and pipeline configuration follows that frontend.

The left two-thirds of the landscape display contains a time-based hue workload and twelve manually scrollable cards. The hue changes are requested by a 33 ms LVGL timer and complete a cycle every two seconds, so missed rendering appears as a jump rather than a slower animation. The right pane reports ESP LVGL Adapter completed-frame FPS and the boot-lifetime WakeNet count.

FPS is the number of LVGL frames whose final flush completed during the adapter's latest one-second window. It is not physical panel scan-out frequency. Compare stopped, paused, and running states on the same board; do not compare absolute FPS between unlike displays.

## ESP32-S31 Korvo-1

Start from a clean generated state so target and board defaults cannot leak from another build:

```sh
rm -rf build components/gen_bmgr_codes managed_components sdkconfig
idf.py --preview set-target esp32s31
idf.py bmgr -b esp32_s31_korvo_1
idf.py build
```

Board Manager configures the native 800x480 RGB display, aligned touch input, ES8389 microphone ADC, flash, and PSRAM. The application initializes only `audio_adc` and opens it at 16 kHz, 16-bit, two-channel input with AFE format `MM`. The Board Manager device remains initialized for the application lifetime because deleting and recreating its shared TDM channel is not restart-safe; Stop still closes codec capture.

The shared defaults enable the same frontend feature set as the Matter touchscreen controller: high-performance AFE allocation, one WakeNet9 `Hi, ESP` model, VAD mode 3, AGC, and completed-frame FPS statistics. AEC and speech enhancement are disabled. No DAC is initialized by application policy. Keeping one WakeNet model is required for real-time S31 processing; loading two made `afe_fetch` fall behind capture.

## Controls

| State | Start | Pause | Resume | Stop | Resources |
| --- | --- | --- | --- | --- | --- |
| Stopped | Enabled | Disabled | Disabled | Disabled | SR resources absent; microphone capture closed; board device retained |
| Starting | Disabled | Disabled | Disabled | Disabled | Resources being created asynchronously |
| Running | Disabled | Enabled | Disabled | Enabled | Microphone and AFE processing active |
| Paused | Disabled | Disabled | Enabled | Enabled | Models, AFE manager, pipeline, tasks, and buffers retained; capture quiescent |
| Stopping | Disabled | Disabled | Disabled | Disabled | Resources being released asynchronously |

Start and Stop may take time because models and AFE resources are created or destroyed. Button callbacks only enqueue intents, so hue animation and scrolling remain on the LVGL task. Lifecycle errors are reported on the serial console rather than in the UI. Wake count is ordinary RAM, survives every lifecycle transition, and resets on reboot.

Pause first pauses the GMF pipeline and then suspends its AFE manager; Resume re-enables the manager before resuming the pipeline. Stop works from Running or Paused by re-enabling a suspended manager, stopping and destroying the pipeline, deinitializing its task, then releasing the pool, AFE manager, models, and codec capture. The Board Manager ADC allocation is intentionally retained so Start can reopen the same channel safely.

## User-Run Validation

The implementation agent does not flash or monitor hardware. After a successful build, the user can run `idf.py flash monitor` and check:

- The display is landscape 800x480 and touch is aligned at all four controls and across the card rail.
- Twelve cards support manual drag and kinetic horizontal scrolling while the hue keeps cycling.
- Completed FPS updates about once per second without resetting across lifecycle transitions.
- Boot starts with only Start enabled; Running enables Pause and Stop; Paused enables Resume and Stop.
- Start and Stop do not freeze hue or scrolling, and rapid invalid taps do not overlap lifecycle work.
- `Hi, ESP` increments the wake count only while Running; the count survives Pause, Stop, and restart.
- Repeated Start, Pause, Resume, and Stop cycles return to Start-only state without accumulating tasks, codec opens, callbacks, or heap loss.
- Where external runtime task diagnostics are available, verify LVGL on core 1 and the Matter frontend's AFE and pipeline tasks on core 0.
- Compare hue continuity, touch latency, scrolling, and FPS in Stopped, Paused, and Running states on this board.
