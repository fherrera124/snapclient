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
  .eqBandRow { display: flex; gap: 0.5rem; align-items: center; margin-top: 0.4rem; }
  .eqBandRow span { width: 4.5rem; font-size: 0.85rem; }
  .eqBandRow input { margin-top: 0; }
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

<div id="dacSection">
<h1>DAC (TAS5805M)</h1>
<form id="dacForm">
  <label for="analogGain">Analog gain (0 = 0dB, 31 = -15.5dB)</label>
  <input id="analogGain" type="number" min="0" max="31" required>

  <label for="dacMode">DAC mode</label>
  <select id="dacMode">
    <option value="btl">BTL</option>
    <option value="pbtl">PBTL</option>
  </select>

  <label for="modMode">Modulation mode</label>
  <select id="modMode">
    <option value="bd">BD</option>
    <option value="1spw">1SPW</option>
    <option value="hybrid">Hybrid</option>
  </select>

  <label for="swFreq">Switching frequency</label>
  <select id="swFreq">
    <option value="768000">768 kHz</option>
    <option value="384000">384 kHz</option>
    <option value="480000">480 kHz</option>
    <option value="576000">576 kHz</option>
  </select>

  <label for="bdFreq">BD frequency</label>
  <select id="bdFreq">
    <option value="80000">80 kHz</option>
    <option value="100000">100 kHz</option>
    <option value="120000">120 kHz</option>
    <option value="175000">175 kHz</option>
  </select>

  <label for="mixerMode">Mixer mode</label>
  <select id="mixerMode">
    <option value="stereo">Stereo</option>
    <option value="stereoInverse">Stereo (inverse)</option>
    <option value="mono">Mono</option>
    <option value="left">Left only</option>
    <option value="right">Right only</option>
  </select>

  <label for="channelGainL">Channel gain L (dB)</label>
  <input id="channelGainL" type="number" min="-24" max="24" required>

  <label for="channelGainR">Channel gain R (dB)</label>
  <input id="channelGainR" type="number" min="-24" max="24" required>

  <h1>15-band EQ</h1>
  <label for="eqMode">EQ mode</label>
  <select id="eqMode">
    <option value="off">Off</option>
    <option value="on">On (left channel)</option>
    <option value="biamp">Biamp (both channels)</option>
    <option value="biampOff">Biamp, off</option>
  </select>

  <label for="eqProfileL">Preset (left)</label>
  <select id="eqProfileL"></select>

  <label for="eqProfileR">Preset (right)</label>
  <select id="eqProfileR"></select>

  <div id="eqBands"></div>

  <button type="submit">Save</button>
  <p id="dacStatus"></p>
  <p id="faults"></p>
</form>
</div>

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

const eqProfiles = [
  ['flat', 'Flat'],
  ['lf60', '60Hz LF'], ['lf70', '70Hz LF'], ['lf80', '80Hz LF'],
  ['lf90', '90Hz LF'], ['lf100', '100Hz LF'], ['lf110', '110Hz LF'],
  ['lf120', '120Hz LF'], ['lf130', '130Hz LF'], ['lf140', '140Hz LF'],
  ['lf150', '150Hz LF'],
  ['hf60', '60Hz HF'], ['hf70', '70Hz HF'], ['hf80', '80Hz HF'],
  ['hf90', '90Hz HF'], ['hf100', '100Hz HF'], ['hf110', '110Hz HF'],
  ['hf120', '120Hz HF'], ['hf130', '130Hz HF'], ['hf140', '140Hz HF'],
  ['hf150', '150Hz HF']
];
const eqBandsHz = [20, 32, 50, 80, 125, 200, 315, 500, 800, 1250, 2000, 3150, 5000, 8000, 16000];

function populateSelectOptions(id, options) {
  document.getElementById(id).innerHTML =
    options.map(([v, label]) => `<option value="${v}">${label}</option>`).join('');
}

function buildEqBandInputs() {
  document.getElementById('eqBands').innerHTML = eqBandsHz.map((hz, i) => `
    <div class="eqBandRow">
      <span>${hz} Hz</span>
      <input id="eqGainL${i}" type="number" min="-15" max="15" step="1">
      <input id="eqGainR${i}" type="number" min="-15" max="15" step="1">
    </div>`).join('');
}

populateSelectOptions('eqProfileL', eqProfiles);
populateSelectOptions('eqProfileR', eqProfiles);
buildEqBandInputs();

function faultsText(faults) {
  const active = Object.entries(faults).filter(([, v]) => v).map(([k]) => k);
  return active.length ? 'Faults: ' + active.join(', ') : 'No faults.';
}

async function loadFaults() {
  try {
    const res = await fetch('/api/dac/faults');
    if (!res.ok) return;
    document.getElementById('faults').textContent = faultsText(await res.json());
  } catch (err) {
    // Transient - next poll will retry.
  }
}

async function loadDac() {
  try {
    const res = await fetch('/api/dac/settings');
    if (!res.ok) {
      document.getElementById('dacSection').style.display = 'none';
      return;
    }
    const dac = (await res.json()).dac;
    document.getElementById('analogGain').value = dac.analogGain;
    document.getElementById('dacMode').value = dac.dacMode;
    document.getElementById('modMode').value = dac.modulation.mode;
    document.getElementById('swFreq').value = dac.modulation.swFreqHz;
    document.getElementById('bdFreq').value = dac.modulation.bdFreqHz;
    document.getElementById('mixerMode').value = dac.mixerMode;
    document.getElementById('channelGainL').value = dac.channelGainL;
    document.getElementById('channelGainR').value = dac.channelGainR;
    document.getElementById('eqMode').value = dac.eq.mode;
    document.getElementById('eqProfileL').value = dac.eq.profileL;
    document.getElementById('eqProfileR').value = dac.eq.profileR;
    eqBandsHz.forEach((_, i) => {
      document.getElementById('eqGainL' + i).value = dac.eq.gainL[i];
      document.getElementById('eqGainR' + i).value = dac.eq.gainR[i];
    });
    loadFaults();
    setInterval(loadFaults, 5000);
  } catch (err) {
    document.getElementById('dacSection').style.display = 'none';
  }
}

document.getElementById('dacForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const payload = {
    dac: {
      analogGain: parseInt(document.getElementById('analogGain').value, 10),
      dacMode: document.getElementById('dacMode').value,
      modulation: {
        mode: document.getElementById('modMode').value,
        swFreqHz: parseInt(document.getElementById('swFreq').value, 10),
        bdFreqHz: parseInt(document.getElementById('bdFreq').value, 10)
      },
      mixerMode: document.getElementById('mixerMode').value,
      channelGainL: parseInt(document.getElementById('channelGainL').value, 10),
      channelGainR: parseInt(document.getElementById('channelGainR').value, 10),
      eq: {
        mode: document.getElementById('eqMode').value,
        profileL: document.getElementById('eqProfileL').value,
        profileR: document.getElementById('eqProfileR').value,
        gainL: eqBandsHz.map((_, i) => parseInt(document.getElementById('eqGainL' + i).value, 10)),
        gainR: eqBandsHz.map((_, i) => parseInt(document.getElementById('eqGainR' + i).value, 10))
      }
    }
  };
  const statusEl = document.getElementById('dacStatus');
  try {
    const res = await fetch('/api/dac/settings', { method: 'POST', body: JSON.stringify(payload) });
    if (!res.ok) {
      const err = await res.json();
      statusEl.textContent = 'Error: ' + (err.error || res.status);
      statusEl.className = 'error';
      return;
    }
    statusEl.textContent = 'Saved.';
    statusEl.className = 'ok';
  } catch (err) {
    statusEl.textContent = 'Error: ' + err.message;
    statusEl.className = 'error';
  }
});

load();
loadDac();
</script>
</body>
</html>
)HTMLPAGE";

}  // namespace snapclient
