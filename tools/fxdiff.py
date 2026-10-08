#!/usr/bin/env python3
"""Render every effect in two engines and diff them.

The shader is one 20 KB string in main_s3.cpp that nothing ever type-checks, and
desktop Chrome is the only engine it has been run in. iOS Safari uses a
different GPU, a different driver and Metal underneath, and treats the
undefined corners of GLSL differently: normalize(vec2(0)), smoothstep with its
edges reversed, pow() of a negative, clamp() with min > max. Desktop quietly
returns a plausible number; Apple returns NaN, and the effect comes out black
or white. Nothing fails to compile -- which is why "some effects don't work on
my iPhone" is invisible from a laptop.

    python3 tools/fxdiff.py serve          # serves the page, collects reports
    python3 tools/fxdiff.py report         # compares what was collected

Run the page under any two engines with ?tag=NAME, e.g. headless Chrome as the
reference and the iOS Simulator as the subject.
"""
import json, os, re, sys, pathlib, http.server, socketserver, urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "main_s3.cpp"
OUT = ROOT / ".fxcheck"
PORT = 8765

def lift(text, start_marker, end_re):
    i = text.index(start_marker)
    lines, out = text[i:].split("\n"), []
    for ln in lines:
        out.append(ln)
        if re.search(end_re, ln.rstrip()) and len(out) > 2:
            return "\n".join(out)
    raise SystemExit("no end for " + start_marker)

HTML = r"""<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>fxdiff</title><style>body{background:#06070a;color:#c6cdd3;font:12px ui-monospace,monospace;padding:10px}
.ok{color:#79c2a4}.bad{color:#e8788f}</style></head><body><div id="o">starting...</div>
<script>
%FSRC%
%PCV%
%PCF%
var TAG=(location.search.match(/tag=([^&]+)/)||[0,'x'])[1];
var out=document.getElementById('o'),logs=[];
function say(s,c){logs.push('<div class="'+(c||'')+'">'+s+'</div>');out.innerHTML=logs.join('');}
function send(o){o.tag=TAG;
  return fetch('/r?d='+encodeURIComponent(JSON.stringify(o)),{cache:'no-store'}).catch(function(){});}

var W=128,H=96,cv=document.createElement('canvas');cv.width=W;cv.height=H;
var gl=cv.getContext('webgl',{preserveDrawingBuffer:true,antialias:false,alpha:false});
if(!gl){say('NO WEBGL','bad');send({kind:'fatal',msg:'no webgl'});}
else run();

function img(variant){                      // a deterministic test picture
  var c=document.createElement('canvas');c.width=W;c.height=H;var x=c.getContext('2d');
  var g=x.createLinearGradient(0,0,W,H);
  g.addColorStop(0,variant?'#e0603a':'#2a6fdb');g.addColorStop(.5,variant?'#2fb27a':'#d9c23a');
  g.addColorStop(1,variant?'#6a2fd0':'#c03a8a');x.fillStyle=g;x.fillRect(0,0,W,H);
  x.fillStyle='#fff';x.beginPath();x.arc(variant?70:48,44,26,0,7);x.fill();
  x.fillStyle='#101418';x.fillRect(variant?20:80,14,26,50);
  x.fillStyle='#f4d03f';x.fillRect(0,70,W,6);
  x.strokeStyle='#000';x.lineWidth=2;x.strokeRect(6,6,W-12,H-12);
  return c;
}
function maskImg(){var c=document.createElement('canvas');c.width=W;c.height=H;var x=c.getContext('2d');
  x.fillStyle='#000';x.fillRect(0,0,W,H);x.fillStyle='#fff';x.beginPath();x.arc(52,46,26,0,7);x.fill();return c;}
function tex(src){var t=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,t);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
  gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,src);return t;}

function run(){
  var dbg=gl.getExtension('WEBGL_debug_renderer_info');
  var hp=gl.getShaderPrecisionFormat(gl.FRAGMENT_SHADER,gl.HIGH_FLOAT);
  send({kind:'env',ua:navigator.userAgent,
    renderer:dbg?gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL):'?',
    vendor:dbg?gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL):'?',
    version:gl.getParameter(gl.VERSION),glsl:gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
    highp:hp?[hp.rangeMin,hp.rangeMax,hp.precision]:null,
    maxTex:gl.getParameter(gl.MAX_TEXTURE_SIZE),maxVtf:gl.getParameter(gl.MAX_VERTEX_TEXTURE_IMAGE_UNITS),
    maxFragUni:gl.getParameter(gl.MAX_FRAGMENT_UNIFORM_VECTORS),
    maxRB:gl.getParameter(gl.MAX_RENDERBUFFER_SIZE),exts:(gl.getSupportedExtensions()||[]).length});

  function sh(ty,src,name){var s=gl.createShader(ty);gl.shaderSource(s,src);gl.compileShader(s);
    if(!gl.getShaderParameter(s,gl.COMPILE_STATUS)){
      var m=gl.getShaderInfoLog(s)||'';say(name+' COMPILE FAILED: '+m.slice(0,300),'bad');
      send({kind:'compile',name:name,ok:false,log:m.slice(0,600)});return null;}
    send({kind:'compile',name:name,ok:true});return s;}
  var vs=sh(gl.VERTEX_SHADER,'attribute vec2 a;varying vec2 v;void main(){v=a;gl_Position=vec4(a*2.-1.,0.,1.);}','vs');
  var fs=sh(gl.FRAGMENT_SHADER,FSRC,'FSRC');
  if(!vs||!fs){send({kind:'done'});return;}
  var p=gl.createProgram();gl.attachShader(p,vs);gl.attachShader(p,fs);gl.linkProgram(p);
  if(!gl.getProgramParameter(p,gl.LINK_STATUS)){
    var m=gl.getProgramInfoLog(p)||'';say('LINK FAILED '+m,'bad');send({kind:'link',ok:false,log:m});send({kind:'done'});return;}
  send({kind:'link',ok:true});
  gl.useProgram(p);
  var buf=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,buf);
  gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([0,0,1,0,0,1,1,1]),gl.STATIC_DRAW);
  var a=gl.getAttribLocation(p,'a');gl.enableVertexAttribArray(a);gl.vertexAttribPointer(a,2,gl.FLOAT,false,0,0);
  function U(n){return gl.getUniformLocation(p,n);}
  var T0=tex(img(0)),T1=tex(img(1)),TM=tex(maskImg());
  gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,T0);
  gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D,T1);
  gl.activeTexture(gl.TEXTURE2);gl.bindTexture(gl.TEXTURE_2D,TM);
  gl.uniform1i(U('T'),0);gl.uniform1i(U('P'),1);gl.uniform1i(U('M'),2);
  gl.uniform1f(U('t'),1.5);gl.uniform1f(U('amt'),.5);gl.uniform1f(U('dep'),.5);
  gl.uniform1f(U('spd'),.5);gl.uniform1f(U('grn'),.15);gl.uniform1f(U('k'),1.);
  gl.uniform2f(U('R'),W,H);gl.uniform3f(U('cA'),.47,.76,.64);gl.uniform3f(U('cB'),.64,.58,.88);
  gl.uniform1f(U('wet'),1.);gl.uniform1f(U('zm'),1.);gl.uniform2f(U('pn'),0,0);
  gl.uniform1i(U('mmode'),0);gl.uniform1i(U('nbx'),0);gl.uniform1i(U('bmode'),0);

  var ids=%IDS%, px=new Uint8Array(W*H*4), n=0;
  gl.viewport(0,0,W,H);
  function one(id){
    gl.uniform1i(U('fx'),id);gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLE_STRIP,0,4);
    var err=gl.getError();gl.readPixels(0,0,W,H,gl.RGBA,gl.UNSIGNED_BYTE,px);
    var sr=0,sg=0,sb=0,blk=0,wht=0,gridL=[];
    for(var i=0;i<px.length;i+=4){sr+=px[i];sg+=px[i+1];sb+=px[i+2];
      var l=(px[i]+px[i+1]+px[i+2])/3;if(l<3)blk++;if(l>252)wht++;}
    var N=W*H;
    for(var gy=0;gy<3;gy++)for(var gx=0;gx<4;gx++){var s=0,c=0;
      for(var y=gy*32;y<gy*32+32;y++)for(var x=gx*32;x<gx*32+32;x++){var o=(y*W+x)*4;s+=(px[o]+px[o+1]+px[o+2])/3;c++;}
      gridL.push(Math.round(s/c));}
    return {id:id,err:err,r:Math.round(sr/N),g:Math.round(sg/N),b:Math.round(sb/N),
            blk:+(blk/N).toFixed(3),wht:+(wht/N).toFixed(3),grid:gridL};
  }
  var results=[];
  ids.forEach(function(id){results.push(one(id));});
  send({kind:'fx',rows:results});

  // the point-cloud effect is a separate program (vertex texture fetch)
  var pv=sh(gl.VERTEX_SHADER,PCV,'PCV'),pf=sh(gl.FRAGMENT_SHADER,PCF,'PCF');
  if(pv&&pf){var q=gl.createProgram();gl.attachShader(q,pv);gl.attachShader(q,pf);gl.linkProgram(q);
    send({kind:'link',name:'pointcloud',ok:!!gl.getProgramParameter(q,gl.LINK_STATUS),
          log:gl.getProgramInfoLog(q)||''});}
  var bad=results.filter(function(r){return r.err||r.blk>.97||r.wht>.97;});
  say('rendered '+results.length+' effects; suspicious: '+(bad.map(function(r){return r.id}).join(',')||'none'),bad.length?'bad':'ok');
  send({kind:'done'});
}
</script></body></html>"""

def build():
    text = SRC.read_text()
    fsrc = lift(text, "var FSRC=", r"';$")
    pcv  = lift(text, "var PCV=", r"';$")
    pcf  = lift(text, "var PCF=", r"';$")
    ids  = sorted({0} | {int(n) for n in re.findall(r"fx==(\d+)", text)})
    OUT.mkdir(exist_ok=True)
    html = (HTML.replace("%FSRC%", fsrc).replace("%PCV%", pcv)
                .replace("%PCF%", pcf).replace("%IDS%", json.dumps(ids)))
    (OUT / "fxdiff.html").write_text(html)
    return len(ids)

class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=str(OUT), **k)
    def log_message(self, *a): pass
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/r":
            d = urllib.parse.parse_qs(u.query).get("d", ["{}"])[0]
            with open(OUT / "reports.jsonl", "a") as f: f.write(d + "\n")
            self.send_response(204); self.end_headers(); return
        return super().do_GET()

def serve():
    n = build()
    (OUT / "reports.jsonl").write_text("")
    print("serving %d effect ids at http://0.0.0.0:%d/fxdiff.html?tag=NAME" % (n, PORT), flush=True)
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("0.0.0.0", PORT), H) as s: s.serve_forever()

def report():
    rows = [json.loads(l) for l in (OUT / "reports.jsonl").read_text().splitlines() if l.strip()]
    tags = {}
    for r in rows: tags.setdefault(r.get("tag", "?"), []).append(r)
    for t, rs in tags.items():
        print("== %s ==" % t)
        for r in rs:
            k = r.get("kind")
            if k == "env":
                print("  %s | %s | highp %s | maxTex %s vtf %s" % (r["renderer"], r["glsl"], r["highp"], r["maxTex"], r["maxVtf"]))
            elif k in ("compile", "link"):
                print("  %s %s %s %s" % (k, r.get("name", ""), "ok" if r["ok"] else "FAILED", (r.get("log") or "")[:200]))
    names = [t for t in tags if any(r.get("kind") == "fx" for r in tags[t])]
    if len(names) < 2: print("\n(need two engines to diff; have: %s)" % names); return
    ref, sub = names[0], names[1]
    def fx(t): return {x["id"]: x for r in tags[t] if r.get("kind") == "fx" for x in r["rows"]}
    A, B = fx(ref), fx(sub)
    print("\nDIFF  reference=%s  subject=%s" % (ref, sub))
    bad = []
    for i in sorted(A):
        a, b = A[i], B.get(i)
        if not b: print("  fx %3d  missing from subject" % i); bad.append(i); continue
        dm = max(abs(a["r"]-b["r"]), abs(a["g"]-b["g"]), abs(a["b"]-b["b"]))
        dg = max(abs(x-y) for x, y in zip(a["grid"], b["grid"]))
        flag = []
        if b["err"]: flag.append("GLerr 0x%x" % b["err"])
        if dm > 20: flag.append("mean off by %d" % dm)
        if dg > 45: flag.append("layout off by %d" % dg)
        if b["blk"] > .9 and a["blk"] < .5: flag.append("SUBJECT BLACK")
        if b["wht"] > .9 and a["wht"] < .5: flag.append("SUBJECT WHITE")
        if flag:
            bad.append(i)
            print("  fx %3d  %-34s ref(%3d,%3d,%3d) sub(%3d,%3d,%3d)" % (i, "; ".join(flag), a["r"],a["g"],a["b"], b["r"],b["g"],b["b"]))
    print("\n%d of %d effects differ materially" % (len(bad), len(A)))

if __name__ == "__main__":
    {"serve": serve, "report": report, "build": lambda: print(build(), "ids")}[sys.argv[1] if len(sys.argv) > 1 else "build"]()
