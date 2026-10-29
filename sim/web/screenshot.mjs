#!/usr/bin/env node
// Headless screenshot of the simulator canvas (native 240x320, not zoomed).
//
//   node sim/web/screenshot.mjs out.png [scenario]
//   node sim/web/screenshot.mjs --all outdir        # every scenario -> outdir/<name>.png
//
// Scenarios live in SCENARIOS below; each one is a function that drives the
// page through window.sim.* (thin wrappers over the exported sim_* C functions).
// Enum values follow components/ui/include/ui_model.h: net OFF/CONNECTING/
// CONNECTED = 0/1/2, reg NONE/REGISTERING/REGISTERED/FAILED = 0..3, call IDLE/
// DIALING/RINGING_OUT/RINGING_IN/ACTIVE/HELD/ENDED = 0..6.
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { execSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const buildDir = path.join(here, 'build');

// Add scenarios here. `sim` is window.sim inside the page; the function is
// serialised and run in the browser, so it cannot capture Node variables.
const SCENARIOS = {
  idle: () => {},
  registered: (sim) => {
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_battery(80, false);
    sim.set_time('12:34');
  },
  incoming: (sim) => {
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_battery(80, false);
    sim.set_time('12:34');
    sim.incoming('1001', 'Bob Example');
  },
  dialing: (sim) => {
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_battery(80, false);
    sim.set_time('12:34');
    sim.set_call(1, '1002', '', 0);   // 1 = DIALING, frozen (no mock timers)
  },
  active: (sim) => {
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_battery(80, false);
    sim.set_time('12:34');
    sim.set_call(4, '1001', 'Bob Example', 42);   // 4 = ACTIVE, 00:42
  },

  // ---- UI review set (node screenshot.mjs --all <dir>) ----
  idle_registered: (sim) => {
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_battery(80, false);
    sim.set_time('12:34');
  },
  offline: (sim) => {
    sim.set_net(0, 0);
    sim.set_reg(0);
    sim.set_battery(12, false);
  },
  dialer: (sim) => {
    sim.set_net(2, -62);
    sim.set_reg(2);
    sim.set_battery(55, true);
    sim.set_time('12:34');
    for (const k of '5551234') sim.key(k, 0);   // first digit jumps to the dialer
  },
  ringing_out: (sim) => {
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_time('12:34');
    sim.set_call(2, '1002', 'Carol Test', 0);   // 2 = RINGING_OUT
  },
  active_muted_keypad: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('12:34');
      sim.set_call(4, '1001', 'Bob Example', 125);
    },
    steps: async (page, click) => {
      await click(42, 167);    // "Mute" (round-trips through the mock model)
      await click(198, 167);   // "Keypad" button
      await page.evaluate(() => { for (const k of '123') window.sim.key(k, 0); });
    },
  },
  active_muted_speaker: {
    sim: (sim) => {
      sim.set_net(2, -48);
      sim.set_reg(2);
      sim.set_time('12:34');
      sim.set_call(4, '1001', 'Bob Example', 125);
    },
    steps: async (page, click) => {
      await click(42, 167);    // Mute
      await click(120, 167);   // Speaker
    },
  },
  held: (sim) => {
    sim.set_net(2, -70);
    sim.set_reg(2);
    sim.set_time('12:34');
    sim.set_call(5, '+4915112345678', '', 3725);   // 5 = HELD, >1h
  },
  ended_busy: (sim) => {
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_time('12:34');
    sim.set_call(2, '1002', '', 0);
    sim.remote_hangup(486);
  },
  settings: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('12:34');
    },
    steps: async (page, click) => {
      await click(178, 282);   // Home -> "Settings"
    },
  },
  settings_wifi_keyboard: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('12:34');
    },
    steps: async (page, click) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await click(120, 106);   // "Wi-Fi" row
      await page.waitForTimeout(100);
      await click(110, 120);   // SSID field -> keyboard
      // type "homewifi" on the on-screen keyboard (keys are ~20x37 px)
      const row1 = { w: 52, e: 72, i: 171, o: 190 };
      const row2 = { f: 108, h: 138 };
      const row3 = { m: 151 };
      for (const c of 'homewifi') {
        if (c in row1) await click(row1[c], 177);
        else if (c in row2) await click(row2[c], 214);
        else await click(row3[c], 251);
      }
    },
  },
  settings_about: {
    sim: (sim) => {
      sim.set_net(2, -67);
      sim.set_reg(1);
      sim.set_time('12:34');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await drag(120, 280, 120, 100);   // list scrolls to its end: About is the last row
      await click(120, 290);   // "About" row
    },
  },
  settings_account_port: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('12:34');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await click(120, 168);   // "SIP account" row
      await page.waitForTimeout(100);
      await click(110, 120);   // Server field -> keyboard
      for (const x of [48, 172, 210]) await click(x, 177);   // a few letters
      // drag the form up so the Port field is visible, then tap it (number pad)
      await drag(120, 150, 120, 100);   // ~50 px up
      await click(110, 140);            // Port field (number pad)
    },
  },
  // Forms open prefilled from the model (the sim mock has stored settings; passwords
  // only as "unchanged" placeholders).
  settings_wifi_prefilled: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('12:34');
    },
    steps: async (page, click) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await click(120, 106);   // "Wi-Fi" row
    },
  },
  settings_account_prefilled: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('12:34');
    },
    steps: async (page, click) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await click(120, 166);   // "SIP account" row
    },
  },
  // ---- 12/24 h clock, Time and Update screens ----
  idle_12h: (sim) => {
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_battery(80, false);
    sim.set_time('15:07');   // 12 h is the default: "3:07 PM"
  },
  idle_12h_wide: (sim) => {   // widest case: "12:59 PM"
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_battery(80, false);
    sim.set_time('12:59');
  },
  idle_24h: (sim) => {
    sim.set_net(2, -55);
    sim.set_reg(2);
    sim.set_battery(80, false);
    sim.set_clock24(true);
    sim.set_time('15:07');
  },
  settings_list_scrolled: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('15:07');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await drag(120, 280, 120, 100);   // scroll the list to its end
    },
  },
  settings_time: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('15:07');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await drag(120, 280, 120, 100);   // scroll the list to its end
      await click(120, 182);   // "Time" row
    },
  },
  settings_time_zones: {   // the zone dropdown open
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('15:07');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await drag(120, 280, 120, 100);   // scroll the list to its end
      await click(120, 182);   // "Time" row
      await page.waitForTimeout(100);
      await click(110, 190);   // time zone dropdown
      await page.waitForTimeout(200);
    },
  },
  settings_time_custom: {   // pick "Custom": the POSIX TZ field shows up
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('15:07');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await drag(120, 280, 120, 100);   // scroll the list to its end
      await click(120, 182);   // "Time" row
      await page.waitForTimeout(100);
      await click(110, 190);   // time zone dropdown
      await page.waitForTimeout(200);
      for (let i = 0; i < 4; i++) await drag(120, 150, 120, 20);   // scroll the option list to its end
      await click(110, 150);   // "Custom" (last option)
      await page.waitForTimeout(200);
    },
  },
  settings_update_idle: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('15:07');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await drag(120, 280, 120, 100);   // scroll the list to its end
      await click(120, 236);   // "Update" row
    },
  },
  settings_update_progress: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('15:07');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await drag(120, 280, 120, 100);   // scroll the list to its end
      await click(120, 236);   // "Update" row
      await page.waitForTimeout(100);
      await page.evaluate(() => window.sim.set_ota(1, 42, 'Downloading 42%'));
    },
  },
  settings_update_keyboard: {   // URL field focused: keyboard up, page still usable
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('15:07');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await drag(120, 280, 120, 100);   // scroll the list to its end
      await click(120, 236);   // "Update" row
      await page.waitForTimeout(100);
      await click(110, 170);   // URL field -> keyboard
    },
  },
  settings_update_failed: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('15:07');
    },
    steps: async (page, click, drag) => {
      await click(178, 282);
      await page.waitForTimeout(250);
      await drag(120, 280, 120, 100);   // scroll the list to its end
      await click(120, 236);
      await page.waitForTimeout(100);
      await page.evaluate(() => window.sim.set_ota(3, 0, 'Wrong firmware: otherapp'));
    },
  },
  // ---- BLF panel / voicemail (state: 0 unknown, 1 idle, 2 ringing, 3 busy) ----
  idle_blf1: (sim) => {
    sim.set_net(2, -55); sim.set_reg(2); sim.set_battery(80, false); sim.set_time('12:34');
    sim.set_blf(0, '1002', 'Bob', 1);
  },
  idle_blf2: (sim) => {
    sim.set_net(2, -55); sim.set_reg(2); sim.set_battery(80, false); sim.set_time('12:34');
    sim.set_blf(0, '1002', 'Bob', 1);
    sim.set_blf(1, '1003', 'Front desk', 3);
  },
  idle_blf3: (sim) => {
    sim.set_net(2, -55); sim.set_reg(2); sim.set_battery(80, false); sim.set_time('12:34');
    sim.set_blf(0, '1002', 'Bob', 1);
    sim.set_blf(1, '1003', 'Front desk', 3);
    sim.set_blf(2, '1004', '', 0);
  },
  idle_blf5_mwi: (sim) => {
    sim.set_net(2, -55); sim.set_reg(2); sim.set_battery(80, false); sim.set_time('15:07');
    const keys = [['1002', 'Bob', 1], ['1003', 'Front desk', 3], ['1004', '', 2], ['1005', 'Carol', 1],
                  ['1006', 'Dave', 0]];
    keys.forEach((k, i) => sim.set_blf(i, k[0], k[1], k[2]));
    sim.set_mwi(1, 2, 5);
  },
  idle_blf8: (sim) => {
    sim.set_net(2, -55); sim.set_reg(2); sim.set_battery(80, false); sim.set_time('12:34');
    const keys = [['1002', 'Bob', 1], ['1003', 'Front desk', 3], ['1004', '', 1], ['1005', 'Carol', 1],
                  ['1006', 'Dave', 0], ['1007', 'Reception', 3], ['1008', 'Eve', 1], ['+4915112345678', '', 1]];
    keys.forEach((k, i) => sim.set_blf(i, k[0], k[1], k[2]));
  },
  idle_mwi: (sim) => {
    sim.set_net(2, -55); sim.set_reg(2); sim.set_battery(80, false); sim.set_time('12:34');
    sim.set_mwi(1, 3, 1);
  },
  idle_blf8_mwi_ringing: (sim) => {
    sim.set_net(2, -61); sim.set_reg(2); sim.set_time('12:59');
    const keys = [['1002', 'Bob', 2], ['1003', 'Front desk', 3], ['1004', '', 1], ['1005', 'Carol', 2],
                  ['1006', 'Dave', 0], ['1007', 'Reception', 3], ['1008', 'Eve', 1], ['1009', 'Lab', 1]];
    keys.forEach((k, i) => sim.set_blf(i, k[0], k[1], k[2]));
    sim.set_mwi(1, 12, 4);
  },
  idle_blf8_offline: (sim) => {
    sim.set_net(0, 0); sim.set_reg(3);
    const keys = [['1002', 'Bob', 0], ['1003', 'Front desk', 0], ['1004', '', 0], ['1005', 'Carol', 0],
                  ['1006', 'Dave', 0], ['1007', 'Reception', 0], ['1008', 'Eve', 0], ['1009', 'Lab', 0]];
    keys.forEach((k, i) => sim.set_blf(i, k[0], k[1], k[2]));
  },
  blf_tap_dials: {   // tapping a tile dials its extension
    sim: (sim) => {
      sim.set_net(2, -55); sim.set_reg(2); sim.set_time('12:34');
      sim.set_blf(0, '1002', 'Bob', 1);
      sim.set_blf(1, '1003', 'Front desk', 3);
      sim.set_blf(2, '1004', '', 0);
    },
    steps: async (page, click) => {
      await click(178, 191);   // second tile ("Front desk")
    },
  },
  // Settings > BLF keys, prefilled from the model (3 keys), then the dial pad for a new one
  settings_blf: {
    sim: (sim) => {
      sim.set_net(2, -55); sim.set_reg(2); sim.set_time('12:34');
      sim.set_blf(0, '1002', 'Bob', 1);
      sim.set_blf(1, '1003', 'Front desk', 3);
      sim.set_blf(2, '1004', '', 0);
    },
    steps: async (page, click) => {
      await click(178, 291);   // Home -> Settings (bottom row with BLF keys)
      await page.waitForTimeout(250);
      await click(120, 208);   // "BLF keys" row
    },
  },
  settings_blf_add_keypad: {   // dial pad (with * and #) for the extension
    sim: (sim) => {
      sim.set_net(2, -55); sim.set_reg(2); sim.set_time('12:34');
      sim.set_blf(0, '1002', 'Bob', 1);
    },
    steps: async (page, click) => {
      await click(178, 291);   // Home -> Settings
      await page.waitForTimeout(250);
      await click(120, 208);   // "BLF keys" row
      await page.waitForTimeout(100);
      await click(110, 172);   // "Add: extension" field -> dial pad
      await page.waitForTimeout(150);
      for (const [x, y] of [[26, 185], [72, 298], [72, 298], [121, 185]]) await click(x, y);   // "1003"
    },
  },
  // add "1005" to two keys, remove "1002", save: back home there are two tiles
  settings_blf_saved: {
    sim: (sim) => {
      sim.set_net(2, -55); sim.set_reg(2); sim.set_time('12:34');
      sim.set_blf(0, '1002', 'Bob', 1);
      sim.set_blf(1, '1003', 'Front desk', 3);
    },
    steps: async (page, click, drag) => {
      await click(178, 291);   // Home -> Settings
      await page.waitForTimeout(250);
      await click(120, 208);   // "BLF keys" row
      await page.waitForTimeout(100);
      await click(110, 238);   // "Add: extension" field -> dial pad
      await page.waitForTimeout(150);
      for (const [x, y] of [[26, 185], [72, 298], [72, 298], [72, 216]]) await click(x, y);   // "1005"
      await click(190, 216);   // OK: keyboard closes
      await page.waitForTimeout(150);
      await click(120, 248);   // "Add key"
      await page.waitForTimeout(100);
      await drag(120, 100, 120, 300);   // back to the top of the form
      await click(212, 122);   // trash on the first key (1002)
      await page.waitForTimeout(150);
      await click(202, 46);    // Save -> toast, back to the list
      await page.waitForTimeout(300);
      await click(26, 46);     // back home
      await page.waitForTimeout(300);
    },
  },
  calibrate_target1: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('12:34');
    },
    steps: async (page, click) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await click(120, 262);   // "Calibrate" row
      await page.waitForTimeout(400);
    },
  },
  // All four crosses touched (the page turns canvas mouse input into fake raw samples
  // of a panel rotated by 180 degrees), then one test touch: the red dot shows where
  // the new calibration puts it.
  calibrate_done: {
    sim: (sim) => {
      sim.set_net(2, -55);
      sim.set_reg(2);
      sim.set_time('12:34');
    },
    steps: async (page, click, drag, hold) => {
      await click(178, 282);   // Home -> Settings
      await page.waitForTimeout(250);
      await click(120, 262);   // "Calibrate" row
      await page.waitForTimeout(500);
      for (const [x, y] of [[20, 20], [219, 20], [219, 299], [20, 299]]) {
        await hold(x, y, 200);
        await page.waitForTimeout(450);   // the screen ignores samples briefly after a point
      }
      await hold(150, 120, 200);   // test touch; the dot stays where it was lifted
    },
  },
};

const args = process.argv.slice(2);
const all = args[0] === '--all';
const [out, scenarioName = 'idle'] = all ? [args[1]] : args;
if (!out || (!all && !SCENARIOS[scenarioName])) {
  console.error(`usage: node sim/web/screenshot.mjs out.png [${Object.keys(SCENARIOS).join('|')}]\n` +
                `       node sim/web/screenshot.mjs --all outdir`);
  process.exit(2);
}
if (!fs.existsSync(path.join(buildDir, 'index.html'))) {
  console.error('no build found; run sim/web/build.sh first');
  process.exit(2);
}

async function loadPlaywright() {
  try {
    return await import('playwright');
  } catch (e) {
    const root = execSync('npm root -g', { encoding: 'utf8' }).trim();
    return createRequire(path.join(root, 'x.js'))('playwright');
  }
}

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
  '.css': 'text/css', '.json': 'application/json', '.png': 'image/png',
};
const server = http.createServer((req, res) => {
  const p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = path.normalize(path.join(buildDir, p === '/' ? 'index.html' : p));
  if (!file.startsWith(buildDir + path.sep) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(p === '/favicon.ico' ? 204 : 404).end();
    return;
  }
  res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${server.address().port}/`;

const { chromium } = await loadPlaywright();
const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
let failed = false;

// A scenario is either a function of window.sim (runs in the page) or
// { sim: fn(sim), steps: async (page, click, drag, hold) => {...} } where `steps` runs
// in Node afterwards and may inject touch input with click(x, y), drag(x1, y1, x2, y2)
// and hold(x, y, ms) in canvas pixels.
async function runScenario(name, file) {
  const page = await browser.newPage({ viewport: { width: 1000, height: 760 } });
  try {
    page.on('pageerror', (e) => { failed = true; console.error('pageerror:', e.message); });
    page.on('console', (m) => {
      if (m.type() === 'error') { failed = true; console.error('console.error:', m.text()); }
    });

    await page.goto(url);
    await page.waitForFunction(() => window.simReady === true, null, { timeout: 30000 });

    // Native size for the screenshot: undo the 2x CSS zoom.
    await page.addStyleTag({ content: '#canvas{width:240px!important;height:320px!important}' });

    const scenario = SCENARIOS[name];
    const setup = typeof scenario === 'function' ? scenario : scenario.sim;
    if (setup) await page.evaluate(`(${setup.toString()})(window.sim)`);
    if (scenario.steps) {
      const box = await page.locator('#canvas').boundingBox();
      const click = async (x, y) => {
        await page.mouse.click(box.x + x, box.y + y);
        await page.waitForTimeout(60);
      };
      const drag = async (x1, y1, x2, y2) => {
        await page.mouse.move(box.x + x1, box.y + y1);
        await page.mouse.down();
        for (let i = 1; i <= 8; i++) {   // slow drag: no scroll momentum
          await page.mouse.move(box.x + x1 + (x2 - x1) * i / 8, box.y + y1 + (y2 - y1) * i / 8);
          await page.waitForTimeout(40);
        }
        await page.waitForTimeout(150);
        await page.mouse.up();
        await page.waitForTimeout(100);
      };
      const hold = async (x, y, ms) => {   // press, keep it down (samples keep coming), lift
        await page.mouse.move(box.x + x, box.y + y);
        await page.mouse.down();
        await page.waitForTimeout(ms);
        await page.mouse.up();
        await page.waitForTimeout(60);
      };
      await page.waitForTimeout(100);
      await scenario.steps(page, click, drag, hold);
    }
    await page.waitForTimeout(300);

    await page.locator('#canvas').screenshot({ path: file });
    const model = await page.evaluate(() => window.sim.model());
    console.log(`wrote ${file} (${name}) model=${JSON.stringify(model)}`);
  } finally {
    await page.close();
  }
}

try {
  if (all) {
    fs.mkdirSync(out, { recursive: true });
    for (const name of Object.keys(SCENARIOS)) await runScenario(name, path.join(out, `${name}.png`));
  } else {
    await runScenario(scenarioName, out);
  }
} catch (e) {
  failed = true;
  console.error(e.message || e);
} finally {
  await browser.close();
  server.close();
}
process.exit(failed ? 1 : 0);
