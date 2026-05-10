// ═══════════════════════════════════════════════════════
// STATE
// ═══════════════════════════════════════════════════════
let state = {
  services: [],
  selectedSvcId: null,
  selectedCharId: null,
  activeTab: 'h',
  openSvcs: {},
  openChars: {},
  baseUUID: { p2: '', p3: '', p4: '', p5: '' },
  profileFrozen: true,
  profileMeta: { project: '', author: '', org: '' },
  useGenericCbs: false,
  domains: [
    { id: 1, name: 'MY_DOMAIN', hex: '0x01' },
  ],
};
let nextDomainId = 2;

let nextSvcId = 1;
let nextCharId = 1;
