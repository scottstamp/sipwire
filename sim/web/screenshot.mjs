#!/usr/bin/env node
// Headless screenshot of the simulator canvas (native 240x320, not zoomed).
//
//   node sim/web/screenshot.mjs out.png [scenario]
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
};

const [out, scenarioName = 'idle'] = process.argv.slice(2);
if (!out || !SCENARIOS[scenarioName]) {
  console.error(`usage: node sim/web/screenshot.mjs out.png [${Object.keys(SCENARIOS).join('|')}]`);
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
try {
  const page = await browser.newPage({ viewport: { width: 1000, height: 760 } });
  page.on('pageerror', (e) => { failed = true; console.error('pageerror:', e.message); });
  page.on('console', (m) => {
    if (m.type() === 'error') { failed = true; console.error('console.error:', m.text()); }
  });

  await page.goto(url);
  await page.waitForFunction(() => window.simReady === true, null, { timeout: 30000 });

  // Native size for the screenshot: undo the 2x CSS zoom.
  await page.addStyleTag({ content: '#canvas{width:240px!important;height:320px!important}' });

  const scenario = SCENARIOS[scenarioName];
  await page.evaluate(`(${scenario.toString()})(window.sim)`);
  await page.waitForTimeout(300);

  await page.locator('#canvas').screenshot({ path: out });
  const model = await page.evaluate(() => window.sim.model());
  console.log(`wrote ${out} (${scenarioName}) model=${JSON.stringify(model)}`);
} catch (e) {
  failed = true;
  console.error(e.message || e);
} finally {
  await browser.close();
  server.close();
}
process.exit(failed ? 1 : 0);
