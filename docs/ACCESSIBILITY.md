# Accessibility

What Elsewhere supports for people using a screen reader, the keyboard, or the system's display
settings, and what isn't there yet. If something here doesn't work for you, please open an issue.

## Screen readers

Elsewhere's controls are named for screen readers through JUCE's accessibility support: VoiceOver on
macOS, and Narrator and NVDA on Windows (through UI Automation).

- **Transport**: Play, Stop, Record and Loop say whether they're on. Tempo is an adjustable value
  ("120.00 BPM"). The metronome and count-in are separate keys. Starting and stopping playback or
  recording is announced, however it happened.
- **Tracks**: each track header is a group named after its track, with its kind and colour
  ("Bass, track, Ember"). Inside it are Mute, Solo, Record Arm, the automation and take lanes, and
  Volume and Pan, which read their value ("-6.0 dB", "Centre") and step with the screen reader's
  increment and decrement.
- **Mixer**: each strip is a group named after its track, with its input, output, inserts (a bypassed
  insert says so), Volume, Pan, sends ("Send to Return A, pre-fader"), Mute, Solo and Record Arm.
- **Devices**: each device in the device panel is a group named after it, with an on/off key and its
  knobs, which read the parameter's own text ("1.2 kHz").
- **Session**: each slot says its clip's name (or "Empty slot") and what it's doing (playing, queued,
  stopped, recording). Pressing it does what clicking its key does: launch the clip, record into an
  armed track, or stop the track.
- **Live sessions**: the Live panel's controls and explanation are readable, each player is a group
  with their connection details, volume and mute, and players joining or leaving are announced.
- **Somewhere**: a phone connecting or disconnecting is announced.
- Projects open and save through the menu bar and the system's own file dialogs.

## Keyboard

The single-key shortcuts (Space to play, Tab to switch views, and so on) work while no control has
focus. To use the controls themselves:

| Key | Action |
|---|---|
| F6 / Shift+F6 | Move focus to the controls in the next / previous part of the window: transport, browser, track list, arrangement or session, mixer, clips and markers, device panel |
| Tab / Shift+Tab | Next / previous control in that part (wraps round) |
| Return | Press the focused key, or open the focused menu |
| Arrow keys | Change the focused knob, fader or menu (Shift: in finer steps) |
| Escape | Leave the controls and go back to the shortcuts |
| Up / Down | Select the track above / below (when no control has focus) |
| Alt+M, Alt+S, Alt+R | Mute, solo, arm the selected track |

A ring shows which control has focus while you're moving through them with the keyboard. Clicking
with the mouse hides it. While a screen reader is running, Tab always moves between controls.

Every command in the menus has its shortcut listed there; the menus are the native ones on macOS.

## Display

- **Contrast**: text reads at 4.5:1 or more against every surface in both themes, and the marks that
  carry meaning (levels, knob pointers, outlines) at 3:1 or more.
- **Increase Contrast**: turned on by Increase Contrast in macOS's Accessibility settings, a high
  contrast theme on Windows, or Options > Appearance > Increase Contrast. Dim text deepens to 7:1,
  and keys, knobs, menus and fields get outlines.
- **Reduce Motion**: with Reduce Motion on (or animations off on Windows), queued Session clips show a
  steady ring instead of flashing, and recording slots stop pulsing. Meters and the playhead still
  move.
- **Not by colour alone**: mute, solo and arm are marked keys (m, s, a dot) that fill when on; a bypassed insert
  is struck through; a pre-fader send underlines its letter; connection status is written out.

## Not there yet

- Clips in the arrangement and notes in the piano roll can't be reached with a screen reader or the
  keyboard yet; editing them needs the mouse.
- The Session grid can be read and launched with a screen reader, but not moved through with the
  arrow keys.
- The track list's rows aren't named for screen readers yet, and the browser and comments panel
  haven't been checked with one.
- There's no interface size setting yet: text follows the system's display scaling only.
- Windows' high contrast themes give Elsewhere's own Increase Contrast look, not the system colours.
- Plug-in windows are the plug-in's own; how accessible they are depends on the plug-in.
