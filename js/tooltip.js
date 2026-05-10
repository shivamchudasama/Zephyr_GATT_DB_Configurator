// ═══════════════════════════════════════════════════════
// TOOLTIP ENGINE
// ═══════════════════════════════════════════════════════
(function() {
  function initTooltip() {
    const TIP_GAP   = 8;
    const CARET_H   = 5;
    const MARGIN    = 10;

    const box = document.getElementById('gatt-tooltip');
    if (!box) return;
    let hideTimer = null;

    function show(anchor, text) {
      clearTimeout(hideTimer);
      box.textContent = text;
      box.classList.add('visible');

      requestAnimationFrame(() => {
        const ar  = anchor.getBoundingClientRect();
        const bw  = box.offsetWidth;
        const bh  = box.offsetHeight;
        const vw  = window.innerWidth;

        let top = ar.top - bh - CARET_H - TIP_GAP;
        if (top < MARGIN) top = ar.bottom + CARET_H + TIP_GAP;

        let left = ar.left + ar.width / 2 - bw / 2;
        left = Math.max(MARGIN, Math.min(left, vw - bw - MARGIN));

        const caretIdeal   = ar.left + ar.width / 2 - left;
        const caretClamped = Math.max(12, Math.min(caretIdeal, bw - 12));

        box.style.cssText = `top:${top}px;left:${left}px;--caret-x:${caretClamped}px;`;
      });
    }

    function hide() {
      hideTimer = setTimeout(() => box.classList.remove('visible'), 80);
    }

    document.addEventListener('mouseover', e => {
      const el = e.target.closest('[data-tooltip]');
      if (el) show(el, el.getAttribute('data-tooltip'));
    });
    document.addEventListener('mouseout', e => {
      const el = e.target.closest('[data-tooltip]');
      if (el) hide();
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initTooltip);
  } else {
    initTooltip();
  }
})();
