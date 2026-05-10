// ═══════════════════════════════════════════════════════
// APP INIT
// ═══════════════════════════════════════════════════════
function init() {
  // Sync theme button icon with saved preference
  const savedTheme = document.documentElement.getAttribute('data-theme');
  document.getElementById('themeToggle').textContent = savedTheme === 'light' ? '🌙' : '☀️';

  // Apply default frozen state to Profile card
  const body = document.getElementById('profileCardBody');
  const btn  = document.getElementById('profileFreezeBtn');
  if (body) body.classList.toggle('frozen', state.profileFrozen);
  if (btn)  { btn.classList.toggle('frozen', state.profileFrozen); btn.textContent = '🔒'; btn.title = 'Unfreeze profile settings'; }

  state.selectedSvcId = null;
  state.openSvcs = {};
  state.openChars = {};
  renderDomainList();
  refreshBaseUUIDDisplay();
  renderTree();
  renderEditor();
  renderCode();
}
init();
