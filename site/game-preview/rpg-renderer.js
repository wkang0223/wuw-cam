import * as THREE from './vendor/three.module.min.js';

// Browser presentation shares the firmware map, photo slots and face order.
// All resources are local so a preview does not depend on a CDN.
class ShrineRenderer {
  constructor(host, art) {
    this.art = art;
    this.view = 'walk';
    this.version = -1;
    this.photoSources = [];
    this.photoMaterials = [];
    this.renderer = new THREE.WebGLRenderer({antialias:false, alpha:false, preserveDrawingBuffer:true});
    this.renderer.setPixelRatio(1);
    this.renderer.setSize(480,252,false);
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.renderer.domElement.id = 'rpg-scene';
    this.renderer.domElement.setAttribute('aria-label','BWO camera shrine world');
    host.prepend(this.renderer.domElement);
    this.scene = new THREE.Scene();
    this.scene.background = new THREE.Color(0x292f30);
    this.scene.fog = new THREE.Fog(0x292f30,8,20);
    this.scene.add(new THREE.HemisphereLight(0xe6f3db,0x5a4b64,2));
    const sun = new THREE.DirectionalLight(0xffe4b4,2.3);
    sun.position.set(-4,9,3);
    this.scene.add(sun);
    this.walk = new THREE.PerspectiveCamera(62,480/252,.05,30);
    this.iso = new THREE.OrthographicCamera(-5.2,5.2,2.73,-2.73,.05,40);
    this.world = new THREE.Group();
    this.scene.add(this.world);
    this.tileTextures = art.map(canvas=>this.texture(canvas));
    this.materials = this.tileTextures.map(map=>new THREE.MeshLambertMaterial({map}));
    this.spriteMaterials = this.tileTextures.map(map=>new THREE.SpriteMaterial({map,alphaTest:.5,transparent:false}));
    this.box = new THREE.BoxGeometry(1,1,1);
    this.floor = new THREE.PlaneGeometry(1,1);
    this.pillar = new THREE.CylinderGeometry(.16,.23,1.35,6);
    this.cap = new THREE.CylinderGeometry(.28,.28,.12,6);
    this.entities = [];
    this.player = new THREE.Sprite(this.spriteMaterials[0]);
    this.player.scale.set(1.1,1.1,1);
    this.scene.add(this.player);
    this.target = new THREE.Mesh(new THREE.PlaneGeometry(.91,.91),new THREE.MeshBasicMaterial({color:0xf3c26a,transparent:true,opacity:.35,depthWrite:false,side:THREE.DoubleSide}));
    this.target.rotation.x=-Math.PI/2;
    this.scene.add(this.target);
    this.lastCell='';
    this.spike=new THREE.ConeGeometry(.2,.65,4);
    this.fxTarget=new THREE.WebGLRenderTarget(480,252,{depthBuffer:true});
    this.fxScene=new THREE.Scene();
    this.fxCamera=new THREE.OrthographicCamera(-1,1,1,-1,0,1);
    this.fxMaterial=new THREE.ShaderMaterial({
      uniforms:{source:{value:this.fxTarget.texture},time:{value:0},strength:{value:0}},
      vertexShader:'varying vec2 vUv; void main(){vUv=uv;gl_Position=vec4(position.xy,0.,1.);}',
      fragmentShader:`uniform sampler2D source; uniform float time; uniform float strength; varying vec2 vUv;
        void main(){float band=step(.96,fract(vUv.y*17.+floor(time*5.)*.137));
        vec2 p=vec2(clamp(vUv.x+band*strength*.004,0.,1.),vUv.y);
        vec3 c=texture2D(source,p).rgb;
        c.r=texture2D(source,vec2(clamp(p.x+band*strength*.002,0.,1.),p.y)).r;
        gl_FragColor=vec4(c,1.);
        #include <tonemapping_fragment>
        #include <colorspace_fragment>
        }`
    });
    this.fxScene.add(new THREE.Mesh(new THREE.PlaneGeometry(2,2),this.fxMaterial));
    this.reducedMotion=window.matchMedia('(prefers-reduced-motion: reduce)');
  }
  texture(canvas) {
    const t=new THREE.CanvasTexture(canvas);
    t.colorSpace=THREE.SRGBColorSpace;
    t.magFilter=THREE.NearestFilter;
    t.minFilter=THREE.NearestFilter;
    t.generateMipmaps=false;
    return t;
  }
  toggleView() { this.view={diorama:'walk',walk:'panel',panel:'diorama'}[this.view]; return this.view; }
  syncPhotos(canvases) {
    let changed=false;
    while(this.photoMaterials.length>canvases.length){const m=this.photoMaterials.pop();m.map.dispose();m.dispose();this.photoSources.pop();changed=true;}
    for(let i=0;i<canvases.length;i++) if(this.photoSources[i]!==canvases[i]) {
      this.photoMaterials[i]?.map.dispose();
      this.photoMaterials[i]?.dispose();
      this.photoSources[i]=canvases[i];
      this.photoMaterials[i]=new THREE.MeshLambertMaterial({map:this.texture(canvases[i])});
      changed=true;
    }
    return changed;
  }
  syncUv(turn) {
    for(let i=0;i<this.tileTextures.length;i++)if(i%16>=8){this.tileTextures[i].center.set(.5,.5);this.tileTextures[i].rotation=-turn*Math.PI/2;}
    for(const m of this.photoMaterials){m.map.center.set(.5,.5);m.map.rotation=-turn*Math.PI/2;}
    this.uv=turn;
  }
  photo(code, fallback) { return code&&this.photoMaterials.length?this.photoMaterials[(code-1)%this.photoMaterials.length]:fallback; }
  surface(tile) { return this.materials[this.artBase + tile]; }
  rebuild(s) {
    this.theme=s.theme||0;
    this.artBase=this.theme===3?32:this.theme?16:0;
    const tones=[0x292f30,0x242d31,0x35151f,0x687d79];
    this.scene.background.setHex(tones[this.theme]);
    this.scene.fog.color.setHex(tones[this.theme]);
    this.world.children.forEach(mesh=>{if(mesh.isInstancedMesh)mesh.dispose()});
    this.world.clear();
    this.wallGroups=[];
    this.ceilingGroups=[];
    const map=s.world[s.level], faces=s.materials[s.level], buckets=Array.from({length:4},()=>[]), floors=[[],[]];
    const transform=new THREE.Object3D();
    for(let z=0;z<24;z++) for(let x=0;x<24;x++) {
      const cell=map[z][x],f=faces[z][x];
      let bottom=f[4],top=f[5];
      if(!bottom&&s.level>0&&(s.world[s.level-1][z][x]&128))bottom=s.materials[s.level-1][z][x][5];
      if(!top&&s.level<2&&(s.world[s.level+1][z][x]&128))top=s.materials[s.level+1][z][x][4];
      for(let side=0;side<2;side++) {
        const code=side?top:bottom, base=this.surface(side?13:s.level===1?14:12);
        if(code&&this.photoMaterials.length){
          const tile=new THREE.Mesh(this.floor,this.photo(code,base));
          tile.rotation.x=side?Math.PI/2:-Math.PI/2;
          tile.position.set(x+.5,side?1.4:0,z+.5);
          this.world.add(tile);
          if(side)this.ceilingGroups.push(tile);
        }else floors[side].push([x,z]);
      }
      if(!cell)continue;
      const region=(Math.floor(x/6)+Math.floor(z/6)+s.level+(this.theme===2?1:0))&3,base=this.surface(8+region);
      if(cell&128){
        // Three.js box material order: +X,-X,+Y,-Y,+Z,-Z.
        const mats=[1,3,5,4,2,0].map(face=>this.photo(f[face]||(face<4?cell&15:0),base));
        const cube=new THREE.Mesh(this.box,mats);
        cube.position.set(x+.5,.5,z+.5);
        this.world.add(cube);
      }else buckets[region].push([x,z]);
    }
    for(let i=0;i<4;i++) {
      const mesh=new THREE.InstancedMesh(this.box,this.surface(8+i),buckets[i].length);
      buckets[i].forEach(([x,z],j)=>{transform.position.set(x+.5,.5,z+.5);transform.rotation.set(0,0,0);transform.scale.set(1,1,1);transform.updateMatrix();mesh.setMatrixAt(j,transform.matrix)});
      this.world.add(mesh);
      this.wallGroups.push(mesh);
    }
    for(let i=0;i<2;i++) {
      const mesh=new THREE.InstancedMesh(this.floor,this.surface(i?13:s.level===1?14:12),floors[i].length);
      floors[i].forEach(([x,z],j)=>{transform.position.set(x+.5,i?1.4:0,z+.5);transform.rotation.set(i?Math.PI/2:-Math.PI/2,0,0);transform.updateMatrix();mesh.setMatrixAt(j,transform.matrix)});
      this.world.add(mesh);
      if(i)this.ceilingGroups.push(mesh);
    }
    for(let z=6;z<24;z+=6)for(let x=6;x<24;x+=6){
      const column=new THREE.Mesh(this.pillar,this.surface(8));column.position.set(x+.5,.67,z+.5);this.world.add(column);
      const crown=new THREE.Mesh(this.cap,this.surface(9));crown.position.set(x+.5,1.37,z+.5);this.world.add(crown);
      if(this.theme===1||this.theme===2){const spike=new THREE.Mesh(this.spike,this.surface(8));spike.position.set(x+.5,1.75,z+.5);this.world.add(spike);}
    }
    this.version=s.version;
    this.level=s.level;
  }
  render(s,t) {
    const photoChanged=this.syncPhotos(s.photos);
    if(photoChanged||this.uv!==(s.uv||0))this.syncUv(s.uv||0);
    if(photoChanged||s.version!==this.version||s.level!==this.level||this.theme!==(s.theme||0))this.rebuild(s);
    this.ceilingGroups.forEach(mesh=>mesh.visible=this.view==='walk');
    this.player.visible=this.view==='diorama';
    this.player.material=this.spriteMaterials[s.moving?Math.floor(t/160)%2:0];
    this.player.position.set(s.x,.57,s.y);
    for(let i=0;i<s.entities.length;i++) {
      if(!this.entities[i]){this.entities[i]=new THREE.Sprite(this.spriteMaterials[4]);this.scene.add(this.entities[i]);}
      const e=s.entities[i],sprite=this.entities[i];
      sprite.visible=!!e[3]&&e[4]===s.level;
      sprite.material=this.spriteMaterials[e[2]?2+Math.floor(t/240+i)%2:(this.artBase||32)+i%8];
      const size=e[2]?1.05:.6;sprite.scale.set(size,size,1);
      sprite.position.set(e[0],size/2+(e[2]?0:.1+Math.sin(t*.003+i)*.045),e[1]);
    }
    this.target.visible=s.building;
    this.target.position.set(Math.floor(s.x+Math.cos(s.angle)*1.5)+.5,.018,Math.floor(s.y+Math.sin(s.angle)*1.5)+.5);
    if(this.view==='walk'){
      this.walk.position.set(s.x,.63,s.y);
      this.walk.lookAt(s.x+Math.cos(s.angle),.61,s.y+Math.sin(s.angle));
    }else{
      this.iso.position.set(s.x-5.4,8.7,s.y+6.8);
      this.iso.lookAt(s.x,.15,s.y);
    }
    const camera=this.view==='walk'?this.walk:this.iso;
    if(s.glitch&&!this.reducedMotion.matches){
      this.fxMaterial.uniforms.time.value=t/1000;this.fxMaterial.uniforms.strength.value=s.glitch;
      this.renderer.setRenderTarget(this.fxTarget);this.renderer.render(this.scene,camera);
      this.renderer.setRenderTarget(null);this.renderer.render(this.fxScene,this.fxCamera);
    }else this.renderer.render(this.scene,camera);
  }
}

window.WuwShrineRenderer=ShrineRenderer;
window.dispatchEvent(new Event('wuw-renderer-ready'));
