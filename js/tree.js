// ═══════════════════════════════════════════════════════
// SIDEBAR TREE
// ═══════════════════════════════════════════════════════
function renderTree() {
  const tree = document.getElementById('serviceTree');
  let html = '';
  html += `<div class="section-divider" style="margin:4px 2px 6px;">
    <div class="section-divider-line"></div>
    <div class="section-divider-label">GATT Database</div>
    <div class="section-divider-line"></div>
  </div>`;
  state.services.forEach(svc => {
    const sel = svc.id === state.selectedSvcId;
    const open = state.openSvcs[svc.id];
    html += `<div class="svc-node ${sel?'selected':''}" data-svcid="${svc.id}">
      <div class="svc-header" onclick="selectSvc(${svc.id})">
        <span class="svc-icon svc-icon-badge">S</span>
        <span class="svc-name">${svc.name}</span>
        <span class="svc-toggle ${open?'open':''}">▶</span>
      </div>`;
    if (open) {
      html += `<div class="svc-chars">`;
      (svc.chars||[]).forEach(c => {
        const selc = c.id === state.selectedCharId;
        const badgeDefs = [
          { key: 'broadcast',  label: 'B',   cls: 'badge-broadcast' },
          { key: 'read',       label: 'R',   cls: 'badge-read'      },
          { key: 'write',      label: 'W',   cls: 'badge-write'     },
          { key: 'writeNoResp',label: 'WNR', cls: 'badge-write-nr'  },
          { key: 'notify',     label: 'N',   cls: 'badge-notify'    },
          { key: 'indicate',   label: 'I',   cls: 'badge-indicate'  },
          { key: 'auth',       label: 'A',   cls: 'badge-auth'      },
          { key: 'extProp',    label: 'EXT', cls: 'badge-ext-prop'  },
        ];
        const activeBadges = badgeDefs
          .filter(b => c.props[b.key])
          .map(b => `<div class="char-badge ${b.cls}" title="${b.key}">${b.label}</div>`)
          .join('');
        html += `<div class="char-node ${selc?'selected':''}" onclick="selectChar(${svc.id},${c.id})">
          <div class="char-badge">C</div>
          <span class="char-name">${c.name}</span>
          <div class="char-badges">${activeBadges}</div>
        </div>`;
      });
      html += `<div class="add-svc-btn" onclick="openAddChar(${svc.id})">+ Add Characteristic</div>`;
      html += `</div>`;
    }
    html += `</div>`;
  });
  html += `<div class="add-svc-btn" onclick="openAddService()">+ Add Service</div>`;
  tree.innerHTML = html;
  updateStatus();
}

function updateStatus() {
  const total = state.services.reduce((a,s)=>a+(s.chars||[]).length,0);
  document.getElementById('statServices').textContent = `Services: ${state.services.length}`;
  document.getElementById('statChars').textContent = `Characteristics: ${total}`;
}

// ═══════════════════════════════════════════════════════
// SELECT
// ═══════════════════════════════════════════════════════
function selectSvc(id) {
  state.openSvcs[id] = !state.openSvcs[id];
  if (state.openSvcs[id]) {
    state.selectedSvcId = id;
    state.selectedCharId = null;
  }
  renderTree();
  renderEditor();
  renderCode();
}
function selectChar(svcId, charId) {
  state.selectedSvcId = svcId;
  state.selectedCharId = charId;
  state.openSvcs[svcId] = true;
  renderTree();
  renderEditor();
  renderCode();
}
