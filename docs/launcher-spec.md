# Kage launcher — implementation brief

## Product contract

- Boot directly into an English bubble launcher in landscape.
- Keep every primary target at least 76 px and avoid corner-only navigation.
- Put Robot in the centre, with Microphone, Motion, Display, System, Wi-Fi and
  Storage around it.
- Open an app with one tap. Swipe up anywhere in an app to return home. Swipe
  left or right on Home to move the magnetic selection.
- Keep Robot to two eyes and one mouth, all solid cyan on true black.
- Preserve random gaze, asymmetric natural blink timing, sleep/wake, tap blink,
  filtered shake/dizzy and automatic left/right landscape rotation.
- Make the microphone test react to real ES8311 audio amplitude.

## Performance contract

- Use native LVGL objects and invalidate only their old/new bounds.
- Never redraw a full-screen canvas for eyes, mouth, bubbles or microphone orb.
- Update the face and microphone at 30 fps; update sensor text at 12.5 fps.
- Keep sensor/audio workers away from the LVGL task and exchange only small
  atomic values.
- Use solid fills; no gradients, blur shaders or large animated shadows.

## Inspiration, adapted rather than embedded

- Bencho Magnetic Select: regular cluster, selected bubble swelling, neighbours
  pushed radially, damped frame-rate-independent spring.
- SmoothUI AI Orb Face: expression-driven AI states and natural blink cadence.
- thinking-orbs: small dotted listening indicator driven by audio state.
- Amicro and Inspora: restrained micro-transitions and clear interaction focus.

The referenced Web components are not shipped on the microcontroller. Their
interaction principles are recreated with LVGL and the Waveshare BSP.

