(function(root) {
  'use strict';
  const integer=(n,a,b)=>Number.isInteger(n)&&n>=a&&n<=b;
  const vector=(a,n,fn)=>Array.isArray(a)&&a.length===n&&a.every(fn);
  function validate(p) {
    if(!p||p.version!==1||!integer(p.theme,0,3)||!integer(p.uv,0,3)||!integer(p.glitch,0,3)||
       !integer(p.level,0,2)||!integer(p.mem,0,7)||!integer(p.ammo,0,255)||
       !integer(p.intensity,0,100)||!integer(p.selectedWall,0,3)||!integer(p.nextWallSlot,0,3)||
       !Number.isFinite(p.px)||!Number.isFinite(p.py)||p.px<1||p.px>=23||p.py<1||p.py>=23||
       !Number.isFinite(p.pa)||Math.abs(p.pa)>1000) throw new Error('Invalid world settings');
    if(!vector(p.world,3,m=>vector(m,24,row=>vector(row,24,n=>n===0||n===1||integer(n,128,132))))||
       !vector(p.materials,3,m=>vector(m,24,row=>vector(row,24,c=>vector(c,6,n=>integer(n,0,4))))))
      throw new Error('Invalid world geometry');
    for(const m of p.world)for(let i=0;i<24;i++)if(m[0][i]!==1||m[23][i]!==1||m[i][0]!==1||m[i][23]!==1)throw new Error('Missing world boundary');
    for(const [dx,dy] of [[-.18,-.18],[.18,-.18],[-.18,.18],[.18,.18]])
      if(p.world[p.level][Math.floor(p.py+dy)][Math.floor(p.px+dx)])throw new Error('Player position blocked');
    if(!vector(p.active,14,n=>n===0||n===1)||p.active.slice(0,7).filter(n=>!n).length!==p.mem)
      throw new Error('Invalid fragment collection');
    if(!Array.isArray(p.photos)||p.photos.length>4||!p.photos.every(photo=>vector(photo,4096,n=>integer(n,0,255))))
      throw new Error('Invalid photo textures');
    return p;
  }
  const key=slot=>{if(!integer(slot,0,3))throw new Error('Invalid patch slot');return 'wuw.bwo.patch.v1.'+slot;};
  function read(storage,slot){const value=storage.getItem(key(slot));return value===null?null:validate(JSON.parse(value));}
  function save(storage,slot,p){validate(p);storage.setItem(key(slot),JSON.stringify(p));}
  const api={validate,read,save};
  if(typeof module!=='undefined')module.exports=api;else root.WuwWorldPatches=api;
})(typeof window==='undefined'?globalThis:window);
