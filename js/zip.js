// ═══════════════════════════════════════════════════════
// INLINE ZIP BUILDER (no external dependency)
// Produces a valid ZIP with DEFLATE-stored (method=0) entries.
// ═══════════════════════════════════════════════════════
const InlineZip = (() => {
  function crc32(buf) {
    const table = (() => {
      const t = new Uint32Array(256);
      for (let i = 0; i < 256; i++) {
        let c = i;
        for (let j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
        t[i] = c;
      }
      return t;
    })();
    let crc = 0xFFFFFFFF;
    for (let i = 0; i < buf.length; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >>> 8);
    return (crc ^ 0xFFFFFFFF) >>> 0;
  }

  function strToBytes(str) {
    return new TextEncoder().encode(str);
  }

  function u16(n) { const b = new Uint8Array(2); new DataView(b.buffer).setUint16(0, n, true); return b; }
  function u32(n) { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, n, true); return b; }

  function concat(...arrays) {
    const total = arrays.reduce((s, a) => s + a.length, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const a of arrays) { out.set(a, off); off += a.length; }
    return out;
  }

  function build(files) {
    // files: [{name, data (string)}]
    const entries = [];
    let offset = 0;

    for (const f of files) {
      const nameBytes = strToBytes(f.name);
      const dataBytes = strToBytes(f.data);
      const crc = crc32(dataBytes);
      const size = dataBytes.length;
      const dosDate = (() => {
        const d = new Date();
        return ((d.getFullYear() - 1980) << 9 | (d.getMonth() + 1) << 5 | d.getDate()) << 16 |
               (d.getHours() << 11 | d.getMinutes() << 5 | (d.getSeconds() >> 1));
      })();

      const local = concat(
        new Uint8Array([0x50,0x4B,0x03,0x04]), // local file header sig
        u16(20),        // version needed
        u16(0),         // general purpose bit flag
        u16(0),         // compression method: stored
        u32(dosDate),   // mod time + date
        u32(crc),       // crc-32
        u32(size),      // compressed size
        u32(size),      // uncompressed size
        u16(nameBytes.length),
        u16(0),         // extra field length
        nameBytes,
        dataBytes
      );

      entries.push({ nameBytes, crc, size, offset, local, dosDate });
      offset += local.length;
    }

    // Central directory
    const cdParts = [];
    for (const e of entries) {
      cdParts.push(concat(
        new Uint8Array([0x50,0x4B,0x01,0x02]), // central dir sig
        u16(20),         // version made by
        u16(20),         // version needed
        u16(0),          // flag
        u16(0),          // method: stored
        u32(e.dosDate),
        u32(e.crc),
        u32(e.size),
        u32(e.size),
        u16(e.nameBytes.length),
        u16(0),          // extra
        u16(0),          // comment
        u16(0),          // disk start
        u16(0),          // int attr
        u32(0),          // ext attr
        u32(e.offset),
        e.nameBytes
      ));
    }
    const cd = concat(...cdParts);
    const cdOffset = offset;
    const cdSize = cd.length;

    const eocd = concat(
      new Uint8Array([0x50,0x4B,0x05,0x06]),
      u16(0), u16(0),
      u16(entries.length),
      u16(entries.length),
      u32(cdSize),
      u32(cdOffset),
      u16(0)
    );

    const zip = concat(...entries.map(e => e.local), cd, eocd);
    return new Blob([zip], { type: 'application/zip' });
  }

  return { build };
})();
