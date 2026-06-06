import { chromium } from 'playwright';
import fs from 'fs';

const file = process.argv[2] || '/Users/angaziz/work/personal/beacon/docs/design/mockups/directions.html';
const out = process.argv[3] || '/Users/angaziz/work/personal/beacon/docs/design/mockups/shots';
fs.mkdirSync(out, { recursive: true });

const browser = await chromium.launch();
const page = await browser.newPage({ deviceScaleFactor: 2 });
await page.setViewportSize({ width: 1180, height: 1200 });
await page.goto('file://' + file, { waitUntil: 'networkidle' });
await page.evaluate(() => document.fonts.ready);
await page.waitForTimeout(800);

// Prefer explicitly tagged shots; fall back to .lane sections.
const tagged = await page.$$('[data-shot]');
if (tagged.length) {
  for (const el of tagged) {
    const name = await el.getAttribute('data-shot');
    await el.screenshot({ path: `${out}/${name}.png` });
    console.log('shot', name);
  }
} else {
  const lanes = await page.$$('.lane');
  const names = ['01-hud', '04-calm', '05-editorial', '07-blueprint', '08-led', '09-oscilloscope', '10-analog'];
  for (let i = 0; i < lanes.length; i++) {
    const name = names[i] || 'lane' + i;
    await lanes[i].screenshot({ path: `${out}/${name}.png` });
    console.log('shot', name);
  }
}
await browser.close();
console.log('done ->', out);
