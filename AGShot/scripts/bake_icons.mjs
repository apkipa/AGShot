import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

function parseArgs(argv) {
    const out = {};
    for (let i = 2; i < argv.length; ++i) {
        const arg = argv[i];
        if (arg === '--header-only') {
            out['header-only'] = true;
        } else if (arg.startsWith('--') && i + 1 < argv.length) {
            out[arg.slice(2)] = argv[++i];
        }
    }
    return out;
}

function parseAttrs(tagText, iconName) {
    if (/[a-zA-Z_:][\w:.-]*\s*=\s*'/.test(tagText)) {
        throw new Error(`Single-quoted attributes are not supported in ${iconName}; use double quotes`);
    }
    const attrs = {};
    const re = /([a-zA-Z_:][\w:.-]*)\s*=\s*"([^"]*)"/g;
    let m;
    while ((m = re.exec(tagText)) !== null) {
        attrs[m[1]] = m[2];
    }
    return attrs;
}

function rectToSvgD(x, y, w, h, rx, ry) {
    const rX = Math.min(Math.max(0, rx || 0), w / 2);
    const rY = Math.min(Math.max(0, ry || rX), h / 2);
    if (rX <= 0 || rY <= 0) {
        return `M${x} ${y}H${x + w}V${y + h}H${x}Z`;
    }
    return [
        `M${x + rX} ${y}`,
        `H${x + w - rX}`,
        `A${rX} ${rY} 0 0 1 ${x + w} ${y + rY}`,
        `V${y + h - rY}`,
        `A${rX} ${rY} 0 0 1 ${x + w - rX} ${y + h}`,
        `H${x + rX}`,
        `A${rX} ${rY} 0 0 1 ${x} ${y + h - rY}`,
        `V${y + rY}`,
        `A${rX} ${rY} 0 0 1 ${x + rX} ${y}Z`,
    ].join('');
}

function circleToSvgD(cx, cy, r) {
    return `M${cx - r} ${cy}A${r} ${r} 0 1 0 ${cx + r} ${cy}A${r} ${r} 0 1 0 ${cx - r} ${cy}Z`;
}

function pointsToSvgD(pointsStr, close) {
    const nums = pointsStr.trim().split(/[\s,]+/).map(Number).filter(n => !Number.isNaN(n));
    if (nums.length < 4) return '';
    let d = `M${nums[0]} ${nums[1]}`;
    for (let i = 2; i + 1 < nums.length; i += 2) {
        d += `L${nums[i]} ${nums[i + 1]}`;
    }
    return close ? `${d}Z` : d;
}

function mapCap(PathKit, val) {
    switch ((val || 'butt').toLowerCase()) {
    case 'round': return PathKit.StrokeCap.ROUND;
    case 'square': return PathKit.StrokeCap.SQUARE;
    default: return PathKit.StrokeCap.BUTT;
    }
}

function mapJoin(PathKit, val) {
    switch ((val || 'miter').toLowerCase()) {
    case 'round': return PathKit.StrokeJoin.ROUND;
    case 'bevel': return PathKit.StrokeJoin.BEVEL;
    default: return PathKit.StrokeJoin.MITER;
    }
}

function fmtNum(n) {
    const r = Math.round(n * 100) / 100;
    if (Object.is(r, -0) || r === 0) return '0';
    let s = String(r);
    if (s.startsWith('0.')) s = s.slice(1);
    else if (s.startsWith('-0.')) s = '-' + s.slice(2);
    return s;
}

function cmdsToSvgD(PathKit, cmds) {
    let out = '';
    let cx = 0;
    let cy = 0;
    let sx = 0;
    let sy = 0;
    let lastCmd = '';

    const emitCmd = (cmd, nums) => {
        const repeat = (cmd === lastCmd && cmd !== 'M');
        let seg = repeat ? '' : cmd;
        for (let i = 0; i < nums.length; ++i) {
            const f = fmtNum(nums[i]);
            if (i === 0) {
                seg += (repeat && !f.startsWith('-')) ? (' ' + f) : f;
            } else {
                seg += f.startsWith('-') ? f : (' ' + f);
            }
        }
        out += seg;
        lastCmd = (cmd === 'M') ? 'L' : cmd;
    };

    // Convert rational quadratic conic (P0, P1, P2, w) to a G1 midpoint-matched
    // cubic Bezier: alpha = 4w / (3 * (1 + w)) (exact 0.55228475 for 90-deg arcs,
    // exact degree elevation 2/3 for w = 1). Subdivide acute conics (w < 0.65).
    const emitConic = (x0, y0, x1, y1, x2, y2, w, depth) => {
        if (w < 0.65 && depth < 2) {
            const denom = 1 + w;
            const mx = (x0 + 2 * w * x1 + x2) / (2 * denom);
            const my = (y0 + 2 * w * y1 + y2) / (2 * denom);
            const l1x = (x0 + w * x1) / denom;
            const l1y = (y0 + w * y1) / denom;
            const r1x = (w * x1 + x2) / denom;
            const r1y = (w * y1 + y2) / denom;
            const nw = Math.sqrt(denom * 0.5);
            emitConic(x0, y0, l1x, l1y, mx, my, nw, depth + 1);
            emitConic(mx, my, r1x, r1y, x2, y2, nw, depth + 1);
            return;
        }
        const alpha = (4 * w) / (3 * (1 + w));
        emitCmd('C', [
            x0 + alpha * (x1 - x0),
            y0 + alpha * (y1 - y0),
            x2 + alpha * (x1 - x2),
            y2 + alpha * (y1 - y2),
            x2,
            y2,
        ]);
    };

    for (const c of cmds) {
        const verb = c[0];
        if (verb === PathKit.MOVE_VERB) {
            emitCmd('M', [c[1], c[2]]);
            cx = sx = c[1];
            cy = sy = c[2];
        } else if (verb === PathKit.LINE_VERB) {
            if (Math.hypot(c[1] - cx, c[2] - cy) >= 0.005) {
                if (Math.abs(c[2] - cy) < 0.005) {
                    emitCmd('H', [c[1]]);
                } else if (Math.abs(c[1] - cx) < 0.005) {
                    emitCmd('V', [c[2]]);
                } else {
                    emitCmd('L', [c[1], c[2]]);
                }
                cx = c[1];
                cy = c[2];
            }
        } else if (verb === PathKit.QUAD_VERB) {
            emitCmd('Q', [c[1], c[2], c[3], c[4]]);
            cx = c[3];
            cy = c[4];
        } else if (verb === PathKit.CONIC_VERB) {
            emitConic(cx, cy, c[1], c[2], c[3], c[4], c[5], 0);
            cx = c[3];
            cy = c[4];
        } else if (verb === PathKit.CUBIC_VERB) {
            emitCmd('C', [c[1], c[2], c[3], c[4], c[5], c[6]]);
            cx = c[5];
            cy = c[6];
        } else if (verb === PathKit.CLOSE_VERB) {
            out += 'Z';
            lastCmd = 'Z';
            cx = sx;
            cy = sy;
        }
    }
    return out;
}

function bakeSvg(PathKit, svgText, iconName) {
    const svgMatch = /<svg\b([^>]*)>([\s\S]*?)<\/svg>/i.exec(svgText);
    if (!svgMatch) {
        throw new Error(`Invalid SVG root in ${iconName}`);
    }
    const rootAttrs = parseAttrs(svgMatch[1], iconName);
    const viewBox = rootAttrs.viewBox || '0 0 16 16';
    const body = svgMatch[2];

    const unsupportedTag = /<(g|ellipse|use|clipPath|mask|defs|style|text|image|symbol|linearGradient|radialGradient)\b/i.exec(body);
    if (unsupportedTag) {
        throw new Error(`Unsupported SVG element <${unsupportedTag[1]}> in ${iconName}`);
    }
    if (/\btransform\s*=/i.test(svgMatch[1]) || /\btransform\s*=/i.test(body)) {
        throw new Error(`Unsupported transform attribute in ${iconName}; bake coordinates directly`);
    }

    const elemRe = /<(path|rect|circle|line|polyline|polygon)\b([^>]*?)\/?>(?:<\/\1>)?/gi;
    let combined = null;

    const addPiece = (piece) => {
        if (!piece) return;
        piece.simplify();
        if (!combined) {
            combined = piece;
            return;
        }
        if (!combined.op(piece, PathKit.PathOp.UNION)) {
            piece.delete();
            throw new Error(`PathKit UNION failed for ${iconName}`);
        }
        piece.delete();
        combined.simplify();
    };

    let m;
    while ((m = elemRe.exec(body)) !== null) {
        const tag = m[1].toLowerCase();
        const a = parseAttrs(m[2], iconName);

        let d = '';
        if (tag === 'path') {
            d = a.d || '';
        } else if (tag === 'rect') {
            d = rectToSvgD(
                parseFloat(a.x || '0'),
                parseFloat(a.y || '0'),
                parseFloat(a.width || '0'),
                parseFloat(a.height || '0'),
                parseFloat(a.rx || '0'),
                parseFloat(a.ry || '0'));
        } else if (tag === 'circle') {
            d = circleToSvgD(
                parseFloat(a.cx || '0'),
                parseFloat(a.cy || '0'),
                parseFloat(a.r || '0'));
        } else if (tag === 'line') {
            d = `M${parseFloat(a.x1 || '0')} ${parseFloat(a.y1 || '0')}L${parseFloat(a.x2 || '0')} ${parseFloat(a.y2 || '0')}`;
        } else if (tag === 'polyline' || tag === 'polygon') {
            d = pointsToSvgD(a.points || '', tag === 'polygon');
        }
        if (!d) continue;

        const fill = (a.fill !== undefined ? a.fill : (rootAttrs.fill || 'none')).toLowerCase();
        const stroke = (a.stroke !== undefined ? a.stroke : (rootAttrs.stroke || 'none')).toLowerCase();
        const strokeWidth = parseFloat(a['stroke-width'] !== undefined ? a['stroke-width'] : (rootAttrs['stroke-width'] || '1'));
        const cap = mapCap(PathKit, a['stroke-linecap'] !== undefined ? a['stroke-linecap'] : rootAttrs['stroke-linecap']);
        const join = mapJoin(PathKit, a['stroke-linejoin'] !== undefined ? a['stroke-linejoin'] : rootAttrs['stroke-linejoin']);

        if (fill !== 'none' && fill !== 'transparent') {
            const pFill = PathKit.FromSVGString(d);
            if (!pFill) {
                throw new Error(`FromSVGString failed (fill) in ${iconName}: ${d}`);
            }
            addPiece(pFill);
        }

        if (stroke !== 'none' && stroke !== 'transparent' && strokeWidth > 0) {
            const pStroke = PathKit.FromSVGString(d);
            if (!pStroke) {
                throw new Error(`FromSVGString failed (stroke) in ${iconName}: ${d}`);
            }
            if (!pStroke.stroke({ width: strokeWidth, join, cap, miter_limit: 4.0 })) {
                pStroke.delete();
                throw new Error(`stroke() failed in ${iconName}`);
            }
            addPiece(pStroke);
        }
    }

    if (!combined) {
        throw new Error(`No drawable elements found in ${iconName}`);
    }
    const fillRule = combined.getFillTypeString();
    const outD = cmdsToSvgD(PathKit, combined.toCmds());
    combined.delete();
    if (!outD) {
        throw new Error(`Empty baked path for ${iconName}`);
    }
    const fillRuleAttr = fillRule === 'evenodd' ? ' fill-rule="evenodd"' : '';
    return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${viewBox}" fill="currentColor"><path${fillRuleAttr} d="${outD}"/></svg>`;
}

function buildBinaryPack(bakedEntries) {
    // Format: [4B magic "AGIP"][4B version=1][4B count]
    //         repeated count times: [4B nameLen][4B svgLen][nameBytes][svgBytes]
    const chunks = [];
    const header = Buffer.alloc(12);
    header.write('AGIP', 0, 4, 'ascii');
    header.writeUInt32LE(1, 4);
    header.writeUInt32LE(bakedEntries.length, 8);
    chunks.push(header);

    for (const entry of bakedEntries) {
        const nameBuf = Buffer.from(entry.name, 'utf8');
        const svgBuf = Buffer.from(entry.svg, 'utf8');
        const rec = Buffer.alloc(8);
        rec.writeUInt32LE(nameBuf.length, 0);
        rec.writeUInt32LE(svgBuf.length, 4);
        chunks.push(rec, nameBuf, svgBuf);
    }
    return Buffer.concat(chunks);
}

function buildHeader(manifest) {
    const lines = [
        '#pragma once',
        '',
        '// Auto-generated by scripts/bake_icons.mjs from assets/icons/manifest.json.',
        '// Do not edit manually.',
        '',
        '#include <cstddef>',
        '',
        'namespace agshot',
        '{',
        '    enum class ToolbarIconKind : std::size_t',
        '    {',
    ];
    let hudCount = 0;
    for (let i = 0; i < manifest.length; ++i) {
        if (!manifest[i].enumName) {
            continue;
        }
        if (i !== hudCount) {
            throw new Error(`Manifest HUD icon "${manifest[i].name}" must precede non-HUD icons`);
        }
        lines.push(`        ${manifest[i].enumName} = ${i},`);
        hudCount++;
    }
    lines.push('        Count');
    lines.push('    };');
    lines.push('}');
    lines.push('');
    return lines.join('\n');
}

function writeFileIfChanged(filePath, data) {
    fs.mkdirSync(path.dirname(path.resolve(filePath)), { recursive: true });
    if (fs.existsSync(filePath)) {
        const prev = fs.readFileSync(filePath);
        const next = Buffer.isBuffer(data) ? data : Buffer.from(data, 'utf8');
        if (prev.equals(next)) {
            return false;
        }
    }
    fs.writeFileSync(filePath, data);
    return true;
}

async function main() {
    const args = parseArgs(process.argv);
    const iconsDir = path.resolve(args['icons-dir'] || 'AGShot/assets/icons');
    const headerOut = args['header-out'] ? path.resolve(args['header-out']) : null;
    const manifest = JSON.parse(fs.readFileSync(path.join(iconsDir, 'manifest.json'), 'utf8'));

    if (args['header-only']) {
        if (!headerOut) {
            throw new Error('--header-only requires --header-out');
        }
        writeFileIfChanged(headerOut, buildHeader(manifest));
        console.log(`Generated icon header -> ${headerOut}`);
        return;
    }

    const packOut = path.resolve(args['pack-out'] || 'Z:/TEMP/icons.pack');
    const pathkitDir = path.resolve(
        args['pathkit-dir'] || 'AGShot/frontend/node_modules/pathkit-wasm/bin');

    const require = createRequire(import.meta.url);
    const PathKitInit = require(path.join(pathkitDir, 'pathkit.js'));
    const wasmBinary = fs.readFileSync(path.join(pathkitDir, 'pathkit.wasm'));
    const PathKit = await PathKitInit({ wasmBinary });

    const baked = [];
    let totalRawSvgBytes = 0;

    for (const item of manifest) {
        const raw = fs.readFileSync(path.join(iconsDir, item.file), 'utf8');
        const outSvg = bakeSvg(PathKit, raw, item.name);
        totalRawSvgBytes += Buffer.byteLength(outSvg, 'utf8');
        baked.push({ name: item.name, svg: outSvg });
    }

    const packBuf = buildBinaryPack(baked);
    fs.mkdirSync(path.dirname(path.resolve(packOut)), { recursive: true });
    fs.writeFileSync(packOut, packBuf);

    if (headerOut) {
        writeFileIfChanged(headerOut, buildHeader(manifest));
    }

    console.log(`Baked ${baked.length} icons -> ${packOut} (${packBuf.length} B raw, ${totalRawSvgBytes} B svg)`);
}

main().catch((err) => {
    console.error(err);
    process.exit(1);
});
