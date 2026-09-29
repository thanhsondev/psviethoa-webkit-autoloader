(function () {
  'use strict';

  var splashEl = document.getElementById('splash');
  var loaderEl = document.getElementById('loader');
  var logContainer = document.getElementById('logContainer');
  var progressBar = document.getElementById('progressBar');
  var progressLabel = document.getElementById('progressLabel');
  var exploitEl = document.getElementById('exploit');

  /* After a WebProcess crash the PS5 browser restores this page together with
     the iframe at its last URL — the armed exploit URL, which would auto-run
     the chain again. Blank it as early as possible (the iframe element is
     already in the DOM at script parse) so the chain only runs after the
     splash screen. */
  try {
    exploitEl.src = 'about:blank';
  } catch (e) { }

  var MAX_LOG_LINES = 80;
  var finished = false;
  var chainStarted = false;
  var lastFrameUrl = '';
  var mirrorTimer = 0;

  /* Build-time exploit override: "auto" (firmware table), "umtx2" or
     "relapse". Replaced by tools/gen_file_registry.py / build_host.py /
     dev_server.py from the FORCE_EXPLOIT env (default "auto"); left as the
     raw placeholder when served straight from source -> auto. A ?force=
     query on this page overrides it at runtime (handy for make dev). */
  var EXPLOIT_MODE = '[[EXPLOIT_MODE]]';
  if (EXPLOIT_MODE.indexOf('[[') === 0) EXPLOIT_MODE = 'auto';

  /* Firmwares supported by each exploit, keyed on the exact UA firmware
     string (/PlayStation 5/x.xx/). Keep in sync with the exploits' own lists:
     umtx2/document/en/ps5/main.js and relapse/src/firmware.js. relapse
     (7.00-13.60) is the only chain for its whole range. */
  var UMTX2_FIRMWARES = ["1.00", "1.01", "1.02", "1.05", "1.10", "1.11", "1.12", "1.13", "1.14", "2.00", "2.20", "2.25", "2.26", "2.30", "2.50", "2.70", "3.00", "3.10", "3.20", "3.21", "4.00", "4.02", "4.03", "4.50", "4.51", "5.00", "5.02", "5.10", "5.50"];
  var RELAPSE_FIRMWARES = ["13.60", "13.42", "13.40", "13.20", "13.00", "12.70", "12.60", "12.40", "12.20", "12.02", "12.00", "11.60", "11.20", "11.00", "10.60", "10.40", "10.20", "10.01", "10.00", "9.60", "9.40", "9.20", "9.00", "8.60", "8.40", "8.20", "8.00", "7.61", "7.60", "7.40", "7.20", "7.01", "7.00"];

  var UMTX2_URL =
    'umtx2/index.html?autoload=payload.elf&v=1';
  var RELAPSE_URL =
    'relapse/index.html?autoload=payload.elf&v=1';

  /* Keep in sync with the *_iframe_url() helpers in tools/gen_file_registry.py —
     the AppCache manifest lists these exact URLs so the console can serve them
     offline (AppCache matches URLs including the query string). */

  var EXPLOIT_URL = '';
  var exploitMode = null;

  function uiLog(message, type) {
    type = type || 'info';
    var entry = document.createElement('div');
    entry.className = 'line ' + type;
    entry.textContent = message;
    logContainer.appendChild(entry);
    while (logContainer.childElementCount > MAX_LOG_LINES) {
      logContainer.removeChild(logContainer.firstChild);
    }
    logContainer.parentNode.scrollTop = logContainer.parentNode.scrollHeight;
    return entry;
  }

  function updateProgress(percent, message) {
    progressBar.style.transform = 'scaleX(' + percent / 100 + ')';
    var pctEl = document.getElementById('progressPct');
    if (pctEl) pctEl.textContent = Math.round(percent) + '%';
    if (message) {
      progressLabel.textContent = message;
      uiLog(message, 'info');
    }
  }

  window.uiLog = uiLog;
  window.updateProgress = updateProgress;

  /* Exploit stage tracker (top bar). Four stages are mirrored from the
     chain's own console lines: pending -> is-active -> is-done / is-fail.
     The progress bar is driven by the number of completed stages. */
  var stageOrder = ['webkit', 'rop', 'kernel', 'payloads'];
  var stageDone = { webkit: false, rop: false, kernel: false, payloads: false };

  function setStage(name, state) {
    var el = document.querySelector('.stage[data-stage="' + name + '"]');
    if (!el) return;
    el.classList.remove('is-active', 'is-done', 'is-fail');
    if (state) el.classList.add('is-' + state);
    if (state === 'done') stageDone[name] = true;
    else if (state === 'active' || state === 'fail') stageDone[name] = false;
    var completed = 0;
    for (var i = 0; i < stageOrder.length; i++) {
      if (stageDone[stageOrder[i]]) completed++;
    }
    progressBar.style.transform = 'scaleX(' + (completed * 0.25) + ')';
    var pctEl = document.getElementById('progressPct');
    if (pctEl) pctEl.textContent = (completed * 25) + '%';
  }

  function setStatus(text, tone) {
    var badge = document.getElementById('statusBadge');
    var label = document.getElementById('statusText');
    if (label && text) label.textContent = text;
    if (badge) badge.className = 'badge badge-status is-' + (tone || 'idle');
  }

  function resetStages() {
    for (var i = 0; i < stageOrder.length; i++) {
      stageDone[stageOrder[i]] = false;
      var el = document.querySelector('.stage[data-stage="' + stageOrder[i] + '"]');
      if (el) el.classList.remove('is-active', 'is-done', 'is-fail');
    }
    setStatus('idle', 'idle');
    progressBar.style.transform = 'scaleX(0)';
    var pctEl = document.getElementById('progressPct');
    if (pctEl) pctEl.textContent = '0%';
  }

  /* Mark the first still-active stage as failed; used when the chain dies
     before a stage-specific line arrives. */
  function failActiveStage() {
    for (var i = 0; i < stageOrder.length; i++) {
      var el = document.querySelector('.stage[data-stage="' + stageOrder[i] + '"]');
      if (el && el.classList.contains('is-active')) {
        setStage(stageOrder[i], 'fail');
        return;
      }
    }
    setStage('payloads', 'fail');
  }

  function detectFirmware() {
    var m = /PlayStation 5\/(\d+\.\d+)/.exec(navigator.userAgent);
    if (!m) return null;
    return { str: m[1], num: parseFloat(m[1]) };
  }

  /* Choose which exploit to arm. Forced modes (build-time EXPLOIT_MODE or a
     ?force= query on this page) bypass the firmware table so a specific chain
     can be exercised on any firmware — the exploit page's own firmware guard
     still applies. Returns 'umtx2' | 'relapse' | null. */
  function pickExploit() {
    var fw = detectFirmware();
    var forced = null;
    try {
      var q = new URLSearchParams(window.location.search).get('force');
      if (q === 'umtx2' || q === 'relapse') forced = q;
    } catch (e) { }
    if (forced) {
      uiLog('[force] using ' + forced + ' on firmware ' + (fw ? fw.str : 'unknown'), 'warning');
      return forced;
    }
    if (EXPLOIT_MODE === 'umtx2' || EXPLOIT_MODE === 'relapse') {
      uiLog('[force] using ' + EXPLOIT_MODE + ' on firmware ' + (fw ? fw.str : 'unknown'), 'warning');
      return EXPLOIT_MODE;
    }
    if (!fw) {
      uiLog('[ERROR] Not a PlayStation 5 browser.', 'error');
      return null;
    }
    if (UMTX2_FIRMWARES.indexOf(fw.str) !== -1) return 'umtx2';
    if (RELAPSE_FIRMWARES.indexOf(fw.str) !== -1) return 'relapse';
    uiLog('[ERROR] Unsupported firmware ' + fw.str +
      ' (supported: 1.00-5.50 via umtx2, 7.00-13.60 via relapse).', 'error');
    return null;
  }

  function revealExploit() {
    splashEl.classList.add('hide');
    setTimeout(function () {
      splashEl.hidden = true;
      loaderEl.hidden = false;
    }, 320);
  }

  function onAutoloadResult(data) {
    if (data.retry) {
      /* The chain page is reloading itself for another attempt; mirror the
         counter and keep streaming instead of declaring failure. */
      failActiveStage();
      setStatus('retrying ' + data.attempt + '/' + data.total, 'run');
      uiLog('[retry] attempt ' + data.attempt + ' of ' + data.total + ' failed: ' +
        (data.why || 'unknown error') + ' - retrying automatically', 'warning');
      return;
    }
    if (finished) return;
    finished = true;
    /* Success is terminal — stop mirroring so the page stays idle while the
       payload runs alongside it. On failure keep streaming the iframe's
       output into the log for diagnostics. */
    if (data.ok && mirrorTimer) {
      clearInterval(mirrorTimer);
      mirrorTimer = 0;
    }
    if (data.ok) {
      /* Every stage must have passed by the time the payload is loaded. */
      setStage('webkit', 'done');
      setStage('rop', 'done');
      setStage('kernel', 'done');
      setStage('payloads', 'done');
      setStatus('payload loaded', 'ok');
      uiLog('Payload loaded (' + data.bytes + ' bytes sent to elfldr).', 'success');
      updateProgress(100, 'Autoload finished.');

      /* Payload is running as its own process now — unload the iframe to
         free the memory it held and avoid a browser OOM dialog.
         NOTE: only safe for umtx2; the relapse chain requires its document to
         remain open (it holds its ROP workers and spawned threads). */
      if (exploitMode === 'umtx2') {
        try { exploitEl.src = 'about:blank'; } catch (e) { }
      }
    } else {
      failActiveStage();
      setStatus('autoload failed', 'fail');
      uiLog('[ERROR] Autoload failed: ' + (data.why || 'unknown error'), 'error');
      progressLabel.textContent = 'Autoload failed.';
    }
    setTimeout(function () {
      if (data.ok) {
        uiLog('Payload running on the console.', 'success');
      }
    }, 1500);
  }

  /* Mirror umtx2's live #console log (#console > div, classed LOG-*) from the
     same-origin exploit iframe into our own log view, mapping its severity
     classes onto ours. umtx2 updates its last console line in place for
     progress logs (FLAG_TEMP, e.g. "Race attempt N-M"), so we update our
     matching last line in place too. */
  var umtx2MirroredLines = 0;
  var umtx2LastEntry = null;
  var umtx2LastText = '';
  function mirrorUmtx2() {
    var doc;
    try {
      doc = exploitEl.contentDocument;
    } catch (e) {
      return;
    }
    if (!doc || !chainStarted) return;
    var lines = doc.querySelectorAll('#console > div');
    if (lines.length < umtx2MirroredLines) {
      /* Iframe reloaded (#console recreated) — restart from a fresh document. */
      umtx2MirroredLines = lines.length;
      umtx2LastEntry = null;
      umtx2LastText = '';
    }
    for (; umtx2MirroredLines < lines.length; umtx2MirroredLines++) {
      var el = lines[umtx2MirroredLines];
      var text = (el.textContent || '').trim();
      if (!text) continue;
      var cls = el.className || '';
      var entry;
      if (/LOG-ERROR/.test(cls)) {
        entry = uiLog('[umtx2] ' + text, 'error');
      } else if (/LOG-WARN/.test(cls)) {
        entry = uiLog('[umtx2] ' + text, 'warning');
      } else if (/LOG-SUCCESS/.test(cls)) {
        entry = uiLog('[umtx2] ' + text, 'success');
      } else {
        entry = uiLog('[umtx2] ' + text, 'info');
      }
      umtx2LastEntry = entry;
      umtx2LastText = text;
    }
    /* Live-update the last mirrored line when umtx2 rewrites it in place. */
    if (lines.length > 0 && umtx2LastEntry
      && umtx2LastEntry === logContainer.lastChild) {
      var last = lines[lines.length - 1];
      var lastText = (last.textContent || '').trim();
      if (lastText && lastText !== umtx2LastText) {
        umtx2LastEntry.textContent = '[umtx2] ' + lastText;
        umtx2LastText = lastText;
      }
    }
  }

  /* Mirror the relapse chain's #console log (site.js writes one div per
     line: "[*]/[+]/[-] <message>") into our log and drive coarse progress
     milestones from its key lines. */
  var relapseMirroredLines = 0;
  function mirrorRelapse() {
    var doc;
    try {
      doc = exploitEl.contentDocument;
    } catch (e) {
      return;
    }
    if (!doc) return;

    var frameUrl = '';
    try {
      frameUrl = exploitEl.contentWindow.location.href;
    } catch (e) { }
    if (frameUrl !== lastFrameUrl) {
      lastFrameUrl = frameUrl;
      relapseMirroredLines = 0;
    }
    if (!chainStarted) return;

    var consoleEl = doc.getElementById('console');
    if (!consoleEl) {
      var isArmedUrl = frameUrl.length > EXPLOIT_URL.length &&
        frameUrl.slice(-EXPLOIT_URL.length) === EXPLOIT_URL;
      if (frameUrl === 'about:blank' || doc.readyState !== 'complete' || isArmedUrl) {
        return;
      }
      if (mirrorRelapse.warned !== frameUrl) {
        mirrorRelapse.warned = frameUrl;
        uiLog('[iframe] page has no relapse console: title="' + (doc.title || '') + '"', 'warning');
      }
      return;
    }

    var lines = consoleEl.children;
    if (lines.length < relapseMirroredLines) {
      relapseMirroredLines = lines.length;
    }
    for (; relapseMirroredLines < lines.length; relapseMirroredLines++) {
      var text = (lines[relapseMirroredLines].textContent || '').trim();
      if (!text) continue;
      if (text.indexOf('[-]') === 0) {
        uiLog('[relapse] ' + text.slice(3).trim(), 'error');
      } else if (text.indexOf('[+]') === 0) {
        uiLog('[relapse] ' + text.slice(3).trim(), 'success');
      } else {
        uiLog('[relapse] ' + (text.indexOf('[*]') === 0 ? text.slice(3).trim() : text), 'info');
      }
      /* Coarse milestones for the slim progress bar (the iframe's own log
         lines are the only progress signal the chain emits). */
      if (text.indexOf('Starting WebKit exploit') !== -1) {
        setStage('webkit', 'active');
        setStatus('webkit exploit', 'run');
        progressLabel.textContent = 'WebKit exploit running...';
      } else if (text.indexOf('ARW ready') !== -1) {
        setStage('webkit', 'done');
        setStage('rop', 'active');
        setStatus('building rop chain', 'run');
        progressLabel.textContent = 'Userland read/write ready.';
      } else if (text.indexOf('Worker chain: ready') !== -1) {
        setStage('rop', 'done');
        setStage('kernel', 'active');
        setStatus('kernel exploit running', 'run');
        progressLabel.textContent = 'Kernel exploit running...';
      } else if (text.indexOf('kernel exploit complete') !== -1) {
        setStage('kernel', 'done');
        setStatus('starting elfldr', 'run');
        progressLabel.textContent = 'Kernel R/W ready.';
      } else if (text.indexOf('elfldr is listening') !== -1) {
        setStage('kernel', 'done');
        setStage('payloads', 'active');
        setStatus('autoloading payload', 'run');
        progressLabel.textContent = 'ELF loader ready - sending payload...';
      } else if (text.indexOf('autoloading ') !== -1) {
        setStage('payloads', 'active');
        setStatus('autoloading payload', 'run');
        progressLabel.textContent = 'Autoloading payload...';
      } else if (text.indexOf('autoloaded ') !== -1) {
        setStage('payloads', 'done');
        setStatus('payload loaded', 'ok');
        progressLabel.textContent = 'Payload sent.';
      } else if (text.indexOf('autoload failed') !== -1) {
        setStage('payloads', 'fail');
        setStatus('autoload failed', 'fail');
        progressLabel.textContent = 'Autoload failed.';
      } else if (text.indexOf('kernel chain complete') !== -1) {
        setStage('kernel', 'done');
        setStage('payloads', 'fail');
        setStatus('finished without elfldr', 'fail');
        progressLabel.textContent = 'Chain finished without elfldr.';
      } else if (text.indexOf('never answered') !== -1 ||
        text.indexOf('Worker chain did not execute') !== -1) {
        setStage('rop', 'fail');
        setStatus('rop chain stalled', 'fail');
      } else if (text.indexOf('kernel exploit did not finish') !== -1) {
        setStage('kernel', 'fail');
        setStatus('kernel exploit failed', 'fail');
      } else if (text.indexOf('Memory primitive unavailable') !== -1) {
        setStage('webkit', 'fail');
        setStatus('webkit exploit failed', 'fail');
      }
    }
  }

  function mirrorExploit() {
    if (exploitMode === 'umtx2') {
      mirrorUmtx2();
      return;
    }
    mirrorRelapse();
  }

  function start() {
    uiLog('PSVietHoa AutoLoader — based on ps5-webkit-autoloader by PLK', 'success');
    updateProgress(0, 'Waiting to start...');

    window.addEventListener('message', function (event) {
      var data = event.data;
      if (!data || data.type !== 'wkal') return;
      if (data.kind === 'autoload') {
        onAutoloadResult(data);
      }
    });

    /* No iframe 'load' listener: its mirroredLines reset re-streamed the
       whole screen mid-run (doubling the log), and the other state resets
       are already handled by the URL-diff branch in the mirror functions plus
       the shrink re-anchor (fresh documents start with an empty screen,
       so their lines stream normally). */

    var picked = pickExploit();
    if (!picked) {
      updateProgress(0, 'Unsupported firmware.');
      return;
    }
    exploitMode = picked;
    EXPLOIT_URL = picked === 'umtx2' ? UMTX2_URL : RELAPSE_URL;

    var fwInfo = detectFirmware();
    var chainBadgeEl = document.getElementById('chainBadge');
    if (chainBadgeEl) chainBadgeEl.textContent = picked === 'umtx2' ? 'UMTX2' : 'RELAPSE';
    var fwBadgeEl = document.getElementById('fwBadge');
    if (fwBadgeEl) fwBadgeEl.textContent = fwInfo ? 'FW ' + fwInfo.str : 'FW ?';

    resetStages();
    setStage('webkit', 'active');
    setStatus('webkit exploit', 'run');

    mirrorTimer = setInterval(mirrorExploit, 500);

    /* umtx2 auto-runs its chain on load when sessionStorage 'on_load_autorun'
       is set (it clears it itself once main() starts); clear it on the
       relapse path so a stale key never re-triggers it. */
    try {
      if (picked === 'umtx2') {
        sessionStorage.setItem('on_load_autorun', 'kernel');
        sessionStorage.setItem('wkal_autoload', 'payload.elf');
      } else {
        sessionStorage.removeItem('on_load_autorun');
        sessionStorage.removeItem('wkal_autoload');
      }
    } catch (e) { }

    chainStarted = true;
    try {
      exploitEl.src = EXPLOIT_URL;
    } catch (e) { }

    setTimeout(revealExploit, 900);
  }

  window.addEventListener('load', start);
})();
