import './app.css';
import { createToastDeck } from './ToastDeck';
import type { HostMessage, HostState, TabId } from './types';

const host = (window as unknown as {
  chrome?: {
    webview?: {
      postMessage(data: unknown): void;
      addEventListener(type: 'message', listener: (ev: MessageEvent<HostMessage>) => void): void;
    };
  };
}).chrome?.webview;

const send = (payload: Record<string, unknown>) => host?.postMessage(payload);
const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

let recording = false;
let currentData: HostState | null = null;

const navButtons = Array.from(document.querySelectorAll<HTMLButtonElement>('.nav-btn[data-tab]'));
const tabPanes = Array.from(document.querySelectorAll<HTMLElement>('.tab-pane'));
const scroller = $<HTMLDivElement>('scroll-content');
const track = $<HTMLDivElement>('overlay-track');
const thumb = $<HTMLDivElement>('overlay-thumb');
const pushToast = createToastDeck($<HTMLDivElement>('toast-stack'));

let hideScrollTimer = 0;
let dragStartY = 0;
let dragStartScroll = 0;
let dragging = false;

function syncOverlayScrollbar(reveal: boolean) {
  const viewH = scroller.clientHeight;
  const fullH = scroller.scrollHeight;
  const trackH = track.clientHeight;
  if (fullH <= viewH + 1 || trackH <= 0) {
    track.classList.remove('scrollable', 'visible');
    return;
  }
  track.classList.add('scrollable');
  const thumbH = Math.max(28, Math.round((viewH / fullH) * trackH));
  const maxScroll = Math.max(1, fullH - viewH);
  const maxTop = Math.max(0, trackH - thumbH);
  thumb.style.height = `${thumbH}px`;
  thumb.style.transform = `translateY(${Math.round((scroller.scrollTop / maxScroll) * maxTop)}px)`;

  if (reveal) {
    track.classList.add('visible');
    clearTimeout(hideScrollTimer);
    hideScrollTimer = window.setTimeout(() => {
      if (!dragging && !track.matches(':hover')) track.classList.remove('visible');
    }, 900);
  }
}

scroller.addEventListener('scroll', () => syncOverlayScrollbar(true), { passive: true });
window.addEventListener('resize', () => syncOverlayScrollbar(false));
document.querySelector('.main')?.addEventListener('mousemove', (e) => {
  if (scroller.getBoundingClientRect().right - (e as MouseEvent).clientX <= 28) syncOverlayScrollbar(true);
}, { passive: true });

thumb.addEventListener('pointerdown', (e) => {
  if (e.button !== 0) return;
  e.preventDefault();
  dragging = true;
  dragStartY = e.clientY;
  dragStartScroll = scroller.scrollTop;
  track.classList.add('dragging', 'visible');
  thumb.setPointerCapture(e.pointerId);
});
thumb.addEventListener('pointermove', (e) => {
  if (!dragging) return;
  const span = Math.max(1, track.clientHeight - thumb.offsetHeight);
  const maxScroll = Math.max(0, scroller.scrollHeight - scroller.clientHeight);
  scroller.scrollTop = dragStartScroll + ((e.clientY - dragStartY) / span) * maxScroll;
});
const endDrag = () => {
  if (!dragging) return;
  dragging = false;
  track.classList.remove('dragging');
  syncOverlayScrollbar(true);
};
thumb.addEventListener('pointerup', endDrag);
thumb.addEventListener('pointercancel', endDrag);

function switchTab(name: TabId | string) {
  const valid: TabId = name === 'ocr' || name === 'storage' || name === 'about' ? name : 'general';
  navButtons.forEach((b) => b.classList.toggle('active', b.dataset.tab === valid));
  tabPanes.forEach((p) => p.classList.toggle('active', p.id === `tab-${valid}`));
  scroller.scrollTop = 0;
  try { localStorage.setItem('agshot.settings.tab', valid); } catch {}
  requestAnimationFrame(() => syncOverlayScrollbar(false));
}

navButtons.forEach((b) => b.addEventListener('click', () => switchTab(b.dataset.tab || 'general')));
try { switchTab(localStorage.getItem('agshot.settings.tab') || 'general'); } catch { switchTab('general'); }

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

function renderState(s: HostState) {
  currentData = s;

  const errBanner = $<HTMLDivElement>('config-error-banner');
  if (!s.config.configOk && s.config.configError) {
    errBanner.className = 'banner show err';
    errBanner.textContent = `Could not parse config.toml (defaults in use): ${s.config.configError}`;
  } else {
    errBanner.className = 'banner';
    errBanner.textContent = '';
  }

  const hotkey = s.config.hotkey || 'F1';
  if (!recording) $<HTMLButtonElement>('hotkey-btn').textContent = hotkey;
  $<HTMLButtonElement>('hotkey-reset-btn').disabled = recording || hotkey === 'F1';

  const hint = $<HTMLDivElement>('hotkey-hint');
  if (recording) {
    hint.className = 'card-desc';
    hint.textContent = 'Press any key combination, or Esc to cancel.';
  } else if (s.runtime.mainRunning && !s.runtime.hotkeyPaused && !s.runtime.hotkeyRegistered) {
    hint.className = 'card-desc err';
    hint.textContent = 'Unavailable: another application already holds this shortcut.';
  } else {
    hint.className = 'card-desc';
    hint.textContent = 'Click to record a new shortcut. Press Esc to cancel.';
  }

  $<HTMLInputElement>('debug-stats-toggle').checked = !!s.config.debugShowStats;

  const ocrBadge = $<HTMLSpanElement>('ocr-status-badge');
  const ocrDesc = $<HTMLDivElement>('ocr-status-desc');
  if (!s.ocr.modelsFound) {
    ocrBadge.className = 'badge warn';
    ocrBadge.textContent = 'Models missing';
    ocrDesc.textContent = 'Place OCR models in the models folder to enable text recognition.';
  } else if (!s.ocr.manifestOk) {
    ocrBadge.className = 'badge err';
    ocrBadge.textContent = 'Model error';
    ocrDesc.textContent = s.ocr.manifestError || 'The OCR model pack could not be verified.';
  } else if (s.runtime.mainRunning && s.runtime.ocrError) {
    ocrBadge.className = 'badge err';
    ocrBadge.textContent = 'Init error';
    ocrDesc.textContent = 'OCR failed to initialize on the last capture attempt.';
  } else if (s.runtime.mainRunning && s.runtime.ocrLoaded) {
    ocrBadge.className = 'badge ok';
    ocrBadge.textContent = 'In memory';
    ocrDesc.textContent = 'Models are currently loaded and will unload automatically when idle.';
  } else {
    ocrBadge.className = 'badge ok';
    ocrBadge.textContent = 'Ready';
    ocrDesc.textContent = s.ocr.packVersion
      ? `Model pack ${s.ocr.packVersion} · loads on demand.`
      : 'Models verified · loads on demand.';
  }

  $<HTMLButtonElement>('trim-ocr-btn').classList.toggle('hidden', !(s.runtime.mainRunning && s.runtime.ocrLoaded));
  $<HTMLSpanElement>('ocr-cache-badge').textContent = formatBytes(s.ocr.cacheBytes);
  $<HTMLButtonElement>('clear-cache-btn').disabled = !(s.ocr.cacheFiles > 0);

  const modeBadge = $<HTMLSpanElement>('storage-mode-badge');
  modeBadge.className = s.storage.portable ? 'badge ok' : 'badge';
  modeBadge.textContent = s.storage.portable ? 'Portable' : 'AppData';
  $<HTMLDivElement>('data-dir-path').textContent = s.storage.dataDir || '';
  $<HTMLSpanElement>('about-version').textContent = s.version || 'dev';

  requestAnimationFrame(() => syncOverlayScrollbar(false));
}

function mapBrowserKey(e: KeyboardEvent): string | null {
  const code = e.code || '';
  const key = e.key || '';
  if (
    ['ControlLeft', 'ControlRight', 'ShiftLeft', 'ShiftRight', 'AltLeft', 'AltRight', 'MetaLeft', 'MetaRight'].includes(code) ||
    ['Control', 'Shift', 'Alt', 'Meta'].includes(key)
  ) return null;
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(code)) return code;
  if (/^Key([A-Z])$/.test(code)) return code.slice(3);
  if (/^Digit([0-9])$/.test(code)) return code.slice(5);
  const named: Record<string, string> = {
    Backspace: 'Backspace', Tab: 'Tab', Enter: 'Enter', Space: 'Space',
    PageUp: 'PageUp', PageDown: 'PageDown', End: 'End', Home: 'Home',
    ArrowLeft: 'Left', ArrowUp: 'Up', ArrowRight: 'Right', ArrowDown: 'Down',
    Insert: 'Insert', Delete: 'Delete', PrintScreen: 'PrintScreen',
    CapsLock: 'CapsLock', NumLock: 'NumLock', ScrollLock: 'ScrollLock', Pause: 'Pause'
  };
  if (named[code]) return named[code];
  if (key === 'PrintScreen') return 'PrintScreen';
  if (/^[a-zA-Z0-9]$/.test(key)) return key.toUpperCase();
  return null;
}

function collectModifiers(e: KeyboardEvent): string[] {
  const parts: string[] = [];
  if (e.ctrlKey) parts.push('Ctrl');
  if (e.shiftKey) parts.push('Shift');
  if (e.altKey) parts.push('Alt');
  if (e.metaKey) parts.push('Win');
  return parts;
}

function buildCombo(e: KeyboardEvent): string | null {
  const k = mapBrowserKey(e);
  if (!k) return null;
  const parts = collectModifiers(e);
  parts.push(k);
  return parts.join('+');
}

function previewModifiers(e: KeyboardEvent): string {
  const parts = collectModifiers(e);
  return parts.length ? `${parts.join('+')}+...` : 'Press a key...';
}

const hotkeyBtn = $<HTMLButtonElement>('hotkey-btn');
function setRecording(active: boolean) {
  if (recording === active) return;
  recording = active;
  hotkeyBtn.classList.toggle('recording', active);
  if (active) {
    hotkeyBtn.textContent = 'Press a key...';
    send({ type: 'hotkeyPause', paused: true });
  } else {
    hotkeyBtn.textContent = currentData?.config.hotkey || 'F1';
    send({ type: 'hotkeyPause', paused: false });
  }
  if (currentData) renderState(currentData);
}

function commitHotkey(combo: string) {
  recording = false;
  hotkeyBtn.classList.remove('recording');
  hotkeyBtn.textContent = combo;
  send({ type: 'setHotkey', value: combo });
}

hotkeyBtn.addEventListener('click', () => setRecording(!recording));
hotkeyBtn.addEventListener('blur', () => recording && setRecording(false));

window.addEventListener('keydown', (e) => {
  if (!recording) return;
  e.preventDefault();
  e.stopPropagation();
  if (e.key === 'Escape' && !e.ctrlKey && !e.shiftKey && !e.altKey && !e.metaKey) {
    setRecording(false);
    return;
  }
  const combo = buildCombo(e);
  if (!combo) {
    hotkeyBtn.textContent = previewModifiers(e);
    return;
  }
  commitHotkey(combo);
}, true);

window.addEventListener('keyup', (e) => {
  if (!recording) return;
  if (e.key === 'PrintScreen' || e.code === 'PrintScreen') {
    e.preventDefault();
    e.stopPropagation();
    const combo = buildCombo(e);
    if (combo) commitHotkey(combo);
    return;
  }
  hotkeyBtn.textContent = previewModifiers(e);
}, true);

$<HTMLButtonElement>('hotkey-reset-btn').addEventListener('click', () => send({ type: 'setHotkey', value: 'F1' }));
$<HTMLInputElement>('debug-stats-toggle').addEventListener('change', (e) =>
  send({ type: 'setDebugStats', value: (e.currentTarget as HTMLInputElement).checked })
);
$<HTMLButtonElement>('trim-ocr-btn').addEventListener('click', () => send({ type: 'trimOcrMemory' }));
$<HTMLButtonElement>('clear-cache-btn').addEventListener('click', () => send({ type: 'clearOcrCache' }));
$<HTMLButtonElement>('open-models-btn').addEventListener('click', () => send({ type: 'openModelsFolder' }));
$<HTMLButtonElement>('open-config-btn').addEventListener('click', () => send({ type: 'openConfig' }));
$<HTMLButtonElement>('open-data-btn').addEventListener('click', () => send({ type: 'openDataFolder' }));
$<HTMLAnchorElement>('repo-link').addEventListener('click', (e) => {
  e.preventDefault();
  send({ type: 'openRepo' });
});

host?.addEventListener('message', (event) => {
  const msg = event.data;
  if (!msg || typeof msg !== 'object') return;
  if (msg.type === 'state') {
    renderState(msg);
  } else if (msg.type === 'hotkeyResult') {
    if (msg.state) renderState(msg.state);
    pushToast(msg.ok ? 'ok' : 'err', msg.message);
  } else if (msg.type === 'actionResult') {
    if (msg.state) renderState(msg.state);
    if (msg.message) pushToast(msg.ok ? 'ok' : 'warn', msg.message);
  }
});

syncOverlayScrollbar(false);
send({ type: 'init' });
