// Exercise picking against synthetic projected triangles without game assets.
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
let script = fs.readFileSync(__dirname + '/itesu_preview_viewer.html', 'utf8').split('<script>')[1].split('</script>')[0];
for (const [token, value] of Object.entries({__MODEL_JSON__:'[]', __VIEW_OPTIONS__:'{"target":[0,0,0],"bounds":[-1,1,-1,1,-1,1]}', __TEXTURE_URL__:'""', __OPAQUE_URL__:'""', __HUD_URL__:'null'})) script=script.replace(token,value);
const events={}; const canvas={getContext:()=>({}),addEventListener:(name,fn)=>events[name]=fn,setPointerCapture(){},getBoundingClientRect:()=>({left:0,top:0})};
const elements={view:canvas,front:{},down:{},toggle:{},hud:{},alpha:{},nearby:{},status:{},selection:{}};
const sandbox=vm.createContext({document:{getElementById:id=>elements[id]},Image:class{constructor(){this.complete=false}},
  URLSearchParams,location:{search:''},window:{addEventListener(){}},Math});
vm.runInContext(script,sandbox);
vm.runInContext(`pickPixels={width:1,height:1,data:[0,0,0,255]};visibleTriangles=[{v:[{x:0,y:0},{x:10,y:0},{x:0,y:10}],uv:[[0,0],[0.9,0],[0,0.9]],batch:{off:42,p:[[0,0,0],[1,0,0],[0,1,0]],uv:[[0,0],[1,0],[0,1]]}}]`,sandbox);
vm.runInContext('inspectPoint(2,2)',sandbox);
assert.match(elements.selection.textContent,/Candidate batch 0x2a/);
assert.match(elements.selection.textContent,/nearest-pixel alpha/);
vm.runInContext('pickPixels.data[3]=0;inspectPoint(2,2)',sandbox);
assert.match(elements.selection.textContent,/No candidate batch/);
vm.runInContext('inspectPoint(30,30)',sandbox);
assert.match(elements.selection.textContent,/No candidate batch/);
console.log('PASS: viewer picks a provisional batch and handles misses');
