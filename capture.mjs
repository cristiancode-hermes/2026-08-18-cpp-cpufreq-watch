import puppeteer from 'puppeteer';
import { mkdirSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

async function captureCppTerminal() {
  const htmlPath = join(__dirname, 'screenshot.html');
  const assetsDir = '/opt/data/portafolio/public/assets';
  mkdirSync(assetsDir, { recursive: true });

  const browser = await puppeteer.launch({
    executablePath: '/opt/data/.agent-browser/browsers/chromium/chrome-linux/chrome',
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage'],
  });

  try {
    const page = await browser.newPage();

    // Desktop capture
    await page.setViewport({ width: 1440, height: 900 });
    await page.goto('file://' + htmlPath, { waitUntil: 'networkidle0', timeout: 15000 });
    await page.waitForSelector('.terminal', { timeout: 5000 });
    await new Promise(r => setTimeout(r, 1000));

    const terminalEl = await page.$('.terminal');
    const box = await terminalEl.boundingBox();

    await page.screenshot({
      path: join(assetsDir, 'cpufreq-watch.png'),
      clip: {
        x: Math.max(0, box.x - 40),
        y: Math.max(0, box.y - 20),
        width: Math.min(box.width + 80, 1440),
        height: Math.min(box.height + 40, 900),
      },
    });
    console.log('Desktop screenshot saved: cpufreq-watch.png');

    // Mobile capture
    await page.setViewport({ width: 390, height: 844 });
    await page.goto('file://' + htmlPath, { waitUntil: 'networkidle0', timeout: 15000 });
    await page.waitForSelector('.terminal', { timeout: 5000 });
    await new Promise(r => setTimeout(r, 1000));

    const mobileTerminal = await page.$('.terminal');
    const mobileBox = await mobileTerminal.boundingBox();

    await page.screenshot({
      path: join(assetsDir, 'cpufreq-watch-m.png'),
      clip: {
        x: Math.max(0, mobileBox.x - 10),
        y: Math.max(0, mobileBox.y - 10),
        width: Math.min(mobileBox.width + 20, 390),
        height: Math.min(mobileBox.height + 20, 844),
      },
    });
    console.log('Mobile screenshot saved: cpufreq-watch-m.png');

  } finally {
    await browser.close();
  }
}

captureCppTerminal().catch(console.error);
