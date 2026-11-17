// Compare raster output and particle commands against a supplied baseline HTML.
// Usage: node tools/spectra-check.cjs BEFORE.html AFTER.html
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const { performance } = require('perf_hooks');
function load(path) {
  let source = fs.readFileSync(path, 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
  source = source.replace(/        try \{ startWorkerPolling\(\); \} catch \(_\) \{ startMainThreadPolling\(\); \}(?:\s*render\(\);)?/, `
    globalThis.fixture = { acceptRAM, draw, resize,
      mode(c, e) { state.colorMode=c; state.effectMode=e; rebuildColorCache(); requestDraw(); },
      pixels() { return pixels; }, stats() { return [lastChanges,lastHotspots]; }
    };`);
  const elements = new Map(), ops = [];
  const ctx = { createImageData(w,h) { return {data:new Uint8ClampedArray(w*h*4)}; },
    putImageData() {}, fillRect(...args) { ops.push([args,this.globalAlpha,this.fillStyle,this.shadowColor,this.shadowBlur]); }, fillText() {} };
  function element(id) {
    if(!elements.has(id)) elements.set(id, {style:{},textContent:'',appendChild(){},getContext(){return ctx;}});
    return elements.get(id);
  }
  let seed = 42;
  const math = Object.create(Math); math.random=()=>((seed=(Math.imul(seed,1664525)+1013904223)>>>0)/4294967296);
  const sandbox = {console, Math:math, performance, Uint8Array,Uint8ClampedArray,Uint32Array,Int32Array,Float32Array,
    localStorage:{getItem(){return null;},setItem(){}},document:{getElementById:element,createElement(){return element(Symbol());},addEventListener(){}},
    window:{innerWidth:1280,innerHeight:720,addEventListener(){}},requestAnimationFrame(){},setTimeout,clearTimeout};
  vm.createContext(sandbox); vm.runInContext(source,sandbox);
  return {...sandbox.fixture, sandbox,ops};
}
if(process.argv.length !== 4) {
  console.error('Usage: node tools/spectra-check.cjs BEFORE.html AFTER.html'); process.exit(2);
}
assert.strictEqual(fs.readFileSync(process.argv[2],'utf8').split('<script>')[0],
                   fs.readFileSync(process.argv[3],'utf8').split('<script>')[0], 'HTML/CSS appearance');
const a=load(process.argv[2]),b=load(process.argv[3]);
let frames=0;
for(const [w,h,size] of [[1280,720,131072],[317,239,65536],[32,23,32768]]) {
  for(const f of [a,b]) {f.sandbox.window.innerWidth=w; f.sandbox.window.innerHeight=h; f.resize();}
  for(let effect=0;effect<4;effect++) for(let color=0;color<4;color++) {
    for(const f of [a,b]) f.mode(color,effect);
    for(let step=0;step<4;step++) {
      const ram=new Uint8Array(size);
      for(let i=0;i<size;i++)ram[i]=(i*37+step*113)%256;
      for(const f of [a,b])f.acceptRAM(ram);
      for(let draw=0;draw<3;draw++) {
        for(const f of [a,b]){f.ops.length=0;f.draw();}
        assert.deepStrictEqual(a.pixels(),b.pixels(),`pixels: ${w} ${effect} ${color} ${step}`);
        assert.strictEqual(JSON.stringify(a.ops),JSON.stringify(b.ops),'particle drawing');
        assert.deepStrictEqual(Array.from(a.stats()),Array.from(b.stats()));
        frames++;
      }
    }
    // Repeated snapshots must keep age/heat decay and cached colors identical.
    // Sparse changes also exercise unchanged cells beside newly flashing cells.
    const ram=new Uint8Array(size);
    for(let i=0;i<size;i++)ram[i]=(i*37+3*113)%256;
    for(let step=0;step<12;step++) {
      if(step===1) for(let i=0;i<size;i+=997) ram[i]^=255;
      for(const f of [a,b]) { f.acceptRAM(ram); f.ops.length=0; f.draw(); }
      assert.deepStrictEqual(a.pixels(),b.pixels(),`decay pixels: ${w} ${effect} ${color} ${step}`);
      assert.strictEqual(JSON.stringify(a.ops),JSON.stringify(b.ops),'decay particle drawing');
      assert.deepStrictEqual(Array.from(a.stats()),Array.from(b.stats()));
      frames++;
    }
  }
}
console.log(`PASS: ${frames} pixel-identical HTML frames; all effects/colors, resizes, zero-width tiles, cached redraws, particle draw operations, statistics`);
for(const f of [a,b]) {f.sandbox.window.innerWidth=1280;f.sandbox.window.innerHeight=720;f.resize();f.mode(0,1);}
const ram=new Uint8Array(131072);for(let i=0;i<ram.length;i++)ram[i]=i*37%256;
function bench(f,repeats){const start=performance.now();for(let i=0;i<repeats;i++){f.acceptRAM(ram);f.draw();}return (performance.now()-start)/repeats;}
bench(a,10);bench(b,10);const ta=bench(a,100),tb=bench(b,100);
console.log(`HTML 1280x720 WRAM Glow snapshot+draw: before ${ta.toFixed(3)} ms, after ${tb.toFixed(3)} ms (${(ta/tb).toFixed(2)}x)`);
