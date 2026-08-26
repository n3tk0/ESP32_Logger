// Screenshot preview.html the way the reader sees it, and print the page height.
//
//   node tools/kindle_preview/shot.mjs [out.png] [width] [height]
//
// The height is the number that matters: at the default width the page must
// come in under 800, and it currently sits at 796. Anything over that scrolls
// on a device with no scrollbar to tell you it did.
import { chromium } from 'playwright';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const out = process.argv[2] || path.join(HERE, 'kindle.png');
const width = Number(process.argv[3] || 600);
const height = Number(process.argv[4] || 800);

// A Paperwhite (10th gen, PQ94WIF) reports 600 CSS px across a 1072 px panel;
// deviceScaleFactor 2 is close enough to see how hairlines land. The greyscale
// filter approximates a panel with no colour to fall back on. A basic Kindle 7
// is 600x800 at DPR 1 — same CSS layout, fewer device pixels under it.
const browser = await chromium.launch({
  executablePath: process.env.CHROMIUM_PATH || '/opt/pw-browsers/chromium',
});
const page = await browser.newPage({
  viewport: { width, height },
  deviceScaleFactor: 2,
});
await page.goto('file://' + path.join(HERE, 'preview.html'));
await page.addStyleTag({ content: 'html{filter:grayscale(1) contrast(1.06)}' });
await page.screenshot({ path: out, fullPage: true });
console.log('height:', await page.evaluate(() => document.body.scrollHeight),
            '(budget', height + ')');
console.log('wrote:', out);
await browser.close();
