// PS2 Showdown - battle royale estilo "arena colorida" (top-down, 10 jogadores)
// PS2SDK + gsKit + libpad. Personagens e nomes sao originais.
//
// Menu: CIMA/BAIXO navega | X confirma | ESQ/DIR troca a classe | START pausa
// Jogo:
//   Analogico esquerdo : mover
//   Analogico direito  : mirar (solte para atirar)
//   R1                 : mira automatica + tiro no inimigo mais proximo
//   QUADRADO           : super (quando a barra azul estiver cheia)
#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <libpad.h>

#define SCR_W 640
#define SCR_H 448
#define WORLD 1800.0f
#define N_ACTORS 10
#define MAX_BULLETS 256
#define MAX_BLOCKS 64
#define MAX_CUBES 64
#define R 12.0f
#define PI 3.14159265f

enum { WALL = 0, CRATE = 1, BUSH = 2 };
enum { S_TITLE = 0, S_HELP, S_PLAY, S_PAUSE, S_OVER };

typedef struct { float x, y, aim, hp, ammo, super, cd; int alive, cls, cubes, aiming, place; } Actor;
typedef struct { float x, y, dx, dy, life, dmg; int owner, active; } Bullet;
typedef struct { float x, y, w, h, hp; int kind; } Block;
typedef struct { float x, y; int active; } Cube;

// classes: 0 = Estilhaco (shotgun), 1 = Falcao (sniper), 2 = Muralha (tanque)
typedef struct { float speed, bspeed, life, dmg, spread, base_hp; int pellets; u64 color; } ClassDef;
static const ClassDef CLS[3] = {
    { 2.4f,  8.0f, 22, 18, 0.35f, 100, 3, GS_SETREG_RGBAQ(255, 150,  30, 0x80, 0) },
    { 2.1f, 14.0f, 55, 50, 0.00f,  80, 1, GS_SETREG_RGBAQ(170,  90, 255, 0x80, 0) },
    { 2.0f,  9.0f, 18, 30, 0.20f, 160, 2, GS_SETREG_RGBAQ( 60, 200, 120, 0x80, 0) },
};
static const char *CLS_NAME[3] = { "ESTILHACO", "FALCAO", "MURALHA" };
static const char *CLS_DESC[3] = { "3 PROJETEIS DE CURTO ALCANCE", "1 TIRO FORTE DE LONGO ALCANCE", "MUITA VIDA E 2 PROJETEIS" };

static Actor actors[N_ACTORS];            // actors[0] = jogador
static Bullet bullets[MAX_BULLETS];
static Block blocks[MAX_BLOCKS];
static Cube cubes[MAX_CUBES];
static int n_blocks, frame_count, winner, player_cls = 0;
static int state = S_TITLE, menu_sel = 0, anim = 0, dead_timer = 0;
static float zone_r;

static char padBuf[256] __attribute__((aligned(64)));
static GSGLOBAL *gs;

#define RGBA(r, g, b) GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00)

static float frand(float a, float b) { return a + (b - a) * ((float)rand() / (float)RAND_MAX); }
static float dist(float x1, float y1, float x2, float y2) { float dx = x2 - x1, dy = y2 - y1; return sqrtf(dx * dx + dy * dy); }
static float maxhp(const Actor *a) { return CLS[a->cls].base_hp * (1.0f + 0.1f * a->cubes); }

// ---------- mapa ----------
static int circle_hits_block(float x, float y, float r, const Block *b) {
    float cx = x < b->x ? b->x : (x > b->x + b->w ? b->x + b->w : x);
    float cy = y < b->y ? b->y : (y > b->y + b->h ? b->y + b->h : y);
    return dist(x, y, cx, cy) < r;
}
static int solid_at(float x, float y, float r) {
    for (int i = 0; i < n_blocks; i++) {
        if (blocks[i].kind == BUSH) continue;
        if (blocks[i].kind == CRATE && blocks[i].hp <= 0) continue;
        if (circle_hits_block(x, y, r, &blocks[i])) return 1;
    }
    return 0;
}
static int in_bush(float x, float y) {
    for (int i = 0; i < n_blocks; i++)
        if (blocks[i].kind == BUSH && x > blocks[i].x && x < blocks[i].x + blocks[i].w &&
            y > blocks[i].y && y < blocks[i].y + blocks[i].h) return 1;
    return 0;
}
static void add_block(int kind, float w, float h, float hp, const float *spx, const float *spy) {
    for (int tries = 0; tries < 30 && n_blocks < MAX_BLOCKS; tries++) {
        float x = frand(60, WORLD - 60 - w), y = frand(60, WORLD - 60 - h);
        int ok = 1;
        for (int i = 0; i < N_ACTORS; i++)
            if (dist(x + w / 2, y + h / 2, spx[i], spy[i]) < 90 + w / 2) ok = 0;
        if (!ok) continue;
        blocks[n_blocks++] = (Block){ x, y, w, h, hp, kind };
        return;
    }
}
static void drop_cube(float x, float y) {
    for (int i = 0; i < MAX_CUBES; i++) if (!cubes[i].active) { cubes[i] = (Cube){ x, y, 1 }; return; }
}

static void reset_game(void) {
    float spx[N_ACTORS], spy[N_ACTORS];
    srand(frame_count * 7 + anim + 4242);
    memset(bullets, 0, sizeof(bullets));
    memset(cubes, 0, sizeof(cubes));
    n_blocks = 0;
    for (int i = 0; i < N_ACTORS; i++) {
        float a = 2 * PI * i / N_ACTORS;
        spx[i] = WORLD / 2 + cosf(a) * 700; spy[i] = WORLD / 2 + sinf(a) * 700;
        actors[i] = (Actor){ spx[i], spy[i], a + PI, 0, 3, 0, 0, 1, (i == 0) ? player_cls : rand() % 3, 0, 0, 0 };
        actors[i].hp = maxhp(&actors[i]);
    }
    for (int i = 0; i < 16; i++) add_block(WALL, (i & 1) ? 36 : frand(80, 160), (i & 1) ? frand(80, 160) : 36, 1, spx, spy);
    for (int i = 0; i < 22; i++) add_block(CRATE, 34, 34, 60, spx, spy);
    for (int i = 0; i < 16; i++) add_block(BUSH, frand(80, 130), frand(60, 100), 1, spx, spy);
    zone_r = WORLD * 0.78f;
    frame_count = 0; winner = -1; dead_timer = 0;
}

// ---------- combate ----------
static void fire(int who, float ang, float dmg_mult) {
    Actor *a = &actors[who];
    const ClassDef *c = &CLS[a->cls];
    for (int p = 0; p < c->pellets; p++) {
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (bullets[i].active) continue;
            float s = ang + (c->pellets > 1 ? (p - (c->pellets - 1) / 2.0f) * c->spread : 0);
            bullets[i] = (Bullet){ a->x + cosf(s) * 16, a->y + sinf(s) * 16,
                                   cosf(s) * c->bspeed, sinf(s) * c->bspeed, c->life,
                                   c->dmg * dmg_mult * (1.0f + 0.1f * a->cubes), who, 1 };
            break;
        }
    }
}
static int try_shoot(int who, float ang) {
    Actor *a = &actors[who];
    if (a->ammo < 1 || a->cd > 0) return 0;
    a->ammo -= 1; a->cd = 15; a->aim = ang;
    fire(who, ang, 1.0f);
    return 1;
}
static void use_super(int who) {
    Actor *a = &actors[who];
    if (a->super < 100) return;
    a->super = 0;
    for (int k = 0; k < 16; k++) fire(who, 2 * PI * k / 16, 1.5f);
}
static int nearest_enemy(int who, float maxd, int need_visible) {
    int best = -1; float bd = maxd;
    for (int j = 0; j < N_ACTORS; j++) {
        if (j == who || !actors[j].alive) continue;
        float d = dist(actors[who].x, actors[who].y, actors[j].x, actors[j].y);
        if (need_visible && d > 90 && in_bush(actors[j].x, actors[j].y)) continue;
        if (d < bd) { bd = d; best = j; }
    }
    return best;
}
static void move_actor(Actor *a, float dx, float dy) {
    if (!solid_at(a->x + dx, a->y, R)) a->x += dx;
    if (!solid_at(a->x, a->y + dy, R)) a->y += dy;
    if (a->x < R) a->x = R;
    if (a->x > WORLD - R) a->x = WORLD - R;
    if (a->y < R) a->y = R;
    if (a->y > WORLD - R) a->y = WORLD - R;
}

static void update_bots(void) {
    for (int i = 1; i < N_ACTORS; i++) {
        Actor *b = &actors[i];
        if (!b->alive) continue;
        const ClassDef *c = &CLS[b->cls];
        float range = c->bspeed * c->life * 0.85f;
        float tx = WORLD / 2, ty = WORLD / 2;
        int stay = 0;
        int outside = dist(b->x, b->y, WORLD / 2, WORLD / 2) > zone_r * 0.85f;
        if (!outside) {
            int e = nearest_enemy(i, 420, 1);
            float bd = 260;
            int cb = -1;
            for (int k = 0; k < MAX_CUBES; k++) if (cubes[k].active) {
                float d = dist(b->x, b->y, cubes[k].x, cubes[k].y);
                if (d < bd) { bd = d; cb = k; }
            }
            if (e >= 0 && (b->hp > maxhp(b) * 0.3f || cb < 0)) {
                float d = dist(b->x, b->y, actors[e].x, actors[e].y);
                b->aim = atan2f(actors[e].y - b->y, actors[e].x - b->x) + frand(-0.1f, 0.1f);
                if (d < range && frame_count % 20 == i) try_shoot(i, b->aim);
                if (b->super >= 100 && d < 200) use_super(i);
                tx = actors[e].x; ty = actors[e].y;
                if (d < range * 0.5f) { tx = b->x - (tx - b->x); ty = b->y - (ty - b->y); }  // recua
            } else if (cb >= 0) {
                tx = cubes[cb].x; ty = cubes[cb].y;
            } else {
                float cd = 300; int ci = -1;
                for (int k = 0; k < n_blocks; k++) if (blocks[k].kind == CRATE && blocks[k].hp > 0) {
                    float d = dist(b->x, b->y, blocks[k].x + 17, blocks[k].y + 17);
                    if (d < cd) { cd = d; ci = k; }
                }
                if (ci >= 0) {
                    tx = blocks[ci].x + 17; ty = blocks[ci].y + 17;
                    b->aim = atan2f(ty - b->y, tx - b->x);
                    if (cd < 150 && frame_count % 20 == i) try_shoot(i, b->aim);
                    if (cd < 60) stay = 1;
                }
            }
        }
        if (stay) continue;
        float ang = atan2f(ty - b->y, tx - b->x);
        move_actor(b, cosf(ang) * c->speed * 0.55f, sinf(ang) * c->speed * 0.55f);
    }
}

static void update(struct padButtonStatus *pad, u32 btns, u32 prev) {
    frame_count++;
    if (frame_count > 600 && zone_r > 160) zone_r -= 0.13f;

    Actor *p = &actors[0];
    if (p->alive) {
        const ClassDef *c = &CLS[p->cls];
        float lx = ((int)pad->ljoy_h - 128) / 128.0f, ly = ((int)pad->ljoy_v - 128) / 128.0f;
        float rx = ((int)pad->rjoy_h - 128) / 128.0f, ry = ((int)pad->rjoy_v - 128) / 128.0f;
        float dx = fabsf(lx) > 0.2f ? lx * c->speed : 0, dy = fabsf(ly) > 0.2f ? ly * c->speed : 0;
        move_actor(p, dx, dy);
        if (fabsf(rx) > 0.35f || fabsf(ry) > 0.35f) { p->aim = atan2f(ry, rx); p->aiming = 1; }
        else if (p->aiming) { try_shoot(0, p->aim); p->aiming = 0; }   // solta = atira
        if ((btns & PAD_R1) && !(prev & PAD_R1)) {
            int e = nearest_enemy(0, c->bspeed * c->life, 1);
            if (e >= 0) try_shoot(0, atan2f(actors[e].y - p->y, actors[e].x - p->x));
            else try_shoot(0, p->aim);
        }
        if ((btns & PAD_SQUARE) && !(prev & PAD_SQUARE)) use_super(0);
    }
    update_bots();

    for (int i = 0; i < N_ACTORS; i++) {
        Actor *a = &actors[i];
        if (!a->alive) continue;
        if (a->cd > 0) a->cd--;
        if (a->ammo < 3) a->ammo += 1.0f / 90.0f;
        if (dist(a->x, a->y, WORLD / 2, WORLD / 2) > zone_r) a->hp -= 0.18f;   // gas
        for (int k = 0; k < MAX_CUBES; k++)
            if (cubes[k].active && dist(a->x, a->y, cubes[k].x, cubes[k].y) < 22) {
                cubes[k].active = 0; a->cubes++; a->hp += CLS[a->cls].base_hp * 0.1f;
            }
        if (a->hp <= 0) {
            a->alive = 0;
            for (int k = 0; k <= a->cubes / 2; k++) drop_cube(a->x + frand(-20, 20), a->y + frand(-20, 20));
        }
    }
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (!b->active) continue;
        b->x += b->dx; b->y += b->dy;
        if (--b->life <= 0) { b->active = 0; continue; }
        int hit = 0;
        for (int k = 0; k < n_blocks && !hit; k++) {
            Block *bl = &blocks[k];
            if (bl->kind == BUSH || (bl->kind == CRATE && bl->hp <= 0)) continue;
            if (circle_hits_block(b->x, b->y, 3, bl)) {
                hit = 1;
                if (bl->kind == CRATE) {
                    bl->hp -= b->dmg;
                    if (bl->hp <= 0) drop_cube(bl->x + bl->w / 2, bl->y + bl->h / 2);
                }
            }
        }
        if (hit) { b->active = 0; continue; }
        for (int j = 0; j < N_ACTORS; j++) {
            if (j == b->owner || !actors[j].alive) continue;
            if (dist(actors[j].x, actors[j].y, b->x, b->y) < R + 3) {
                actors[j].hp -= b->dmg;
                actors[b->owner].super += 8;
                if (actors[b->owner].super > 100) actors[b->owner].super = 100;
                b->active = 0; break;
            }
        }
    }
    int alive = 0, last = -1;
    for (int i = 0; i < N_ACTORS; i++) if (actors[i].alive) { alive++; last = i; }
    if (winner < 0 && alive <= 1) winner = (alive == 1) ? last : 99;
    if (!p->alive) {
        if (!p->place) p->place = alive + 1;
        dead_timer++;
    }
    if (winner == 0) p->place = 1;
}

// ---------- desenho basico ----------
static void fcircle(float cx, float cy, float r, u64 color, int z) {
    for (int i = 0; i < 12; i++) {
        float a1 = 2 * PI * i / 12, a2 = 2 * PI * (i + 1) / 12;
        gsKit_prim_triangle(gs, cx, cy, cx + cosf(a1) * r, cy + sinf(a1) * r,
                                cx + cosf(a2) * r, cy + sinf(a2) * r, z, color);
    }
}
static void rect(float x1, float y1, float x2, float y2, u64 c, int z) { gsKit_prim_sprite(gs, x1, y1, x2, y2, z, c); }

// ---------- fonte de pixels 5x7 (so maiusculas, numeros e alguns simbolos) ----------
static const char FONT_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:!-/+<>";
static const unsigned char FONT[44][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // .
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, // :
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, // !
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // -
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10}, // /
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // +
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, // <
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, // >
};

static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }

static void draw_text(int x, int y, int sc, u64 col, int z, const char *s) {
    for (; *s; s++, x += 6 * sc) {
        char ch = (char)toupper((unsigned char)*s);
        const char *f = strchr(FONT_CHARS, ch);
        if (ch == ' ' || f == NULL) continue;
        const unsigned char *g = FONT[f - FONT_CHARS];
        for (int r = 0; r < 7; r++) {
            int c = 0;
            while (c < 5) {
                if (g[r] & (0x10 >> c)) {
                    int c0 = c;
                    while (c < 5 && (g[r] & (0x10 >> c))) c++;
                    gsKit_prim_sprite(gs, x + c0 * sc, y + r * sc, x + c * sc, y + (r + 1) * sc, z, col);
                } else c++;
            }
        }
    }
}
// texto com sombra
static void text_sh(int x, int y, int sc, u64 col, const char *s) {
    draw_text(x + sc, y + sc, sc, RGBA(10, 8, 30), 19, s);
    draw_text(x, y, sc, col, 20, s);
}
static void ctext(int y, int sc, u64 col, const char *s) { text_sh((SCR_W - text_w(s, sc)) / 2, y, sc, col, s); }

// ---------- jogo ----------
static void render(void) {
    Actor *p = &actors[0];
    float camx = p->alive ? p->x : WORLD / 2, camy = p->alive ? p->y : WORLD / 2;
    float ox = SCR_W / 2 - camx, oy = SCR_H / 2 - camy;

    gsKit_clear(gs, RGBA(20, 20, 40));                                         // fora do mapa
    rect(ox, oy, WORLD + ox, WORLD + oy, RGBA(225, 190, 110), 0);                // chao de areia
    for (int i = 0; i < 48; i++) {                                               // gas
        float a1 = 2 * PI * i / 48, a2 = 2 * PI * (i + 1) / 48;
        float cx = WORLD / 2 + ox, cy = WORLD / 2 + oy;
        gsKit_prim_triangle(gs, cx + cosf(a1) * zone_r, cy + sinf(a1) * zone_r,
                                cx + cosf(a2) * zone_r, cy + sinf(a2) * zone_r,
                                cx + cosf(a1) * 3000, cy + sinf(a1) * 3000, 1, RGBA(90, 200, 90));
        gsKit_prim_triangle(gs, cx + cosf(a2) * zone_r, cy + sinf(a2) * zone_r,
                                cx + cosf(a2) * 3000, cy + sinf(a2) * 3000,
                                cx + cosf(a1) * 3000, cy + sinf(a1) * 3000, 1, RGBA(90, 200, 90));
    }
    for (int i = 0; i < n_blocks; i++) {       // paredes e caixas
        Block *b = &blocks[i];
        if (b->kind == WALL) { rect(b->x + ox - 2, b->y + oy - 2, b->x + b->w + ox + 2, b->y + b->h + oy + 2, RGBA(60, 40, 30), 2);
                               rect(b->x + ox, b->y + oy, b->x + b->w + ox, b->y + b->h + oy, RGBA(150, 100, 70), 3); }
        if (b->kind == CRATE && b->hp > 0) { rect(b->x + ox - 2, b->y + oy - 2, b->x + b->w + ox + 2, b->y + b->h + oy + 2, RGBA(50, 30, 10), 2);
                                             rect(b->x + ox, b->y + oy, b->x + b->w + ox, b->y + b->h + oy, RGBA(230, 150, 40), 3); }
    }
    for (int k = 0; k < MAX_CUBES; k++) if (cubes[k].active)    // cubos de poder
        fcircle(cubes[k].x + ox, cubes[k].y + oy, 7, RGBA(170, 60, 255), 4);
    for (int i = 0; i < MAX_BULLETS; i++) if (bullets[i].active)
        fcircle(bullets[i].x + ox, bullets[i].y + oy, 4, CLS[actors[bullets[i].owner].cls].color, 6);

    for (int i = 0; i < N_ACTORS; i++) {
        Actor *a = &actors[i];
        if (!a->alive) continue;
        int hidden = in_bush(a->x, a->y) && i != 0 && dist(a->x, a->y, p->x, p->y) > 90;
        if (hidden) continue;
        float sx = a->x + ox, sy = a->y + oy;
        fcircle(sx, sy, R + 2, RGBA(20, 20, 20), 7);
        fcircle(sx, sy, R, (i == 0) ? RGBA(60, 140, 255) : CLS[a->cls].color, 8);
        fcircle(sx + cosf(a->aim) * 5, sy + sinf(a->aim) * 5, 4, RGBA(255, 255, 255), 9);
        float mh = maxhp(a);
        rect(sx - 16, sy - 26, sx + 16, sy - 21, RGBA(20, 20, 20), 9);
        rect(sx - 15, sy - 25, sx - 15 + 30 * (a->hp / mh > 1 ? 1 : a->hp / mh), sy - 22,
             (i == 0) ? RGBA(60, 220, 60) : RGBA(230, 50, 50), 10);
    }
    for (int i = 0; i < n_blocks; i++) {       // arbustos cobrem os personagens
        Block *b = &blocks[i];
        if (b->kind != BUSH) continue;
        rect(b->x + ox, b->y + oy, b->x + b->w + ox, b->y + b->h + oy, RGBA(40, 150, 60), 11);
        rect(b->x + ox + 4, b->y + oy + 4, b->x + b->w + ox - 4, b->y + b->h + oy - 4, RGBA(60, 190, 80), 12);
    }
    if (p->alive && in_bush(p->x, p->y)) fcircle(p->x + ox, p->y + oy, 5, RGBA(60, 140, 255), 13);
    if (p->alive && p->aiming) {               // linha de mira
        for (int s = 1; s <= 6; s++)
            fcircle(p->x + ox + cosf(p->aim) * s * 30, p->y + oy + sinf(p->aim) * s * 30, 2, RGBA(255, 255, 255), 13);
    }

    // HUD: vida, municao (3), super, cubos, jogadores vivos
    float mh = maxhp(p);
    rect(18, 18, 222, 34, RGBA(20, 20, 20), 14);
    rect(20, 20, 20 + 200 * (p->hp / mh > 1 ? 1 : (p->hp < 0 ? 0 : p->hp / mh)), 32, RGBA(60, 220, 60), 15);
    for (int i = 0; i < 3; i++) {
        float f = p->ammo - i; f = f < 0 ? 0 : (f > 1 ? 1 : f);
        rect(20 + i * 70, 40, 20 + i * 70 + 64, 50, RGBA(20, 20, 20), 14);
        rect(21 + i * 70, 41, 21 + i * 70 + 62 * f, 49, RGBA(255, 160, 30), 15);
    }
    rect(20, 56, 220, 64, RGBA(20, 20, 20), 14);
    rect(21, 57, 21 + 198 * (p->super / 100.0f), 63, (p->super >= 100) ? RGBA(255, 240, 60) : RGBA(80, 160, 255), 15);
    for (int i = 0; i < p->cubes && i < 20; i++) rect(20 + i * 12, 70, 28 + i * 12, 78, RGBA(170, 60, 255), 15);
    int alive = 0;
    for (int i = 0; i < N_ACTORS; i++) if (actors[i].alive) { rect(SCR_W - 20 - alive * 14, 20, SCR_W - 10 - alive * 14, 30, RGBA(255, 255, 255), 15); alive++; }
    char buf[32];
    snprintf(buf, sizeof(buf), "VIVOS %d", alive);
    text_sh(SCR_W - 12 - text_w(buf, 2), 38, 2, RGBA(255, 255, 255), buf);
    if (p->alive && dist(p->x, p->y, WORLD / 2, WORLD / 2) > zone_r && (anim / 20) % 2 == 0)
        ctext(90, 3, RGBA(255, 92, 138), "FORA DA ZONA!");
    if (!p->alive && winner < 0) ctext(90, 3, RGBA(255, 92, 138), "VOCE FOI ELIMINADO");
}

// ---------- menus ----------
static void draw_items(int y0, int sc, const char **items, int n, int sel) {
    for (int i = 0; i < n; i++) {
        int y = y0 + i * (sc * 7 + 18), w = text_w(items[i], sc);
        if (i == sel) {
            rect((SCR_W - w) / 2 - 16, y - 7, (SCR_W + w) / 2 + 16, y + sc * 7 + 7, RGBA(255, 210, 63), 18);
            draw_text((SCR_W - w) / 2, y, sc, RGBA(58, 26, 0), 20, items[i]);
        } else {
            ctext(y, sc, RGBA(255, 255, 255), items[i]);
        }
    }
}
static void nav(u32 pressed, int count) {
    if (pressed & PAD_UP) menu_sel = (menu_sel + count - 1) % count;
    if (pressed & PAD_DOWN) menu_sel = (menu_sel + 1) % count;
}

static void render_bg_checker(void) {
    int off = (anim / 2) % 128;
    for (int gx = -128; gx < SCR_W; gx += 64)
        for (int gy = -128; gy < SCR_H; gy += 64)
            if ((((gx + 128) / 64) + ((gy + 128) / 64)) % 2 == 0)
                rect(gx + off, gy + off, gx + off + 64, gy + off + 64, RGBA(31, 28, 78), 1);
}

static void render_title(void) {
    char cls_line[48];
    gsKit_clear(gs, RGBA(22, 20, 58));
    render_bg_checker();
    int bob = (int)(sinf(anim * 0.06f) * 5.0f);
    ctext(40 + bob, 7, RGBA(255, 210, 63), "SHOWDOWN");
    ctext(104 + bob, 4, RGBA(53, 201, 245), "ARENA");

    snprintf(cls_line, sizeof(cls_line), "CLASSE: < %s >", CLS_NAME[player_cls]);
    const char *items[3] = { "JOGAR", cls_line, "COMO JOGAR" };
    draw_items(170, 3, items, 3, menu_sel);

    for (int i = 0; i < 3; i++) {                      // previa das classes
        float cx = SCR_W / 2 + (i - 1) * 100, cy = 340;
        int sel = (i == player_cls);
        fcircle(cx, cy, sel ? 26 : 18, sel ? RGBA(255, 255, 255) : RGBA(10, 8, 30), 17);
        fcircle(cx, cy, sel ? 22 : 15, CLS[i].color, 18);
        fcircle(cx + 5, cy - 3, sel ? 5 : 3, RGBA(255, 255, 255), 19);
    }
    ctext(382, 2, RGBA(185, 180, 230), CLS_DESC[player_cls]);
    ctext(420, 2, RGBA(185, 180, 230), "CIMA BAIXO: NAVEGAR   X: ESCOLHER");
}

static void render_help(void) {
    static const char *lines[] = {
        "ANALOGICO ESQ: MOVER",
        "ANALOGICO DIR: MIRAR E SOLTAR PARA ATIRAR",
        "R1: MIRA AUTOMATICA NO MAIS PROXIMO",
        "QUADRADO: SUPER QUANDO A BARRA ESTIVER CHEIA",
        "START: PAUSA",
        "QUEBRE CAIXAS PARA PEGAR CUBOS DE PODER",
        "ARBUSTOS ESCONDEM VOCE",
        "FUJA DO GAS VERDE E SEJA O ULTIMO DE PE",
    };
    gsKit_clear(gs, RGBA(22, 20, 58));
    render_bg_checker();
    ctext(30, 5, RGBA(255, 210, 63), "COMO JOGAR");
    for (int i = 0; i < 8; i++) text_sh(50, 110 + i * 29, 2, RGBA(255, 255, 255), lines[i]);
    ctext(410, 2, RGBA(185, 180, 230), "X: VOLTAR");
}

static void draw_overlay(const char *title, const char *sub, const char **items, int n) {
    rect(106, 106, 534, 334, RGBA(255, 210, 63), 16);
    rect(110, 110, 530, 330, RGBA(30, 27, 76), 17);
    ctext(124, 4, RGBA(255, 210, 63), title);
    if (sub) ctext(166, 2, RGBA(185, 180, 230), sub);
    draw_items(200, 3, items, n, menu_sel);
}

// ---------- entrada e inicializacao ----------
static void init_pad(void) {
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, padBuf);
    while (padGetState(0, 0) != PAD_STATE_STABLE) {}
    padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
}

int main(void) {
    init_pad();
    gs = gsKit_init_global();
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gs->PrimAlphaEnable = GS_SETTING_OFF;

    reset_game();
    struct padButtonStatus pad;
    memset(&pad, 0x80, sizeof(pad));
    u32 prev = 0;

    while (1) {
        u32 btns = 0;
        if (padGetState(0, 0) == PAD_STATE_STABLE && padRead(0, 0, &pad) != 0)
            btns = 0xffff ^ pad.btns;
        u32 pressed = btns & ~prev;
        anim++;

        switch (state) {
        case S_TITLE:
            nav(pressed, 3);
            if (menu_sel == 1) {
                if (pressed & PAD_LEFT) player_cls = (player_cls + 2) % 3;
                if (pressed & (PAD_RIGHT | PAD_CROSS)) player_cls = (player_cls + 1) % 3;
            } else if (pressed & PAD_CROSS) {
                if (menu_sel == 0) { reset_game(); state = S_PLAY; }
                else if (menu_sel == 2) state = S_HELP;
            }
            if (pressed & PAD_START) { reset_game(); state = S_PLAY; }
            render_title();
            break;

        case S_HELP:
            if (pressed & (PAD_CROSS | PAD_CIRCLE | PAD_START)) state = S_TITLE;
            render_help();
            break;

        case S_PLAY:
            if (pressed & PAD_START) { state = S_PAUSE; menu_sel = 0; }
            else {
                update(&pad, btns, prev);
                if (winner >= 0 || dead_timer > 120) { state = S_OVER; menu_sel = 0; }
            }
            render();
            break;

        case S_PAUSE: {
            const char *items[3] = { "CONTINUAR", "REINICIAR", "MENU PRINCIPAL" };
            nav(pressed, 3);
            if (pressed & PAD_START) state = S_PLAY;
            else if (pressed & PAD_CROSS) {
                if (menu_sel == 0) state = S_PLAY;
                else if (menu_sel == 1) { reset_game(); state = S_PLAY; }
                else { state = S_TITLE; menu_sel = 0; }
            }
            render();
            if (state == S_PAUSE) draw_overlay("PAUSA", CLS_NAME[player_cls], items, 3);
            break;
        }

        case S_OVER: {
            char sub[40];
            const char *items[2] = { "JOGAR DE NOVO", "MENU PRINCIPAL" };
            nav(pressed, 2);
            if (pressed & PAD_CROSS) {
                if (menu_sel == 0) { reset_game(); state = S_PLAY; }
                else { state = S_TITLE; menu_sel = 0; }
            }
            render();
            if (state == S_OVER) {
                if (winner == 0) snprintf(sub, sizeof(sub), "VOCE E O ULTIMO DE PE");
                else snprintf(sub, sizeof(sub), "POSICAO: %d/%d", actors[0].place ? actors[0].place : N_ACTORS, N_ACTORS);
                draw_overlay(winner == 0 ? "VITORIA!" : "FIM DE JOGO", sub, items, 2);
            }
            break;
        }
        }

        prev = btns;
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
    return 0;
}
