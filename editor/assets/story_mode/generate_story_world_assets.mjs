import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const outputDir = path.dirname(fileURLToPath(import.meta.url));
const mtlName = "story_world_v3.mtl";
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const mul = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const cross = (a, b) => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0],
];
const norm = (a, f = [0, 1, 0]) => {
  const l = Math.hypot(...a);
  return l > 1e-8 ? mul(a, 1 / l) : [...f];
};
function hash(x, y, s = 0) {
  let v = Math.imul((x | 0) ^ Math.imul(s + 17, 0x45d9f3b), 0x27d4eb2d);
  v ^= Math.imul((y | 0) ^ Math.imul(s + 31, 0x165667b1), 0x85ebca6b);
  v ^= v >>> 15;
  return (v >>> 0) / 0xffffffff;
}
function rotate(p, r = [0, 0, 0]) {
  const [rx, ry, rz] = r.map((v) => (v * Math.PI) / 180);
  let [x, y, z] = p;
  [y, z] = [
    y * Math.cos(rx) - z * Math.sin(rx),
    y * Math.sin(rx) + z * Math.cos(rx),
  ];
  [x, z] = [
    x * Math.cos(ry) + z * Math.sin(ry),
    -x * Math.sin(ry) + z * Math.cos(ry),
  ];
  [x, y] = [
    x * Math.cos(rz) - y * Math.sin(rz),
    x * Math.sin(rz) + y * Math.cos(rz),
  ];
  return [x, y, z];
}

const materials = {
  grass: [[0.42, 0.55, 0.25], "grass", 8],
  grass_light: [[0.53, 0.65, 0.31], "grass", 8],
  highland: [[0.36, 0.42, 0.24], "grass", 7],
  rock: [[0.43, 0.44, 0.41], "rock", 16],
  stone: [[0.51, 0.53, 0.5], "stone", 18],
  stone_dark: [[0.32, 0.35, 0.34], "stone", 12],
  water: [[0.08, 0.42, 0.47], null, 90, 0.92],
  shallows: [[0.19, 0.54, 0.49], null, 60, 0.9],
  earth: [[0.5, 0.35, 0.2], "dirt", 7],
  plaster: [[0.88, 0.85, 0.75], "plaster", 10],
  plaster_weathered: [[0.7, 0.7, 0.61], "plaster", 7],
  wood: [[0.34, 0.18, 0.08], "wood", 22],
  wood_dark: [[0.2, 0.09, 0.035], "wood", 18],
  roof: [[0.19, 0.25, 0.27], "roof_tile", 30],
  roof_edge: [[0.1, 0.15, 0.17], "roof_tile", 24],
  roof_red: [[0.33, 0.13, 0.08], "roof_tile", 25],
  leaf: [[0.12, 0.34, 0.16], "grass", 10],
  leaf_light: [[0.27, 0.48, 0.19], "grass", 11],
  leaf_dark: [[0.08, 0.24, 0.12], "grass", 8],
  reed: [[0.4, 0.48, 0.17], "reed", 8],
  gold: [[0.75, 0.52, 0.16], null, 48],
  lantern: [[0.7, 0.075, 0.035], null, 18],
  paper: [[0.96, 0.72, 0.32], null, 12, 0.94],
  metal: [[0.2, 0.21, 0.2], "rock", 54],
};
const mtl = Object.entries(materials)
  .map(([name, [c, tex, ns = 12, opacity = 1]]) => {
    const lines = [
      `newmtl ${name}`,
      `Ka ${c.map((v) => (v * 0.3).toFixed(3)).join(" ")}`,
      `Kd ${c.map((v) => v.toFixed(3)).join(" ")}`,
      "Ks 0.080 0.080 0.080",
      `Ns ${ns.toFixed(3)}`,
      `d ${opacity.toFixed(3)}`,
      "illum 2",
    ];
    if (tex)
      lines.push(
        `map_Kd textures/${tex}_diffuse.png`,
        `map_Bump -bm 0.65 textures/${tex}_normal.png`,
      );
    return lines.join("\n");
  })
  .join("\n\n");
fs.writeFileSync(path.join(outputDir, mtlName), `${mtl}\n`, "utf8");

class ObjBuilder {
  constructor(name) {
    this.name = name;
    this.triangles = [];
  }
  triangle(a, b, c, material, uvs = null, normals = null) {
    const n = norm(cross(sub(b, a), sub(c, a)));
    this.triangles.push({
      points: [a, b, c].map((p) => [...p]),
      uvs: uvs || [
        [0, 0],
        [1, 0],
        [1, 1],
      ],
      normals: normals || [n, n, n],
      material,
    });
  }
  quad(a, b, c, d, material, uvs = null, normals = null) {
    const t = uvs || [
      [0, 0],
      [1, 0],
      [1, 1],
      [0, 1],
    ];
    this.triangle(
      a,
      b,
      c,
      material,
      [t[0], t[1], t[2]],
      normals && [normals[0], normals[1], normals[2]],
    );
    this.triangle(
      a,
      c,
      d,
      material,
      [t[0], t[2], t[3]],
      normals && [normals[0], normals[2], normals[3]],
    );
  }
  transformedBox(center, size, material, rotation = [0, 0, 0], uv = [1, 1]) {
    const [sx, sy, sz] = size;
    const p = [
      [-sx / 2, -sy / 2, -sz / 2],
      [sx / 2, -sy / 2, -sz / 2],
      [sx / 2, sy / 2, -sz / 2],
      [-sx / 2, sy / 2, -sz / 2],
      [-sx / 2, -sy / 2, sz / 2],
      [sx / 2, -sy / 2, sz / 2],
      [sx / 2, sy / 2, sz / 2],
      [-sx / 2, sy / 2, sz / 2],
    ].map((v) => add(rotate(v, rotation), center));
    const t = [
      [0, 0],
      [uv[0], 0],
      [uv[0], uv[1]],
      [0, uv[1]],
    ];
    this.quad(p[0], p[3], p[2], p[1], material, t);
    this.quad(p[4], p[5], p[6], p[7], material, t);
    this.quad(p[0], p[4], p[7], p[3], material, t);
    this.quad(p[1], p[2], p[6], p[5], material, t);
    this.quad(p[3], p[7], p[6], p[2], material, t);
    this.quad(p[0], p[1], p[5], p[4], material, t);
  }
  box(cx, y, cz, sx, sy, sz, material, rotation = [0, 0, 0], uv = [1, 1]) {
    this.transformedBox(
      [cx, y + sy / 2, cz],
      [sx, sy, sz],
      material,
      rotation,
      uv,
    );
  }
  cylinderBetween(start, end, radius, sides, material) {
    const axis = norm(sub(end, start)),
      tangent = norm(
        cross(Math.abs(axis[1]) < 0.92 ? [0, 1, 0] : [1, 0, 0], axis),
        [1, 0, 0],
      ),
      bitangent = norm(cross(axis, tangent), [0, 0, 1]);
    const a = [],
      b = [];
    for (let i = 0; i < sides; i++) {
      const angle = (Math.PI * 2 * i) / sides,
        radial = add(
          mul(tangent, Math.cos(angle) * radius),
          mul(bitangent, Math.sin(angle) * radius),
        );
      a.push(add(start, radial));
      b.push(add(end, radial));
    }
    for (let i = 0; i < sides; i++) {
      const j = (i + 1) % sides,
        n0 = norm(sub(a[i], start)),
        n1 = norm(sub(a[j], start));
      this.quad(
        a[i],
        a[j],
        b[j],
        b[i],
        material,
        [
          [i / sides, 0],
          [(i + 1) / sides, 0],
          [(i + 1) / sides, 1],
          [i / sides, 1],
        ],
        [n0, n1, n1, n0],
      );
      this.triangle(start, a[j], a[i], material);
      this.triangle(end, b[i], b[j], material);
    }
  }
  cylinder(cx, y, cz, radius, height, sides, material) {
    this.cylinderBetween(
      [cx, y, cz],
      [cx, y + height, cz],
      radius,
      sides,
      material,
    );
  }
  ellipsoid(center, radii, segments, rings, material, seed = 0) {
    const p = [],
      n = [];
    for (let r = 0; r <= rings; r++) {
      const v = r / rings,
        phi = v * Math.PI,
        pr = [],
        nr = [];
      for (let s = 0; s <= segments; s++) {
        const u = s / segments,
          theta = u * Math.PI * 2,
          w = 0.9 + hash(s, r, seed) * 0.16,
          local = [
            Math.sin(phi) * Math.cos(theta) * radii[0] * w,
            Math.cos(phi) * radii[1] * (0.94 + hash(r, s, seed + 13) * 0.1),
            Math.sin(phi) * Math.sin(theta) * radii[2] * w,
          ];
        pr.push(add(center, local));
        nr.push(
          norm([
            local[0] / radii[0] ** 2,
            local[1] / radii[1] ** 2,
            local[2] / radii[2] ** 2,
          ]),
        );
      }
      p.push(pr);
      n.push(nr);
    }
    for (let r = 0; r < rings; r++)
      for (let s = 0; s < segments; s++)
        this.quad(
          p[r][s],
          p[r + 1][s],
          p[r + 1][s + 1],
          p[r][s + 1],
          material,
          [
            [s / segments, r / rings],
            [s / segments, (r + 1) / rings],
            [(s + 1) / segments, (r + 1) / rings],
            [(s + 1) / segments, r / rings],
          ],
          [n[r][s], n[r + 1][s], n[r + 1][s + 1], n[r][s + 1]],
        );
  }
  write(filename) {
    const lines = [
      `# Deterministic Story World v3 asset: ${this.name}`,
      `# Triangles: ${this.triangles.length}`,
      `mtllib ${mtlName}`,
      `o ${this.name}`,
      "s 1",
    ];
    for (const t of this.triangles)
      for (const p of t.points)
        lines.push(`v ${p.map((v) => v.toFixed(6)).join(" ")}`);
    for (const t of this.triangles)
      for (const uv of t.uvs)
        lines.push(`vt ${uv.map((v) => v.toFixed(6)).join(" ")}`);
    for (const t of this.triangles)
      for (const n of t.normals)
        lines.push(
          `vn ${norm(n)
            .map((v) => v.toFixed(6))
            .join(" ")}`,
        );
    let active = "",
      i = 1;
    for (const t of this.triangles) {
      if (t.material !== active) {
        active = t.material;
        lines.push(`usemtl ${active}`);
      }
      lines.push(
        `f ${i}/${i}/${i} ${i + 1}/${i + 1}/${i + 1} ${i + 2}/${i + 2}/${i + 2}`,
      );
      i += 3;
    }
    fs.writeFileSync(
      path.join(outputDir, filename),
      `${lines.join("\n")}\n`,
      "utf8",
    );
  }
}

function terrainHeight(x, z) {
  const center = Math.hypot(x * 0.72, z * 0.72),
    rim = Math.max(0, (center - 25) / 35),
    hills = rim * rim * 12.5,
    peaks =
      7.8 * Math.exp(-((x + 48) ** 2 + (z - 34) ** 2) / 360) +
      9.5 * Math.exp(-((x - 48) ** 2 + (z + 38) ** 2) / 310) +
      6.5 * Math.exp(-((x + 45) ** 2 + (z + 45) ** 2) / 280),
    basin = 2.7 * Math.exp(-((x - 34) ** 2 + (z - 18) ** 2) / 270),
    flat = Math.exp(-(x * x + z * z) / 520),
    ripple =
      (Math.sin(x * 0.14) +
        Math.cos(z * 0.12) +
        Math.sin((x + z) * 0.09) * 0.6) *
      0.38 *
      (1 - flat);
  return Math.max(-1.3, hills + peaks + ripple - basin);
}
function terrainNormal(x, z, s = 0.35) {
  return norm([
    -(terrainHeight(x + s, z) - terrainHeight(x - s, z)) / (2 * s),
    1,
    -(terrainHeight(x, z + s) - terrainHeight(x, z - s)) / (2 * s),
  ]);
}

{
  const b = new ObjBuilder("StoryWorld_Terrain_v3"),
    size = 120,
    cells = 64,
    step = size / cells,
    p = [],
    n = [];
  for (let iz = 0; iz <= cells; iz++) {
    const z = -60 + iz * step,
      pr = [],
      nr = [];
    for (let ix = 0; ix <= cells; ix++) {
      const x = -60 + ix * step;
      pr.push([x, terrainHeight(x, z), z]);
      nr.push(terrainNormal(x, z));
    }
    p.push(pr);
    n.push(nr);
  }
  for (let iz = 0; iz < cells; iz++)
    for (let ix = 0; ix < cells; ix++) {
      const h = terrainHeight(-60 + (ix + 0.5) * step, -60 + (iz + 0.5) * step),
        m =
          h > 9
            ? "rock"
            : h > 4
              ? "highland"
              : (ix + iz) % 4 === 0
                ? "grass_light"
                : "grass",
        u0 = (ix / cells) * 8,
        u1 = ((ix + 1) / cells) * 8,
        v0 = (iz / cells) * 8,
        v1 = ((iz + 1) / cells) * 8;
      b.quad(
        p[iz][ix],
        p[iz + 1][ix],
        p[iz + 1][ix + 1],
        p[iz][ix + 1],
        m,
        [
          [u0, v0],
          [u0, v1],
          [u1, v1],
          [u1, v0],
        ],
        [n[iz][ix], n[iz + 1][ix], n[iz + 1][ix + 1], n[iz][ix + 1]],
      );
    }
  b.write("terrain_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_YunxiLake_v3"),
    segments = 64,
    c = [0, -0.7, 0],
    inner = [],
    outer = [];
  for (let i = 0; i <= segments; i++) {
    const a = (Math.PI * 2 * i) / segments,
      w = 0.96 + Math.sin(a * 3 + 0.7) * 0.025 + Math.sin(a * 7) * 0.015;
    outer.push([Math.cos(a) * 25 * w, -0.69, Math.sin(a) * 18 * w]);
    inner.push([Math.cos(a) * 22.2 * w, -0.7, Math.sin(a) * 15.5 * w]);
  }
  for (let i = 0; i < segments; i++) {
    b.triangle(
      c,
      inner[i],
      inner[i + 1],
      "water",
      [
        [0.5, 0.5],
        [inner[i][0] / 50 + 0.5, inner[i][2] / 36 + 0.5],
        [inner[i + 1][0] / 50 + 0.5, inner[i + 1][2] / 36 + 0.5],
      ],
      [
        [0, 1, 0],
        [0, 1, 0],
        [0, 1, 0],
      ],
    );
    b.quad(inner[i], outer[i], outer[i + 1], inner[i + 1], "shallows", null, [
      [0, 1, 0],
      [0, 1, 0],
      [0, 1, 0],
      [0, 1, 0],
    ]);
  }
  [
    -2.8, -2.35, -1.92, -1.45, -0.88, -0.32, 0.18, 0.73, 1.17, 1.65, 2.12, 2.55,
  ].forEach((a, i) => {
    const x = Math.cos(a) * 23.7,
      z = Math.sin(a) * 16.9;
    b.ellipsoid(
      [x, -0.55, z],
      [0.75 + (i % 3) * 0.16, 0.42, 0.55],
      8,
      5,
      "rock",
      190 + i,
    );
    if (i % 2 === 0)
      for (let r = 0; r < 5; r++) {
        const o = (r - 2) * 0.17;
        b.cylinderBetween(
          [x + o, -0.62, z + o * 0.3],
          [x + o + 0.05, 0.65 + r * 0.04, z + o * 0.3],
          0.035,
          5,
          "reed",
        );
      }
  });
  b.write("water_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_RoadSegment_v3"),
    segments = 16,
    left = [],
    right = [];
  for (let i = 0; i <= segments; i++) {
    const t = i / segments,
      z = -8 + t * 16,
      cx = Math.sin(t * Math.PI * 1.4 - 0.7) * 0.42,
      w = 2.3 + Math.sin(t * Math.PI * 3.1) * 0.16;
    left.push([cx - w, 0.02 + Math.sin(t * Math.PI * 2) * 0.015, z]);
    right.push([cx + w, 0.02 + Math.sin(t * Math.PI * 2) * 0.015, z]);
  }
  for (let i = 0; i < segments; i++)
    b.quad(
      left[i],
      right[i],
      right[i + 1],
      left[i + 1],
      "earth",
      [
        [0, i / 3],
        [1, i / 3],
        [1, (i + 1) / 3],
        [0, (i + 1) / 3],
      ],
      [
        [0, 1, 0],
        [0, 1, 0],
        [0, 1, 0],
        [0, 1, 0],
      ],
    );
  for (let r = 0; r < 12; r++) {
    const z = -7.3 + r * 1.27,
      cx = Math.sin((r / 12) * Math.PI * 1.4 - 0.7) * 0.42,
      count = r % 3 === 0 ? 3 : 2;
    for (let col = 0; col < count; col++) {
      const x =
        cx + (col - (count - 1) / 2) * 1.35 + (hash(r, col, 17) - 0.5) * 0.2;
      b.transformedBox(
        [x, 0.075, z],
        [1.05 + hash(r, col, 21) * 0.22, 0.11, 0.68 + hash(r, col, 25) * 0.16],
        r % 4 === 0 ? "stone_dark" : "stone",
        [0, (hash(r, col, 31) - 0.5) * 10, 0],
        [1.2, 0.8],
      );
    }
  }
  b.write("road_v3.obj");
}
function windowDetail(b, cx, y, cz, w, h) {
  b.box(cx, y, cz, w, h, 0.12, "wood_dark");
  b.box(cx, y + 0.09, cz - 0.07, w - 0.18, h - 0.18, 0.05, "paper");
  b.box(cx, y + 0.06, cz - 0.11, 0.08, h - 0.22, 0.08, "wood");
  b.box(cx, y + h * 0.5 - 0.02, cz - 0.11, w - 0.2, 0.08, 0.08, "wood");
}
function roofDetail(b, sx, sz, wall, roofH, red = false) {
  const m = red ? "roof_red" : "roof",
    angle = (Math.atan2(roofH, sx / 2) * 180) / Math.PI,
    length = Math.hypot(sx / 2, roofH);
  for (const side of [-1, 1])
    b.transformedBox(
      [side * sx * 0.255, wall + roofH * 0.52, 0],
      [length + 0.9, 0.2, sz + 1.2],
      m,
      [0, 0, side * -angle],
    );
  b.cylinderBetween(
    [0, wall + roofH + 0.06, -sz / 2 - 0.64],
    [0, wall + roofH + 0.06, sz / 2 + 0.64],
    0.15,
    8,
    "roof_edge",
  );
  const rows = Math.max(8, Math.round(sz / 0.65));
  for (let row = 0; row <= rows; row++) {
    const z = -sz / 2 - 0.46 + (row / rows) * (sz + 0.92);
    for (const side of [-1, 1])
      b.transformedBox(
        [side * sx * 0.26, wall + roofH * 0.54 + 0.08, z],
        [length + 0.74, 0.055, 0.08],
        "roof_edge",
        [0, 0, side * -angle],
      );
  }
  for (const z of [-sz / 2 - 0.55, sz / 2 + 0.55])
    for (const side of [-1, 1]) {
      const x = side * (sx / 2 + 0.42);
      b.cylinderBetween(
        [x, wall + 0.1, z],
        [x + side * 0.18, wall + 0.3, z],
        0.1,
        7,
        "roof_edge",
      );
    }
}
function makeHouse(
  filename,
  { sx, sz, wall, roofH, red = false, wing = false },
) {
  const b = new ObjBuilder(filename.replace(".obj", ""));
  b.box(0, 0, 0, sx - 0.25, 0.55, sz - 0.2, "stone", [0, 0, 0], [3, 2]);
  b.box(
    0,
    0.5,
    0,
    sx - 0.55,
    wall - 0.5,
    sz - 0.55,
    "plaster",
    [0, 0, 0],
    [3.2, 2.4],
  );
  const cols = [-sx / 2 + 0.38, -sx * 0.25, 0, sx * 0.25, sx / 2 - 0.38];
  for (const x of cols)
    b.box(x, 0.45, -sz / 2 - 0.02, 0.22, wall - 0.1, 0.25, "wood");
  b.box(0, wall - 0.25, -sz / 2 - 0.04, sx - 0.25, 0.24, 0.26, "wood_dark");
  b.box(0, 1.18, -sz / 2 - 0.05, sx - 0.35, 0.16, 0.24, "wood");
  b.box(0, 0.54, -sz / 2 - 0.46, 2.35, 0.28, 0.9, "stone");
  b.box(0, 0.82, -sz / 2 - 0.34, 1.8, 0.24, 0.65, "stone_dark");
  b.box(0, 0.86, -sz / 2 - 0.08, 1.55, 2.75, 0.18, "wood_dark");
  b.box(0, 0.99, -sz / 2 - 0.2, 1.25, 2.45, 0.08, "wood");
  b.box(-0.42, 1.1, -sz / 2 - 0.26, 0.06, 2.15, 0.06, "gold");
  b.box(0.42, 1.1, -sz / 2 - 0.26, 0.06, 2.15, 0.06, "gold");
  windowDetail(b, -sx * 0.28, 1.65, -sz / 2 - 0.16, 1.42, 1.35);
  windowDetail(b, sx * 0.28, 1.65, -sz / 2 - 0.16, 1.42, 1.35);
  for (const side of [-1, 1]) {
    b.box(
      side * (sx / 2 - 0.28),
      0.55,
      0,
      0.22,
      wall - 0.25,
      sz - 0.45,
      "wood",
    );
    for (const z of [-sz * 0.22, sz * 0.22])
      b.box(side * (sx / 2 - 0.31), 1.55, z, 0.12, 1.35, 1.15, "wood_dark");
  }
  roofDetail(b, sx, sz, wall, roofH, red);
  if (wing) {
    b.box(
      sx * 0.34,
      0.3,
      sz * 0.25,
      sx * 0.28,
      wall * 0.58,
      sz * 0.46,
      "plaster_weathered",
    );
    b.transformedBox(
      [sx * 0.34, wall * 0.62, sz * 0.25],
      [sx * 0.36, 0.18, sz * 0.55],
      red ? "roof_red" : "roof",
      [0, 0, -18],
    );
  }
  b.write(filename);
}
makeHouse("house_small_v3.obj", { sx: 9.5, sz: 7.8, wall: 5.1, roofH: 2.55 });
makeHouse("house_large_v3.obj", {
  sx: 12.3,
  sz: 9.6,
  wall: 5.9,
  roofH: 2.75,
  red: true,
  wing: true,
});
{
  const b = new ObjBuilder("StoryWorld_Bridge_v3");
  b.box(0, 0.1, 0, 4.25, 0.5, 12, "wood_dark", [0, 0, 0], [2, 6]);
  for (let p = 0; p < 18; p++) {
    const z = -5.6 + p * 0.66;
    b.transformedBox(
      [0, 0.66 + Math.cos((z / 12) * Math.PI) * 0.32, z],
      [4.7, 0.16, 0.52],
      p % 4 === 0 ? "wood_dark" : "wood",
      [0, (hash(p, 1, 151) - 0.5) * 1.5, 0],
      [2, 1],
    );
  }
  for (const x of [-2.35, 2.35]) {
    for (const z of [-5.25, -2.65, 0, 2.65, 5.25])
      b.box(x, 0, z, 0.34, 2.4, 0.34, "wood_dark");
    b.cylinderBetween([x, 1.85, -5.4], [x, 2.15, 0], 0.12, 8, "wood");
    b.cylinderBetween([x, 2.15, 0], [x, 1.85, 5.4], 0.12, 8, "wood");
  }
  for (const x of [-1.75, 1.75])
    for (const z of [-4.4, 4.4]) b.box(x, -0.25, z, 0.8, 1.15, 1.1, "stone");
  b.write("bridge_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_VillageGate_v3");
  for (const x of [-4.25, 4.25]) {
    b.box(x, 0, 0, 1.05, 0.5, 1.15, "stone");
    b.cylinder(x, 0.5, 0, 0.39, 6.45, 10, "wood_dark");
    b.box(x, 5, 0, 1.2, 0.38, 1.15, "wood");
  }
  b.box(0, 5.55, 0, 10.4, 0.6, 0.78, "wood_dark");
  b.box(0, 6.25, 0, 8.9, 0.48, 0.7, "wood");
  for (const x of [-3.8, -2.5, 2.5, 3.8])
    b.transformedBox([x, 5.42, 0], [0.28, 1.2, 0.72], "wood", [
      0,
      0,
      x < 0 ? -35 : 35,
    ]);
  for (const side of [-1, 1])
    b.transformedBox([side * 2.9, 7.05, 0], [6.5, 0.22, 2.35], "roof_red", [
      0,
      0,
      side * -24,
    ]);
  b.cylinderBetween([0, 7.82, -1.18], [0, 7.82, 1.18], 0.13, 8, "roof_edge");
  b.box(0, 4.45, -0.5, 4.7, 1.15, 0.18, "gold");
  b.write("gate_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_LakesidePavilion_v3");
  b.box(0, 0, 0, 7.2, 0.42, 7.2, "stone", [0, 0, 0], [3, 3]);
  for (const x of [-2.75, 2.75])
    for (const z of [-2.75, 2.75]) {
      b.cylinder(x, 0.42, z, 0.27, 4.45, 10, "wood_dark");
      b.box(x, 4, z, 0.7, 0.3, 0.7, "wood");
    }
  for (const z of [-2.85, 2.85]) b.box(0, 3.86, z, 6.5, 0.28, 0.34, "wood");
  for (const x of [-2.85, 2.85]) b.box(x, 3.86, 0, 0.34, 0.28, 6.5, "wood");
  for (const side of [-1, 1])
    b.transformedBox([side * 2.2, 5.55, 0], [5.2, 0.2, 9], "roof_red", [
      0,
      0,
      side * -29,
    ]);
  b.cylinderBetween([0, 6.82, -4.45], [0, 6.82, 4.45], 0.14, 8, "roof_edge");
  b.write("pavilion_v3.obj");
}
function makeTree(filename, variant = 0) {
  const b = new ObjBuilder(filename.replace(".obj", "")),
    height = variant ? 5.35 : 4.9;
  b.cylinderBetween(
    [0, 0, 0],
    [0.12, height, -0.08],
    variant ? 0.38 : 0.43,
    10,
    "wood_dark",
  );
  const branches = variant
    ? [
        [
          [0.02, 2.9, 0],
          [-1.1, 4.75, -0.7],
        ],
        [
          [0.06, 3.3, 0],
          [1.25, 5.05, 0.6],
        ],
        [
          [0.05, 3.75, 0],
          [0.55, 5.8, -0.45],
        ],
        [
          [0.02, 4, 0],
          [-0.75, 5.55, 0.8],
        ],
      ]
    : [
        [
          [-0.02, 2.7, 0],
          [-1.35, 4.25, 0.45],
        ],
        [
          [0.08, 3, 0],
          [1.45, 4.55, -0.5],
        ],
        [
          [0.03, 3.65, 0],
          [-0.55, 5.45, -0.8],
        ],
      ];
  for (const [a, c] of branches) b.cylinderBetween(a, c, 0.19, 8, "wood");
  const crowns = variant
    ? [
        [[0.2, 6.45, 0], [2.25, 1.65, 2.4], "leaf_dark"],
        [[-1.2, 5.95, -0.8], [1.55, 1.35, 1.45], "leaf"],
        [[1.45, 6.05, 0.7], [1.45, 1.28, 1.55], "leaf_light"],
        [[0.4, 7.8, -0.35], [1.45, 1.3, 1.45], "leaf"],
      ]
    : [
        [[0, 6.35, 0], [2.5, 1.7, 2.25], "leaf"],
        [[-1.45, 5.5, 0.45], [1.55, 1.22, 1.45], "leaf_dark"],
        [[1.48, 5.8, -0.5], [1.65, 1.35, 1.5], "leaf_light"],
        [[-0.45, 7.55, -0.35], [1.5, 1.25, 1.35], "leaf_light"],
      ];
  crowns.forEach(([c, r, m], i) =>
    b.ellipsoid(c, r, 12, 8, m, 250 + variant * 40 + i),
  );
  b.write(filename);
}
makeTree("tree_v3_a.obj");
makeTree("tree_v3_b.obj", 1);
{
  const b = new ObjBuilder("StoryWorld_Rock_v3"),
    c = [
      [0, 1.25, 0],
      [-0.65, 0.75, 0.55],
      [0.75, 0.58, -0.42],
    ],
    r = [
      [1.75, 1.55, 1.55],
      [1, 0.82, 0.9],
      [0.86, 0.68, 0.82],
    ];
  c.forEach((center, i) =>
    b.ellipsoid(center, r[i], 10, 6, i === 0 ? "rock" : "stone_dark", 320 + i),
  );
  b.write("rock_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_Fence_v3");
  for (const x of [-3.9, -1.95, 0, 1.95, 3.9]) {
    b.cylinderBetween(
      [x, 0, 0],
      [x + 0.03, 2.08 - Math.abs(x) * 0.04, 0],
      0.14,
      7,
      "wood_dark",
    );
    b.transformedBox(
      [x, 2.03 - Math.abs(x) * 0.04, 0],
      [0.32, 0.24, 0.3],
      "wood",
      [0, 0, 45],
    );
  }
  b.cylinderBetween([-4.1, 0.7, 0], [4.1, 0.82, 0], 0.12, 7, "wood");
  b.cylinderBetween([-4.1, 1.43, 0], [4.1, 1.5, 0], 0.12, 7, "wood");
  b.write("fence_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_Lantern_v3");
  b.cylinder(0, 0, 0, 0.13, 4.2, 8, "wood_dark");
  b.box(0, 3.7, 0, 1.55, 0.16, 0.18, "wood");
  b.cylinderBetween([0.62, 3.7, 0], [0.62, 3.27, 0], 0.055, 6, "metal");
  b.box(0.62, 2.22, 0, 0.92, 1.1, 0.82, "lantern");
  b.box(0.62, 2.32, -0.43, 0.7, 0.88, 0.045, "paper");
  b.box(0.62, 3.32, 0, 1.05, 0.16, 0.95, "gold");
  b.box(0.62, 2.08, 0, 1.05, 0.16, 0.95, "gold");
  for (const x of [0.18, 1.06])
    for (const z of [-0.39, 0.39])
      b.box(x, 2.12, z, 0.06, 1.26, 0.06, "wood_dark");
  b.write("lantern_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_Courtyard_v3");
  b.box(0, 0, 0, 10, 0.18, 7.6, "earth", [0, 0, 0], [3, 2]);
  for (const z of [-3.65, 3.65]) {
    b.box(-3.7, 0.15, z, 2.6, 1.55, 0.34, "stone");
    b.box(3.7, 0.15, z, 2.6, 1.55, 0.34, "stone");
  }
  for (const x of [-4.82, 4.82]) b.box(x, 0.15, 0, 0.34, 1.55, 7.6, "stone");
  for (const x of [-1.15, 1.15])
    b.cylinder(x, 0.12, -3.65, 0.17, 2.15, 8, "wood_dark");
  b.box(0, 1.8, -3.65, 3.2, 0.22, 0.3, "wood");
  b.write("courtyard_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_Barrels_v3");
  [-0.62, 0.62].forEach((x, i) => {
    b.cylinder(x, 0, 0, 0.52, 1.25 + i * 0.12, 12, "wood");
    for (const y of [0.18, 1.02 + i * 0.12])
      b.cylinderBetween(
        [x - 0.01, y, 0],
        [x + 0.01, y + 0.05, 0],
        0.55,
        12,
        "metal",
      );
  });
  b.write("barrels_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_Woodpile_v3");
  for (let row = 0; row < 3; row++)
    for (let i = 0; i < 5 - row; i++) {
      const x = (i - (4 - row) / 2) * 0.56,
        y = 0.23 + row * 0.45;
      b.cylinderBetween(
        [x, y, -0.65],
        [x + (hash(row, i, 401) - 0.5) * 0.12, y + 0.02, 0.65],
        0.21,
        8,
        row % 2 ? "wood" : "wood_dark",
      );
    }
  b.write("woodpile_v3.obj");
}
{
  const b = new ObjBuilder("StoryWorld_Reeds_v3");
  for (let i = 0; i < 28; i++) {
    const x = (hash(i, 1, 501) - 0.5) * 4,
      z = (hash(i, 2, 503) - 0.5) * 2,
      h = 1.3 + hash(i, 3, 509) * 1.2;
    b.cylinderBetween(
      [x, 0, z],
      [x + Math.sin(i) * 0.08, h, z + Math.cos(i) * 0.08],
      0.025,
      5,
      "reed",
    );
    if (i % 3 === 0)
      b.ellipsoid([x, h + 0.1, z], [0.08, 0.3, 0.08], 6, 4, "earth", 540 + i);
  }
  b.write("reeds_v3.obj");
}
const assets = fs
  .readdirSync(outputDir)
  .filter((name) => name.includes("_v3") && name.endsWith(".obj"))
  .sort();
console.log(`Generated ${assets.length} Story World v3 assets in ${outputDir}`);
