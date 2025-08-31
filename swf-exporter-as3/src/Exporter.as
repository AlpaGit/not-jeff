// Exporter.as — AIR headless exporter for SWF symbols → Atlases + single SWF JSON
// Usage:
//   adl application.xml -- args "SWF|OUT|FPS|SCALE|PAD|ATLASW|ATLASH|DEDUP|PACKMODE"
//   PACKMODE: "global" (pack toutes les anims ensemble) ou "perSymbol" (par symbole)
package {
import flash.desktop.NativeApplication;
import flash.display.*;
import flash.events.*;
import flash.filesystem.*;
import flash.geom.*;
import flash.net.*;
import flash.system.*;
import flash.utils.*;

public class Exporter extends Sprite {
    private static function writeBytesTo(file:File, bytes:ByteArray):void {
        const fs:FileStream = new FileStream();
        fs.open(file, FileMode.WRITE);
        fs.writeBytes(bytes);
        fs.close();
    }

    private static function sanitize(s:String):String {
        return s.replace(/[\/\\:\*\?"<>\|]/g, "_").replace(/\./g, "_");
    }

    public function Exporter() {
        const raw:String = LoaderInfo(root.loaderInfo).parameters["args"];
        const parts:Array = raw ? raw.split("|") : [];
        const swfPath:String = parts[0] || "C:\\Users\\Alpa\\Documents\\GitHub\\SwfExporter\\431.swf";
        const outDir:String = parts[1] || "C:\\Users\\Alpa\\Documents\\GitHub\\SwfExporter\\sprites_uncompressed";
        const forcedFPS:Number = parts[2] ? Number(parts[2]) : NaN;
        const scale:Number = parts[3] ? Number(parts[3]) : 2.0;
        const pad:int = parts[4] ? int(parts[4]) : 2;
        const atlasW:int = parts[5] ? int(parts[5]) : 1024;
        const atlasH:int = parts[6] ? int(parts[6]) : 1024;
        const dedup:Boolean = parts[7] ? (int(parts[7]) != 0) : true;
        const packMode:String = parts[8] ? String(parts[8]) : "global"; // "global" | "perSymbol"

        const oldQ:String = stage ? stage.quality : StageQuality.HIGH;
        if (stage) stage.quality = StageQuality.BEST;

        const swfFile:File = new File(swfPath);
        if (!swfFile.exists) {
            trace("[err] SWF not found:", swfPath);
            exit(1);
            return;
        }

        const loader:Loader = new Loader();
        loader.contentLoaderInfo.addEventListener(Event.COMPLETE, function (_:Event):void {
            try {
                runExport(loader, swfFile, new File(outDir), forcedFPS, scale, pad, atlasW, atlasH, dedup, packMode);
            } catch (e:Error) {
                trace("[err] Exception:", e.name, e.message);
                exit(1);
            } finally {
                if (stage) stage.quality = oldQ;
            }
        });
        const ctx:LoaderContext = new LoaderContext(false, new ApplicationDomain());
        loader.load(new URLRequest(swfFile.url), ctx);
    }

    // ========= MAIN ==========================================================
    private function runExport(loader:Loader, swfFile:File, outRoot:File,
                               forcedFPS:Number, scale:Number, pad:int,
                               atlasW:int, atlasH:int, dedup:Boolean, packMode:String):void {
        const domain:ApplicationDomain = loader.contentLoaderInfo.applicationDomain;
        const stageFPS:Number =
                !isNaN(forcedFPS) ? forcedFPS :
                        (loader.content is MovieClip && MovieClip(loader.content).stage ?
                                MovieClip(loader.content).stage.frameRate : 24);

        const swfBase:String = swfFile.name.replace(/\.[sS][wW][fF]$/, "");
        const swfOutDir:File = outRoot.resolvePath(swfBase);
        swfOutDir.createDirectory();

        if (!("getQualifiedDefinitionNames" in domain)) {
            trace("[err] Runtime lacks getQualifiedDefinitionNames(); need AIR/FP11+");
            exit(1);
            return;
        }
        const names:Vector.<String> = domain["getQualifiedDefinitionNames"]();

        // SWF-level meta
        const swfMeta:Object = {
            swf: swfFile.name,
            fps: stageFPS,
            scale: scale,
            padding: pad,
            atlasSize: {w: atlasW, h: atlasH},
            pages: [],     // rempli en mode global
            symbols: []    // { name, export, type, labels, pages?, frames[] }
        };

        if (packMode == "global") {
            exportGlobal(domain, names, swfOutDir, swfMeta, stageFPS, scale, pad, atlasW, atlasH, dedup);
        } else {
            exportPerSymbol(domain, names, swfOutDir, swfMeta, stageFPS, scale, pad, atlasW, atlasH, dedup);
        }

        // Écrit le JSON SWF unique
        const jsonBytes:ByteArray = new ByteArray();
        jsonBytes.writeUTFBytes(JSON.stringify(swfMeta, null, 2));
        writeBytesTo(swfOutDir.resolvePath(swfBase + ".json"), jsonBytes);

        trace("[ok] Export complete:", swfOutDir.nativePath);
        exit(0);
    }

    // ========= GLOBAL PACK: toutes les frames ensemble ======================
    private function exportGlobal(domain:ApplicationDomain, names:Vector.<String>, swfOutDir:File, swfMeta:Object,
                                  fps:Number, scale:Number, pad:int, atlasW:int, atlasH:int, dedup:Boolean):void {
        // 1) Rendre toutes les frames de tous les symboles -> frames[]
        var frames:Array = []; // items: { bmp,w,h,ox,oy, idx, duration, symIndex:int, symName:String }
        var symbols:Array = []; // sortie: mapping des frames par symbole
        var symIndex:int = 0;

        for each (var qname:String in names) {
            var cls:Class;
            try {
                cls = domain.hasDefinition(qname) ? domain.getDefinition(qname) as Class : null;
            } catch (_:*) {
                cls = null;
            }
            if (!cls) continue;

            if (qname.indexOf("DisplayInfo_") != -1) {
                continue;
            }

            var inst:Object;
            try {
                inst = new cls();
            } catch (_:*) {
                continue;
            }
            if (!(inst is DisplayObject)) continue;

            var mc:MovieClip = inst as MovieClip;
            var disp:DisplayObject = inst as DisplayObject;

            addChild(disp);

            var total:int = mc ? mc.totalFrames : 1;
            var labels:Array = [];
            if (mc) for each (var fl:FrameLabel in mc.currentLabels) labels.push({name: fl.name, frame: fl.frame});

            var rendered:Array = [];
            var last:Object = null;

            for (var f:int = 1; f <= total; f++) {
                if (mc) mc.gotoAndStop(f);

                var raw:Rectangle = disp.getBounds(this);
                if (raw.width < 1 || raw.height < 1) raw = new Rectangle(0, 0, Math.max(1, disp.width), Math.max(1, disp.height));

                var outW:int = int(Math.ceil(raw.width * scale)) + pad * 2;
                var outH:int = int(Math.ceil(raw.height * scale)) + pad * 2;
                var ox:Number = raw.x * scale - pad;
                var oy:Number = raw.y * scale - pad;

                var bmd:BitmapData = new BitmapData(outW, outH, true, 0);
                var mtx:Matrix = new Matrix();
                mtx.scale(scale, scale);
                mtx.translate(-raw.x * scale + pad, -raw.y * scale + pad);
                bmd.draw(disp, mtx, null, null, null, true);

                var cur:Object = {
                    bmp: bmd,
                    w: outW,
                    h: outH,
                    ox: ox,
                    oy: oy,
                    idx: f,
                    duration: 1,
                    symIndex: symIndex,
                    symName: qname
                };
                cur.poly = computeConvexHullPolygon(
                        bmd,
                        /*alphaThresh*/ 96,   // plus haut = moins permissif
                        /*sampleStep*/ -1,    // adaptatif
                        /*shrinkPx*/   1.0    // rétrécit un peu pour éviter l'AA
                );


                if (dedup && last && samePixels(last.bmp as BitmapData, bmd)) {
                    last.duration += 1;
                    bmd.dispose();
                } else {
                    rendered.push(cur);
                    last = cur;
                }
            }

            // Garder la liste des indices (dans frames) pour ce symbole
            var perSymIndices:Array = [];
            for each (var rf:Object in rendered) {
                perSymIndices.push(frames.length);
                frames.push(rf);
            }

            symbols.push({
                name: qname,
                export: sanitize(qname),
                type: (mc ? "MovieClip" : "DisplayObject"),
                labels: labels,
                frameIndices: perSymIndices
            });

            removeChild(disp);
            symIndex++;
        }

        // 2) Packer toutes les frames ensemble
        var packRes:Object = packIntoAtlases(frames, atlasW, atlasH, /*spacing*/2);

        // 3) Sauver les pages globales
        var pageFiles:Array = [];
        for (var pi:int = 0; pi < packRes.pages.length; ++pi) {
            var pageBmp:BitmapData = packRes.pages[pi] as BitmapData;
            var bytes:ByteArray = new ByteArray();
            pageBmp.encode(pageBmp.rect, new PNGEncoderOptions(true), bytes);
            var pageName:String = "atlas_" + pi + ".png";
            writeBytesTo(swfOutDir.resolvePath(pageName), bytes);
            pageBmp.dispose();
            pageFiles.push(pageName);
        }
        swfMeta.pages = pageFiles;

        // 4) Construire les frames par symbole, mappées sur pages globales
        // packRes.items est aligné sur frames[] (même ordre)
        for (var si:int = 0; si < symbols.length; ++si) {
            var sym:Object = symbols[si];
            var framesMeta:Array = [];
            for each (var fi:int in sym.frameIndices) {
                var it:Object = packRes.items[fi]; // {page,x,y,w,h,ox,oy,idx,duration,symIndex}
                framesMeta.push({
                    idx: it.idx,
                    page: it.page,
                    x: it.x, y: it.y, w: it.w, h: it.h,
                    ox: it.ox, oy: it.oy,
                    duration: it.duration,
                    poly: it.poly
                });
            }
            framesMeta.sortOn("idx", Array.NUMERIC);
            swfMeta.symbols.push({
                name: sym.name,
                export: sym.export,
                type: sym.type,
                labels: sym.labels,
                frames: framesMeta
            });
        }

        // cleanup des bitmaps sources (déjà libérés par pack)
        for each (var fr:Object in frames) if (fr.bmp) BitmapData(fr.bmp).dispose();
    }

    // ========= PER SYMBOL PACK (comme avant, mais atlas + JSON par symbole) ==
    private function exportPerSymbol(domain:ApplicationDomain, names:Vector.<String>, swfOutDir:File, swfMeta:Object,
                                     fps:Number, scale:Number, pad:int, atlasW:int, atlasH:int, dedup:Boolean):void {
        for each (var qname:String in names) {
            var cls:Class;
            try {
                cls = domain.hasDefinition(qname) ? domain.getDefinition(qname) as Class : null;
            } catch (_:*) {
                cls = null;
            }
            if (!cls) continue;

            var inst:Object;
            try {
                inst = new cls();
            } catch (_:*) {
                continue;
            }
            if (!(inst is DisplayObject)) continue;

            var mc:MovieClip = inst as MovieClip;
            var disp:DisplayObject = inst as DisplayObject;
            addChild(disp);

            var total:int = mc ? mc.totalFrames : 1;
            var labels:Array = [];
            if (mc) for each (var fl:FrameLabel in mc.currentLabels) labels.push({name: fl.name, frame: fl.frame});

            var rendered:Array = [];
            var last:Object = null;
            for (var f:int = 1; f <= total; f++) {
                if (mc) mc.gotoAndStop(f);
                var raw:Rectangle = disp.getBounds(this);
                if (raw.width < 1 || raw.height < 1) raw = new Rectangle(0, 0, Math.max(1, disp.width), Math.max(1, disp.height));
                var outW:int = int(Math.ceil(raw.width * scale)) + pad * 2;
                var outH:int = int(Math.ceil(raw.height * scale)) + pad * 2;
                var ox:Number = raw.x * scale - pad;
                var oy:Number = raw.y * scale - pad;

                var bmd:BitmapData = new BitmapData(outW, outH, true, 0);
                var mtx:Matrix = new Matrix();
                mtx.scale(scale, scale);
                mtx.translate(-raw.x * scale + pad, -raw.y * scale + pad);
                bmd.draw(disp, mtx, null, null, null, true);

                var cur:Object = {bmp: bmd, w: outW, h: outH, ox: ox, oy: oy, idx: f, duration: 1};
                cur.poly = computeConvexHullPolygon(bmd, 96, -1, 1.0);


                if (dedup && last && samePixels(last.bmp as BitmapData, bmd)) {
                    last.duration += 1;
                    bmd.dispose();
                } else {
                    rendered.push(cur);
                    last = cur;
                }
            }

            // pack par symbole
            var packRes:Object = packIntoAtlases(rendered, atlasW, atlasH, 2);

            var symFolder:String = sanitize(qname);
            var symDir:File = swfOutDir.resolvePath(symFolder);
            symDir.createDirectory();

            var pageFiles:Array = [];
            for (var pi:int = 0; pi < packRes.pages.length; ++pi) {
                var pageBmp:BitmapData = packRes.pages[pi] as BitmapData;
                var bytes:ByteArray = new ByteArray();
                pageBmp.encode(pageBmp.rect, new PNGEncoderOptions(true), bytes);
                var pageName:String = symFolder + "_" + pi + ".png";
                writeBytesTo(symDir.resolvePath(pageName), bytes);
                pageBmp.dispose();
                pageFiles.push(pageName);
            }

            packRes.items.sortOn("idx", Array.NUMERIC);
            var framesMeta:Array = [];
            for each (var it:Object in packRes.items) {
                framesMeta.push({
                    idx: it.idx,
                    page: it.page,
                    x: it.x, y: it.y, w: it.w, h: it.h,
                    ox: it.ox, oy: it.oy,
                    duration: it.duration
                });
            }

            swfMeta.symbols.push({
                name: qname,
                export: symFolder,
                type: (mc ? "MovieClip" : "DisplayObject"),
                labels: labels,
                pages: pageFiles,  // en perSymbol, chaque symbole a ses propres pages
                frames: framesMeta
            });

            for each (var fr:Object in rendered) if (fr.bmp) BitmapData(fr.bmp).dispose();
            removeChild(disp);
        }
    }

    // ========= PACKER / UTILS ===============================================
    private function samePixels(a:BitmapData, b:BitmapData):Boolean {
        if (!a || !b) return false;
        if (a.width != b.width || a.height != b.height) return false;
        var cmp:* = a.compare(b);
        return (cmp is uint) && (uint(cmp) == 0);
    }

    // Shelf packer: retourne { pages:[BitmapData], items:[{..}] } dans le même ordre que frames[]
    private function packIntoAtlases(frames:Array, pageW:int, pageH:int, spacing:int):Object {
        var pages:Array = [];
        var items:Array = [];
        var page:BitmapData = new BitmapData(pageW, pageH, true, 0);
        pages.push(page);
        var x:int = 0, y:int = 0, shelfH:int = 0, pageIndex:int = 0;

        function newPage():void {
            page = new BitmapData(pageW, pageH, true, 0);
            pages.push(page);
            x = 0;
            y = 0;
            shelfH = 0;
            pageIndex++;
        }

        for each (var f:Object in frames) {
            var w:int = f.w, h:int = f.h;
            if (x + w > pageW) {
                x = 0;
                y += shelfH + spacing;
                shelfH = 0;
            }
            if (y + h > pageH) {
                newPage();
            }
            page.copyPixels(f.bmp as BitmapData, new Rectangle(0, 0, w, h), new Point(x, y), null, null, true);
            items.push({
                idx: f.idx,
                page: pageIndex,
                x: x,
                y: y,
                w: w,
                h: h,
                ox: f.ox,
                oy: f.oy,
                duration: f.duration,
                symIndex: f.symIndex,
                poly: f.poly
            });
            x += w + spacing;
            shelfH = Math.max(shelfH, h);
        }
        return {pages: pages, items: items};
    }

    private function exit(code:int):void {
        stage.addEventListener(Event.ENTER_FRAME, function (_:Event):void {
            NativeApplication.nativeApplication.exit(code);
        });
    }

// ---------- CONVEX HULL (Graham scan) ---------------------------------------

// Renvoie un polygone convexe en coords pixels locales au bitmap recadré (0..w,0..h)
    private function computeConvexHullPolygon(
            bmd:BitmapData,
            alphaThresh:int = 96,   // seuil alpha (plus haut => moins permissif)
            sampleStep:int = -1,    // -1 = step adaptatif en fct de la taille
            shrinkPx:Number = 1.0   // 0 pour désactiver, sinon "inset" anti-AA
    ):Array {
        var W:int = bmd.width, H:int = bmd.height;
        if (W <= 0 || H <= 0) return [];

        var step:int = (sampleStep > 0) ? sampleStep
                : Math.max(1, Math.floor(Math.min(W, H) / 50)); // ~200 pts max

        var pts:Array = sampleOpaquePoints(bmd, alphaThresh, step);
        if (pts.length < 3) return [];

        var hull:Array = grahamScan(pts);

        if (shrinkPx > 0 && hull.length >= 3) {
            hull = shrinkPolygonTowardCentroid(hull, shrinkPx);
        }
        return hull;
    }

// Echantillonne les pixels opaques (stride = step) -> Array de {x,y}
    private function sampleOpaquePoints(bmd:BitmapData, alphaThresh:int, step:int):Array {
        var W:int = bmd.width, H:int = bmd.height, pts:Array = [];
        var a:int;
        for (var y:int = 0; y < H; y += step) {
            for (var x:int = 0; x < W; x += step) {
                a = (bmd.getPixel32(x, y) >>> 24) & 0xFF;
                if (a >= alphaThresh) pts.push({x: x, y: y});
            }
        }
        // Toujours assurer qu'on a des points en bord si jamais l'objet touche le bord
        // (utile quand le pad=0) :
        // lignes du bas/droite (sans dupliquer)
        for (x = 0; x < W; x += step) {
            y = H - 1;
            a = (bmd.getPixel32(x, y) >>> 24) & 0xFF;
            if (a >= alphaThresh) pts.push({x: x, y: y});
        }
        for (y = 0; y < H; y += step) {
            x = W - 1;
            a = (bmd.getPixel32(x, y) >>> 24) & 0xFF;
            if (a >= alphaThresh) pts.push({x: x, y: y});
        }
        // dédoublonnage grossier
        var seen:Object = {};
        var uniq:Array = [];
        for each (var p:Object in pts) {
            var k:String = (p.x << 16) + ":" + p.y;
            if (!seen[k]) {
                seen[k] = true;
                uniq.push(p);
            }
        }
        return uniq;
    }

// Graham scan sur Array<{x,y}> -> convex hull (Array<{x,y}>)
    private function grahamScan(points:Array):Array {
        if (points.length < 3) return points.concat();

        // 1) Point de départ = plus bas (y), puis le plus à gauche (x)
        var start:Object = points[0];
        for each (var p:Object in points) {
            if (p.y < start.y || (p.y == start.y && p.x < start.x)) start = p;
        }

        // 2) Trie par angle polaire autour de "start", puis par distance
        var rest:Array = [];
        for each (p in points) if (p !== start) rest.push(p);

        rest.sort(function (a:Object, b:Object):Number {
            var angA:Number = Math.atan2(a.y - start.y, a.x - start.x);
            var angB:Number = Math.atan2(b.y - start.y, b.x - start.x);
            if (angA < angB) return -1;
            if (angA > angB) return 1;
            // même angle -> plus proche en premier
            var dA:Number = (a.x - start.x) * (a.x - start.x) + (a.y - start.y) * (a.y - start.y);
            var dB:Number = (b.x - start.x) * (b.x - start.x) + (b.y - start.y) * (b.y - start.y);
            if (dA < dB) return -1;
            if (dA > dB) return 1;
            return 0;
        });

        // 3) Construction
        var hull:Array = [start];
        if (rest.length > 0) hull.push(rest[0]);

        for (var i:int = 1; i < rest.length; ++i) {
            var q:Object = rest[i];
            while (hull.length > 1 && ccw(hull[hull.length - 2], hull[hull.length - 1], q) <= 0) {
                hull.pop();
            }
            hull.push(q);
        }
        // optionnel : fermer (pas nécessaire pour le JSON si tu traces en boucle)
        return hull;
    }

    private function ccw(a:Object, b:Object, c:Object):Number {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

// Petit "inset" (réduit légèrement le polygone pour ignorer l'AA des bords)
    private function shrinkPolygonTowardCentroid(poly:Array, px:Number):Array {
        if (poly.length < 3 || px <= 0) return poly;
        var cx:Number = 0, cy:Number = 0;
        for each (var p:Object in poly) {
            cx += p.x;
            cy += p.y;
        }
        cx /= poly.length;
        cy /= poly.length;
        var out:Array = [];
        for each (p in poly) {
            var dx:Number = p.x - cx, dy:Number = p.y - cy;
            var len:Number = Math.sqrt(dx * dx + dy * dy);
            if (len > 1e-6) out.push(
                    {
                        x: Math.round(p.x - px * dx / len),
                        y: Math.round(p.y - px * dy / len)
            });
            else out.push({x: Math.round(p.x), y: Math.round(p.y)});
        }
        return out;
    }

}
}
