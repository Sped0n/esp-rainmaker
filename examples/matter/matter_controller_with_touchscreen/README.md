# RainMaker Matter Controller With Touchscreen Example

This example runs the controller example with a touchscreen UI. For the shared controller flow and RainMaker CLI usage, see the [Matter controller README](../matter_controller/README.md).

## Touchscreen UI

- This demo supports Matter On/Off devices only.
- Commission the controller by scanning the QR code shown on the screen with the ESP RainMaker app.
- After commissioning, the controller shows supported On/Off devices in the same fabric.
- Tap an on-screen device card to control an On/Off device locally.
- Use the Reset button in the `About Us` page to factory reset the controller and recommission it.

## ESP Agents

- A gray waveform in the status bar means the agent is ready. It turns blue while connecting and animates during an active conversation.
- Say "Hi, ESP" or tap the waveform icon to activate the agent. Start speaking after you hear the chime.
- Tap the blue waveform to stop the conversation; a stop cue confirms that the session has ended.
- Voice interaction supports English only.
- To test Matter control, say "Turn on all the lights" or "Turn off all the lights."

## Setup And Build

Run these commands from this example directory after setting up ESP-IDF (`v6.1-beta1-592-gcadfa9c0969`, or HEAD from https://github.com/espressif/esp-idf/tree/release/v6.1),
ESP-Matter (`9e2f6f01`):

```bash
idf.py --preview set-target esp32s31
idf.py bmgr -b esp32_s31_korvo_1
idf.py build
```

## OpenThread Border Router

Thread Border Router support requires a compatible RCP and board-specific UART
wiring. See the `OpenThread Border Router` section in the
[Matter controller README](../matter_controller/README.md).
