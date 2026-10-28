# sipwire web UI simulator

Renders `components/ui` (unchanged, globbed from `components/ui/*.c`) with
LVGL 9.6.0 compiled to WebAssembly (Emscripten + SDL2) on a 240x320 canvas. A
mock phone (`src/sim_main.c`) owns the `ui_model_t` and implements
`ui_actions_t`; an HTML side panel injects network/registration/battery/call
events and keypad presses. No flashing, rebuild takes about a second.

## Setup (once)

Needs Emscripten, CMake, Ninja, Python 3.

```sh
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
export EMSDK=~/emsdk          # hint for build.sh; or: source ~/emsdk/emsdk_env.sh
```

`build.sh` uses `emcmake` from `PATH`; if it is missing and `EMSDK` is set, it
sources `$EMSDK/emsdk_env.sh` itself. The first build downloads LVGL v9.6.0
(CMake `FetchContent`) and Emscripten builds its SDL2 port (takes ~1 min).

## Build / serve / watch

```sh
sim/web/build.sh              # -> sim/web/build/index.{html,js,wasm}
sim/web/serve.sh              # http://localhost:8080  (PORT=9000 to change)
sim/web/serve.sh --watch      # also rebuild on changes to components/ui/**,
                              # sim/web/src/**, shell.html, lv_conf.h
```

Reload the page after a rebuild. Watch mode polls mtimes once a second (no
inotify). Incremental rebuild after touching `components/ui/ui.c`: ~1.5 s
(only that file and the final link; LVGL is built once).

`sim/web/lv_conf.h` mirrors the rendering-relevant firmware settings from
`sdkconfig.defaults` (RGB565, Montserrat 14 + 24, default font 14, log level
warn). Keep them in sync. Note that LVGL 9.6 deprecates `LV_COLOR_DEPTH`
(and Kconfig `LV_COLOR_DEPTH_16`) in favour of `LV_COLOR_FORMAT_DEFAULT`.

## Screenshots (agents / CI)

```sh
node sim/web/screenshot.mjs out.png [idle|registered|incoming|dialing|active]
```

Serves `sim/web/build/` on a random local port, loads it in headless Chromium
(Playwright, resolved from the global `npm root -g` if not found locally),
waits for the runtime, applies the scenario, waits 300 ms and screenshots only
the canvas at native 240x320. Exits non-zero on page errors or
`console.error`. The final model is printed as JSON.

### Adding a scenario

Add an entry to `SCENARIOS` in `screenshot.mjs`. It is a function of
`window.sim` and runs in the page:

```js
muted_call: (sim) => {
    sim.set_net(2, -55); sim.set_reg(2); sim.set_time('09:41');
    sim.set_call(4, '1001', 'Bob Example', 125);   // 4 = ACTIVE
},
```

`window.sim` wraps the exported C functions in `src/sim_main.c`:
`set_net(state, rssi)`, `set_reg(state)`, `set_battery(pct, charging)`,
`incoming(number, name)`, `remote_hangup(code)`, `set_call(state, number,
name, seconds)` (jump to a call state without the mock's timers),
`key(char, action)` (0 press, 1 long, 2 repeat), `set_time('HH:MM')` and
`model()`. Enum values follow `components/ui/include/ui_model.h`. Anything
else the screen needs (new model fields) should get a new `sim_*` function
there, added to `SIM_EXPORTS` in `CMakeLists.txt` and wrapped in `shell.html`.

## Side panel

Keypad (click = PRESS on release, hold 600 ms = LONG; keyboard 0-9 * # work
too), network state + RSSI, registration state, battery + charging, clock,
incoming call (number/name), remote hangup (with SIP code), action log (every
`ui_actions_t` call and mock event) and a live JSON view of the model. Mouse
clicks on the canvas are touch input.

Mock call flow: `dial` -> DIALING -> 1 s -> RINGING_OUT -> 2 s -> ACTIVE (timer
ticks); `hangup` -> ENDED -> 1.5 s -> IDLE; `answer` from RINGING_IN -> ACTIVE.

## Troubleshooting

- `failed to download port "sdl2"` (403): Emscripten fetches SDL2 as a GitHub
  archive zip. If your network blocks that, clone SDL and use Emscripten's
  local-port override for every build:
  `git clone --depth 1 --branch release-2.32.10 https://github.com/libsdl-org/SDL ~/SDL`
  `export EMCC_LOCAL_PORTS="sdl2=$HOME/SDL"` (the tag must match your emsdk's
  SDL2 port version; see `upstream/emscripten/tools/ports/sdl2.py`).
- Blank canvas in a headless/VM browser: WebGL is required (SDL's renderer);
  `screenshot.mjs` launches Chromium with SwiftShader for this.
