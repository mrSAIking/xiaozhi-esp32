// Browser-level smoke test and reproducible screenshots using a local Chromium installation.
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { mkdtemp, readFile, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { setTimeout as pause } from "node:timers/promises";
const web = "main/boards/anime-companion-s3/web/",
  vendor = "components/78__esp-wifi-connect/assets/";
const snapshot = {
  home: false, white:false, half:false, subtitles:false, volume:65,
  enabled: false,
  city: "",
  country: "",
  units: "metric",
  zone: "Asia/Kolkata",
  automatic: true,
  h24: false,
  zones: ["Asia/Kolkata", "Europe/London"],
  firmware: "2.6.0", board:"anime-companion-s3",
  ip: "192.168.4.1",
  clock: { synchronized: false },
  cities: [],
  weather_status: "Waiting for home Wi-Fi",
};
let home = false, disconnected = false;
const server = createServer(async (req, res) => {
  const route = new URL(req.url, "http://localhost").pathname;
  if (route === "/api/aura") {
    if(disconnected){res.writeHead(503);res.end("Connection unavailable");return;}
    if (home && req.headers.authorization !== "Bearer demo") {
      res.writeHead(401);
      res.end();
      return;
    }
    if (req.method === "POST") {
      assert.equal(req.headers["x-aura-request"], "1");
      let raw = "";
      for await (const part of req) raw += part;
      const p = JSON.parse(raw);
      if (p.action === "appearance" || p.action==="volume") Object.assign(snapshot,p);
      if (p.action === "time") {
        snapshot.zone = p.zone;
        snapshot.h24 = p.h24;
      }
      if (p.action === "test") {
        res.writeHead(400);
        res.end("Offline. Save settings and validate on home Wi-Fi.");
        return;
      }
    }
    res.setHeader("content-type", "application/json");
    res.end(JSON.stringify({ ...snapshot, home }));
    return;
  }
  const files = {
    "/": [web + "portal.html", "text/html"],
    "/done.html": [web + "portal.html", "text/html"],
    "/aura-logo.webp": [web + "aura_logo.webp", "image/webp"],
    "/network": [vendor + "wifi_configuration.html", "text/html"],
    "/advanced.js": [vendor + "wifi_config_group.js", "text/javascript"],
  };
  if (files[route]) {
    res.setHeader("content-type", files[route][1]);
    res.end(await readFile(files[route][0]));
    return;
  }
  res.setHeader("content-type", "application/json");
  res.end(route === "/scan" ? '{"aps":[]}' : "[]");
});
await new Promise((ok) => server.listen(0, "127.0.0.1", ok));
const profile = await mkdtemp(path.join(tmpdir(), "aura-browser-"));
const child = spawn(
  process.env.BROWSER || "chromium",
  [
    "--headless",
    "--disable-gpu",
    "--disable-background-networking",
    "--no-first-run",
    "--remote-debugging-pipe",
    "--user-data-dir=" + profile,
  ],
  { windowsHide: true, stdio: ["ignore", "ignore", "ignore", "pipe", "pipe"] },
);
let seq = 0,
  buf = "",
  sessionId;
const requests = new Map();
child.stdio[4].setEncoding("utf8");
child.stdio[4].on("data", (chunk) => {
  buf += chunk;
  let i;
  while ((i = buf.indexOf("\0")) >= 0) {
    const m = JSON.parse(buf.slice(0, i));
    buf = buf.slice(i + 1);
    if (requests.has(m.id)) {
      requests.get(m.id)(m);
      requests.delete(m.id);
    }
  }
});
async function call(method, params = {}, session = sessionId) {
  const id = ++seq;
  const result = new Promise((ok) => requests.set(id, ok));
  child.stdio[3].write(
    JSON.stringify({ id, method, params, sessionId: session }) + "\0",
  );
  const m = await result;
  if (m.error) throw Error(m.error.message);
  return m.result;
}
async function evaluate(expression) {
  const r = await call("Runtime.evaluate", {
    expression,
    awaitPromise: true,
    returnByValue: true,
  });
  if (r.exceptionDetails) throw Error(r.exceptionDetails.text);
  return r.result.value;
}
async function until(expr) {
  for (let n = 0; n < 80; n++) {
    if (await evaluate(expr)) return;
    await pause(50);
  }
  throw Error(expr);
}
try {
  const { targetId } = await call("Target.createTarget", {
    url: "about:blank",
  });
  ({ sessionId } = await call("Target.attachToTarget", {
    targetId,
    flatten: true,
  }));
  await call("Page.enable");
  const url = "http://127.0.0.1:" + server.address().port;
  for (const [name, width, height] of [
    ["desktop", 1360, 960],
    ["mobile", 412, 915],
  ]) {
    await call("Emulation.setDeviceMetricsOverride", {
      width,
      height,
      deviceScaleFactor: 1,
      mobile: name === "mobile",
    });
    await call("Page.navigate", { url });
    await until("document.getElementById('ip')?.textContent==='192.168.4.1'");
    await until("Array.from(document.images).every(i=>i.complete)");
    assert.ok(
      await evaluate("document.documentElement.scrollWidth<=innerWidth"),
    );
    await evaluate("page('appearance')");
    assert.ok(await evaluate("document.documentElement.scrollWidth<=innerWidth"));
    await pause(150);
    const shot = await call("Page.captureScreenshot", {
      format: "png",
      captureBeyondViewport: false,
    });
    await writeFile(
      web + name + "-preview.png",
      Buffer.from(shot.data, "base64"),
    );
  }
  await evaluate("$('bgwhite').click();$('bodyMode').value='half';$('showSubs').click();$('applyControls').click()");
  await until("state.white && state.half && state.subtitles && state.volume===65");
  await evaluate("$('mobileNav').value='audio';$('mobileNav').onchange();$('volume').value=25;$('applyVolume').click()");
  await until("state.volume===25 && state.white && state.subtitles");
  await evaluate("page('weather');$('key').value='new';$('reveal').click();");
  assert.equal(await evaluate("$('key').type"), "text");
  await evaluate("$('clear').click();$('test').click()");
  await until("$('feedback').textContent.startsWith('Offline')");
  await evaluate(
    "page('time');$('zone').value='Europe/London';$('saveTime').click()",
  );
  await until("state.zone==='Europe/London'");
  await evaluate("page('wifi')");
  await until("$('legacy').contentDocument?.getElementById('ssid')!=null");
  await evaluate("page('advanced')");
  await until(
    "$('legacy').contentDocument?.getElementById('advanced-tab')?.classList.contains('active')",
  );
  disconnected=true;
  await evaluate("refresh()");
  await until("$('connection').textContent==='Disconnected'");
  disconnected=false;
  await evaluate("refresh()");
  await until("$('connection').textContent==='Setup hotspot' && $('feedback').textContent===''");
  home = true;
  await call("Page.navigate", { url });
  await until("document.getElementById('login')?.hidden===false");
  await evaluate("$('access').value='demo';$('unlock').click()");
  await until("state?.home===true");
  await evaluate("page('advanced')");
  assert.equal(
    await evaluate("$('legacy').hidden && $('pairBox').hidden"),
    true,
  );
  console.log(
    "Responsive screenshots, offline weather, settings, embedded provisioning panels and home login passed.",
  );
} finally {
  child.kill();
  server.close();
  await pause(200);
  await rm(profile, {
    recursive: true,
    force: true,
    maxRetries: 4,
    retryDelay: 100,
  });
}
