#include "snapclient/SettingsUiHtml.h"

namespace snapclient {

const char kSettingsUiHtml[] = R"HTMLPAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>snapclient settings</title>
<style>
  body { font-family: system-ui, sans-serif; max-width: 480px; margin: 2rem auto; padding: 0 1rem; }
  h1 { font-size: 1.25rem; }
  label { display: block; margin-top: 0.75rem; font-weight: 600; }
  input, select { width: 100%; padding: 0.4rem; margin-top: 0.25rem; box-sizing: border-box; }
  #hint { font-size: 0.85rem; color: #555; margin-top: 0.5rem; }
  button { margin-top: 1rem; padding: 0.5rem 1rem; }
  #status.ok { color: green; }
  #status.error { color: red; }
</style>
</head>
<body>
<h1>snapclient settings</h1>
<form id="form">
  <label for="host">Server host</label>
  <input id="host" type="text" required>

  <label for="port">Server port</label>
  <input id="port" type="number" min="1" max="65535" required>

  <label for="flow">DSP flow</label>
  <select id="flow">
    <option value="stereo">Stereo</option>
    <option value="biamp">Biamp</option>
    <option value="bassBoost">Bass Boost</option>
    <option value="eqBassTreble">EQ Bass/Treble</option>
  </select>
  <p id="hint"></p>

  <label for="freqPrimary">Primary frequency (Hz)</label>
  <input id="freqPrimary" type="number" step="any">

  <label for="gainPrimary">Primary gain (dB)</label>
  <input id="gainPrimary" type="number" step="any">

  <label for="freqTertiary">Tertiary frequency (Hz)</label>
  <input id="freqTertiary" type="number" step="any">

  <label for="gainTertiary">Tertiary gain (dB)</label>
  <input id="gainTertiary" type="number" step="any">

  <button type="submit">Save</button>
  <p id="status"></p>
</form>
<script>
let state = null;

const hints = {
  stereo: "No filtering - pass-through stereo.",
  biamp: "Primary = ch1 lowpass cutoff (Hz). Tertiary = ch2 highpass cutoff (Hz). Gains unused.",
  bassBoost: "Primary = bass shelf frequency (Hz) and gain (dB). Tertiary unused.",
  eqBassTreble: "Primary = bass shelf freq/gain (Hz/dB). Tertiary = treble shelf freq/gain (Hz/dB)."
};

function populateFlowFields(flowName) {
  const params = (state.dsp.flows && state.dsp.flows[flowName]) || {};
  document.getElementById('freqPrimary').value = params.freqPrimaryHz ?? 0;
  document.getElementById('gainPrimary').value = params.gainPrimaryDb ?? 0;
  document.getElementById('freqTertiary').value = params.freqTertiaryHz ?? 0;
  document.getElementById('gainTertiary').value = params.gainTertiaryDb ?? 0;
  document.getElementById('hint').textContent = hints[flowName] || '';
}

async function load() {
  const res = await fetch('/api/settings');
  state = await res.json();
  document.getElementById('host').value = state.server.host;
  document.getElementById('port').value = state.server.port;
  document.getElementById('flow').value = state.dsp.activeFlow;
  populateFlowFields(state.dsp.activeFlow);
}

document.getElementById('flow').addEventListener('change', (e) => {
  populateFlowFields(e.target.value);
});

document.getElementById('form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const flow = document.getElementById('flow').value;
  const payload = {
    server: {
      host: document.getElementById('host').value,
      port: parseInt(document.getElementById('port').value, 10)
    },
    dsp: {
      activeFlow: flow,
      flows: {
        [flow]: {
          freqPrimaryHz: parseFloat(document.getElementById('freqPrimary').value),
          gainPrimaryDb: parseFloat(document.getElementById('gainPrimary').value),
          freqTertiaryHz: parseFloat(document.getElementById('freqTertiary').value),
          gainTertiaryDb: parseFloat(document.getElementById('gainTertiary').value)
        }
      }
    }
  };
  const statusEl = document.getElementById('status');
  try {
    const res = await fetch('/api/settings', { method: 'POST', body: JSON.stringify(payload) });
    if (!res.ok) {
      const err = await res.json();
      statusEl.textContent = 'Error: ' + (err.error || res.status);
      statusEl.className = 'error';
      return;
    }
    state = await res.json();
    statusEl.textContent = 'Saved.';
    statusEl.className = 'ok';
  } catch (err) {
    statusEl.textContent = 'Error: ' + err.message;
    statusEl.className = 'error';
  }
});

load();
</script>
</body>
</html>
)HTMLPAGE";

}  // namespace snapclient
