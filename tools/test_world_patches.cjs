const test=require('node:test');
const assert=require('node:assert/strict');
const patches=require('../site/game-preview/world-patches.js');
function fixture(){return {
  version:1,theme:3,uv:2,glitch:1,level:0,px:3.5,py:3.5,pa:0,ammo:24,mem:1,intensity:92,selectedWall:0,nextWallSlot:1,
  world:Array.from({length:3},()=>Array.from({length:24},(_,y)=>Array.from({length:24},(_,x)=>x===0||y===0||x===23||y===23?1:0))),
  materials:Array.from({length:3},()=>Array.from({length:24},()=>Array.from({length:24},()=>[0,0,0,0,0,0]))),
  active:[0,...Array(13).fill(1)],photos:[Array.from({length:4096},(_,i)=>i%256)]
};}
test('self-contained world, photo faces and collection survive serialization',()=>{
  const data=new Map(),storage={getItem:k=>data.get(k)??null,setItem:(k,v)=>data.set(k,v)},p=fixture();
  p.world[0][5][5]=129;p.materials[0][5][5]=[1,0,1,0,1,0];
  patches.save(storage,0,p);assert.deepEqual(patches.read(storage,0),p);assert.equal(patches.read(storage,1),null);
});
test('a newly placed wall can remain unpainted when photos are added later',()=>{
  const data=new Map(),storage={getItem:k=>data.get(k)??null,setItem:(k,v)=>data.set(k,v)},p=fixture();
  p.world[0][3][5]=128;p.photos=[];p.materials[0][3][5]=[0,0,0,0,0,0];
  patches.save(storage,1,p);
  const loaded=patches.read(storage,1);
  loaded.photos.push(Array(4096).fill(255));
  assert.equal(loaded.world[0][3][5],128);
  assert.deepEqual(loaded.materials[0][3][5],[0,0,0,0,0,0]);
});
test('reject malformed geometry, unsafe spawn, invalid pixels and progress',()=>{
  for(const mutate of [p=>p.world.pop(),p=>p.world[0][0][2]=0,p=>p.world[0][3][3]=1,p=>p.photos[0][0]=256,p=>p.photos[0].pop(),p=>p.px=NaN,p=>p.mem=7,p=>p.uv=4,p=>p.materials[0][2][2][0]=5]){
    const p=fixture();mutate(p);assert.throws(()=>patches.validate(p));
  }
});
test('invalid saves and storage failures never replace a previous patch',()=>{
  let saved=JSON.stringify(fixture());
  const storage={getItem:()=>saved,setItem:()=>{throw new Error('Quota exceeded')}};
  assert.throws(()=>patches.save(storage,0,fixture()),/Quota/);assert.equal(patches.read(storage,0).theme,3);
  const bad=fixture();bad.theme=8;assert.throws(()=>patches.save(storage,0,bad),/Invalid/);
  assert.throws(()=>patches.read(storage,4),/slot/);
});
