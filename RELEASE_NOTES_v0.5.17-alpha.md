## v0.5.17-alpha — Refractor flight paradigms (research pass)

**Download `ProjectDalian-v0.5.17-alpha-win64.zip`**

Closes the remaining implementable gaps from the Battlefield 2 Flight Mechanics research blueprint. This is **not** a full Refractor Wing/RotationalBundle rewrite — it is the core input + aerodynamics stack the doc’s §8 diagnostic checklist calls for.

### Flight (vs research doc)

| Paradigm | Status |
| :---- | :---- |
| Virtual joystick (−1…1), buffered mouse → fixed tick | Done |
| Fixed-step physics (decoupled from render) | Done (60 Hz; retail was ~30 Hz) |
| Jet `setAutomaticReset` stick recenter | Done |
| Heli rotor AoA → tilted thrust → body follow | Done |
| Heli horizon / horizontal-vel damp + AirFlowEffect yaw | Done |
| Jet WingLift / FlapLift / RegulateToLift, drag & gravity modifiers | Done |
| Wing `PositionOffset` elevator/aileron torque arms | Done |
| Landing gear spring / damping on touchdown | Done |
| Mouse-wheel throttle (`c_PIThrottle` analog) | Done |
| Full Wing + RotationalBundle `setInputToPitch` force reorientation | **Not** — roll/pitch sticks map directly |
| col0 / col1 / col2 collision mesh selection | **Not** |
| Exact retail 30 Hz Euler tick | **Not** (fixed 60 Hz) |

### Controls

- Jets / helis: mouse = virtual stick; A/D = rudder; W/S = throttle/collective; **mouse wheel** = throttle notches.
- Jets: stick auto-centers when mouse stops (`AutomaticReset`). Helis hold cyclic.

### Includes

- Hawk mortar SAM path (v0.5.16)
- LDR / Compat Mode / HDR capture notes (v0.5.13–14)
