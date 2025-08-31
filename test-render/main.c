#define RAYGUI_IMPLEMENTATION
#define SUPPORT_FILEFORMAT_DDS

#include "raygui.h"
#include "raylib.h"
#include "physfs.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#if defined(_WIN32)
    #include <direct.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
#endif

static bool gShowAtlas = false;
static bool gIgnoreOffsets = false;
static bool gDrawHit = true;   // en haut, global


// ---------------- small utils ----------------
static char *TextDuplicate(const char *src) {
    if (!src) return NULL;
    int len = (int)strlen(src);
    char *dst = (char*)MemAlloc(len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}
static bool EndsWith(const char* s, const char* suf) {
    size_t ls = strlen(s), lt = strlen(suf);
    return (ls>=lt) && (strcmp(s+ls-lt, suf)==0);
}

// --------------- PhysFS helpers --------------
static const char* PhysfsErrorStr(void) {
    PHYSFS_ErrorCode ec = PHYSFS_getLastErrorCode();
    return PHYSFS_getErrorByCode(ec);
}

static unsigned char* ReadAllPhysFS(const char* path, int* outSize) {
    PHYSFS_File* f = PHYSFS_openRead(path);
    if (!f) { TraceLog(LOG_ERROR, "PHYSFS: open failed: %s (%s)", path, PhysfsErrorStr()); return NULL; }
    PHYSFS_sint64 len = PHYSFS_fileLength(f);
    if (len <= 0) { PHYSFS_close(f); return NULL; }
    unsigned char* buf = (unsigned char*)MemAlloc((size_t)len);
    if (!buf) { PHYSFS_close(f); return NULL; }
    PHYSFS_sint64 rd = PHYSFS_readBytes(f, buf, len);
    PHYSFS_close(f);
    if (rd != len) { MemFree(buf); return NULL; }
    if (outSize) *outSize = (int)len;
    return buf;
}
static char* ReadTextPhysFS(const char* path) {
    int sz = 0;
    unsigned char* data = ReadAllPhysFS(path, &sz);
    if (!data) return NULL;
    char* s = (char*)MemAlloc(sz + 1);
    memcpy(s, data, sz);
    s[sz] = 0;
    MemFree(data);
    return s;
}

static void EnsureDirExists(const char* dir) {
    // naive mkdir (works on Windows/MinGW and POSIX)
#if defined(_WIN32)
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}


static Texture2D LoadTextureFromPak(const char* path) {
    int sz = 0;
    unsigned char* data = ReadAllPhysFS(path, &sz);
    if (!data) return (Texture2D){0};

    // If it's a DDS, write to a cache file and let raylib load it as a texture
    const char* ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".dds") == 0)) {
        // cache path: ./.cache_textures/<original_name>.dds
        EnsureDirExists(".cache_textures");
        char outPath[512];
        snprintf(outPath, sizeof(outPath), ".cache_textures/%s", path); // keep the same name
        // ensure subdirs not present in DDS names: if your paths include folders, sanitize here if needed

        // write file
        FILE* fp = fopen(outPath, "wb");
        if (!fp) { MemFree(data); TraceLog(LOG_ERROR, "Temp write failed: %s", outPath); return (Texture2D){0}; }
        fwrite(data, 1, (size_t)sz, fp);
        fclose(fp);
        MemFree(data);

        // load gpu texture directly (compressed BC3/DXT5 kept on GPU)
        Texture2D tex = LoadTexture(outPath);
        if (!tex.id) TraceLog(LOG_ERROR, "LoadTexture failed: %s", outPath);
        else         TraceLog(LOG_INFO, "DDS Texture OK: %s  -> %dx%d", path, tex.width, tex.height);
        return tex;
    }

    // Fallback for PNG/JPG/etc. → Image path
    Image img = LoadImageFromMemory(ext ? ext : ".png", data, sz);
    MemFree(data);
    if (!img.data) { TraceLog(LOG_ERROR, "LoadImageFromMemory failed: %s", path); return (Texture2D){0}; }

    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    if (!tex.id) TraceLog(LOG_ERROR, "LoadTextureFromImage failed: %s", path);
    else         TraceLog(LOG_INFO, "Texture OK: %s  -> %dx%d", path, tex.width, tex.height);
    return tex;
}

// --------------- Anim structures --------------
typedef struct { int x, y, w, h; } HitRect;
typedef struct { float x, y; } Pt;
typedef struct { Pt* pts; int count; } Poly;

typedef struct {
    int idx, page, x, y, w, h, ox, oy, duration;
    Poly* polys;
    int polyCount;
} Frame;
typedef struct {
    const char* name; // symbol name
    Frame* frames;
    int frameCount;
} Symbol;
typedef struct {
    float fps;
    Texture2D* pages;
    int pageCount;
    Symbol* symbols;
    int symbolCount;
} SwfPack;

// --------------- JSON -> SwfPack --------------
static SwfPack LoadSwfPackFromJson(const char* jsonPath) {
    SwfPack sw = (SwfPack){0};
    char* txt = ReadTextPhysFS(jsonPath);
    if (!txt) { TraceLog(LOG_ERROR, "Missing JSON: %s", jsonPath); return sw; }
    cJSON* root = cJSON_Parse(txt);
    if (!root) { TraceLog(LOG_ERROR, "JSON parse error"); MemFree(txt); return sw; }

    cJSON* fps = cJSON_GetObjectItem(root, "fps");
    sw.fps = (fps && cJSON_IsNumber(fps)) ? (float)fps->valuedouble : 24.0f;

    // global pages
    cJSON* pages = cJSON_GetObjectItem(root, "pages");
    if (pages && cJSON_IsArray(pages)) {
        sw.pageCount = cJSON_GetArraySize(pages);
        sw.pages = (Texture2D*)MemAlloc(sizeof(Texture2D) * sw.pageCount);
        for (int i = 0; i < sw.pageCount; ++i) {
            cJSON* it = cJSON_GetArrayItem(pages, i);
            const char* pth = cJSON_IsString(it) ? it->valuestring : NULL;
            sw.pages[i] = pth ? LoadTextureFromPak(pth) : (Texture2D){0};
        }
    }

    cJSON* symbols = cJSON_GetObjectItem(root, "symbols");
    if (symbols && cJSON_IsArray(symbols)) {
        sw.symbolCount = cJSON_GetArraySize(symbols);
        sw.symbols = (Symbol*)MemAlloc(sizeof(Symbol) * sw.symbolCount);
        memset(sw.symbols, 0, sizeof(Symbol) * sw.symbolCount);
        for (int si = 0; si < sw.symbolCount; ++si) {
            cJSON* sym = cJSON_GetArrayItem(symbols, si);
            cJSON* name = cJSON_GetObjectItem(sym, "name");
            sw.symbols[si].name = name && cJSON_IsString(name) ? TextDuplicate(name->valuestring) : TextDuplicate("symbol");
            cJSON* frames = cJSON_GetObjectItem(sym, "frames");
            if (frames && cJSON_IsArray(frames)) {
                int fc = cJSON_GetArraySize(frames);
                sw.symbols[si].frameCount = fc;
                sw.symbols[si].frames = (Frame*)MemAlloc(sizeof(Frame) * fc);
                for (int fi = 0; fi < fc; ++fi) {
                    cJSON* fr = cJSON_GetArrayItem(frames, fi);
                    Frame f = {0};
                    cJSON* v = NULL;
                    v = cJSON_GetObjectItem(fr, "idx");      if (cJSON_IsNumber(v)) f.idx = (int)v->valuedouble;
                    v = cJSON_GetObjectItem(fr, "page");     if (cJSON_IsNumber(v)) f.page = (int)v->valuedouble;
                    v = cJSON_GetObjectItem(fr, "x");        if (cJSON_IsNumber(v)) f.x = (int)v->valuedouble;
                    v = cJSON_GetObjectItem(fr, "y");        if (cJSON_IsNumber(v)) f.y = (int)v->valuedouble;
                    v = cJSON_GetObjectItem(fr, "w");        if (cJSON_IsNumber(v)) f.w = (int)v->valuedouble;
                    v = cJSON_GetObjectItem(fr, "h");        if (cJSON_IsNumber(v)) f.h = (int)v->valuedouble;
                    v = cJSON_GetObjectItem(fr, "ox");       if (cJSON_IsNumber(v)) f.ox = (int)v->valuedouble;
                    v = cJSON_GetObjectItem(fr, "oy");       if (cJSON_IsNumber(v)) f.oy = (int)v->valuedouble;
                    v = cJSON_GetObjectItem(fr, "duration"); if (cJSON_IsNumber(v)) f.duration = (int)v->valuedouble; else f.duration = 1;
                    cJSON* poly = cJSON_GetObjectItem(fr, "poly");
                    if (poly && cJSON_IsArray(poly)) {
                        int outerCount = cJSON_GetArraySize(poly);
                        if (outerCount > 0 && cJSON_IsObject(cJSON_GetArrayItem(poly, 0))) {
                            // CAS 1 : poly = [ {x,y}, {x,y}, ... ]  => un seul polygone
                            f.polyCount = 1;
                            f.polys = (Poly*)MemAlloc(sizeof(Poly));
                            int n = outerCount;
                            f.polys[0].count = n;
                            f.polys[0].pts = (Pt*)MemAlloc(sizeof(Pt)*n);
                            for (int k = 0; k < n; ++k) {
                                cJSON* p = cJSON_GetArrayItem(poly, k);
                                cJSON* vx = cJSON_GetObjectItem(p, "x");
                                cJSON* vy = cJSON_GetObjectItem(p, "y");
                                f.polys[0].pts[k].x = (float)(cJSON_IsNumber(vx) ? vx->valuedouble : 0.0);
                                f.polys[0].pts[k].y = (float)(cJSON_IsNumber(vy) ? vy->valuedouble : 0.0);
                            }
                        } else {
                            // CAS 2 : poly = [ [ {x,y}... ], [ {x,y}... ], ... ]
                            f.polyCount = outerCount;
                            f.polys = (Poly*)MemAlloc(sizeof(Poly)*outerCount);
                            for (int pi = 0; pi < outerCount; ++pi) {
                                cJSON* arr = cJSON_GetArrayItem(poly, pi);
                                if (!arr || !cJSON_IsArray(arr)) {
                                    f.polys[pi].count = 0;
                                    f.polys[pi].pts = NULL;
                                    continue;
                                }
                                int n = cJSON_GetArraySize(arr);
                                f.polys[pi].count = n;
                                f.polys[pi].pts = (Pt*)MemAlloc(sizeof(Pt)*n);
                                for (int k = 0; k < n; ++k) {
                                    cJSON* p = cJSON_GetArrayItem(arr, k);
                                    cJSON* vx = cJSON_GetObjectItem(p, "x");
                                    cJSON* vy = cJSON_GetObjectItem(p, "y");
                                    f.polys[pi].pts[k].x = (float)(cJSON_IsNumber(vx) ? vx->valuedouble : 0.0);
                                    f.polys[pi].pts[k].y = (float)(cJSON_IsNumber(vy) ? vy->valuedouble : 0.0);
                                }
                            }
                        }
                    } else {
                        f.polyCount = 0;
                        f.polys = NULL;
                    }
                    sw.symbols[si].frames[fi] = f;
                }
            }
        }
    }

    cJSON_Delete(root);
    MemFree(txt);
    return sw;
}

static void UnloadSwfPack(SwfPack* sw) {
    if (sw->symbols) {
        for (int s=0;s<sw->symbolCount;s++) {
            if (sw->symbols[s].frames) {
                for (int f=0; f<sw->symbols[s].frameCount; ++f) {
                    if (sw->symbols[s].frames[f].polys) {
                        for (int pi=0; pi<sw->symbols[s].frames[f].polyCount; ++pi)
                            if (sw->symbols[s].frames[f].polys[pi].pts) MemFree(sw->symbols[s].frames[f].polys[pi].pts);
                        MemFree(sw->symbols[s].frames[f].polys);
                    }
                }
                MemFree(sw->symbols[s].frames);
            }
            if (sw->symbols[s].name) MemFree((void*)sw->symbols[s].name);
        }
        MemFree(sw->symbols);
    }
    if (sw->pages) {
        for (int i=0;i<sw->pageCount;i++) if (sw->pages[i].id) UnloadTexture(sw->pages[i]);
        MemFree(sw->pages);
    }
    *sw = (SwfPack){0};
}

// -------- mount all *.pak in working directory --------
static void MountAllPaksInCwd(void) {
    // List only *.pak files in the current working directory
    FilePathList list = LoadDirectoryFilesEx(GetWorkingDirectory(), ".pak", false);
    for (int i = 0; i < list.count; ++i) {
        const char* path = list.paths[i];
        if (!PHYSFS_mount(path, "/", 1)) {
            TraceLog(LOG_WARNING, "PHYSFS: cannot mount %s (%s)", path, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        } else {
            TraceLog(LOG_INFO, "Mounted: %s", path);
        }
    }
    UnloadDirectoryFiles(list);
}

// -------- find available SWF bases (dirs with dir/dir.json) --------
typedef struct { char** names; int count; } SwfList;
typedef struct { char* displayName; char* jsonPath; } PackEntry;
typedef struct { PackEntry* arr; int count; } PackList;

static void FreePackList(PackList* L) {
    if (!L || !L->arr) return;
    for (int i=0;i<L->count;i++) {
        if (L->arr[i].displayName) MemFree(L->arr[i].displayName);
        if (L->arr[i].jsonPath)    MemFree(L->arr[i].jsonPath);
    }
    MemFree(L->arr);
    *L = (PackList){0};
}

static void PushPack(PackList* L, const char* display, const char* path) {
    PackEntry* newArr = (PackEntry*)MemAlloc(sizeof(PackEntry)*(L->count+1));
    if (L->arr) { memcpy(newArr, L->arr, sizeof(PackEntry)*L->count); MemFree(L->arr); }
    L->arr = newArr;
    L->arr[L->count].displayName = TextDuplicate(display);
    L->arr[L->count].jsonPath    = TextDuplicate(path);
    L->count++;
}

static PackList FindRootJsonPacks(void) {
    PackList L = {0};
    char **list = PHYSFS_enumerateFiles("/");
    for (char **i = list; *i; i++) {
        const char* name = *i;                   // ex: "431.json"
        char full[512]; snprintf(full, sizeof(full), "%s", name); // racine => path = name
        if (!PHYSFS_isDirectory(full)) {
            const char* dot = strrchr(name, '.');
            if (dot && (strcasecmp(dot, ".json") == 0)) {
                PushPack(&L, name, full);        // display = "431.json", path = "431.json"
            }
        }
    }
    PHYSFS_freeList(list);
    return L;
}


static void ListPakContents(const char* base, int depth) {
    char **list = PHYSFS_enumerateFiles(base);
    for (char **i = list; *i; i++) {
        const char* name = *i;
        char path[512];
        if (base && base[0] && strcmp(base, "/") != 0)
            snprintf(path, sizeof(path), "%s/%s", base, name);
        else
            snprintf(path, sizeof(path), "%s", name);

        if (PHYSFS_isDirectory(path)) {
            TraceLog(LOG_INFO, "[DIR ] %s", path);
            if (depth < 6) ListPakContents(path, depth+1);
        } else {
            TraceLog(LOG_INFO, "[FILE] %s", path);
        }
    }
    PHYSFS_freeList(list);
}


static SwfList FindSwfBases(void) {
    SwfList L = {0};
    char **list = PHYSFS_enumerateFiles("/");
    // first pass count
    int n=0;
    for (char **i = list; *i; i++) {
        const char* entry = *i;
        // check /entry/entry.json exists
        char jsonPath[256];
        TraceLog(LOG_DEBUG, "Check SWF pack: %s", entry);
        snprintf(jsonPath, sizeof(jsonPath), "%s/%s.json", entry, entry);
        if (PHYSFS_exists(jsonPath)) n++;
    }
    if (n==0) { PHYSFS_freeList(list); return L; }
    L.count = n;
    L.names = (char**)MemAlloc(sizeof(char*)*n);
    int k=0;
    for (char **i = list; *i; i++) {
        const char* entry = *i;
        char jsonPath[256];
        snprintf(jsonPath, sizeof(jsonPath), "%s/%s.json", entry, entry);
        if (PHYSFS_exists(jsonPath)) {
            L.names[k++] = TextDuplicate(entry);
        }
    }
    PHYSFS_freeList(list);
    return L;
}
static void FreeSwfList(SwfList* L) {
    if (!L || !L->names) return;
    for (int i=0;i<L->count;i++) if (L->names[i]) MemFree(L->names[i]);
    MemFree(L->names);
    *L = (SwfList){0};
}


static bool PointInPoly(const Vector2 p, const Poly* poly, float offx, float offy, float scale) {
    bool inside = false;
    for (int i=0, j=poly->count-1; i<poly->count; j=i++) {
        float xi = offx + poly->pts[i].x*scale;
        float yi = offy + poly->pts[i].y*scale;
        float xj = offx + poly->pts[j].x*scale;
        float yj = offy + poly->pts[j].y*scale;
        bool intersect = ((yi>p.y) != (yj>p.y)) &&
                         (p.x < (xj - xi) * (p.y - yi) / ((yj - yi)!=0 ? (yj - yi):1e-6f) + xi);
        if (intersect) inside = !inside;
    }
    return inside;
}

// ----------------------- main --------------------------
int main(void) {
    // PhysFS init
    PHYSFS_init(NULL);
    TraceLog(LOG_INFO, "CWD: %s", GetWorkingDirectory());

    MountAllPaksInCwd();
    ListPakContents("/", 0); // optionnel, utile pour debug

    // Trouve tous les JSON à la racine des .pak montés
    PackList packs = FindRootJsonPacks();  // -> packs.arr[i].displayName / .jsonPath
    if (packs.count == 0) {
        TraceLog(LOG_FATAL, "Aucun pack SWF trouvé (attendu: *.json à la racine) dans les .pak montés");
        PHYSFS_deinit();
        return 1;
    }

    // Construit la liste pour le dropdown raygui (séparée par ';')
    size_t total = 1;
    for (int i = 0; i < packs.count; ++i) total += strlen(packs.arr[i].displayName) + 1;
    char* ddPacks = (char*)MemAlloc(total);
    ddPacks[0] = 0;
    for (int i = 0; i < packs.count; ++i) {
        strcat(ddPacks, packs.arr[i].displayName);
        if (i < packs.count - 1) strcat(ddPacks, ";");
    }

    // Raylib
    SetConfigFlags(FLAG_WINDOW_HIGHDPI);
    InitWindow(1280, 720, "Raylib + PhysFS + DDS + 2 dropdowns");
    SetTargetFPS(60);

    // Charge le 1er pack par défaut
    int ddPack = 0, ddPackEdit = false;
    int ddSym  = 0, ddSymEdit  = false;

    SwfPack sw = LoadSwfPackFromJson(packs.arr[ddPack].jsonPath);

    // Construit la liste des symboles
    char* ddSyms = NULL;
    {
        size_t tot = 1;
        for (int i = 0; i < sw.symbolCount; ++i) tot += strlen(sw.symbols[i].name) + 1;
        ddSyms = (char*)MemAlloc(tot);
        ddSyms[0] = 0;
        for (int i = 0; i < sw.symbolCount; ++i) {
            strcat(ddSyms, sw.symbols[i].name);
            if (i < sw.symbolCount - 1) strcat(ddSyms, ";");
        }
    }

    // Animation state
    int   curFrame = 0;
    int   curFrameDurationLeft = (sw.symbolCount > 0 && sw.symbols[0].frameCount > 0) ? sw.symbols[0].frames[0].duration : 1;
    float fps      = (sw.fps > 1.0f) ? sw.fps : 24.0f;
    float speed    = 1.0f;
    bool  playing  = true;
    Vector2 P = { 640.0f, 580.0f };
    float  previewScale = 1.0f;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        // Raccourcis
        if (IsKeyPressed(KEY_A)) gShowAtlas = !gShowAtlas;        // show whole page
        if (IsKeyPressed(KEY_O)) gIgnoreOffsets = !gIgnoreOffsets; // ignore ox/oy
        if (IsKeyPressed(KEY_H)) gDrawHit = !gDrawHit;

        if (IsKeyPressed(KEY_SPACE)) playing = !playing;
        if (IsKeyPressed(KEY_R)) {
            curFrame = 0;
            curFrameDurationLeft = (sw.symbolCount ? sw.symbols[ddSym].frames[0].duration : 1);
        }

        // Update anim
        if (sw.symbolCount > 0) {
            Symbol* S = &sw.symbols[ddSym];
            if (S->frameCount > 0 && playing) {
                static float acc = 0.f;
                acc += dt * fps * speed;
                while (acc >= 1.0f) {
                    acc -= 1.0f;
                    curFrameDurationLeft--;
                    if (curFrameDurationLeft <= 0) {
                        curFrame = (curFrame + 1) % S->frameCount;
                        curFrameDurationLeft = S->frames[curFrame].duration;
                    }
                }
            }
        }

        BeginDrawing();
        ClearBackground((Color){25,28,36,255});

        DrawText("SWF Pack", 30, 24, 18, RAYWHITE);
        if (GuiDropdownBox((Rectangle){30, 45, 380, 30}, ddPacks, &ddPack, ddPackEdit)) ddPackEdit = !ddPackEdit;
        if (!ddPackEdit) {
            // si pack changé → reload JSON + rebuild liste symboles
            static int lastPack = -1;
            if (lastPack != ddPack) {
                lastPack = ddPack;
                if (ddSyms) MemFree(ddSyms);
                UnloadSwfPack(&sw);
                sw = LoadSwfPackFromJson(packs.arr[ddPack].jsonPath);
                ddSym = 0;
                size_t tot = 1;
                for (int i = 0; i < sw.symbolCount; ++i) tot += strlen(sw.symbols[i].name) + 1;
                ddSyms = (char*)MemAlloc(tot);
                ddSyms[0] = 0;
                for (int i = 0; i < sw.symbolCount; ++i) {
                    strcat(ddSyms, sw.symbols[i].name);
                    if (i < sw.symbolCount - 1) strcat(ddSyms, ";");
                }
                // reset anim
                curFrame = 0;
                curFrameDurationLeft = (sw.symbolCount > 0 && sw.symbols[0].frameCount > 0) ? sw.symbols[0].frames[0].duration : 1;
                fps = (sw.fps > 1.0f) ? sw.fps : 24.0f;
            }
        }

        DrawText("Symbol", 30, 85, 18, RAYWHITE);
        if (GuiDropdownBox((Rectangle){30, 106, 380, 30}, ddSyms ? ddSyms : "", &ddSym, ddSymEdit)) ddSymEdit = !ddSymEdit;
        if (!ddSymEdit) {
            static int lastSym = -1;
            if (lastSym != ddSym) {
                lastSym = ddSym;
                curFrame = 0;
                curFrameDurationLeft = (sw.symbolCount > 0 && sw.symbols[ddSym].frameCount > 0) ? sw.symbols[ddSym].frames[0].duration : 1;
            }
        }

        DrawLine(0, (int)P.y, GetScreenWidth(), (int)P.y, (Color){120,120,120,80}); // ground line
        DrawLine((int)P.x-10, (int)P.y, (int)P.x+10, (int)P.y, RED);
        DrawLine((int)P.x, (int)P.y-10, (int)P.x, (int)P.y+10, RED);

        if (gShowAtlas && sw.pageCount > 0) {
            DrawTexture(sw.pages[0], 40, 180, WHITE);
            DrawText("SHOW ATLAS: on (press A to toggle)", 30, 150, 16, (Color){200,200,80,255});
            TraceLog(LOG_INFO, "Draw page0 id=%u size=%dx%d", sw.pages[0].id, sw.pages[0].width, sw.pages[0].height);
        }

        if (GuiButton((Rectangle){430, 45, 100, 30}, playing ? "Pause" : "Play")) playing = !playing;
        if (GuiButton((Rectangle){540, 45, 100, 30}, "Restart")) {
            curFrame = 0;
            curFrameDurationLeft = (sw.symbolCount > 0 && sw.symbols[ddSym].frameCount > 0) ? sw.symbols[ddSym].frames[0].duration : 1;
        }
        DrawText("Speed", 430, 90, 16, RAYWHITE);

        DrawLine(0, (int)P.y, GetScreenWidth(), (int)P.y, (Color){120,120,120,80});

        if (sw.symbolCount > 0 && sw.symbols[ddSym].frameCount > 0) {
            Symbol* S = &sw.symbols[ddSym];
            Frame f = S->frames[curFrame];
            if (f.page >= 0 && f.page < sw.pageCount) {
                Texture2D tex = sw.pages[f.page];
                Rectangle src = { (float)f.x, (float)f.y, (float)f.w, (float)f.h };
                float dx = P.x + (gIgnoreOffsets ? 0.0f : f.ox*previewScale);
                float dy = P.y + (gIgnoreOffsets ? 0.0f : f.oy*previewScale);
                Rectangle dst = { dx, dy, f.w*previewScale, f.h*previewScale };

                DrawTexturePro(tex, src, dst, (Vector2){0,0}, 0.0f, WHITE);
                if (gDrawHit && f.polyCount > 0) {
                    for (int pi = 0; pi < f.polyCount; ++pi) {
                        Poly* poly = &f.polys[pi];
                        if (poly->count < 2) continue;

                        for (int i = 0; i < poly->count; ++i) {
                            Pt a = poly->pts[i];
                            Pt b = poly->pts[(i + 1) % poly->count];

                            float ax = P.x + (gIgnoreOffsets ? 0.0f : f.ox*previewScale) + a.x*previewScale;
                            float ay = P.y + (gIgnoreOffsets ? 0.0f : f.oy*previewScale) + a.y*previewScale;
                            float bx = P.x + (gIgnoreOffsets ? 0.0f : f.ox*previewScale) + b.x*previewScale;
                            float by = P.y + (gIgnoreOffsets ? 0.0f : f.oy*previewScale) + b.y*previewScale;

                            DrawLineEx((Vector2){ax, ay}, (Vector2){bx, by}, 2.0f, (Color){255,60,60,220});
                        }
                    }
                    DrawText("HITBOX (H): ON", 30, 630, 16, (Color){255,100,100,255});
                } else {
                    DrawText("HITBOX (H): OFF", 30, 630, 16, (Color){140,140,150,255});
                }

                Vector2 M = GetMousePosition();
                bool hovered = false;
                float offx = P.x + (gIgnoreOffsets ? 0.0f : f.ox*previewScale);
                float offy = P.y + (gIgnoreOffsets ? 0.0f : f.oy*previewScale);

                for (int pi = 0; pi < f.polyCount && !hovered; ++pi) {
                    hovered = PointInPoly(M, &f.polys[pi], offx, offy, previewScale);
                }

                DrawText(hovered ? "HIT(poly): YES" : "HIT(poly): NO", 30, 600, 18,
                         hovered ? (Color){255,200,100,255} : (Color){200,200,220,255});

                DrawRectangleLines((int)dst.x, (int)dst.y, (int)dst.width, (int)dst.height, (Color){0,255,0,120});

                char info[256];
                snprintf(info, sizeof(info),
                         "%s | Frame %d/%d (dur=%d)  fps=%.1f x%.2f  page=%d  off=(%d,%d)  A=atlas O=ignoreOffs",
                         S->name, curFrame+1, S->frameCount, f.duration, fps, speed, f.page, f.ox, f.oy);
                DrawText(info, 30, 680, 18, (Color){200,200,220,255});
            }
        } else {
            DrawText("No symbols/frames", 30, 680, 18, RED);
        }

        EndDrawing();
    }

    // cleanup
    UnloadSwfPack(&sw);
    if (ddPacks) MemFree(ddPacks);
    if (ddSyms)  MemFree(ddSyms);
    FreePackList(&packs);
    CloseWindow();
    PHYSFS_deinit();
    return 0;
}

