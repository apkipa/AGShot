import type { ToastKind } from './types';

interface ToastEntry {
  kind: ToastKind;
  message: string;
  remaining: number;
  startedAt: number;
  removed: boolean;
  timer: number;
  el: HTMLDivElement;
}

const MAX_VISIBLE = 3;
const GAP = 8;

function createToastIcon(kind: ToastKind): SVGSVGElement {
  const svgNS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(svgNS, 'svg');
  svg.setAttribute('viewBox', '0 0 16 16');
  svg.setAttribute('width', '16');
  svg.setAttribute('height', '16');
  svg.setAttribute('fill', 'none');

  const path = document.createElementNS(svgNS, 'path');
  path.setAttribute('stroke', 'currentColor');
  path.setAttribute('stroke-width', '1.6');
  path.setAttribute('stroke-linecap', 'round');
  path.setAttribute('stroke-linejoin', 'round');

  if (kind === 'ok') {
    const c = document.createElementNS(svgNS, 'circle');
    c.setAttribute('cx', '8');
    c.setAttribute('cy', '8');
    c.setAttribute('r', '6.2');
    c.setAttribute('stroke', 'currentColor');
    c.setAttribute('stroke-width', '1.5');
    svg.appendChild(c);
    path.setAttribute('d', 'M5.4 8.1l1.8 1.8 3.5-3.6');
  } else if (kind === 'warn') {
    path.setAttribute('d', 'M8 2.4L14 13H2L8 2.4ZM8 6.4v3.1');
    const dot = document.createElementNS(svgNS, 'circle');
    dot.setAttribute('cx', '8');
    dot.setAttribute('cy', '11.4');
    dot.setAttribute('r', '0.75');
    dot.setAttribute('fill', 'currentColor');
    svg.appendChild(dot);
  } else {
    const c = document.createElementNS(svgNS, 'circle');
    c.setAttribute('cx', '8');
    c.setAttribute('cy', '8');
    c.setAttribute('r', '6.2');
    c.setAttribute('stroke', 'currentColor');
    c.setAttribute('stroke-width', '1.5');
    svg.appendChild(c);
    path.setAttribute('d', 'M6 6l4 4M10 6l-4 4');
  }
  svg.appendChild(path);
  return svg;
}

export function createToastDeck(stackEl: HTMLElement) {
  const toasts: ToastEntry[] = [];
  let expanded = false;

  function layout() {
    const frontH = toasts.length ? toasts[0].el.offsetHeight : 0;
    let offsetY = 0;

    toasts.forEach((entry, idx) => {
      const el = entry.el;
      const h = el.offsetHeight || frontH;
      const visible = idx < MAX_VISIBLE;

      el.style.zIndex = String(100 - idx);
      if (expanded) {
        el.style.setProperty('--toast-y', `${-offsetY}px`);
        el.style.setProperty('--toast-scale', '1');
        el.style.setProperty('--toast-opacity', visible ? '1' : '0');
        offsetY += h + GAP;
      } else {
        const step = Math.min(idx, MAX_VISIBLE - 1);
        el.style.setProperty('--toast-y', `${-step * 11}px`);
        el.style.setProperty('--toast-scale', String(1 - step * 0.05));
        const op = !visible ? 0 : idx === 0 ? 1 : idx === 1 ? 0.9 : 0.78;
        el.style.setProperty('--toast-opacity', String(op));
        if (idx > 0 && frontH > 0) {
          el.style.height = `${frontH}px`;
          el.style.overflow = 'hidden';
        } else {
          el.style.height = '';
          el.style.overflow = '';
        }
      }
      el.style.pointerEvents = visible ? 'auto' : 'none';
    });

    if (expanded && toasts.length > 0) {
      stackEl.classList.add('expanded');
      stackEl.style.height = `${Math.max(frontH, offsetY - GAP)}px`;
    } else {
      stackEl.classList.remove('expanded');
      stackEl.style.height = '0px';
    }
  }

  function remove(entry: ToastEntry) {
    if (entry.removed) return;
    entry.removed = true;
    clearTimeout(entry.timer);
    entry.timer = 0;
    const idx = toasts.indexOf(entry);
    if (idx >= 0) toasts.splice(idx, 1);

    entry.el.style.pointerEvents = 'none';
    entry.el.classList.remove('mounted', 'swiping');
    entry.el.classList.add('leaving');
    if (toasts.length === 0) expanded = false;
    layout();

    const cleanup = () => entry.el.remove();
    entry.el.addEventListener('transitionend', cleanup, { once: true });
    setTimeout(cleanup, 320);
  }

  stackEl.addEventListener('mouseenter', () => {
    if (toasts.length === 0) return;
    expanded = true;
    const now = performance.now();
    for (const t of toasts) {
      if (t.timer) {
        clearTimeout(t.timer);
        t.timer = 0;
        t.remaining = Math.max(400, t.remaining - (now - t.startedAt));
      }
      t.el.style.height = '';
      t.el.style.overflow = '';
    }
    layout();
  });

  stackEl.addEventListener('mouseleave', () => {
    expanded = false;
    layout();
    const now = performance.now();
    for (const t of toasts) {
      if (!t.removed && !t.timer) {
        t.startedAt = now;
        t.timer = window.setTimeout(() => remove(t), t.remaining);
      }
    }
  });

  return function push(kind: ToastKind, message: string) {
    if (!message) return;
    const duration = kind === 'ok' ? 3000 : 5000;
    while (toasts.length >= MAX_VISIBLE) remove(toasts[toasts.length - 1]);

    const el = document.createElement('div');
    el.className = `toast ${kind}`;

    const icon = document.createElement('span');
    icon.className = 'toast-icon';
    icon.setAttribute('aria-hidden', 'true');
    icon.appendChild(createToastIcon(kind));

    const textEl = document.createElement('span');
    textEl.className = 'toast-text';
    textEl.textContent = message;

    const closeBtn = document.createElement('button');
    closeBtn.className = 'toast-close';
    closeBtn.type = 'button';
    closeBtn.setAttribute('aria-label', 'Dismiss');
    const svgNS = 'http://www.w3.org/2000/svg';
    const closeSvg = document.createElementNS(svgNS, 'svg');
    closeSvg.setAttribute('viewBox', '0 0 12 12');
    closeSvg.setAttribute('width', '12');
    closeSvg.setAttribute('height', '12');
    closeSvg.setAttribute('fill', 'none');
    const closePath = document.createElementNS(svgNS, 'path');
    closePath.setAttribute('d', 'M3 3l6 6M9 3l-6 6');
    closePath.setAttribute('stroke', 'currentColor');
    closePath.setAttribute('stroke-width', '1.5');
    closePath.setAttribute('stroke-linecap', 'round');
    closeSvg.appendChild(closePath);
    closeBtn.appendChild(closeSvg);

    el.append(icon, textEl, closeBtn);
    stackEl.appendChild(el);

    const entry: ToastEntry = {
      kind,
      message,
      remaining: duration,
      startedAt: performance.now(),
      removed: false,
      timer: 0,
      el
    };
    toasts.unshift(entry);

    closeBtn.addEventListener('pointerdown', (e) => e.stopPropagation());
    closeBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      remove(entry);
    });

    let startY = 0;
    let dy = 0;
    let swiping = false;
    el.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || (e.target as Element | null)?.closest('.toast-close')) return;
      swiping = true;
      startY = e.clientY;
      dy = 0;
      el.classList.add('swiping');
      el.setPointerCapture(e.pointerId);
    });
    el.addEventListener('pointermove', (e) => {
      if (!swiping) return;
      dy = Math.max(0, e.clientY - startY);
      el.style.setProperty('--swipe-y', `${dy}px`);
      el.style.opacity = String(Math.max(0.2, 1 - dy / 80));
    });
    const finishSwipe = () => {
      if (!swiping) return;
      swiping = false;
      el.classList.remove('swiping');
      if (dy >= 28) {
        remove(entry);
      } else {
        el.style.setProperty('--swipe-y', '0px');
        el.style.opacity = '';
      }
    };
    el.addEventListener('pointerup', finishSwipe);
    el.addEventListener('pointercancel', finishSwipe);

    if (!expanded) {
      entry.startedAt = performance.now();
      entry.timer = window.setTimeout(() => remove(entry), entry.remaining);
    }

    layout();
    requestAnimationFrame(() => {
      if (!entry.removed) el.classList.add('mounted');
    });
  };
}
