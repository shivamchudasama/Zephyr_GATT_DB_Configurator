// ═══════════════════════════════════════════════════════
// RESIZABLE PANES
// ═══════════════════════════════════════════════════════
(function() {
  const layout = document.querySelector('.layout');
  const MIN = 160; // minimum pane width px

  function getColumns() {
    // returns [leftPx, rightPx] — middle is flexible
    const cols = getComputedStyle(layout).gridTemplateColumns.split(' ');
    return [parseFloat(cols[0]), parseFloat(cols[4])];
  }

  function makeDragger(resizerId, side) {
    const el = document.getElementById(resizerId);
    let startX, startLeft, startRight, totalW;

    el.addEventListener('mousedown', e => {
      e.preventDefault();
      el.classList.add('dragging');
      startX = e.clientX;
      [startLeft, startRight] = getColumns();
      totalW = layout.getBoundingClientRect().width - 8; // subtract both 4px resizers

      document.addEventListener('mousemove', onMove);
      document.addEventListener('mouseup', onUp);
    });

    function onMove(e) {
      const dx = e.clientX - startX;
      let left = startLeft, right = startRight;

      if (side === 'left') {
        left = Math.max(MIN, Math.min(startLeft + dx, totalW - right - MIN));
      } else {
        right = Math.max(MIN, Math.min(startRight - dx, totalW - left - MIN));
      }

      layout.style.gridTemplateColumns = `${left}px 4px 1fr 4px ${right}px`;
    }

    function onUp() {
      el.classList.remove('dragging');
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
    }
  }

  makeDragger('resizerLeft',  'left');
  makeDragger('resizerRight', 'right');
})();
