// Optional independent integration test; caller supplies emulator and ROMs.
// CPU: https://to7.fr/static/to7/js/mc6809.js
// Monitor: https://to7.fr/static/to7/rom/to770.rom
// BASIC 1.0: a 16 KB cartridge image (not bundled with this project).
// Runs real BASIC's LOADM parser, then verifies all 16000 displayed bytes.
// Keyboard and cassette byte I/O are trapped, as in to7.fr's fast LEP mode.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const [tapePath, rawPath, cpuPath, romPath, basicPath] = process.argv.slice(2);
if (!basicPath) {
  throw new Error('Usage: node check_thomson_k7_loader.cjs picture.k7 picture.raw mc6809.js to770.rom basic-1.rom');
}
const tape = fs.readFileSync(tapePath);
const raw = fs.readFileSync(rawPath);
const basic = fs.readFileSync(basicPath);
const rom = fs.readFileSync(romPath);
assert.equal(raw.length, 16000);
assert.equal(basic.length, 16384);
assert.equal(rom.length, 6144);
vm.runInThisContext(fs.readFileSync(cpuPath, 'utf8') + '\nglobalThis.TestCPU = MC6809;');
const memory = new Uint8Array(65536);
const pages = [new Uint8Array(8192), new Uint8Array(8192)];
memory.set(rom, 0xe800);
memory.set(basic);
let port = 1;
const cpu = new TestCPU({}, address => {
  if (address >= 0x4000 && address < 0x6000) return pages[port & 1][address - 0x4000];
  if (address >= 0xe000 && address < 0xe7c0) return 255;
  return memory[address];
}, (address, value) => {
  if (address >= 0x4000 && address < 0x6000) pages[port & 1][address - 0x4000] = value;
  else if (address >= 0x6000 && address < 0xe000) memory[address] = value;
  if (address >= 0xe7c0 && address < 0xe800) memory[address] = value;
  if (address === 0xe7c3) port = value;
});
cpu.reset();
// Allow the monitor to initialize its RAM work area, then select BASIC's
// cold-start entry from the cartridge header, bypassing the boot menu.
for (let i = 0; i < 1000000; ++i) cpu.step();
cpu.set('PC', basic.readUInt16BE(0x1e));
cpu.set('SP', 0x9fff);
let tapePosition = 0;
let keyPosition = 0;
let finished = false;
const keys = 'LOADM"",,R\r';
function returnFromMonitor() {
  const sp = cpu.status().sp;
  cpu.set('PC', (memory[sp] << 8) | memory[sp + 1]);
  cpu.set('SP', sp + 2);
}
for (let i = 0; i < 10000000; ++i) {
  const pc = cpu.status().pc;
  if (pc === 0xe806) {
    assert(keyPosition < keys.length, 'BASIC requested more input instead of running the viewer');
    cpu.set('B', keys.charCodeAt(keyPosition++));
    returnFromMonitor();
    continue;
  }
  if (pc === 0xe809) {
    cpu.set('FLAGS', cpu.status().flags & 254); // no pending key
    returnFromMonitor();
    continue;
  }
  if (pc === 0xe815) {
    if (memory[0x6029] === 2) {
      assert(tapePosition < tape.length, 'BASIC read beyond end of tape');
      cpu.set('B', tape[tapePosition++]);
    }
    cpu.set('FLAGS', cpu.status().flags & 254);
    memory[0x602a] = memory[0x6029];
    returnFromMonitor();
    continue;
  }
  if (pc >= 0x8000 && pc < 0x8040 && memory[pc] === 0x20 && memory[pc + 1] === 0xfe) {
    finished = true;
    break;
  }
  cpu.step();
}
assert(finished, 'viewer did not finish within the instruction budget');
assert.equal(tapePosition, tape.length, 'BASIC did not consume the complete tape');
assert.equal(cpu.status().flags & 0x50, 0x50, 'viewer must disable cursor interrupts');
assert.deepEqual(Buffer.from(pages[0].subarray(0, 8000)), raw.subarray(0, 8000));
assert.deepEqual(Buffer.from(pages[1].subarray(0, 8000)), raw.subarray(8000));
console.log(`${tapePath}: real BASIC 1 LOADM and 6809 viewer replay passed (16000 screen bytes)`);
