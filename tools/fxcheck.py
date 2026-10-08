#!/usr/bin/env python3
"""Compile the firmware's real fragment shader in a real WebGL context.

The 48-plus effects are GLSL living inside a C string inside a C++ file, so
nothing in the build ever type-checks them. A shader that fails to compile
does not break the build -- it breaks the camera, silently, in someone's
hand, after a flash. This lifts FSRC straight out of main_s3.cpp, compiles
it, links it, checks every uniform survived, then RENDERS every effect id and
reads the pixels back.

    python3 tools/fxcheck.py          # writes .fxcheck/index.html
    then open that file in a browser  (must be under the project directory,
                                       previews of outside paths do not run JS)

Regenerate and re-run this after touching any shader.
"""
import os, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC  = ROOT / "main_s3.cpp"
OUT  = ROOT / ".fxcheck"

def extract_fsrc(text):
    i = text.index("var FSRC=")
    lines, out = text[i:].split("\n"), []
    for ln in lines:
        out.append(ln)
        if ln.rstrip().endswith("';") and len(out) > 5:
            return "\n".join(out)
    raise SystemExit("could not find the end of FSRC")

HTML = """<!DOCTYPE html><html><head><meta charset="utf-8"><title>FSRC compile</title>
<style>body{background:#06070a;color:#c6cdd3;font:13px ui-monospace,monospace;padding:16px}
.ok{color:#79c2a4}.bad{color:#e8788f;white-space:pre-wrap}</style></head><body>
<div id="out">running...</div>
<script>
%FSRC%
var VS='attribute vec2 a;varying vec2 v;void main(){v=a;gl_Position=vec4(a*2.-1.,0.,1.);}';
var log=[],cv=document.createElement('canvas');cv.width=64;cv.height=64;
var gl=cv.getContext('webgl')||cv.getContext('experimental-webgl');
if(!gl){log.push('<div class="bad">NO WEBGL CONTEXT</div>');}else{
 function mk(t,s,n){var sh=gl.createShader(t);gl.shaderSource(sh,s);gl.compileShader(sh);
  if(!gl.getShaderParameter(sh,gl.COMPILE_STATUS)){
   log.push('<div class="bad">'+n+' COMPILE FAILED:\\n'+gl.getShaderInfoLog(sh)+'</div>');return null;}
  log.push('<div class="ok">'+n+' compiled OK</div>');return sh;}
 var vs=mk(gl.VERTEX_SHADER,VS,'vertex');
 var fs=mk(gl.FRAGMENT_SHADER,FSRC,'FRAGMENT (FSRC, '+FSRC.length+' chars)');
 if(vs&&fs){var p=gl.createProgram();gl.attachShader(p,vs);gl.attachShader(p,fs);gl.linkProgram(p);
  if(!gl.getProgramParameter(p,gl.LINK_STATUS))
   log.push('<div class="bad">LINK FAILED:\\n'+gl.getProgramInfoLog(p)+'</div>');
  else{log.push('<div class="ok">program LINKED OK</div>');
   var want=['T','P','t','fx','amt','dep','spd','grn','k','R','cA','cB','wet','zm','pn','M','mmode','bx','nbx','bmode'];
   var miss=[];for(var i=0;i<want.length;i++)if(gl.getUniformLocation(p,want[i])===null)miss.push(want[i]);
   log.push(miss.length?'<div class="bad">uniforms missing/optimised out: '+miss.join(', ')+'</div>'
                       :'<div class="ok">all '+want.length+' uniforms present</div>');
   var tex=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,tex);
   var px=new Uint8Array(64*64*4);
   for(var q=0;q<64*64;q++){px[q*4]=(q%64)*4;px[q*4+1]=((q/64)|0)*4;px[q*4+2]=128;px[q*4+3]=255;}
   gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,64,64,0,gl.RGBA,gl.UNSIGNED_BYTE,px);
   gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);
   gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
   gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
   gl.useProgram(p);
   var b=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,b);
   gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([0,0,1,0,0,1,0,1,1,0,1,1]),gl.STATIC_DRAW);
   var a=gl.getAttribLocation(p,'a');gl.enableVertexAttribArray(a);
   gl.vertexAttribPointer(a,2,gl.FLOAT,false,0,0);
   gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,tex);
   gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D,tex);
   gl.activeTexture(gl.TEXTURE2);gl.bindTexture(gl.TEXTURE_2D,tex);
   function U(n){return gl.getUniformLocation(p,n)}
   gl.uniform1i(U('T'),0);gl.uniform1i(U('P'),1);gl.uniform1i(U('M'),2);
   gl.uniform1f(U('t'),1.5);gl.uniform1f(U('amt'),.6);gl.uniform1f(U('dep'),.5);
   gl.uniform1f(U('spd'),.5);gl.uniform1f(U('grn'),.1);gl.uniform1f(U('k'),1.);
   gl.uniform2f(U('R'),64,64);gl.uniform3f(U('cA'),.5,.8,.6);gl.uniform3f(U('cB'),.6,.5,.9);
   gl.uniform1f(U('wet'),1.);gl.uniform1f(U('zm'),1.);gl.uniform2f(U('pn'),0,0);
   gl.uniform1i(U('mmode'),0);gl.uniform1i(U('nbx'),0);gl.uniform1i(U('bmode'),0);
   var dead=[],errs=[],N=%MAXFX%;
   for(var f=0;f<=N;f++){gl.uniform1i(U('fx'),f);gl.viewport(0,0,64,64);
    gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);gl.drawArrays(gl.TRIANGLES,0,6);
    var e=gl.getError();if(e!==0){errs.push(f+':0x'+e.toString(16));continue;}
    var rp=new Uint8Array(64*64*4);gl.readPixels(0,0,64,64,gl.RGBA,gl.UNSIGNED_BYTE,rp);
    var sum=0;for(var z=0;z<rp.length;z+=4)sum+=rp[z]+rp[z+1]+rp[z+2];
    if(sum===0)dead.push(f);}
   log.push(errs.length?'<div class="bad">GL errors on fx: '+errs.join(' ')+'</div>'
                       :'<div class="ok">all '+(N+1)+' effect ids drew with no GL error</div>');
   /* fx 95 is "subject = current minus learned background". The harness binds
      the SAME texture as current and previous, so the difference is genuinely
      zero and black is the right answer -- not a failure. Anything else in
      this list is worth opening. */
   var expected={95:1}, unexpected=dead.filter(function(f){return !expected[f]});
   log.push(unexpected.length
     ?'<div class="bad">rendered pure black: fx '+unexpected.join(', ')+'</div>'
     :'<div class="ok">no unexpected black frames'
       +(dead.length?' (fx '+dead.join(', ')+' black by design: identical input frames)':'')
       +'</div>');}}}
document.getElementById('out').innerHTML=log.join('');
</script></body></html>"""

def main():
    text = SRC.read_text()
    frag = extract_fsrc(text)
    maxfx = max(int(n) for n in __import__("re").findall(r"fx==(\d+)", text))
    OUT.mkdir(exist_ok=True)
    (OUT / "index.html").write_text(
        HTML.replace("%FSRC%", frag).replace("%MAXFX%", str(maxfx)))
    print("wrote %s  (shader %d chars, effects 0..%d)"
          % (OUT / "index.html", len(frag), maxfx))
    print("open it with:  file://%s" % (OUT / "index.html"))

if __name__ == "__main__":
    main()
