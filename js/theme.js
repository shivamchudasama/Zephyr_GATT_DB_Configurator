// ═══════════════════════════════════════════════════════
// THEME TOGGLE
// ═══════════════════════════════════════════════════════
function toggleTheme() {
  const html = document.documentElement;
  const isLight = html.getAttribute('data-theme') === 'light';
  const next = isLight ? 'dark' : 'light';
  html.setAttribute('data-theme', next);
  document.getElementById('themeToggle').textContent = next === 'light' ? '🌙' : '☀️';
  localStorage.setItem('gatt-theme', next);
}

(function applySavedTheme() {
  const saved = localStorage.getItem('gatt-theme');
  if (saved === 'light') {
    document.documentElement.setAttribute('data-theme', 'light');
    // Button text will be set after DOM is ready in init()
  }
})();
