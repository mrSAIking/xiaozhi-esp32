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
  animation:{breath_on:true,hand_on:true,breath_ds:36,hand_ds:40,breath_pct:100,hand_pct:100},animation_saved:true,
  internal_free:201240,psram_free:6870280,build:"8c27f1ba3dca",
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
  tavily:{enabled:true,key_saved:false,status:"API key required",last_success:0},station_connected:false,
  weather_status: "Waiting for home Wi-Fi",
};
let home = false, disconnected = false, persisted={...snapshot.animation}, saves=0;
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
      if(["animation","preview_animation"].includes(p.action)){
        const {action,...settings}=p;snapshot.animation=settings;
        if(action==="animation"){persisted={...settings};saves++;}
        snapshot.animation_saved=JSON.stringify(settings)===JSON.stringify(persisted);
      }
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
  const fixtures={
    "/scan":{aps:[{ssid:"Studio",rssi:-52,authmode:3},{ssid:"Guest",rssi:-67,authmode:3}]},
    "/saved/list":["Studio"],
    "/advanced/config":["Tavily search","OTA updates","Sleep settings"].map((title,i)=>({
      id:"group"+i,title,fields:[{id:"enabled",label:"Enable",type:"boolean"}],values:{enabled:false}})),
  };
  res.end(JSON.stringify(fixtures[route] || []));
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
    for(const id of ["overview","animation","appearance","audio","wifi","weather","webSearch","services","time","advanced"]){
      await evaluate("page("+JSON.stringify(id)+")");
      if(id==="wifi" || id==="advanced") await until("$('legacy').contentDocument?.getElementById('ssid')!=null");
      await pause(160);
      assert.ok(await evaluate("document.documentElement.scrollWidth<=innerWidth"),name+" "+id);
      const metrics=await call("Page.getLayoutMetrics");
      const shot=await call("Page.captureScreenshot",{format:"png",captureBeyondViewport:true,
        clip:{x:0,y:0,width,height:Math.max(height,metrics.cssContentSize.height),scale:1}});
      const suffix=id==="overview"?"":("-"+id);
      await writeFile(web+name+suffix+"-preview.png",Buffer.from(shot.data,"base64"));
    }
  }
  await evaluate("page('animation');$('breath_ds').value=24;$('breath_ds').oninput()");
  await until("state.animation.breath_ds===24 && !state.animation_saved");
  assert.equal(saves,0);
  await evaluate("$('saveMotion').click()");
  await until("state.animation_saved && !motionBusy");
  assert.equal(saves,1);
  await evaluate("page('appearance')");
  await evaluate("$('bgwhite').click();$('bodyMode').value='half';$('showSubs').click();$('applyControls').click()");
  await until("state.white && state.half && state.subtitles && state.volume===65");
  await evaluate("$('mobileNav').click();document.querySelector('[data-page=audio]').click();$('volume').value=25;$('applyVolume').click()");
  await until("state.volume===25 && state.white && state.subtitles");
  await evaluate("page('weather');$('key').value='new';$('reveal').click();");
  assert.equal(await evaluate("$('key').type"), "text");
  await evaluate("$('clear').click();$('test').click()");
  assert.equal(await evaluate("$('test').disabled && $('weatherHint').textContent.includes('hotspot')"),true);
  await evaluate("page('webSearch')");
  assert.equal(await evaluate("$('testSearch').disabled"),true);
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
  // Discard unsaved RAM edits and recreate the simulated device from its saved record.
  await evaluate("page('animation');$('hand_pct').value=17;$('hand_pct').oninput()");
  await until("state.animation.hand_pct===17 && !state.animation_saved");
  snapshot.animation={...persisted}; snapshot.animation_saved=true;
  home = true;
  await call("Page.navigate", { url });
  await until("document.getElementById('login')?.hidden===false");
  await evaluate("$('access').value='demo';$('unlock').click()");
  await until("state?.home===true");
  await evaluate("page('animation')");
  assert.equal(await evaluate("Number($('breath_ds').value)"),24);
  assert.equal(await evaluate("Number($('hand_pct').value)"),100);
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
