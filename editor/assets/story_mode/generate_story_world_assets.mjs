import fs from 'node:fs';
import path from 'node:path';

const outputDir = path.dirname(new URL(import.meta.url).pathname.replace(/^\/(?:[A-Za-z]:)/, (value) => value.slice(1)));
const mtlName = 'story_world.mtl';

const materials = {
  grass: [0.29, 0.45, 0.22],
  grass_light: [0.42, 0.56, 0.29],
  highland: [0.39, 0.43, 0.27],
  rock: [0.35, 0.36, 0.34],
  water: [0.09, 0.42, 0.48],
  earth: [0.48, 0.35, 0.2],
  plaster: [0.83, 0.8, 0.69],
  wood: [0.27, 0.14, 0.07],
  roof: [0.16, 0.2, 0.2],
  roof_red: [0.34, 0.12, 0.08],
  leaf: [0.12, 0.33, 0.15],
  leaf_light: [0.25, 0.47, 0.2],
  gold: [0.82, 0.55, 0.16],
  lantern: [0.72, 0.08, 0.04],
};

const mtl = Object.entries(materials)
  .map(([name, color]) => `newmtl ${name}\nKa ${color.map((v) => (v * 0.35).toFixed(3)).join(' ')}\nKd ${color.map((v) => v.toFixed(3)).join(' ')}\nKs 0.060 0.060 0.060\nNs 12.000\nd 1.0\nillum 2\n`)
  .join('\n');
fs.writeFileSync(path.join(outputDir, mtlName), `${mtl}\n`, 'utf8');

class ObjBuilder {
  constructor(name) {
    this.name = name;
    this.vertices = [];
    this.faces = [];
  }
  v(x, y, z) {
    this.vertices.push([x, y, z]);
    return this.vertices.length;
  }
  face(indices, material) {
    this.faces.push({ indices, material });
  }
  quad(a, b, c, d, material) {
    this.face([a, b, c, d], material);
  }
  box(cx, y, cz, sx, sy, sz, material) {
    const x0 = cx - sx / 2, x1 = cx + sx / 2;
    const y0 = y, y1 = y + sy;
    const z0 = cz - sz / 2, z1 = cz + sz / 2;
    const p = [
      this.v(x0,y0,z0), this.v(x1,y0,z0), this.v(x1,y1,z0), this.v(x0,y1,z0),
      this.v(x0,y0,z1), this.v(x1,y0,z1), this.v(x1,y1,z1), this.v(x0,y1,z1),
    ];
    this.quad(p[0],p[1],p[2],p[3],material);
    this.quad(p[5],p[4],p[7],p[6],material);
    this.quad(p[4],p[0],p[3],p[7],material);
    this.quad(p[1],p[5],p[6],p[2],material);
    this.quad(p[3],p[2],p[6],p[7],material);
    this.quad(p[4],p[5],p[1],p[0],material);
  }
  pyramid(cx, y, cz, radius, height, sides, material) {
    const ring = [];
    for (let i=0;i<sides;i+=1) {
      const a = (Math.PI * 2 * i) / sides;
      ring.push(this.v(cx + Math.cos(a)*radius, y, cz + Math.sin(a)*radius));
    }
    const tip = this.v(cx, y + height, cz);
    for (let i=0;i<sides;i+=1) this.face([ring[i], ring[(i+1)%sides], tip], material);
    this.face([...ring].reverse(), material);
  }
  roof(cx, y, cz, sx, height, sz, material) {
    const x0=cx-sx/2, x1=cx+sx/2, z0=cz-sz/2, z1=cz+sz/2;
    const a=this.v(x0,y,z0), b=this.v(x1,y,z0), c=this.v(x1,y,z1), d=this.v(x0,y,z1);
    const e=this.v(cx,y+height,z0), f=this.v(cx,y+height,z1);
    this.face([a,b,e],material); this.face([d,f,c],material);
    this.quad(a,e,f,d,material); this.quad(b,c,f,e,material);
  }
  write(filename) {
    const lines = [`# Deterministic Story World asset: ${this.name}`, `mtllib ${mtlName}`, `o ${this.name}`];
    for (const vertex of this.vertices) lines.push(`v ${vertex.map((v)=>v.toFixed(5)).join(' ')}`);
    let active = '';
    for (const face of this.faces) {
      if (face.material !== active) { active = face.material; lines.push(`usemtl ${active}`); }
      lines.push(`f ${face.indices.join(' ')}`);
    }
    fs.writeFileSync(path.join(outputDir, filename), `${lines.join('\n')}\n`, 'utf8');
  }
}

function terrainHeight(x, z) {
  const center = Math.hypot(x * 0.72, z * 0.72);
  const rim = Math.max(0, (center - 26) / 34);
  const hills = rim * rim * 13;
  const peaks =
    8 * Math.exp(-((x + 48) ** 2 + (z - 34) ** 2) / 340) +
    10 * Math.exp(-((x - 48) ** 2 + (z + 38) ** 2) / 290) +
    7 * Math.exp(-((x + 45) ** 2 + (z + 45) ** 2) / 260);
  const lakeBasin = 2.6 * Math.exp(-((x - 34) ** 2 + (z - 18) ** 2) / 260);
  const villageFlatten = Math.exp(-(x*x + z*z) / 500);
  const ripple = (Math.sin(x * 0.14) + Math.cos(z * 0.12)) * 0.42 * (1 - villageFlatten);
  return Math.max(-1.2, hills + peaks + ripple - lakeBasin);
}

{
  const b = new ObjBuilder('StoryWorld_TerrainMesh');
  const size = 120, cells = 32, step = size / cells, rows = [];
  for (let iz=0; iz<=cells; iz+=1) {
    const row=[]; const z=-60+iz*step;
    for (let ix=0; ix<=cells; ix+=1) {
      const x=-60+ix*step; row.push(b.v(x, terrainHeight(x,z), z));
    }
    rows.push(row);
  }
  for (let iz=0; iz<cells; iz+=1) for (let ix=0; ix<cells; ix+=1) {
    const cx=-60+(ix+0.5)*step, cz=-60+(iz+0.5)*step;
    const h=terrainHeight(cx,cz);
    const material=h>10?'rock':h>4?'highland':((ix+iz)%3===0?'grass_light':'grass');
    b.quad(rows[iz][ix],rows[iz][ix+1],rows[iz+1][ix+1],rows[iz+1][ix],material);
  }
  b.write('terrain.obj');
}

{
  const b=new ObjBuilder('StoryWorld_WaterMesh');
  const rings=24, center=[34,-0.7,18];
  const c=b.v(...center); const ring=[];
  for(let i=0;i<rings;i+=1){const a=Math.PI*2*i/rings; ring.push(b.v(center[0]+Math.cos(a)*25,center[1],center[2]+Math.sin(a)*18));}
  for(let i=0;i<rings;i+=1)b.face([c,ring[i],ring[(i+1)%rings]],'water');
  b.write('water.obj');
}

{
  const b=new ObjBuilder('StoryWorld_RoadSegment'); b.box(0,0,0,5,0.12,16,'earth'); b.write('road_segment.obj');
}

function makeHouse(filename, large=false, red=false){
  const b=new ObjBuilder(filename.replace('.obj',''));
  const sx=large?12:9, sz=large?9:7, sy=large?6:5;
  b.box(0,0,0,sx,sy,sz,'plaster');
  b.box(0,0.05,-sz/2-0.08,2.2,3,0.3,'wood');
  b.box(-sx*0.28,1.6,-sz/2-0.1,1.3,1.25,0.24,'wood');
  b.box(sx*0.28,1.6,-sz/2-0.1,1.3,1.25,0.24,'wood');
  b.roof(0,sy,0,sx+1.8,3,sz+2,red?'roof_red':'roof');
  b.write(filename);
}
makeHouse('house_small.obj'); makeHouse('house_large.obj',true,true);

{
  const b=new ObjBuilder('StoryWorld_Bridge');
  b.box(0,0.5,0,4,0.65,12,'wood');
  for(const x of [-2.3,2.3]){b.box(x,0,0,0.32,2.2,12,'wood');}
  for(const z of [-5.2,0,5.2]) for(const x of [-2.3,2.3]) b.box(x,0,z,0.55,2.6,0.55,'wood');
  b.write('bridge.obj');
}
{
  const b=new ObjBuilder('StoryWorld_Gate');
  for(const x of [-4.5,4.5]) b.box(x,0,0,0.8,7.5,0.8,'wood');
  b.box(0,5.8,0,11,0.8,1,'wood'); b.roof(0,6.6,0,12,2,2.4,'roof_red');
  b.box(0,4.5,-0.55,4.8,1.25,0.18,'gold'); b.write('gate.obj');
}
{
  const b=new ObjBuilder('StoryWorld_Pavilion');
  for(const x of [-3,3]) for(const z of [-3,3]) b.box(x,0,z,0.45,4.8,0.45,'wood');
  b.box(0,0,0,7,0.35,7,'rock'); b.roof(0,4.5,0,9,3.1,9,'roof_red'); b.write('pavilion.obj');
}
{
  const b=new ObjBuilder('StoryWorld_Tree');
  b.box(0,0,0,0.9,4.5,0.9,'wood'); b.pyramid(0,3.2,0,3.2,5,7,'leaf'); b.pyramid(0,5.1,0,2.5,4,7,'leaf_light'); b.write('tree.obj');
}
{
  const b=new ObjBuilder('StoryWorld_Rock'); b.pyramid(0,0,0,2.3,2.8,7,'rock'); b.write('rock.obj');
}
{
  const b=new ObjBuilder('StoryWorld_Fence');
  for(const x of [-3.8,0,3.8]) b.box(x,0,0,0.32,2.1,0.32,'wood');
  b.box(0,0.65,0,8.2,0.28,0.28,'wood'); b.box(0,1.55,0,8.2,0.28,0.28,'wood'); b.write('fence.obj');
}
{
  const b=new ObjBuilder('StoryWorld_Lantern');
  b.box(0,0,0,0.28,4.2,0.28,'wood'); b.box(0,3.4,0,1.5,0.2,0.2,'wood');
  b.box(0.55,2.45,0,0.75,1.05,0.75,'lantern'); b.pyramid(0.55,3.5,0,0.65,0.45,4,'gold'); b.write('lantern.obj');
}

console.log(`Generated Story World assets in ${outputDir}`);
