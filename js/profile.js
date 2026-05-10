// ═══════════════════════════════════════════════════════
// PROFILE META (project)
// ═══════════════════════════════════════════════════════
function updateProfileMeta() {
  state.profileMeta.project = document.getElementById('profile-project')?.value || '';
  state.profileMeta.author  = document.getElementById('profile-author')?.value  || '';
  state.profileMeta.org     = document.getElementById('profile-org')?.value ?? '';
  const prev = state.useGenericCbs;
  state.useGenericCbs = document.getElementById('profile-generic-cbs')?.checked || false;
  // When generic callbacks is turned OFF, purge any custom hook names that were
  // entered while it was ON — they have no meaning in per-characteristic stub mode
  // and must not leak into the generated XML.
  if (prev && !state.useGenericCbs) {
    state.services.forEach(svc => {
      (svc.chars || []).forEach(c => {
        c.customReadCb  = '';
        c.customWriteCb = '';
      });
    });
  }
  renderCode();
  renderEditor();
}

// ═══════════════════════════════════════════════════════
// PROFILE FREEZE
// ═══════════════════════════════════════════════════════
function toggleProfileFreeze() {
  state.profileFrozen = !state.profileFrozen;
  const body = document.getElementById('profileCardBody');
  const btn  = document.getElementById('profileFreezeBtn');
  if (!body || !btn) return;
  body.classList.toggle('frozen', state.profileFrozen);
  btn.classList.toggle('frozen',  state.profileFrozen);
  btn.textContent = state.profileFrozen ? '🔒' : '🔓';
  btn.title = state.profileFrozen ? 'Unfreeze profile settings' : 'Freeze profile settings';
}
