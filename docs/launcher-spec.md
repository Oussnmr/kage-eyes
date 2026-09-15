# Kage launcher — implementation brief

## Product contract

- Boot directly into an English bubble launcher in landscape.
- Keep every primary target at least 76 px and avoid corner-only navigation.
- Put Robot in the centre, with Microphone, Motion, Display, System, Wi-Fi and
  Storage around it.
- Open an app with one tap. Swipe up anywhere in an app to return home. Swipe
  left or right on Home to move the magnetic selection.
- Keep Home icon-only: no brand, app names or instructions. Give each bubble
  its own solid colour and use the original built-in LVGL app icons.
- Keep Robot to two eyes and one mouth on true black. Preserve random gaze,
  natural blinks, sleep/wake, tap blink, shake/dizzy and left/right rotation.
- Add curious/happy idle poses, rising sleep marks, a five-tap angry state and
  a green charge reaction. Tap or shake leaves the angry state.
- Make a filled, organic grain orb react to real ES8311 audio amplitude.
- Expose read-only AXP2101 battery data and resting-pose motion calibration.

## Performance contract

- Use native LVGL objects and invalidate only their old/new bounds.
- Never redraw a full-screen canvas for eyes, mouth, bubbles or microphone orb.
- Update the face and microphone at 30 fps; update sensor text at 12.5 fps.
- Keep sensor/audio workers away from the LVGL task and exchange only small
  atomic values.
- Use solid fills; no gradients, blur shaders or large animated shadows.
- Never transform the large eye objects; animate only their bounds. Rotating a
  132 px LVGL object can allocate a costly intermediate layer on this target.

## Inspiration, adapted rather than embedded

- Bencho Magnetic Select: regular cluster, selected bubble swelling, neighbours
  pushed radially, damped frame-rate-independent spring.
- SmoothUI AI Orb Face: expression-driven AI states and natural blink cadence.
- thinking-orbs and the supplied Waitstate grain example: a living filled
  particle volume driven by audio, rather than fixed concentric rings.
- Amicro and Inspora: restrained micro-transitions and clear interaction focus.

The referenced Web components are not shipped on the microcontroller. Their
interaction principles are recreated with LVGL and the Waveshare BSP.

