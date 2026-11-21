// Arena Reis - battle royale estilo "arena colorida" (top-down, 10 jogadores)
// PS2SDK + gsKit + libpad. Personagens e nomes sao originais.
//
// Modos: BATTLE ROYALE (10 jogadores) e FUTE REIS 3x3 (2 gols ou 2 minutos)
// Fute: arena vertical com obstaculos; R1 atira, R2 chuta a bola, L1 super; quem morre volta em 3 s
// Menu: L1/R1 troca de aba (JOGAR / PERSONAGENS / COMO JOGAR) | ESQ/DIR navega | X confirma | START pausa
// Jogo:
//   Analogico esquerdo : mover (com inercia)
//   Analogico direito  : mirar   |   R1: atirar
//   L1                 : super (cada classe tem o seu)
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
#define MAX_BLOCKS 200
#define MAX_CUBES 64
#define MAX_PART 200
#define MAX_DMG 24
#define MAX_FEED 5
#define N_DECOR 160
#define N_PATCH 16
#define R 12.0f
#define PI 3.14159265f
#define ZONE_START 1300.0f
#define NCLS 5
#define LOB_FUSE 36                           // lancador: a bomba pousa e explode 0.6 s depois (36 quadros)
#define INVIS_FRAMES 420                      // ninja: 7 segundos a 60 quadros

enum { WALL = 0, CRATE = 1, BUSH = 2, WATER = 3, ROCK = 4, FENCE = 5, BARREL = 6 };
enum { S_TITLE = 0, S_PLAY, S_PAUSE, S_OVER };

typedef struct {
    float x, y, vx, vy, aim, face, hp, ammo, super, cd, recoil, aimd, ua;
    int alive, cls, cubes, invis, team, stuck, aiming, place, kills, flash, since_hit, strafe_dir, strafe_t;
} Actor;
typedef struct { float x, y, dx, dy, life, dmg; int owner, active; float tx, ty, t0; int lob; } Bullet;
typedef struct { float x, y, w, h, hp; int kind; } Block;
typedef struct { float x, y; int active; } Cube;
typedef struct { float x, y, vx, vy, life, max, size; u64 color; int active; } Particle;
typedef struct { float x, y, vy, life; int val; u64 color; int active; } DmgNum;
typedef struct { float x, y; int type; } Decor;
typedef struct { char a[8], b[8]; int ca, cb, gas, ttl; } Feed;

// classes: 0 = Estilhaco (shotgun), 1 = Falcao (sniper), 2 = Muralha (tanque), 3 = Lancador (bomba), 4 = Ninja (invisivel)
typedef struct {
    int r, g, b;
    float speed, bspeed, life, dmg, spread, base_hp, cd, reload, kb;
    int pellets, lob;
} ClassDef;
static const ClassDef CLS[NCLS] = {
    { 255, 150,  30, 2.5f,  8.5f, 22, 16, 0.30f, 100, 14, 70, 2.2f, 3 },
    { 170,  90, 255, 2.2f, 15.0f, 55, 52, 0.00f,  80, 30, 95, 3.5f, 1 },
    {  60, 200, 120, 2.0f,  9.5f, 18, 28, 0.22f, 170, 20, 80, 1.5f, 2 },
    { 240,  90, 170, 2.2f,  7.0f, 60, 38, 0.00f, 110, 26, 85, 3.0f, 1, 1 },
    { 100, 120, 195, 2.7f, 11.0f, 30, 24, 0.14f,  90, 12, 65, 1.8f, 2 },   // ninja: rapido, 2 kunais, super = invisivel
};
static const char *CLS_NAME[NCLS] = { "ESTILHACO", "FALCAO", "MURALHA", "LANCADOR", "NINJA" };

static Actor actors[N_ACTORS];            // actors[0] = jogador
static Bullet bullets[MAX_BULLETS];
static Block blocks[MAX_BLOCKS];
static Cube cubes[MAX_CUBES];
static Particle parts[MAX_PART];
static DmgNum dmgs[MAX_DMG];
static Decor decor[N_DECOR];
static float patch_x[N_PATCH], patch_y[N_PATCH], patch_rx[N_PATCH], patch_ry[N_PATCH];
static Feed feed[MAX_FEED];
static char names[N_ACTORS][8];
static float spx[N_ACTORS], spy[N_ACTORS];
static int n_blocks, frame_count, winner, player_cls = 0;
static int state = S_TITLE, menu_sel = 0, anim = 0, dead_timer = 0, tz = 60;
static int tab = 0, char_cur = 0, pick_flash = 0, bg_blue = 0, game_mode = 0, no_gun = 0;   // game_mode: 0 = batalha, 1 = fute
static int cls_wins[NCLS], cls_best[NCLS];            // recordes da sessao por classe      // menu principal: aba atual, cursor de personagem, aviso "escolhido"
static float zone_r, zone_from, shake;
static float ball_x, ball_y, ball_vx, ball_vy, ball_rot, cam_x, cam_y;   // bola do modo Fute
static int fute_bullet_ball(float bx, float by, float dx, float dy);    // (modo Fute) tiro empurra a bola (1 = tiro consumido)
static void fute_blast_ball(float x, float y);                          // (modo Fute) explosao empurra a bola

static char padBuf[256] __attribute__((aligned(64)));
static GSGLOBAL *gs;

#define RGBA(r, g, b) GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00)

#define GOLD     RGBA(255, 210, 63)
#define GOLD_HI  RGBA(255, 236, 130)
#define GOLD_LO  RGBA(226, 160, 20)
#define GOLD_INK RGBA(58, 26, 0)
#define MUTED    RGBA(185, 180, 230)
#define NAVY_EDGE RGBA(90, 80, 170)
#define NAVY_FILL RGBA(26, 24, 70)


static float frand(float a, float b) { return a + (b - a) * ((float)rand() / (float)RAND_MAX); }
static float dist(float x1, float y1, float x2, float y2) { float dx = x2 - x1, dy = y2 - y1; return sqrtf(dx * dx + dy * dy); }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float maxhp(const Actor *a) { return CLS[a->cls].base_hp * (1.0f + 0.1f * a->cubes); }
static float angle_diff(float a, float b) {
    float d = b - a;
    while (d > PI) d -= 2 * PI;
    while (d < -PI) d += 2 * PI;
    return d;
}
static float angle_lerp(float a, float b, float t) { return a + angle_diff(a, b) * t; }
static u64 ccol(int cls, int add) {
    int r = CLS[cls].r + add, g = CLS[cls].g + add, b = CLS[cls].b + add;
    r = r < 0 ? 0 : (r > 255 ? 255 : r); g = g < 0 ? 0 : (g > 255 ? 255 : g); b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return RGBA(r, g, b);
}

// ---------- efeitos ----------
static void spawn_part(float x, float y, float vx, float vy, float life, float size, u64 color) {
    for (int i = 0; i < MAX_PART; i++) if (!parts[i].active) {
        parts[i] = (Particle){ x, y, vx, vy, life, life, size, color, 1 };
        return;
    }
}
static void burst(float x, float y, int n, u64 color, float speed, float size) {
    for (int i = 0; i < n; i++) {
        float a = frand(0, 2 * PI), s = frand(0.3f, 1.0f) * speed;
        spawn_part(x, y, cosf(a) * s, sinf(a) * s, frand(14, 26), size, color);
    }
}
static void add_dmgnum(float x, float y, int val, u64 color) {
    for (int i = 0; i < MAX_DMG; i++) if (!dmgs[i].active) {
        dmgs[i] = (DmgNum){ x + frand(-8, 8), y - 18, -0.9f, 42, val, color, 1 };
        return;
    }
}
static void push_feed(int killer, int victim) {
    for (int i = MAX_FEED - 1; i > 0; i--) feed[i] = feed[i - 1];
    Feed *f = &feed[0];
    memset(f, 0, sizeof(*f));
    snprintf(f->b, sizeof(f->b), "%s", names[victim]);
    f->cb = actors[victim].cls;
    f->gas = (killer < 0 || killer == victim);
    if (!f->gas) { snprintf(f->a, sizeof(f->a), "%s", names[killer]); f->ca = actors[killer].cls; }
    f->ttl = 360;
}

// ---------- mapa ----------
static int circle_hits_block(float x, float y, float r, const Block *b) {
    float cx = x < b->x ? b->x : (x > b->x + b->w ? b->x + b->w : x);
    float cy = y < b->y ? b->y : (y > b->y + b->h ? b->y + b->h : y);
    return dist(x, y, cx, cy) < r;
}
static int solid_at(float x, float y, float r) {          // bloqueia movimento
    for (int i = 0; i < n_blocks; i++) {
        const Block *b = &blocks[i];
        if (b->kind == BUSH) continue;
        if (b->kind == CRATE && b->hp <= 0) continue;
        if (x < b->x - r || x > b->x + b->w + r || y < b->y - r || y > b->y + b->h + r) continue;
        if (circle_hits_block(x, y, r, b)) return 1;
    }
    return 0;
}
static int in_bush(float x, float y) {
    for (int i = 0; i < n_blocks; i++)
        if (blocks[i].kind == BUSH && x > blocks[i].x && x < blocks[i].x + blocks[i].w &&
            y > blocks[i].y && y < blocks[i].y + blocks[i].h) return 1;
    return 0;
}
static int spawn_ok(float x, float y, float w, float h) {
    float big = w > h ? w : h;
    for (int i = 0; i < N_ACTORS; i++)
        if (dist(x + w / 2, y + h / 2, spx[i], spy[i]) < 110 + big / 2) return 0;
    return 1;
}
static int area_free(float x, float y, float w, float h, float m, int bush) {
    for (int i = 0; i < n_blocks; i++) {
        const Block *b = &blocks[i];
        if (bush && b->kind == BUSH) continue;
        if (x < b->x + b->w + m && x + w > b->x - m && y < b->y + b->h + m && y + h > b->y - m) return 0;
    }
    return 1;
}
static int try_place(int kind, float w, float h, float hp, float margin) {
    for (int tries = 0; tries < 40 && n_blocks < MAX_BLOCKS; tries++) {
        float x = frand(70, WORLD - 70 - w), y = frand(70, WORLD - 70 - h);
        if (!spawn_ok(x, y, w, h) || !area_free(x, y, w, h, margin, kind == BUSH)) continue;
        blocks[n_blocks++] = (Block){ x, y, w, h, hp, kind };
        return n_blocks - 1;
    }
    return -1;
}
static void wall_seg(float x, float y, float w, float h) {
    if (n_blocks < MAX_BLOCKS) blocks[n_blocks++] = (Block){ x, y, w, h, 1, WALL };
}
static void crate_at(float x, float y) {
    if (n_blocks < MAX_BLOCKS) blocks[n_blocks++] = (Block){ x, y, 34, 34, 60, CRATE };
}
static void add_house(void) {
    const float t = 30, gap = 64;
    for (int tries = 0; tries < 40; tries++) {
        float w = frand(190, 250), h = frand(160, 210);
        float x = frand(100, WORLD - 100 - w), y = frand(100, WORLD - 100 - h);
        if (n_blocks > MAX_BLOCKS - 12) return;
        if (!spawn_ok(x, y, w, h) || !area_free(x, y, w, h, 40, 0)) continue;
        int door = rand() % 4;
        if (door == 0) { wall_seg(x, y, (w - gap) / 2, t); wall_seg(x + (w + gap) / 2, y, (w - gap) / 2, t); } else wall_seg(x, y, w, t);
        if (door == 1) { wall_seg(x, y + h - t, (w - gap) / 2, t); wall_seg(x + (w + gap) / 2, y + h - t, (w - gap) / 2, t); } else wall_seg(x, y + h - t, w, t);
        if (door == 2) { wall_seg(x, y, t, (h - gap) / 2); wall_seg(x, y + (h + gap) / 2, t, (h - gap) / 2); } else wall_seg(x, y, t, h);
        if (door == 3) { wall_seg(x + w - t, y, t, (h - gap) / 2); wall_seg(x + w - t, y + (h + gap) / 2, t, (h - gap) / 2); } else wall_seg(x + w - t, y, t, h);
        crate_at(x + t + 12, y + t + 12);
        crate_at(x + w - t - 46, y + h - t - 46);
        return;
    }
}
static void build_map(void) {
    n_blocks = 0;
    for (int i = 0; i < 5; i++) add_house();
    for (int i = 0; i < 3; i++) try_place(WATER, frand(150, 230), frand(100, 160), 1, 50);
    for (int i = 0; i < 12; i++) try_place(ROCK, frand(34, 56), frand(30, 46), 1, 24);
    for (int i = 0; i < 6; i++) {
        if (rand() & 1) try_place(WALL, frand(100, 170), 28, 1, 30);
        else try_place(WALL, 28, frand(100, 170), 1, 30);
    }
    for (int i = 0; i < 8; i++) {
        int k = try_place(CRATE, 34, 34, 60, 12);
        if (k < 0) continue;
        for (int j = 1; j <= 2; j++) {
            float nx = blocks[k].x + j * 38, ny = blocks[k].y;
            if (area_free(nx, ny, 34, 34, 2, 0) && spawn_ok(nx, ny, 34, 34)) crate_at(nx, ny);
        }
    }
    for (int i = 0; i < 16; i++) try_place(BUSH, frand(90, 140), frand(60, 100), 1, 20);
    for (int i = 0; i < N_DECOR; i++) decor[i] = (Decor){ frand(60, WORLD - 60), frand(60, WORLD - 60), rand() % 3 };
    for (int i = 0; i < N_PATCH; i++) {
        patch_x[i] = frand(100, WORLD - 100); patch_y[i] = frand(100, WORLD - 100);
        patch_rx[i] = frand(50, 130); patch_ry[i] = frand(30, 80);
    }
}
static void drop_cube(float x, float y) {
    for (int i = 0; i < MAX_CUBES; i++) if (!cubes[i].active) { cubes[i] = (Cube){ x, y, 1 }; return; }
}

static void reset_game(void) {
    srand(frame_count * 7 + anim + 4242);
    memset(bullets, 0, sizeof(bullets));
    memset(cubes, 0, sizeof(cubes));
    memset(parts, 0, sizeof(parts));
    memset(dmgs, 0, sizeof(dmgs));
    memset(feed, 0, sizeof(feed));
    memset(actors, 0, sizeof(actors));
    for (int i = 0; i < N_ACTORS; i++) {
        float a = 2 * PI * i / N_ACTORS;
        spx[i] = WORLD / 2 + cosf(a) * 700; spy[i] = WORLD / 2 + sinf(a) * 700;
        Actor *ac = &actors[i];
        ac->x = spx[i]; ac->y = spy[i];
        ac->aim = ac->face = a + PI;
        ac->ammo = 3; ac->alive = 1;
        ac->cls = (i == 0) ? player_cls : rand() % NCLS;
        ac->hp = maxhp(ac);
        ac->strafe_dir = (i & 1) ? 1 : -1; ac->strafe_t = 30 + i * 7;
        ac->since_hit = 999;
        if (i == 0) snprintf(names[i], sizeof(names[i]), "VOCE");
        else snprintf(names[i], sizeof(names[i]), "BOT %d", i);
    }
    build_map();
    zone_r = ZONE_START; zone_from = ZONE_START; shake = 0;
    frame_count = 0; winner = -1; dead_timer = 0;
}

// ---------- combate ----------
static void kill_actor(int who, int killer) {
    Actor *a = &actors[who];
    if (!a->alive) return;
    a->alive = 0;
    burst(a->x, a->y, 24, ccol(a->cls, 0), 3.6f, 4);
    burst(a->x, a->y, 8, RGBA(255, 255, 255), 2.5f, 3);
    if (game_mode == 1) {                                      // fute: reaparece em 3 segundos
        a->strafe_t = 180; a->vx = a->vy = 0;
        if (killer >= 0 && killer != who) actors[killer].kills++;
        if (who == 0 && shake < 8) shake = 8;
        return;
    }
    for (int k = 0; k <= a->cubes / 2; k++) drop_cube(a->x + frand(-22, 22), a->y + frand(-22, 22));
    if (killer >= 0 && killer != who) {
        actors[killer].kills++;
        push_feed(killer, who);
    } else push_feed(-1, who);
    if (who == 0) shake = shake > 8 ? shake : 8;
}
static void hurt(int who, float dmg, int src, float dirx, float diry) {
    Actor *a = &actors[who];
    if (!a->alive) return;
    if (game_mode == 1 && src >= 0 && src != who && actors[src].team == a->team) return;   // sem fogo amigo
    a->hp -= dmg; a->flash = 6; a->since_hit = 0;
    if (src >= 0) {
        float kb = CLS[actors[src].cls].kb;
        a->vx += dirx * kb; a->vy += diry * kb;
        Actor *o = &actors[src];
        o->super = clampf(o->super + 6 + dmg * 0.2f, 0, 100);
    }
    add_dmgnum(a->x, a->y, (int)(dmg + 0.5f), who == 0 ? RGBA(255, 92, 110) : RGBA(255, 236, 90));
    burst(a->x, a->y, 4, RGBA(255, 255, 255), 2.2f, 2);
    if (who == 0) { float s = 3 + dmg * 0.12f; if (s > shake) shake = s; }
    if (a->hp <= 0) kill_actor(who, src);
}
static void fire(int who, float ang, float dmg_mult) {
    Actor *a = &actors[who];
    const ClassDef *c = &CLS[a->cls];
    if (c->lob) {                                              // bomba lancada em arco: passa por cima de tudo
        float rng = c->bspeed * c->life;
        float d = clampf(a->aimd, 60, rng);
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (bullets[i].active) continue;
            float fl = ceilf(d / c->bspeed);
            bullets[i] = (Bullet){ a->x, a->y, cosf(ang) * d / fl, sinf(ang) * d / fl, fl,
                                   c->dmg * dmg_mult * (1.0f + 0.1f * a->cubes), who, 1,
                                   clampf(a->x + cosf(ang) * d, 5, WORLD - 5),
                                   clampf(a->y + sinf(ang) * d, 5, WORLD - 5), fl, 1 };
            break;
        }
        return;
    }
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
static void explode(float x, float y, int owner, float dmg) {
    burst(x, y, 22, RGBA(255, 170, 50), 4.5f, 5);
    burst(x, y, 10, RGBA(255, 240, 160), 3.0f, 4);
    burst(x, y, 8, RGBA(90, 80, 80), 2.0f, 6);
    for (int k = 0; k < 20; k++) {
        float an = 2 * PI * k / 20;
        spawn_part(x, y, cosf(an) * 4.5f, sinf(an) * 4.5f, 18, 3, RGBA(255, 210, 90));
    }
    for (int j = 0; j < N_ACTORS; j++) {
        if (j == owner || !actors[j].alive) continue;
        float d = dist(x, y, actors[j].x, actors[j].y);
        if (d < 58 + R) {
            float nx = (actors[j].x - x) / (d + 0.01f), ny = (actors[j].y - y) / (d + 0.01f);
            hurt(j, dmg, owner, nx, ny);
        }
    }
    for (int k = 0; k < n_blocks; k++) {
        Block *bl = &blocks[k];
        if (bl->kind != CRATE || bl->hp <= 0) continue;
        if (circle_hits_block(x, y, 58, bl)) {
            bl->hp -= dmg;
            if (bl->hp <= 0) {
                burst(bl->x + 17, bl->y + 17, 10, RGBA(230, 150, 40), 2.6f, 3);
                drop_cube(bl->x + bl->w / 2, bl->y + bl->h / 2);
            }
        }
    }
    if (game_mode == 1) fute_blast_ball(x, y);
    if (dist(x, y, actors[0].x, actors[0].y) < 300 && shake < 5) shake = 5;
}
static int try_shoot(int who, float ang) {
    Actor *a = &actors[who];
    if (a->ammo < 1 || a->cd > 0) return 0;
    a->ammo -= 1; a->cd = CLS[a->cls].cd; a->aim = ang; a->recoil = 5;
    fire(who, ang, 1.0f);
    burst(a->x + cosf(ang) * 20, a->y + sinf(ang) * 20, 3, RGBA(255, 230, 120), 2.0f, 3);
    a->vx -= cosf(ang) * 0.6f; a->vy -= sinf(ang) * 0.6f;
    if (who == 0 && shake < 1.5f) shake = 1.5f;
    return 1;
}
static void use_super(int who) {
    Actor *a = &actors[who];
    if (!a->alive || a->super < 100) return;
    a->super = 0;
    if (a->cls == 0) {
        for (int k = 0; k < 16; k++) fire(who, 2 * PI * k / 16, 1.5f);
    } else if (a->cls == 1) {
        for (int k = -2; k <= 2; k++) fire(who, a->aim + k * 0.1f, 1.3f);
    } else if (a->cls == 3) {
        float base = a->aimd > 140 ? a->aimd : 140;
        for (int k = -2; k <= 2; k++) {
            a->aimd = base * (1.0f - 0.07f * abs(k));
            fire(who, a->aim + k * 0.22f, 1.0f);
        }
        a->aimd = base;
    } else if (a->cls == 4) {                                  // ninja: invisivel por 7 segundos
        a->invis = INVIS_FRAMES;
        burst(a->x, a->y, 26, RGBA(120, 130, 170), 3.5f, 5);
        burst(a->x, a->y, 10, RGBA(255, 255, 255), 2.5f, 3);
    } else {
        for (int j = 0; j < N_ACTORS; j++) {
            if (j == who || !actors[j].alive) continue;
            if (game_mode == 1 && actors[j].team == a->team) continue;
            float d = dist(a->x, a->y, actors[j].x, actors[j].y);
            if (d < 150) {
                float nx = (actors[j].x - a->x) / (d + 0.01f), ny = (actors[j].y - a->y) / (d + 0.01f);
                actors[j].vx += nx * 5; actors[j].vy += ny * 5;
                hurt(j, 45, who, nx, ny);
            }
        }
        for (int k = 0; k < 28; k++) {
            float ang = 2 * PI * k / 28;
            spawn_part(a->x, a->y, cosf(ang) * 5, sinf(ang) * 5, 22, 5, RGBA(120, 255, 170));
        }
    }
    if (who == 0 && shake < 8) shake = 8;
}
static int nearest_enemy(int who, float maxd, int need_visible) {
    int best = -1; float bd = maxd;
    for (int j = 0; j < N_ACTORS; j++) {
        if (j == who || !actors[j].alive) continue;
        float d = dist(actors[who].x, actors[who].y, actors[j].x, actors[j].y);
        if (need_visible && d > 90 && in_bush(actors[j].x, actors[j].y)) continue;
        if (actors[j].invis > 0 && d > 70) continue;                       // ninja invisivel so e visto de perto
        if (d < bd) { bd = d; best = j; }
    }
    return best;
}

// ---------- movimento com inercia ----------
static void move_actor(Actor *a, float dx, float dy) {
    if (!solid_at(a->x + dx, a->y, R)) a->x += dx; else a->vx = 0;
    if (!solid_at(a->x, a->y + dy, R)) a->y += dy; else a->vy = 0;
    if (a->x < R) { a->x = R; a->vx = 0; }
    if (a->x > WORLD - R) { a->x = WORLD - R; a->vx = 0; }
    if (a->y < R) { a->y = R; a->vy = 0; }
    if (a->y > WORLD - R) { a->y = WORLD - R; a->vy = 0; }
}
static void accel_move(Actor *a, float ix, float iy, float maxspeed) {
    float m = sqrtf(ix * ix + iy * iy);
    if (m > 1) { ix /= m; iy /= m; m = 1; }
    float k = (m > 0.05f) ? 0.16f : 0.26f;       // acelera devagar, freia mais rapido
    a->vx += (ix * maxspeed - a->vx) * k;
    a->vy += (iy * maxspeed - a->vy) * k;
    move_actor(a, a->vx, a->vy);
}
static float steer(const Actor *b, float ang) {
    static const float off[7] = { 0, 0.6f, -0.6f, 1.2f, -1.2f, 1.9f, -1.9f };
    for (int k = 0; k < 7; k++) {
        float a = ang + off[k];
        if (!solid_at(b->x + cosf(a) * 30, b->y + sinf(a) * 30, R)) return a;
    }
    return ang + PI;
}

static void update_bots(void) {
    for (int i = 1; i < N_ACTORS; i++) {
        Actor *b = &actors[i];
        if (!b->alive) continue;
        const ClassDef *c = &CLS[b->cls];
        float range = c->bspeed * c->life * 0.8f;
        if (--b->strafe_t <= 0) { b->strafe_dir = (rand() & 1) ? 1 : -1; b->strafe_t = 40 + rand() % 80; }

        float mvx = 0, mvy = 0, mag = 0, aim_t = b->aim;
        int has_aim = 0;
        int outside = dist(b->x, b->y, WORLD / 2, WORLD / 2) > zone_r * 0.88f;
        if (outside) {
            float a = atan2f(WORLD / 2 - b->y, WORLD / 2 - b->x);
            mvx = cosf(a); mvy = sinf(a); mag = 1;
        } else {
            int e = nearest_enemy(i, 460, 1);
            float bd = 260; int cb = -1;
            for (int k = 0; k < MAX_CUBES; k++) if (cubes[k].active) {
                float d = dist(b->x, b->y, cubes[k].x, cubes[k].y);
                if (d < bd) { bd = d; cb = k; }
            }
            if (e >= 0 && (b->hp > maxhp(b) * 0.3f || cb < 0)) {
                const Actor *t = &actors[e];
                float d = dist(b->x, b->y, t->x, t->y);
                float tt = d / c->bspeed;                                   // mira com antecipacao
                aim_t = atan2f(t->y + t->vy * tt - b->y, t->x + t->vx * tt - b->x);
                has_aim = 1;
                float err = fabsf(angle_diff(b->aim, aim_t));
                b->aimd = d;
                if (d < range && err < 0.2f) try_shoot(i, b->aim + frand(-0.07f, 0.07f));
                if (b->super >= 100 && d < 230 && err < 0.25f) use_super(i);
                float toward = atan2f(t->y - b->y, t->x - b->x);
                float rad = (d > range * 0.7f) ? 1.0f : ((d < range * 0.4f) ? -1.0f : 0.0f);
                float tang = b->strafe_dir * 0.8f;
                mvx = cosf(toward) * rad - sinf(toward) * tang;
                mvy = sinf(toward) * rad + cosf(toward) * tang;
                mag = 1;
            } else if (cb >= 0) {
                float a = atan2f(cubes[cb].y - b->y, cubes[cb].x - b->x);
                mvx = cosf(a); mvy = sinf(a); mag = 1;
            } else {
                float cd = 300; int ci = -1;
                for (int k = 0; k < n_blocks; k++) if (blocks[k].kind == CRATE && blocks[k].hp > 0) {
                    float d = dist(b->x, b->y, blocks[k].x + 17, blocks[k].y + 17);
                    if (d < cd) { cd = d; ci = k; }
                }
                if (ci >= 0) {
                    float tx = blocks[ci].x + 17, ty = blocks[ci].y + 17;
                    float a = atan2f(ty - b->y, tx - b->x);
                    aim_t = a; has_aim = 1;
                    b->aimd = cd; if (cd < 150 && fabsf(angle_diff(b->aim, a)) < 0.2f) try_shoot(i, b->aim);
                    if (cd > 70) { mvx = cosf(a); mvy = sinf(a); mag = 1; }
                } else {
                    float a = atan2f(WORLD / 2 - b->y, WORLD / 2 - b->x);
                    mvx = cosf(a) * 0.6f; mvy = sinf(a) * 0.6f; mag = 0.6f;
                }
            }
        }
        if (mag > 0.01f) {
            float a = steer(b, atan2f(mvy, mvx));
            float m = sqrtf(mvx * mvx + mvy * mvy); if (m > 1) m = 1;
            accel_move(b, cosf(a) * m, sinf(a) * m, c->speed * 0.62f * (b->invis > 0 ? 1.2f : 1.0f));
            if (!has_aim) aim_t = a;
        } else accel_move(b, 0, 0, 0);
        b->aim = angle_lerp(b->aim, aim_t, 0.16f);
    }
}

static void update_bullets(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (!b->active) continue;
        if (b->lob == 2) {                                             // bomba no chao: pavio de 0.6 s
            if (--b->life <= 0) { b->active = 0; explode(b->tx, b->ty, b->owner, b->dmg); }
            continue;
        }
        if (b->lob) {                                                  // em arco: sem colisao ate chegar ao alvo
            b->x += b->dx; b->y += b->dy;
            if (--b->life <= 0) {
                b->x = b->tx; b->y = b->ty; b->dx = b->dy = 0;
                b->lob = 2; b->life = LOB_FUSE;                        // pousou: acende o pavio
                burst(b->x, b->y, 5, RGBA(150, 118, 70), 1.5f, 3);
            }
            continue;
        }
        for (int s = 0; s < 2 && b->active; s++) {                // 2 sub-passos evitam atravessar alvos
            b->x += b->dx * 0.5f; b->y += b->dy * 0.5f;
            for (int k = 0; k < n_blocks; k++) {
                Block *bl = &blocks[k];
                if (bl->kind == BUSH || bl->kind == WATER || (bl->kind == CRATE && bl->hp <= 0)) continue;
                if (circle_hits_block(b->x, b->y, 3, bl)) {
                    b->active = 0;
                    burst(b->x, b->y, 3, RGBA(240, 220, 170), 1.6f, 2);
                    if (bl->kind == CRATE) {
                        bl->hp -= b->dmg;
                        if (bl->hp <= 0) {
                            burst(bl->x + 17, bl->y + 17, 10, RGBA(230, 150, 40), 2.6f, 3);
                            drop_cube(bl->x + bl->w / 2, bl->y + bl->h / 2);
                        }
                    }
                    break;
                }
            }
            if (!b->active) break;
            if (game_mode == 1 && fute_bullet_ball(b->x, b->y, b->dx, b->dy)) { b->active = 0; break; }
            for (int j = 0; j < N_ACTORS; j++) {
                if (j == b->owner || !actors[j].alive) continue;
                if (game_mode == 1 && actors[j].team == actors[b->owner].team) continue;      // sem fogo amigo
                if (dist(actors[j].x, actors[j].y, b->x, b->y) < R + 3) {
                    float sp = sqrtf(b->dx * b->dx + b->dy * b->dy) + 0.001f;
                    b->active = 0;
                    hurt(j, b->dmg, b->owner, b->dx / sp, b->dy / sp);
                    break;
                }
            }
        }
        if (b->active && --b->life <= 0) b->active = 0;
    }
}

static void update(struct padButtonStatus *pad, u32 btns, u32 prev) {
    frame_count++;

    // zona: espera 10s, depois fecha em etapas
    if (frame_count >= 600) {
        int st = (frame_count - 600) / 1100, ph = (frame_count - 600) % 1100;
        static const float ZT[5] = { 1000, 720, 480, 300, 140 };
        if (st < 5) {
            if (ph == 0) zone_from = zone_r;
            if (ph < 500) zone_r = zone_from + (ZT[st] - zone_from) * (ph / 500.0f);
        }
    }
    float gas_dmg = 0.15f + (1.0f - zone_r / ZONE_START) * 0.5f;

    Actor *p = &actors[0];
    if (p->alive) {
        const ClassDef *c = &CLS[p->cls];
        float lx = ((int)pad->ljoy_h - 128) / 128.0f, ly = ((int)pad->ljoy_v - 128) / 128.0f;
        float rx = ((int)pad->rjoy_h - 128) / 128.0f, ry = ((int)pad->rjoy_v - 128) / 128.0f;
        float lm = sqrtf(lx * lx + ly * ly);
        if (lm > 0.2f) { float s = (lm - 0.2f) / 0.8f; if (s > 1) s = 1; lx = lx / lm * s; ly = ly / lm * s; } else { lx = 0; ly = 0; }
        accel_move(p, lx, ly, c->speed * (p->aiming ? 0.75f : 1.0f) * (p->invis > 0 ? 1.2f : 1.0f));
        float rm = sqrtf(rx * rx + ry * ry), rng = c->bspeed * c->life;
        if (rm > 0.35f) {                                              // analogico direito so mira
            p->aim = atan2f(ry, rx); p->aiming = 1;
            p->aimd = 90 + clampf((rm - 0.35f) / 0.65f, 0, 1) * (rng - 90);   // lancador: quanto mais empurra, mais longe
        } else {
            p->aiming = 0;
            if (lm > 0.2f) p->aim = atan2f(ly, lx);                    // sem mirar: arma segue o movimento
            p->aimd = rng * 0.6f;
        }
        if (btns & PAD_R1) try_shoot(0, p->aim);                       // R1 atira (segure para repetir)
        if ((btns & PAD_L1) && !(prev & PAD_L1)) use_super(0);
    }
    update_bots();

    for (int i = 0; i < N_ACTORS; i++) {
        Actor *a = &actors[i];
        if (!a->alive) continue;
        if (a->cd > 0) a->cd--;
        if (a->ammo < 3) a->ammo = clampf(a->ammo + 1.0f / CLS[a->cls].reload, 0, 3);
        if (a->flash > 0) a->flash--;
        if (a->invis > 0 && --a->invis == 0) burst(a->x, a->y, 14, RGBA(120, 130, 170), 2.5f, 4);   // fumaca ao reaparecer
        if (a->recoil > 0) a->recoil *= 0.8f;
        a->since_hit++;
        float spd = sqrtf(a->vx * a->vx + a->vy * a->vy);
        float target = (a->aiming || (i != 0 && spd < 0.3f)) ? a->aim : (spd > 0.3f ? atan2f(a->vy, a->vx) : a->face);
        if (i != 0 && !a->aiming) target = a->aim;
        a->face = angle_lerp(a->face, target, 0.2f);
        if (spd > 1.2f && a->invis <= 0 && (frame_count + i) % 7 == 0)
            spawn_part(a->x - a->vx * 2, a->y + 8, frand(-0.3f, 0.3f), frand(-0.3f, 0.1f), 16, 3, RGBA(196, 160, 96));
        if (dist(a->x, a->y, WORLD / 2, WORLD / 2) > zone_r) {
            a->hp -= gas_dmg; a->since_hit = 0;
            if (a->hp <= 0) { kill_actor(i, -1); continue; }
        } else if (a->since_hit > 240 && a->hp < maxhp(a)) {
            a->hp = clampf(a->hp + 0.12f, 0, maxhp(a));          // regenera fora de combate
        }
        for (int k = 0; k < MAX_CUBES; k++)
            if (cubes[k].active && dist(a->x, a->y, cubes[k].x, cubes[k].y) < 22) {
                cubes[k].active = 0; a->cubes++; a->hp += CLS[a->cls].base_hp * 0.1f;
                burst(cubes[k].x, cubes[k].y, 6, RGBA(200, 120, 255), 2.0f, 3);
            }
    }
    update_bullets();
    for (int i = 0; i < MAX_PART; i++) if (parts[i].active) {
        Particle *q = &parts[i];
        q->x += q->vx; q->y += q->vy; q->vx *= 0.93f; q->vy *= 0.93f;
        if (--q->life <= 0) q->active = 0;
    }
    for (int i = 0; i < MAX_DMG; i++) if (dmgs[i].active) {
        dmgs[i].y += dmgs[i].vy; dmgs[i].vy *= 0.94f;
        if (--dmgs[i].life <= 0) dmgs[i].active = 0;
    }
    for (int i = 0; i < MAX_FEED; i++) if (feed[i].ttl > 0) feed[i].ttl--;
    shake *= 0.85f; if (shake < 0.2f) shake = 0;

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
static void fellipse(float cx, float cy, float rx, float ry, u64 color, int z) {
    for (int i = 0; i < 14; i++) {
        float a1 = 2 * PI * i / 14, a2 = 2 * PI * (i + 1) / 14;
        gsKit_prim_triangle(gs, cx, cy, cx + cosf(a1) * rx, cy + sinf(a1) * ry,
                                cx + cosf(a2) * rx, cy + sinf(a2) * ry, z, color);
    }
}
static void rect(float x1, float y1, float x2, float y2, u64 c, int z) { gsKit_prim_sprite(gs, x1, y1, x2, y2, z, c); }

// ---------- fonte de pixels 5x7 (so maiusculas, numeros e alguns simbolos) ----------
static const char FONT_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:!-/+<>%";
static const unsigned char FONT[45][7] = {
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
    {0x19,0x19,0x02,0x04,0x08,0x13,0x13}, // %
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
// texto com sombra (z controlado por tz)
static void text_sh(int x, int y, int sc, u64 col, const char *s) {
    draw_text(x + sc, y + sc, sc, RGBA(10, 8, 30), tz - 1, s);
    draw_text(x, y, sc, col, tz, s);
}
static void ctext(int y, int sc, u64 col, const char *s) { text_sh((SCR_W - text_w(s, sc)) / 2, y, sc, col, s); }

// ---------- jogo ----------
static void bar(int x, int y, int w, int h, float f, u64 fill, u64 hi, int z) {
    f = clampf(f, 0, 1);
    rect(x - 2, y - 2, x + w + 2, y + h + 2, RGBA(10, 8, 30), z);
    rect(x, y, x + w, y + h, RGBA(52, 48, 110), z + 1);
    if (f > 0) {
        rect(x, y, x + w * f, y + h, fill, z + 2);
        rect(x, y, x + w * f, y + h * 0.4f, hi, z + 3);
    }
}

// ---------- personagens humanos ----------
static const int SKIN_C[NCLS][3] = { { 255, 206, 160 }, { 242, 192, 152 }, { 196, 140, 98 }, { 236, 182, 140 }, { 240, 196, 150 } };
static const int HAIR_C[NCLS][3] = { { 214, 74, 30 }, { 76, 44, 130 }, { 44, 32, 26 }, { 34, 38, 74 }, { 40, 46, 78 } };

static int visible_to(const Actor *a, int i, const Actor *p) {
    if (game_mode == 1 && a->team == p->team) return 1;                            // aliados sempre visiveis
    if (i != 0 && a->invis > 0 && dist(a->x, a->y, p->x, p->y) > 70) return 0;      // ninja invisivel
    return !(i != 0 && in_bush(a->x, a->y) && dist(a->x, a->y, p->x, p->y) > 90);
}

static void draw_human(float sx, float sy, int cls, float face, float aim, float walk,
                       int moving, int flash, float recoil, int me) {
    u64 skin = RGBA(SKIN_C[cls][0], SKIN_C[cls][1], SKIN_C[cls][2]);
    u64 hair = RGBA(HAIR_C[cls][0], HAIR_C[cls][1], HAIR_C[cls][2]);
    u64 body = flash == 1 ? RGBA(255, 255, 255) : ccol(cls, 0);
    u64 dark = ccol(cls, -70);
    u64 red = RGBA(226, 48, 64);
    if (flash == 2) { skin = hair = body = RGBA(160, 200, 240); dark = RGBA(110, 150, 210); red = RGBA(205, 230, 255); }   // fantasma (invisivel)
    u64 ink = RGBA(20, 18, 44);
    u64 white = RGBA(255, 255, 255);
    float sw = moving ? sinf(walk) * 3.5f : 0;
    float by = sy - (moving ? fabsf(sinf(walk)) * 1.5f : 0);

    if (me) fellipse(sx, sy + 16, 17, 7, RGBA(53, 201, 245), 13);          // anel do jogador
    fellipse(sx, sy + 16, 13, 5, RGBA(150, 118, 70), 14);                   // sombra

    // pernas
    float l1 = sy + 13 + sw, l2 = sy + 13 - sw;
    rect(sx - 7, by + 4, sx - 1, l1, RGBA(46, 50, 92), 15);
    rect(sx + 1, by + 4, sx + 7, l2, RGBA(46, 50, 92), 15);
    rect(sx - 8, l1 - 2, sx - 1, l1 + 2, ink, 15);
    rect(sx + 1, l2 - 2, sx + 8, l2 + 2, ink, 15);

    // tronco
    rect(sx - 9, by - 8, sx + 9, by + 8, ink, 16);
    rect(sx - 8, by - 7, sx + 8, by + 7, body, 17);
    rect(sx - 8, by + 3, sx + 8, by + 7, dark, 18);
    rect(sx - 6, by - 6, sx - 2, by - 3, ccol(cls, 60), 18);
    if (cls == 4) rect(sx - 8, by + 1, sx + 8, by + 3.5f, red, 19);          // faixa do ninja

    // braco que segura a arma (aponta para a mira) e braco solto
    float sh = cosf(aim) >= 0 ? 1.0f : -1.0f;
    float shx = sx + sh * 8, shy = by - 3;
    float hx = sx + cosf(aim) * 11, hy = by - 1 + sinf(aim) * 9;
    int gz = sinf(aim) > -0.3f ? 22 : 15;
    for (int t = 0; t <= 2; t++) fcircle(shx + (hx - shx) * t / 2.0f, shy + (hy - shy) * t / 2.0f, 3.4f, body, gz);
    fcircle(hx, hy, 3.0f, skin, gz);
    for (int d = 5; d <= 17 && !no_gun; d += 4) {
        float g = d - recoil * 0.5f;
        float gr = cls == 3 ? (d == 17 ? 3.4f : 3.8f) : (cls == 4 ? (d == 17 ? 1.6f : 1.9f) : (d == 17 ? 2.0f : 2.6f));       // lancador: tubo grosso
        u64 gc = d == 17 ? (cls == 3 ? RGBA(255, 150, 50) : (cls == 4 ? RGBA(215, 225, 245) : RGBA(120, 122, 140))) : RGBA(60, 62, 78);
        fcircle(hx + cosf(aim) * g, hy + sinf(aim) * g, gr, gc, gz);
    }
    fcircle(sx - sh * 9, by + 1, 3.4f, body, 18);
    fcircle(sx - sh * 9.5f, by + 6, 3.0f, skin, 18);

    // cabeca
    float hy2 = by - 14;
    int back = sinf(face) < -0.5f;
    u64 cap = cls == 2 ? ccol(2, -30) : hair;
    if (cls == 1) fcircle(sx - cosf(face) * 8, hy2 + 3, 3.8f, hair, 18);     // rabo de cavalo
    fcircle(sx, hy2 + 0.5f, 7.8f, ink, 19);
    if (back) {
        fcircle(sx, hy2 + 0.5f, 6.8f, cap, 21);
    } else {
        fcircle(sx, hy2 + 1, 6.8f, skin, 20);
        float ex = cosf(face) * 2.5f;
        for (int k = -1; k <= 1; k += 2) {
            fcircle(sx + k * 2.6f + ex, hy2 + 2.5f, 2.1f, white, 21);
            fcircle(sx + k * 2.6f + ex + cosf(face) * 0.8f, hy2 + 2.7f, 1.1f, ink, 22);
        }
        if (cls == 2) fcircle(sx, hy2 - 5.2f, 6.4f, cap, 21);
        else fcircle(sx, hy2 - 4.5f, 4.6f, hair, 21);
    }
    if (cls == 0) {                                                          // cabelo espetado
        gsKit_prim_triangle(gs, sx - 7, hy2 - 4, sx - 4, hy2 - 14, sx - 1, hy2 - 6, 21, hair);
        gsKit_prim_triangle(gs, sx - 3, hy2 - 6, sx, hy2 - 16, sx + 3, hy2 - 6, 21, hair);
        gsKit_prim_triangle(gs, sx + 1, hy2 - 6, sx + 4, hy2 - 14, sx + 7, hy2 - 4, 21, hair);
    } else if (cls == 1) {
        rect(sx - 6.8f, hy2 - 3, sx + 6.8f, hy2 - 1, body, 21);               // faixa
    } else if (cls == 2) {
        rect(sx - 7, hy2 - 1, sx + 7, hy2 + 0.5f, ccol(2, 70), 21);           // aba do capacete
    } else if (cls == 3) {
        rect(sx - 7.5f, hy2 - 4, sx + 7.5f, hy2 - 1.5f, RGBA(40, 40, 60), 22); // oculos de lancador
        fcircle(sx - 3.5f, hy2 - 2.7f, 2.7f, RGBA(255, 220, 60), 22);
        fcircle(sx + 3.5f, hy2 - 2.7f, 2.7f, RGBA(255, 220, 60), 22);
    } else {                                                                 // ninja: capuz, mascara e faixa vermelha
        float wag = sinf(anim * 0.25f) * 2.0f, bx = -cosf(face) * 7.0f;
        gsKit_prim_triangle(gs, sx + bx, hy2 - 3, sx + bx * 2.0f, hy2 - 6 + wag, sx + bx * 1.9f, hy2 - 1 + wag, 18, red);
        gsKit_prim_triangle(gs, sx + bx, hy2 - 2, sx + bx * 1.8f, hy2 + 1 - wag, sx + bx * 1.5f, hy2 + 3 - wag, 18, red);
        if (!back) rect(sx - 4.6f, hy2 + 4.4f, sx + 4.6f, hy2 + 7, hair, 22);   // mascara
        rect(sx - 7, hy2 - 3, sx + 7, hy2 - 0.8f, red, 22);                     // faixa
    }
}

// vida, municao, nome e cubos ficam em cima do personagem
static void draw_overhead(const Actor *a, int idx, float sx, float sy) {
    int old = tz; tz = 36;
    char buf[16];
    const int w = 46;
    float top = sy - 34, x0 = sx - w / 2;
    float f = clampf(a->hp / maxhp(a), 0, 1);
    int ally = (game_mode == 1) ? (a->team == 0) : (idx == 0);
    u64 fill = ally ? RGBA(70, 226, 100) : RGBA(255, 74, 90);
    u64 hi = ally ? RGBA(160, 255, 170) : RGBA(255, 150, 150);
    bar((int)x0, (int)top - 18, w, 9, f, fill, hi, 31);
    snprintf(buf, sizeof(buf), "%d", (int)(a->hp > 0 ? a->hp + 0.5f : 0));
    text_sh((int)(sx - text_w(buf, 1) / 2.0f), (int)top - 17, 1, RGBA(255, 255, 255), buf);
    for (int k = 0; k < 3; k++) {                                            // 3 segmentos de municao
        float fa = clampf(a->ammo - k, 0, 1);
        float x = x0 + k * 16;
        rect(x - 1, top - 7, x + 15, top - 1, RGBA(10, 8, 30), 31);
        rect(x, top - 6, x + 14, top - 2, RGBA(52, 48, 110), 32);
        if (fa > 0) rect(x, top - 6, x + 14 * fa, top - 2, RGBA(255, 170, 40), 33);
    }
    text_sh((int)(sx - text_w(names[idx], 1) / 2.0f), (int)top - 29, 1, ally ? RGBA(130, 255, 160) : RGBA(255, 130, 140), names[idx]);
    if (game_mode == 1) { tz = old; return; }
    snprintf(buf, sizeof(buf), "%d", a->cubes);
    float xs = sx - (9 + text_w(buf, 1)) / 2.0f;
    rect(xs, top - 39, xs + 8, top - 31, RGBA(60, 16, 104), 31);
    rect(xs + 1, top - 38, xs + 7, top - 32, RGBA(178, 78, 255), 32);
    text_sh((int)xs + 11, (int)top - 39, 1, RGBA(255, 255, 255), buf);
    tz = old;
}

static void draw_blocks(float ox, float oy) {
    for (int i = 0; i < n_blocks; i++) {
        const Block *b = &blocks[i];
        float x = b->x + ox, y = b->y + oy;
        if (x > SCR_W + 8 || y > SCR_H + 8 || x + b->w < -8 || y + b->h < -8) continue;
        if (b->kind == WATER) {
            rect(x - 4, y - 4, x + b->w + 4, y + b->h + 4, RGBA(150, 214, 255), 2);
            rect(x, y, x + b->w, y + b->h, RGBA(44, 138, 224), 3);
            for (int k = 0; k < 3; k++) {
                float rx = x + 10 + fmodf(anim * 0.4f + k * 37, b->w - 40);
                rect(rx, y + 14 + k * (b->h - 28) / 3, rx + 22, y + 17 + k * (b->h - 28) / 3, RGBA(170, 226, 255), 4);
            }
        } else if (b->kind == WALL) {
            rect(x + 4, y + 6, x + b->w + 4, y + b->h + 6, RGBA(168, 134, 80), 4);
            rect(x - 2, y - 2, x + b->w + 2, y + b->h + 2, RGBA(58, 36, 22), 5);
            rect(x, y, x + b->w, y + b->h, RGBA(158, 104, 70), 6);
            rect(x, y, x + b->w, y + (b->h < 8 ? b->h : 8), RGBA(198, 140, 98), 7);
            if (b->w >= b->h) { for (float sx = x + 28; sx < x + b->w - 4; sx += 28) rect(sx, y + 2, sx + 2, y + b->h - 2, RGBA(120, 76, 50), 7); }
            else { for (float sy = y + 28; sy < y + b->h - 4; sy += 28) rect(x + 2, sy, x + b->w - 2, sy + 2, RGBA(120, 76, 50), 7); }
        } else if (b->kind == ROCK) {
            rect(x + 4, y + 6, x + b->w + 4, y + b->h + 6, RGBA(168, 134, 80), 4);
            rect(x - 2, y - 2, x + b->w + 2, y + b->h + 2, RGBA(48, 48, 60), 5);
            rect(x, y, x + b->w, y + b->h, RGBA(128, 130, 142), 6);
            rect(x + 4, y + 4, x + b->w * 0.6f, y + b->h * 0.4f, RGBA(176, 178, 192), 7);
            rect(x, y + b->h - 6, x + b->w, y + b->h, RGBA(98, 100, 112), 7);
        } else if (b->kind == FENCE) {                                       // cerca de madeira
            rect(x + 3, y + 4, x + b->w + 3, y + b->h + 4, RGBA(30, 100, 52), 4);
            rect(x - 1, y - 1, x + b->w + 1, y + b->h + 1, RGBA(70, 40, 14), 5);
            rect(x, y, x + b->w, y + b->h, RGBA(206, 140, 70), 6);
            rect(x, y, x + b->w, y + 3, RGBA(240, 180, 104), 7);
            if (b->w >= b->h) { for (float px = x + 6; px < x + b->w; px += 18) rect(px, y - 4, px + 7, y + b->h + 2, RGBA(150, 96, 44), 8); }
            else { for (float py = y + 6; py < y + b->h; py += 18) rect(x - 4, py, x + b->w + 4, py + 7, RGBA(150, 96, 44), 8); }
        } else if (b->kind == BARREL) {                                      // barril
            float cx = x + b->w / 2, cy = y + b->h / 2, rr = b->w / 2;
            fellipse(cx + 2, cy + rr * 0.8f, rr, rr * 0.45f, RGBA(30, 100, 52), 4);
            fcircle(cx, cy, rr + 1.5f, RGBA(60, 34, 12), 5);
            fcircle(cx, cy, rr, RGBA(176, 112, 52), 6);
            rect(cx - rr, cy - 4, cx + rr, cy - 2, RGBA(90, 56, 24), 7);
            rect(cx - rr, cy + 3, cx + rr, cy + 5, RGBA(90, 56, 24), 7);
            fcircle(cx - rr * 0.3f, cy - rr * 0.35f, rr * 0.35f, RGBA(214, 150, 84), 7);
        } else if (b->kind == CRATE && b->hp > 0) {
            rect(x + 3, y + 4, x + b->w + 3, y + b->h + 4, RGBA(168, 134, 80), 4);
            rect(x - 2, y - 2, x + b->w + 2, y + b->h + 2, RGBA(70, 40, 10), 5);
            rect(x, y, x + b->w, y + b->h, b->hp < 30 ? RGBA(210, 132, 40) : RGBA(240, 163, 58), 6);
            rect(x + 3, y + 3, x + b->w - 3, y + 7, RGBA(255, 198, 110), 7);
            rect(x + 4, y + b->h / 2 - 1, x + b->w - 4, y + b->h / 2 + 2, RGBA(150, 90, 24), 7);
            rect(x + b->w / 2 - 1, y + 4, x + b->w / 2 + 2, y + b->h - 4, RGBA(150, 90, 24), 7);
            if (b->hp < 30) rect(x + 5, y + 10, x + b->w - 9, y + 12, RGBA(70, 40, 10), 7);
        }
    }
}

static void draw_bullets(float ox, float oy) {
    for (int i = 0; i < MAX_BULLETS; i++) if (bullets[i].active) {
        const Bullet *b = &bullets[i];
        int cl = actors[b->owner].cls;
        if (b->lob) {
            int ground = (b->lob == 2);
            float pr = ground ? 1.0f : 1.0f - b->life / (b->t0 > 0 ? b->t0 : 1.0f);
            float arc = ground ? 0.0f : sinf(pr * PI) * 46.0f;
            float fz = ground ? b->life / (float)LOB_FUSE : 1.0f;              // 1 -> 0 durante o pavio
            float gx = b->x + ox, gy = b->y + oy;
            for (int k = 0; k < 14; k++) {                              // circulo de perigo no alvo
                float a1 = 2 * PI * k / 14, a2 = 2 * PI * (k + 1) / 14;
                gsKit_prim_line(gs, b->tx + ox + cosf(a1) * 55, b->ty + oy + sinf(a1) * 55,
                                    b->tx + ox + cosf(a2) * 55, b->ty + oy + sinf(a2) * 55, 10,
                                    ground && (anim / 3) % 2 ? RGBA(255, 230, 120) : RGBA(255, 90, 100));
                if (ground)                                                    // anel interno fecha ate explodir
                    gsKit_prim_line(gs, b->tx + ox + cosf(a1) * 55 * (1 - fz), b->ty + oy + sinf(a1) * 55 * (1 - fz),
                                        b->tx + ox + cosf(a2) * 55 * (1 - fz), b->ty + oy + sinf(a2) * 55 * (1 - fz), 10, RGBA(255, 255, 255));
            }
            fellipse(gx, gy + 4, 7, 3, RGBA(150, 118, 70), 10);
            fcircle(gx, gy - arc, 7, RGBA(24, 22, 44), 12);
            fcircle(gx, gy - arc, 5.5f, RGBA(70, 72, 96), 13);
            fcircle(gx - 2, gy - arc - 2, 2, RGBA(170, 175, 200), 14);
            fcircle(gx + 3, gy - arc - 8, ground ? 2.0f + (1 - fz) * 2.5f : 2.0f, (anim / (ground && fz < 0.4f ? 2 : 3)) % 2 ? RGBA(255, 220, 80) : RGBA(255, 120, 40), 14);
            continue;
        }
        fcircle(b->x - b->dx * 0.7f + ox, b->y - b->dy * 0.7f + oy, 3, ccol(cl, -40), 11);
        fcircle(b->x + ox, b->y + oy, 5, ccol(cl, 0), 12);
        fcircle(b->x + ox, b->y + oy, 2.5f, RGBA(255, 255, 255), 13);
    }
}

static void draw_bushes(float ox, float oy) {
    for (int i = 0; i < n_blocks; i++) {
        const Block *b = &blocks[i];
        if (b->kind != BUSH) continue;
        float x = b->x + ox, y = b->y + oy;
        if (x > SCR_W + 8 || y > SCR_H + 8 || x + b->w < -8 || y + b->h < -8) continue;
        rect(x, y, x + b->w, y + b->h, RGBA(44, 152, 78), 23);
        rect(x + 5, y + 5, x + b->w - 5, y + b->h - 5, RGBA(70, 194, 102), 24);
        for (int k = 0; k < 6; k++) {
            float bx = x + 12 + (float)((i * 37 + k * 53) % (int)(b->w - 24));
            float by = y + 12 + (float)((i * 29 + k * 31) % (int)(b->h - 24));
            fcircle(bx, by, 7, RGBA(44, 152, 78), 25);
            fcircle(bx - 2, by - 2, 3.5f, RGBA(108, 218, 130), 26);
        }
    }
}

static void render_world(float ox, float oy, const Actor *p) {
    // chao
    for (int gx = 0; gx < (int)WORLD; gx += 100)
        for (int gy = 0; gy < (int)WORLD; gy += 100) {
            float sx = gx + ox, sy = gy + oy;
            if (sx > SCR_W || sy > SCR_H || sx < -100 || sy < -100) continue;
            rect(sx, sy, sx + 100, sy + 100, ((gx + gy) / 100) % 2 ? RGBA(230, 196, 124) : RGBA(222, 188, 114), 0);
        }
    for (int i = 0; i < N_PATCH; i++) {
        float sx = patch_x[i] + ox, sy = patch_y[i] + oy;
        if (sx + patch_rx[i] < 0 || sx - patch_rx[i] > SCR_W || sy + patch_ry[i] < 0 || sy - patch_ry[i] > SCR_H) continue;
        fellipse(sx, sy, patch_rx[i], patch_ry[i], RGBA(206, 170, 100), 1);
    }
    for (int i = 0; i < N_DECOR; i++) {
        float sx = decor[i].x + ox, sy = decor[i].y + oy;
        if (sx < -8 || sx > SCR_W + 8 || sy < -8 || sy > SCR_H + 8) continue;
        if (decor[i].type == 0) rect(sx, sy, sx + 5, sy + 3, RGBA(176, 160, 140), 1);
        else if (decor[i].type == 1) { rect(sx, sy - 5, sx + 2, sy, RGBA(98, 160, 70), 1); rect(sx + 4, sy - 3, sx + 6, sy, RGBA(98, 160, 70), 1); }
        else { rect(sx, sy, sx + 4, sy + 4, RGBA(255, 120, 160), 1); rect(sx + 1, sy + 1, sx + 3, sy + 3, RGBA(255, 230, 100), 1); }
    }
    // borda do mapa
    rect(ox - 10, oy - 10, WORLD + ox + 10, oy, RGBA(74, 42, 20), 1);
    rect(ox - 10, WORLD + oy, WORLD + ox + 10, WORLD + oy + 10, RGBA(74, 42, 20), 1);
    rect(ox - 10, oy, ox, WORLD + oy, RGBA(74, 42, 20), 1);
    rect(WORLD + ox, oy, WORLD + ox + 10, WORLD + oy, RGBA(74, 42, 20), 1);
    // gas
    for (int i = 0; i < 48; i++) {
        float a1 = 2 * PI * i / 48, a2 = 2 * PI * (i + 1) / 48;
        float cx = WORLD / 2 + ox, cy = WORLD / 2 + oy;
        gsKit_prim_triangle(gs, cx + cosf(a1) * zone_r, cy + sinf(a1) * zone_r,
                                cx + cosf(a2) * zone_r, cy + sinf(a2) * zone_r,
                                cx + cosf(a1) * 3000, cy + sinf(a1) * 3000, 1, RGBA(90, 200, 100));
        gsKit_prim_triangle(gs, cx + cosf(a2) * zone_r, cy + sinf(a2) * zone_r,
                                cx + cosf(a2) * 3000, cy + sinf(a2) * 3000,
                                cx + cosf(a1) * 3000, cy + sinf(a1) * 3000, 1, RGBA(90, 200, 100));
        gsKit_prim_line(gs, cx + cosf(a1) * zone_r, cy + sinf(a1) * zone_r, cx + cosf(a2) * zone_r, cy + sinf(a2) * zone_r, 2, RGBA(200, 255, 170));
    }
    draw_blocks(ox, oy);
    // cubos de poder
    for (int k = 0; k < MAX_CUBES; k++) if (cubes[k].active) {
        float bob = sinf(frame_count * 0.2f + cubes[k].x) * 2.5f, x = cubes[k].x + ox, y = cubes[k].y + oy + bob;
        fellipse(x, y + 12, 8, 3, RGBA(176, 142, 90), 8);
        fcircle(x, y, 13, RGBA(220, 180, 255), 8);
        rect(x - 8, y - 8, x + 8, y + 8, RGBA(60, 16, 104), 9);
        rect(x - 6, y - 6, x + 6, y + 6, RGBA(178, 78, 255), 10);
        rect(x - 4, y - 4, x, y, RGBA(228, 180, 255), 11);
    }
    draw_bullets(ox, oy);
    // personagens
    for (int i = 0; i < N_ACTORS; i++) {
        const Actor *a = &actors[i];
        if (!a->alive || !visible_to(a, i, p)) continue;
        float spd = sqrtf(a->vx * a->vx + a->vy * a->vy);
        int look = a->flash > 0 ? 1 : 0;
        if (!look && a->invis > 0 && !(a->invis < 120 && (anim / 6) % 2)) look = 2;      // fantasma; pisca nos ultimos 2s
        draw_human(a->x + ox, a->y + oy, a->cls, a->face, a->aim, frame_count * 0.45f + i * 1.7f,
                   spd > 0.6f, look, a->recoil, i == 0);
    }
    draw_bushes(ox, oy);
    if (p->alive && in_bush(p->x, p->y)) fcircle(p->x + ox, p->y + oy, 6, RGBA(53, 201, 245), 27);
    for (int i = 0; i < N_ACTORS; i++) {
        const Actor *a = &actors[i];
        if (a->alive && visible_to(a, i, p)) draw_overhead(a, i, a->x + ox, a->y + oy);
    }
    // particulas
    for (int i = 0; i < MAX_PART; i++) if (parts[i].active) {
        const Particle *q = &parts[i];
        float s = q->size * (q->life / q->max) + 0.6f;
        rect(q->x + ox - s, q->y + oy - s, q->x + ox + s, q->y + oy + s, q->color, 28);
    }
    // linha de mira
    if (p->alive && p->aiming) {
        const ClassDef *c = &CLS[p->cls];
        float len = c->lob ? p->aimd : c->bspeed * c->life;
        float rr = c->lob ? 55.0f : 9.0f;
        for (float d = 28; d < len; d += 24)
            fcircle(p->x + ox + cosf(p->aim) * d, p->y + oy + sinf(p->aim) * d, 2.5f - d / len, RGBA(255, 255, 255), 29);
        for (int k = 0; k < 16; k++) {
            float a = 2 * PI * k / 16 + anim * 0.05f;
            fcircle(p->x + ox + cosf(p->aim) * len + cosf(a) * rr, p->y + oy + sinf(p->aim) * len + sinf(a) * rr, 2, ccol(p->cls, 40), 29);
        }
    }
}

static void draw_skull(float cx, float cy, float r, u64 col, int z) {
    fcircle(cx, cy, r, col, z);
    rect(cx - r * 0.5f, cy + r * 0.4f, cx + r * 0.5f, cy + r * 1.2f, col, z);
    fcircle(cx - r * 0.4f, cy, r * 0.28f, RGBA(30, 20, 60), z + 1);
    fcircle(cx + r * 0.4f, cy, r * 0.28f, RGBA(30, 20, 60), z + 1);
}

static void draw_portrait(float x, float y, float sz, int cls, u64 border) {
    u64 skin = RGBA(SKIN_C[cls][0], SKIN_C[cls][1], SKIN_C[cls][2]);
    u64 hair = RGBA(HAIR_C[cls][0], HAIR_C[cls][1], HAIR_C[cls][2]);
    float cx = x + sz / 2, cy = y + sz / 2;
    rect(x - 2, y - 2, x + sz + 2, y + sz + 2, border, 39);
    rect(x, y, x + sz, y + sz, ccol(cls, -60), 40);
    rect(x + sz * 0.12f, y + sz * 0.74f, x + sz * 0.88f, y + sz, ccol(cls, 0), 41);
    fcircle(cx, cy - 1, sz * 0.31f, cls == 2 ? ccol(2, -30) : hair, 42);
    fcircle(cx, cy + sz * 0.05f, sz * 0.25f, skin, 43);
    fcircle(cx, cy - sz * 0.17f, sz * 0.17f, cls == 2 ? ccol(2, -30) : hair, 44);
    fcircle(cx - sz * 0.09f, cy + sz * 0.08f, sz * 0.05f, RGBA(20, 18, 44), 45);
    fcircle(cx + sz * 0.09f, cy + sz * 0.08f, sz * 0.05f, RGBA(20, 18, 44), 45);
    if (cls == 4) {                                                          // ninja: mascara e faixa
        fcircle(cx, cy + sz * 0.22f, sz * 0.14f, hair, 46);
        rect(cx - sz * 0.26f, cy - sz * 0.12f, cx + sz * 0.26f, cy - sz * 0.05f, RGBA(226, 48, 64), 46);
    }
}

static void render_hud(const Actor *p) {
    char buf[48];
    tz = 49;

    // vinheta: vida baixa (vermelho) ou fora da zona (verde)
    int low = p->alive && p->hp < maxhp(p) * 0.35f;
    int ingas = p->alive && dist(p->x, p->y, WORLD / 2, WORLD / 2) > zone_r;
    if ((low || p->flash > 0) && (anim / 8) % 2 == 0) {
        u64 c = RGBA(210, 30, 50);
        rect(0, 0, SCR_W, 7, c, 30); rect(0, SCR_H - 7, SCR_W, SCR_H, c, 30);
        rect(0, 0, 7, SCR_H, c, 30); rect(SCR_W - 7, 0, SCR_W, SCR_H, c, 30);
    } else if (ingas) {
        u64 c = RGBA(80, 200, 90);
        rect(0, 0, SCR_W, 5, c, 30); rect(0, SCR_H - 5, SCR_W, SCR_H, c, 30);
        rect(0, 0, 5, SCR_H, c, 30); rect(SCR_W - 5, 0, SCR_W, SCR_H, c, 30);
    }

    // jogadores restantes
    int alive = 0;
    for (int i = 0; i < N_ACTORS; i++) if (actors[i].alive) alive++;
    snprintf(buf, sizeof(buf), "RESTAM: %d", alive);
    text_sh(16, 12, 3, RGBA(255, 255, 255), buf);

    // quem matou quem (canto superior esquerdo)
    int row = 0;
    for (int i = 0; i < MAX_FEED && row < 3; i++) {
        const Feed *f = &feed[i];
        if (f->ttl <= 0) continue;
        int y = 48 + row * 56; row++;
        rect(10, y, 176, y + 52, RGBA(10, 8, 30), 38);
        rect(12, y + 2, 174, y + 50, RGBA(24, 72, 190), 39);
        rect(12, y + 2, 54, y + 50, RGBA(44, 118, 238), 40);
        draw_skull(33, y + 26, 11, RGBA(255, 255, 255), 41);
        if (f->gas) {
            fcircle(74, y + 22, 9, RGBA(90, 200, 100), 41);
            fcircle(86, y + 26, 8, RGBA(120, 225, 130), 41);
            fcircle(68, y + 28, 7, RGBA(70, 175, 90), 42);
            text_sh(64, y + 39, 1, RGBA(170, 255, 180), "GAS");
        } else {
            draw_portrait(62, y + 6, 32, f->ca, RGBA(90, 160, 255));
            text_sh(62, y + 39, 1, RGBA(255, 255, 255), f->a);
            rect(100, y + 14, 118, y + 20, RGBA(200, 200, 215), 41);             // revolver
            rect(100, y + 20, 106, y + 30, RGBA(150, 100, 60), 41);
            rect(116, y + 12, 120, y + 16, RGBA(120, 122, 140), 42);
        }
        draw_portrait(128, y + 6, 32, f->cb, RGBA(230, 50, 70));
        draw_skull(160, y + 6, 6, RGBA(255, 255, 255), 46);
        text_sh(128, y + 39, 1, RGBA(255, 255, 255), f->b);
    }

    // botao do super (canto inferior direito)
    if (p->alive) {
        int ready = p->super >= 100;
        float bx = SCR_W - 62, by = SCR_H - 66;
        fcircle(bx, by, 31, RGBA(10, 8, 30), 38);
        if (ready) {
            fcircle(bx, by, 28, (anim / 10) % 2 ? RGBA(255, 232, 80) : RGBA(255, 190, 40), 39);
            draw_skull(bx, by - 2, 12, RGBA(255, 255, 255), 40);
        } else {
            fcircle(bx, by, 28, RGBA(40, 60, 140), 39);
            snprintf(buf, sizeof(buf), "%d%%", (int)p->super);
            text_sh((int)(bx - text_w(buf, 2) / 2.0f), (int)by - 7, 2, RGBA(200, 225, 255), buf);
        }
        text_sh((int)(bx - text_w("L1", 1) / 2.0f), SCR_H - 30, 1, RGBA(255, 255, 255), "L1");
    }

    if (p->alive && p->invis > 0) {                                            // tempo de invisibilidade
        snprintf(buf, sizeof(buf), "INVISIVEL %d", p->invis / 60 + 1);
        ctext(344, 2, RGBA(150, 220, 255), buf);
        bar(SCR_W / 2 - 60, 366, 120, 8, p->invis / (float)INVIS_FRAMES, RGBA(120, 190, 255), RGBA(200, 235, 255), 38);
    }

    // avisos da zona
    if (frame_count < 600) { snprintf(buf, sizeof(buf), "ZONA FECHA EM %d", (600 - frame_count) / 60 + 1); if (frame_count > 300) ctext(22, 2, RGBA(255, 255, 255), buf); }
    else {
        int ph = (frame_count - 600) % 1100, st = (frame_count - 600) / 1100;
        if (st < 5 && ph < 500) ctext(22, 2, RGBA(255, 92, 138), "ZONA FECHANDO!");
        else if (st < 4 && ph >= 1100 - 600) { snprintf(buf, sizeof(buf), "PROXIMA ZONA EM %d", (1100 - ph) / 60 + 1); if (1100 - ph < 480) ctext(22, 2, RGBA(255, 255, 255), buf); }
    }
    if (ingas && (anim / 20) % 2 == 0) ctext(70, 3, RGBA(255, 92, 138), "FORA DA ZONA!");
    if (!p->alive && winner < 0) ctext(70, 3, RGBA(255, 92, 138), "VOCE FOI ELIMINADO");
    if (p->alive && frame_count < 480) ctext(396, 2, RGBA(255, 232, 120), "DIR: MIRAR   R1: ATIRAR   L1: SUPER");
    tz = 60;
}

static void render(void) {
    Actor *p = &actors[0];
    float camx = p->alive ? p->x : WORLD / 2, camy = p->alive ? p->y : WORLD / 2;
    float ox = SCR_W / 2 - camx + (shake > 0.3f ? frand(-shake, shake) : 0);
    float oy = SCR_H / 2 - camy + (shake > 0.3f ? frand(-shake, shake) : 0);

    gsKit_clear(gs, RGBA(20, 20, 40));
    render_world(ox, oy, p);
    tz = 37;                                                              // numeros de dano flutuantes
    for (int i = 0; i < MAX_DMG; i++) if (dmgs[i].active) {
        char b[12];
        snprintf(b, sizeof(b), "%d", dmgs[i].val);
        text_sh((int)(dmgs[i].x + ox) - text_w(b, 2) / 2, (int)(dmgs[i].y + oy), 2, dmgs[i].color, b);
    }
    tz = 60;
    render_hud(p);
}

// ---------- modo FUTE REIS 3x3 (arena vertical) ----------
// Voce + 2 bots (azul, defende embaixo) x 3 bots (vermelho, defende em cima). Quem fizer 2 gols ganha; 2 minutos.
// Acabou o tempo: mais gols vence; empate vai para o gol de ouro. Armas funcionam: tiro tira vida do rival,
// quem morre reaparece em 3 s. Tiros e explosoes tambem empurram a bola.
// Controles: analogico esq = mover | dir = mirar | R1 = atirar | R2 (ou O) = chutar a bola | L1 = super
#define FW 720.0f                      // largura do campo
#define FH 1240.0f                     // comprimento do campo
#define GX1 260.0f                     // boca do gol (x inicial)
#define GX2 460.0f                     // boca do gol (x final)
#define GOAL_D 60.0f                   // profundidade do gol
#define BALL_R 9.0f
#define FUTE_N 6
#define FUTE_TIME 7200                 // 2 minutos a 60 quadros
#define FUTE_GOALS 2
#define KICK_CD 22

static const float KICK_POW[NCLS] = { 10.5f, 13.0f, 11.5f, 10.5f, 10.0f };   // forca do chute por classe
static const float FORM[FUTE_N][2] = { { 0.50f, 0.80f }, { 0.26f, 0.70f }, { 0.74f, 0.70f },
                                       { 0.50f, 0.20f }, { 0.74f, 0.30f }, { 0.26f, 0.30f } };

static int ball_idle, score[2], fute_phase, fute_t, fute_time, fute_golden, fute_done, fute_result, goal_team, fute_finish, golden_msg;
// fute_phase: 0 = contagem, 1 = jogo, 2 = pausa (gol ou fim)

static void flow_init(void);
static void flow_build(void);

// ---- mapa: obstaculos com simetria de 180 graus (justo para os dois times) ----
static void fb_add(int kind, float x, float y, float w, float h) {
    if (n_blocks >= MAX_BLOCKS - 1) return;
    blocks[n_blocks++] = (Block){ x, y, w, h, kind == CRATE ? 1e9f : 1.0f, kind };
}
static void fb_sym(int kind, float x, float y, float w, float h) {
    fb_add(kind, x, y, w, h);
    float x2 = FW - x - w, y2 = FH - y - h;
    if (fabsf(x2 - x) > 1 || fabsf(y2 - y) > 1) fb_add(kind, x2, y2, w, h);
}
static void fute_build_map(void) {
    n_blocks = 0;
    fb_sym(BUSH,  36, 250, 150, 120);
    fb_sym(BUSH, 540, 420, 150, 100);
    fb_sym(BUSH, 270, 480, 130,  80);
    fb_sym(CRATE, 300, 300, 34, 34); fb_sym(CRATE, 334, 300, 34, 34); fb_sym(CRATE, 368, 300, 34, 34);
    fb_sym(CRATE,  60, 500, 34, 34); fb_sym(CRATE,  94, 500, 34, 34); fb_sym(CRATE, 60, 534, 34, 34);
    fb_sym(CRATE, 600, 200, 34, 34); fb_sym(CRATE, 600, 234, 34, 34);
    fb_sym(CRATE, 170, 603, 34, 34); fb_sym(CRATE, 204, 603, 34, 34);
    fb_sym(BARREL, 120, 170, 30, 30); fb_sym(BARREL, 590, 520, 30, 30);
    fb_sym(BARREL, 200, 440, 30, 30); fb_sym(BARREL, 480, 260, 30, 30);
    fb_sym(FENCE, 120, 580, 110, 16); fb_sym(FENCE, 440, 560, 130, 16);
    fb_sym(FENCE,  40, 100,  16, 90); fb_sym(FENCE, 664, 330,  16, 90);
}

static void fute_spawn(int i) {                                       // posiciona no ponto inicial
    Actor *a = &actors[i];
    a->x = FW * FORM[i][0]; a->y = FH * FORM[i][1];
    a->vx = a->vy = 0; a->alive = 1; a->hp = maxhp(a); a->ammo = 3; a->cd = 0;
    a->since_hit = 999; a->flash = 0; a->invis = 0; a->strafe_t = 0; a->stuck = 0;
    a->aim = a->face = (i < 3) ? -PI / 2 : PI / 2;
}
static void fute_kickoff(int frames) {
    ball_x = FW / 2; ball_y = FH / 2; ball_vx = ball_vy = 0;
    memset(bullets, 0, sizeof(bullets));
    for (int i = 0; i < FUTE_N; i++) fute_spawn(i);
    fute_phase = 0; fute_t = frames; ball_idle = 0;
    flow_build();
}
static void fute_reset(void) {
    srand(frame_count * 7 + anim + 777);
    memset(actors, 0, sizeof(actors));
    memset(parts, 0, sizeof(parts));
    memset(dmgs, 0, sizeof(dmgs));
    for (int i = 0; i < FUTE_N; i++) {
        Actor *a = &actors[i];
        a->team = i / 3;
        a->cls = (i == 0) ? player_cls : rand() % NCLS;
        if (i == 0) snprintf(names[i], sizeof(names[i]), "VOCE");
        else if (i < 3) snprintf(names[i], sizeof(names[i]), "AMIGO %d", i);
        else snprintf(names[i], sizeof(names[i]), "INIM %d", i - 2);
    }
    fute_build_map();
    flow_init();
    score[0] = score[1] = 0;
    fute_time = FUTE_TIME; fute_golden = 0; fute_done = 0; fute_result = 0; fute_finish = 0; goal_team = -1; golden_msg = 0;
    shake = 0; frame_count = 0;
    fute_kickoff(180);
    cam_x = ball_x; cam_y = actors[0].y;
}

static void fmove(Actor *a, float ix, float iy, float maxspeed) {
    float m = sqrtf(ix * ix + iy * iy);
    if (m > 1) { ix /= m; iy /= m; m = 1; }
    float k = (m > 0.05f) ? 0.16f : 0.22f;
    a->vx += (ix * maxspeed - a->vx) * k;
    a->vy += (iy * maxspeed - a->vy) * k;
    if (!solid_at(a->x + a->vx, a->y, R)) a->x += a->vx; else a->vx = 0;
    if (!solid_at(a->x, a->y + a->vy, R)) a->y += a->vy; else a->vy = 0;
    if (a->x < R) { a->x = R; a->vx = 0; }
    if (a->x > FW - R) { a->x = FW - R; a->vx = 0; }
    if (a->y < R) { a->y = R; a->vy = 0; }
    if (a->y > FH - R) { a->y = FH - R; a->vy = 0; }
}

static int fute_kick(Actor *a, float ang) {
    if (a->cd > 0) return 0;
    float d = dist(a->x, a->y, ball_x, ball_y);
    a->recoil = 5;
    if (d > R + BALL_R + 16) { a->cd = 10; return 0; }                // chutou no ar
    a->cd = KICK_CD;
    ball_vx = cosf(ang) * KICK_POW[a->cls] + a->vx * 0.3f;
    ball_vy = sinf(ang) * KICK_POW[a->cls] + a->vy * 0.3f;
    burst(ball_x, ball_y, 6, RGBA(255, 255, 255), 2.5f, 3);
    return 1;
}
static int fute_bullet_ball(float bx, float by, float dx, float dy) {   // tiro acerta a bola: empurra
    if (dist(bx, by, ball_x, ball_y) > BALL_R + 4) return 0;
    ball_vx += dx * 0.5f; ball_vy += dy * 0.5f;
    burst(ball_x, ball_y, 3, RGBA(255, 255, 255), 2.0f, 2);
    return 1;
}
static void fute_blast_ball(float x, float y) {                       // explosao joga a bola longe
    float d = dist(x, y, ball_x, ball_y);
    if (d > 58 + BALL_R + 8) return;
    ball_vx += (ball_x - x) / (d + 0.01f) * 10.0f;
    ball_vy += (ball_y - y) / (d + 0.01f) * 10.0f;
}

// ---- caminhos dos bots: mapa de distancias (BFS) ate a bola numa grade de 24 px ----
#define GC 24.0f
#define GW 30
#define GH 52
static unsigned char walk[GW * GH];
static short flow[GW * GH];
static void flow_init(void) {
    for (int gy = 0; gy < GH; gy++)
        for (int gx = 0; gx < GW; gx++)
            walk[gy * GW + gx] = !solid_at((gx + 0.5f) * GC, (gy + 0.5f) * GC, R + 1.0f);
}
static int cell_of(float x, float y) {
    int gx = (int)(x / GC), gy = (int)(y / GC);
    gx = gx < 0 ? 0 : (gx >= GW ? GW - 1 : gx); gy = gy < 0 ? 0 : (gy >= GH ? GH - 1 : gy);
    return gy * GW + gx;
}
static void flow_build(void) {
    static short q[GW * GH];
    for (int i = 0; i < GW * GH; i++) flow[i] = -1;
    int h = 0, t = 0, c0 = cell_of(ball_x, ball_y);
    flow[c0] = 0; q[t++] = c0;
    while (h < t) {
        int c = q[h++], cx = c % GW, cy = c / GW;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) continue;
                int n = ny * GW + nx;
                if (flow[n] >= 0 || !walk[n]) continue;
                if (dx && dy && (!walk[cy * GW + nx] || !walk[ny * GW + cx])) continue;   // sem cortar quina
                flow[n] = flow[c] + 1; q[t++] = n;
            }
    }
}
static int flow_dir(float x, float y, float *ox, float *oy) {      // direcao do proximo passo rumo a bola
    int c = cell_of(x, y), cx = c % GW, cy = c / GW, best = -1; short bv = flow[c] >= 0 ? flow[c] : 32000;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = cx + dx, ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) continue;
            int n = ny * GW + nx;
            if (!walk[n] || flow[n] < 0 || flow[n] >= bv) continue;
            if (dx && dy && (!walk[cy * GW + nx] || !walk[ny * GW + cx])) continue;
            bv = flow[n]; best = n;
        }
    if (best < 0) return 0;
    float tx = (best % GW + 0.5f) * GC - x, ty = (best / GW + 0.5f) * GC - y, d = sqrtf(tx * tx + ty * ty) + 0.01f;
    *ox = tx / d; *oy = ty / d;
    return 1;
}
static int clear_path(float x, float y, float tx, float ty) {      // linha reta livre de obstaculos?
    float d = dist(x, y, tx, ty);
    for (float k = 14; k < d; k += 14)
        if (solid_at(x + (tx - x) * k / d, y + (ty - y) * k / d, R - 1.0f)) return 0;
    return 1;
}

static void fute_bot_shoot(int i) {                                   // bot atira no inimigo mais perto
    Actor *b = &actors[i];
    const ClassDef *c = &CLS[b->cls];
    float rng = (c->lob ? c->bspeed * c->life : c->bspeed * c->life * 0.9f);
    int ne = -1; float bd = rng;
    for (int j = 0; j < FUTE_N; j++) {
        const Actor *e = &actors[j];
        if (!e->alive || e->team == b->team) continue;
        float d = dist(b->x, b->y, e->x, e->y);
        if (d > 90 && in_bush(e->x, e->y)) continue;
        if (e->invis > 0 && d > 70) continue;
        if (d < bd) { bd = d; ne = j; }
    }
    if (ne < 0) return;
    float ang = atan2f(actors[ne].y - b->y, actors[ne].x - b->x);
    if (b->super >= 100 && bd < 220) { b->aim = ang; b->aimd = bd; use_super(i); return; }
    if (b->ammo >= 1 && b->cd <= 0 && rand() % 8 == 0) {
        b->aimd = bd;
        try_shoot(i, ang + frand(-0.12f, 0.12f));
    }
}

static void fute_bot(int i, int play) {
    Actor *b = &actors[i];
    if (!b->alive) return;
    int team = b->team, slot = i % 3;
    float diry = team == 0 ? -1.0f : 1.0f;
    float own_y = team == 0 ? FH : 0.0f, opp_y = team == 0 ? 0.0f : FH;
    int chaser = -1; float best = 1e9f;
    for (int j = team * 3; j < team * 3 + 3; j++) {
        if (!actors[j].alive) continue;
        float d = dist(actors[j].x, actors[j].y, ball_x, ball_y);
        if (j == 0) d -= 40;                                           // se o humano esta perto da bola, o bot nao disputa
        if (d < best) { best = d; chaser = j; }
    }
    float db = dist(b->x, b->y, ball_x, ball_y);
    float ix = 0, iy = 0, spd = 0.85f, want = atan2f(ball_y - b->y, ball_x - b->x);
    int use_steer = 1;
    int chase = (chaser == i);
    if (slot == 2 && !chase && fabsf(ball_y - own_y) < 300 && db < 300) chase = 1;    // zagueiro limpa a area

    if (chase) {
        float tgx = FW * 0.5f + sinf(i * 1.7f + frame_count * 0.01f) * 55.0f;
        float ga = atan2f(opp_y - ball_y, tgx - ball_x);
        float lx = ball_x + ball_vx * 6.0f - b->x, ly = ball_y + ball_vy * 6.0f - b->y, ld = sqrtf(lx * lx + ly * ly) + 0.01f;
        ix = lx / ld; iy = ly / ld;                                    // vai direto na bola (com leve antecipacao)
        if (!clear_path(b->x, b->y, ball_x, ball_y)) { float fx, fy; if (flow_dir(b->x, b->y, &fx, &fy)) { ix = fx; iy = fy; } }   // senao contorna pelo caminho livre
        use_steer = 0;
        spd = 0.95f; want = ga;
        if (play && db < R + BALL_R + 12.0f) { b->aim = ga; fute_kick(b, ga + frand(-0.10f, 0.10f)); }   // chute sempre rumo ao gol rival
    } else {
        float px, py;
        if (slot == 2) { px = clampf(ball_x, GX1 - 20, GX2 + 20); py = own_y + diry * 160.0f; }
        else if (slot == 1) { py = clampf(ball_y + diry * 170.0f, 160, FH - 160); px = (team == 0 ? 0.25f : 0.75f) * FW + (ball_x - FW * 0.5f) * 0.2f; }
        else { py = clampf(ball_y - diry * 110.0f, 140, FH - 140); px = FW * 0.5f + (ball_x - FW * 0.5f) * 0.6f; }
        float dx = px - b->x, dy = py - b->y, dd = sqrtf(dx * dx + dy * dy);
        if (dd > 12.0f) { float m = dd / 70.0f; if (m > 1) m = 1; ix = dx / dd * m; iy = dy / dd * m; }
    }
    float mm = sqrtf(ix * ix + iy * iy);
    if (mm > 0.05f && use_steer) {                                     // desvia de obstaculos
        float a2 = steer(b, atan2f(iy, ix));
        ix = cosf(a2) * mm; iy = sinf(a2) * mm;
    }
    if (play) {                                                        // destrava: preso contra cerca/caixa, anda sem rumo por 40 quadros
        float sp = sqrtf(b->vx * b->vx + b->vy * b->vy);
        if (b->strafe_t <= 0 && mm > 0.3f && sp < 0.25f) b->stuck++; else if (b->strafe_t <= 0) b->stuck = 0;
        if (b->stuck > 25 && b->strafe_t <= 0) { b->strafe_t = 40; b->ua = frand(0, 2 * PI); b->stuck = 0; }
        if (b->strafe_t > 0) { b->strafe_t--; ix = cosf(b->ua); iy = sinf(b->ua); }
    }
    if (play) { fmove(b, ix, iy, CLS[b->cls].speed * spd); fute_bot_shoot(i); } else fmove(b, 0, 0, 0);
    b->aim = angle_lerp(b->aim, want, 0.16f);
}

static void fute_goal(int team) {
    score[team]++; goal_team = team; fute_phase = 2; fute_t = 150; shake = 10;
    if (score[team] >= FUTE_GOALS || fute_golden) { fute_finish = 1; fute_result = (team == 0); }
    float gy = team == 0 ? 0 : FH;
    u64 c1 = team == 0 ? RGBA(70, 150, 255) : RGBA(255, 90, 100);
    for (int k = 0; k < 40; k++) spawn_part(frand(GX1, GX2), gy + frand(-30, 30), frand(-3, 3), frand(-4, 1), frand(30, 50), 4, k % 2 ? c1 : RGBA(255, 220, 80));
}

static void ball_vs_blocks(void) {
    for (int k = 0; k < n_blocks; k++) {
        const Block *b = &blocks[k];
        if (b->kind == BUSH || b->kind == WATER || (b->kind == CRATE && b->hp <= 0)) continue;
        float cx = clampf(ball_x, b->x, b->x + b->w), cy = clampf(ball_y, b->y, b->y + b->h);
        float dx = ball_x - cx, dy = ball_y - cy, d2 = dx * dx + dy * dy;
        if (d2 >= BALL_R * BALL_R) continue;
        float d = sqrtf(d2), nx, ny;
        if (d > 0.01f) { nx = dx / d; ny = dy / d; }
        else {                                                         // centro dentro do bloco: sai pelo lado mais perto
            float l = ball_x - b->x, r = b->x + b->w - ball_x, t = ball_y - b->y, bt = b->y + b->h - ball_y;
            float m = fminf(fminf(l, r), fminf(t, bt));
            if (m == l) { nx = -1; ny = 0; d = -l; } else if (m == r) { nx = 1; ny = 0; d = -r; }
            else if (m == t) { nx = 0; ny = -1; d = -t; } else { nx = 0; ny = 1; d = -bt; }
        }
        ball_x += nx * (BALL_R - d); ball_y += ny * (BALL_R - d);
        float vn = ball_vx * nx + ball_vy * ny;
        if (vn < 0) { ball_vx -= 1.75f * vn * nx; ball_vy -= 1.75f * vn * ny; }
    }
}
static void ball_walls(void) {
    if (ball_y > 0 && ball_y < FH) {                                   // laterais do campo
        if (ball_x < BALL_R) { ball_x = BALL_R; ball_vx = fabsf(ball_vx) * 0.8f; }
        if (ball_x > FW - BALL_R) { ball_x = FW - BALL_R; ball_vx = -fabsf(ball_vx) * 0.8f; }
    }
    int mouth = (ball_x > GX1 + 8 && ball_x < GX2 - 8);
    if (ball_y < BALL_R) {                                             // linha de fundo de cima
        if (mouth) {
            if (ball_y < -GOAL_D + BALL_R) { ball_y = -GOAL_D + BALL_R; ball_vy = fabsf(ball_vy) * 0.25f; }
            if (ball_y < 0) {
                if (ball_x < GX1 + BALL_R) { ball_x = GX1 + BALL_R; ball_vx = fabsf(ball_vx) * 0.5f; }
                if (ball_x > GX2 - BALL_R) { ball_x = GX2 - BALL_R; ball_vx = -fabsf(ball_vx) * 0.5f; }
            }
        } else { ball_y = BALL_R; ball_vy = fabsf(ball_vy) * 0.8f; }
    }
    if (ball_y > FH - BALL_R) {                                        // linha de fundo de baixo
        if (mouth) {
            if (ball_y > FH + GOAL_D - BALL_R) { ball_y = FH + GOAL_D - BALL_R; ball_vy = -fabsf(ball_vy) * 0.25f; }
            if (ball_y > FH) {
                if (ball_x < GX1 + BALL_R) { ball_x = GX1 + BALL_R; ball_vx = fabsf(ball_vx) * 0.5f; }
                if (ball_x > GX2 - BALL_R) { ball_x = GX2 - BALL_R; ball_vx = -fabsf(ball_vx) * 0.5f; }
            }
        } else { ball_y = FH - BALL_R; ball_vy = -fabsf(ball_vy) * 0.8f; }
    }
}
static void ball_physics(int allow_goal) {
    float sp = sqrtf(ball_vx * ball_vx + ball_vy * ball_vy);
    if (sp > 17.0f) { ball_vx *= 17.0f / sp; ball_vy *= 17.0f / sp; sp = 17.0f; }
    int steps = (int)(sp / 5.0f) + 1;
    for (int s = 0; s < steps; s++) {                                  // sub-passos: a bola nao atravessa cercas
        ball_x += ball_vx / steps; ball_y += ball_vy / steps;
        ball_walls(); ball_vs_blocks();
    }
    ball_vx *= 0.985f; ball_vy *= 0.985f;
    if (sp < 0.06f) ball_vx = ball_vy = 0;
    ball_rot += sp * 0.08f;
    if (allow_goal) {                                                  // bola parada por 8 s: volta ao centro
        ball_idle = (sp < 0.1f) ? ball_idle + 1 : 0;
        if (ball_idle > 480) {
            burst(ball_x, ball_y, 12, RGBA(255, 255, 255), 2.5f, 3);
            ball_x = FW / 2; ball_y = FH / 2; ball_vx = ball_vy = 0; ball_idle = 0;
            burst(ball_x, ball_y, 12, RGBA(255, 255, 255), 2.5f, 3);
        }
    }
    int mouth = (ball_x > GX1 + 8 && ball_x < GX2 - 8);
    if (allow_goal && mouth) {
        if (ball_y < -BALL_R * 0.5f) fute_goal(0);                     // azul ataca o gol de cima
        else if (ball_y > FH + BALL_R * 0.5f) fute_goal(1);
    }
}

static void fute_update(struct padButtonStatus *pad, u32 btns, u32 prev) {
    frame_count++;
    for (int i = 0; i < FUTE_N; i++) {
        Actor *a = &actors[i];
        if (!a->alive) {                                               // reaparecer
            if (--a->strafe_t <= 0) fute_spawn(i);
            continue;
        }
        if (a->cd > 0) a->cd--;
        if (a->ammo < 3) a->ammo = clampf(a->ammo + 1.0f / CLS[a->cls].reload, 0, 3);
        if (a->flash > 0) a->flash--;
        if (a->invis > 0 && --a->invis == 0) burst(a->x, a->y, 14, RGBA(120, 130, 170), 2.5f, 4);
        if (a->recoil > 0) a->recoil *= 0.8f;
        a->since_hit++;
        if (a->since_hit > 120 && a->hp < maxhp(a)) a->hp = clampf(a->hp + 0.25f, 0, maxhp(a));
        if (fute_phase == 1) a->super = clampf(a->super + 0.025f, 0, 100);       // super carrega com o tempo e com acertos
    }
    if (golden_msg > 0) golden_msg--;

    if (fute_phase == 0) {                                             // contagem: todos parados
        for (int i = 0; i < FUTE_N; i++) if (actors[i].alive) fmove(&actors[i], 0, 0, 0);
        if (--fute_t <= 0) fute_phase = 1;
    } else if (fute_phase == 1) {
        if (!fute_golden && --fute_time <= 0) {                        // fim do tempo
            fute_time = 0;
            if (score[0] != score[1]) { fute_phase = 2; fute_t = 120; goal_team = -1; fute_finish = 1; fute_result = score[0] > score[1]; }
            else { fute_golden = 1; golden_msg = 180; }
        }
        Actor *p = &actors[0];
        if (p->alive) {
            const ClassDef *c = &CLS[p->cls];
            float lx = ((int)pad->ljoy_h - 128) / 128.0f, ly = ((int)pad->ljoy_v - 128) / 128.0f;
            float rx = ((int)pad->rjoy_h - 128) / 128.0f, ry = ((int)pad->rjoy_v - 128) / 128.0f;
            float lm = sqrtf(lx * lx + ly * ly);
            if (lm > 0.2f) { float s = (lm - 0.2f) / 0.8f; if (s > 1) s = 1; lx = lx / lm * s; ly = ly / lm * s; } else { lx = 0; ly = 0; }
            fmove(p, lx, ly, c->speed * 0.97f * (p->aiming ? 0.85f : 1.0f) * (p->invis > 0 ? 1.2f : 1.0f));
            float rm = sqrtf(rx * rx + ry * ry), rng = c->bspeed * c->life;
            if (rm > 0.35f) { p->aim = atan2f(ry, rx); p->aiming = 1; p->aimd = 90 + clampf((rm - 0.35f) / 0.65f, 0, 1) * (rng - 90); }
            else { p->aiming = 0; if (lm > 0.2f) p->aim = atan2f(ly, lx); p->aimd = rng * 0.6f; }
            if (btns & PAD_R1) try_shoot(0, p->aim);                   // R1 atira
            if (btns & (PAD_R2 | PAD_CIRCLE)) fute_kick(p, p->aim);    // R2 chuta a bola
            if ((btns & PAD_L1) && !(prev & PAD_L1)) use_super(0);
        }
        if (frame_count % 10 == 0) flow_build();
        for (int i = 1; i < FUTE_N; i++) fute_bot(i, 1);

        for (int i = 0; i < FUTE_N; i++)                               // jogadores se empurram
            for (int j = i + 1; j < FUTE_N; j++) {
                if (!actors[i].alive || !actors[j].alive) continue;
                float d = dist(actors[i].x, actors[i].y, actors[j].x, actors[j].y);
                if (d < 2 * R && d > 0.01f) {
                    float nx = (actors[j].x - actors[i].x) / d, ny = (actors[j].y - actors[i].y) / d, push = (2 * R - d) * 0.5f;
                    actors[i].x -= nx * push; actors[i].y -= ny * push; actors[j].x += nx * push; actors[j].y += ny * push;
                }
            }
        for (int i = 0; i < FUTE_N; i++) {                             // bola x jogador
            Actor *a = &actors[i];
            if (!a->alive) continue;
            float d = dist(a->x, a->y, ball_x, ball_y), md = R + BALL_R;
            if (d < md && d > 0.01f) {
                float nx = (ball_x - a->x) / d, ny = (ball_y - a->y) / d;
                ball_x += nx * (md - d); ball_y += ny * (md - d);
                float rv = (ball_vx - a->vx) * nx + (ball_vy - a->vy) * ny;
                if (rv < 0) { ball_vx -= 1.55f * rv * nx; ball_vy -= 1.55f * rv * ny; }
                ball_vx += a->vx * 0.25f; ball_vy += a->vy * 0.25f;
            }
        }
        ball_physics(1);
    } else {                                                           // pausa: gol ou fim de jogo
        for (int i = 0; i < FUTE_N; i++) if (actors[i].alive) fmove(&actors[i], 0, 0, 0);
        ball_physics(0);
        if (fute_t % 8 == 0 && goal_team >= 0)
            spawn_part(frand(GX1, GX2), goal_team == 0 ? 0 : FH, frand(-2, 2), frand(-4, -1), 40, 4,
                       goal_team == 0 ? RGBA(70, 150, 255) : RGBA(255, 90, 100));
        if (--fute_t <= 0) {
            if (fute_finish) fute_done = 1;
            else { goal_team = -1; fute_kickoff(100); }
        }
    }
    update_bullets();

    for (int i = 0; i < FUTE_N; i++) {                                 // direcao do corpo
        Actor *a = &actors[i];
        if (!a->alive) continue;
        float spd = sqrtf(a->vx * a->vx + a->vy * a->vy);
        float target = (i != 0 || a->aiming) ? a->aim : (spd > 0.3f ? atan2f(a->vy, a->vx) : a->face);
        a->face = angle_lerp(a->face, target, 0.2f);
        if (spd > 1.2f && a->invis <= 0 && (frame_count + i) % 7 == 0)
            spawn_part(a->x - a->vx * 2, a->y + 8, frand(-0.3f, 0.3f), frand(-0.3f, 0.1f), 16, 3, RGBA(196, 220, 150));
    }
    for (int i = 0; i < MAX_PART; i++) if (parts[i].active) {
        Particle *q = &parts[i];
        q->x += q->vx; q->y += q->vy; q->vx *= 0.93f; q->vy *= 0.93f;
        if (--q->life <= 0) q->active = 0;
    }
    for (int i = 0; i < MAX_DMG; i++) if (dmgs[i].active) {
        dmgs[i].y += dmgs[i].vy; dmgs[i].vy *= 0.94f;
        if (--dmgs[i].life <= 0) dmgs[i].active = 0;
    }
    shake *= 0.85f; if (shake < 0.2f) shake = 0;
}

// ---- desenho ----
static void fute_ground(float ox, float oy) {
    u64 line = RGBA(160, 212, 150);
    for (int i = 0; i * 80 < (int)FH; i++) {                           // faixas do gramado
        float y0 = oy + i * 80, y1 = oy + fminf((i + 1) * 80.0f, FH);
        if (y1 < 0 || y0 > SCR_H) continue;
        rect(ox, y0, ox + FW, y1, i % 2 ? RGBA(88, 178, 92) : RGBA(78, 166, 84), 1);
    }
    rect(ox - 32, oy - 90, ox - 26, oy + FH + 90, RGBA(54, 120, 66), 2);       // laterais: caminho claro + muro
    rect(ox - 26, oy - 90, ox, oy + FH + 90, RGBA(150, 196, 150), 2);
    rect(ox + FW, oy - 90, ox + FW + 26, oy + FH + 90, RGBA(150, 196, 150), 2);
    rect(ox + FW + 26, oy - 90, ox + FW + 32, oy + FH + 90, RGBA(54, 120, 66), 2);
    for (int k = 0; k < 60; k++) {                                     // flores e arbustos fora do campo
        float wy = fmodf(k * 131.0f, FH + 160.0f) - 80.0f;
        float lx = ox - 50 - (k * 37 % 40), rx = ox + FW + 40 + (k * 53 % 40);
        float sy = wy + oy;
        if (sy < -10 || sy > SCR_H + 10) continue;
        fcircle(k % 2 ? lx : rx, sy, 9, RGBA(44, 150, 76), 2);
        rect(k % 2 ? lx - 2 : rx - 2, sy - 2, k % 2 ? lx + 2 : rx + 2, sy + 2, k % 3 ? RGBA(255, 140, 170) : RGBA(255, 240, 120), 3);
    }
    rect(ox, oy + FH / 2 - 4, ox + FW, oy + FH / 2 + 4, line, 3);              // meio de campo
    for (int k = 0; k < 40; k++) {
        float a = 2 * PI * k / 40;
        rect(ox + FW / 2 + cosf(a) * 75 - 2, oy + FH / 2 + sinf(a) * 75 - 2, ox + FW / 2 + cosf(a) * 75 + 2, oy + FH / 2 + sinf(a) * 75 + 2, line, 3);
    }
    for (int s = 0; s < 2; s++) {                                      // grandes areas e gols
        float gy = s ? FH : 0, d = s ? GOAL_D : -GOAL_D, ay = s ? FH - 170 : 170;
        u64 tc = s ? RGBA(70, 150, 255) : RGBA(255, 90, 100);          // embaixo = azul, em cima = vermelho
        rect(ox + GX1 - 50, oy + ay - 2, ox + GX2 + 50, oy + ay + 2, line, 3);
        rect(ox + GX1 - 52, oy + (s ? ay : gy), ox + GX1 - 48, oy + (s ? gy : ay), line, 3);
        rect(ox + GX2 + 48, oy + (s ? ay : gy), ox + GX2 + 52, oy + (s ? gy : ay), line, 3);
        float ya = oy + (d < 0 ? gy + d : gy), yb = oy + (d < 0 ? gy : gy + d);
        rect(ox + GX1, ya, ox + GX2, yb, RGBA(58, 74, 86), 3);
        for (float x = ox + GX1; x < ox + GX2; x += 14) rect(x, ya, x + 2, yb, RGBA(150, 168, 180), 4);
        for (float y = ya; y < yb; y += 14) rect(ox + GX1, y, ox + GX2, y + 2, RGBA(150, 168, 180), 4);
        rect(ox + GX1 - 4, ya - 3, ox + GX1 + 3, yb + 3, tc, 5);
        rect(ox + GX2 - 3, ya - 3, ox + GX2 + 4, yb + 3, tc, 5);
        rect(ox + GX1 - 4, s ? yb - 3 : ya - 3, ox + GX2 + 4, s ? yb + 4 : ya + 3, tc, 5);
        fcircle(ox + GX1, oy + gy, 5, RGBA(255, 255, 255), 6);
        fcircle(ox + GX2, oy + gy, 5, RGBA(255, 255, 255), 6);
    }
}

static void draw_ball_icon(float cx, float cy, float r, int z) {
    fcircle(cx, cy, r + 1.5f, RGBA(20, 18, 44), z);
    fcircle(cx, cy, r, RGBA(255, 255, 255), z + 1);
    for (int k = 0; k < 3; k++) {
        float a = k * 2 * PI / 3 + 0.5f;
        fcircle(cx + cosf(a) * r * 0.5f, cy + sinf(a) * r * 0.5f, r * 0.3f, RGBA(40, 40, 60), z + 2);
    }
}

static void fute_hud(void) {
    char buf[32];
    Actor *p = &actors[0];
    tz = 49;
    int secs = (fute_time + 59) / 60;
    rect(0, 0, 114, 34, RGBA(10, 8, 30), 38);                          // placar azul (canto esquerdo)
    rect(0, 0, 114, 30, RGBA(40, 84, 214), 39);
    gsKit_prim_triangle(gs, 114, 0, 130, 0, 114, 30, 39, RGBA(40, 84, 214));
    draw_ball_icon(24, 15, 11, 40);
    snprintf(buf, sizeof(buf), "%d", score[0]); text_sh(66, 6, 3, RGBA(255, 255, 255), buf);
    rect(SCR_W - 114, 0, SCR_W, 34, RGBA(10, 8, 30), 38);              // placar vermelho (canto direito)
    rect(SCR_W - 114, 0, SCR_W, 30, RGBA(226, 52, 72), 39);
    gsKit_prim_triangle(gs, SCR_W - 114, 0, SCR_W - 130, 0, SCR_W - 114, 30, 39, RGBA(226, 52, 72));
    draw_ball_icon(SCR_W - 24, 15, 11, 40);
    snprintf(buf, sizeof(buf), "%d", score[1]); text_sh(SCR_W - 66 - text_w(buf, 3), 6, 3, RGBA(255, 255, 255), buf);
    rect(272, 0, 368, 30, RGBA(10, 8, 30), 38);                        // cronometro
    rect(274, 0, 366, 28, RGBA(26, 24, 70), 39);
    if (fute_golden) text_sh(320 - text_w("OURO", 2) / 2, 7, 2, GOLD, "OURO");
    else { snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60); text_sh(320 - text_w(buf, 2) / 2, 7, 2, RGBA(255, 255, 255), buf); }

    float bx = SCR_W - 52, by = SCR_H - 52;                            // botao de super (L1)
    int ready = p->super >= 100;
    fcircle(bx, by, 27, RGBA(10, 8, 30), 38);
    fcircle(bx, by, 24, ready ? ((anim / 10) % 2 ? RGBA(255, 232, 80) : RGBA(255, 190, 40)) : RGBA(36, 70, 170), 39);
    draw_skull(bx, by - 2, 11, ready ? GOLD_INK : RGBA(255, 255, 255), 40);
    for (int k = 0; k < 20; k++) {                                     // anel de carga
        float a = -PI / 2 + 2 * PI * k / 20;
        fcircle(bx + cosf(a) * 31, by + sinf(a) * 31, 2.2f, k < (int)(p->super / 5.0f) ? RGBA(255, 220, 60) : RGBA(30, 30, 70), 41);
    }
    text_sh((int)bx - text_w("L1", 1) / 2, (int)by + 14, 1, RGBA(255, 255, 255), "L1");
    float kx = bx - 68, ky = by + 8;                                   // botao de chute (R2)
    fcircle(kx, ky, 20, RGBA(10, 8, 30), 38);
    fcircle(kx, ky, 17, p->cd > 0 ? RGBA(60, 90, 70) : RGBA(70, 190, 100), 39);
    draw_ball_icon(kx, ky - 3, 8, 40);
    text_sh((int)kx - text_w("R2", 1) / 2, (int)ky + 8, 1, RGBA(255, 255, 255), "R2");

    if (fute_phase == 0) {
        if (fute_t < 20) ctext(120, 8, GOLD, "JA!");
        else { snprintf(buf, sizeof(buf), "%d", fute_t / 60 + 1); ctext(110, 10, RGBA(255, 255, 255), buf); }
    } else if (fute_phase == 2) {
        if (goal_team >= 0) {
            ctext(62, 8, GOLD, "GOOOL!");
            ctext(132, 3, goal_team == 0 ? RGBA(120, 180, 255) : RGBA(255, 130, 140), goal_team == 0 ? "TIME AZUL MARCOU!" : "TIME VERMELHO MARCOU!");
        } else ctext(70, 6, GOLD, "FIM DE JOGO!");
    } else if (golden_msg > 0) ctext(70, 5, GOLD, "GOL DE OURO!");
    if (!p->alive && fute_phase != 2) {
        snprintf(buf, sizeof(buf), "REAPARECE EM %d", p->strafe_t / 60 + 1);
        ctext(190, 3, RGBA(255, 255, 255), buf);
    }
    if (frame_count < 480 && fute_phase != 2) text_sh(14, 424, 2, RGBA(255, 232, 120), "R1 ATIRA  R2 CHUTA  L1 SUPER");
    tz = 60;
}

static void fute_render(void) {
    Actor *p = &actors[0];
    float fx = p->alive ? p->x : ball_x, fy = p->alive ? p->y : ball_y;
    cam_x += (ball_x * 0.35f + fx * 0.65f - cam_x) * 0.1f;
    cam_y += (ball_y * 0.35f + fy * 0.65f - cam_y) * 0.1f;
    float cx = clampf(cam_x, SCR_W / 2 - 60.0f, FW + 60.0f - SCR_W / 2), cy = clampf(cam_y, SCR_H / 2 - 80.0f, FH + 80.0f - SCR_H / 2);
    float ox = SCR_W / 2 - cx + (shake > 0.3f ? frand(-shake, shake) : 0);
    float oy = SCR_H / 2 - cy + (shake > 0.3f ? frand(-shake, shake) : 0);
    gsKit_clear(gs, RGBA(30, 112, 62));
    fute_ground(ox, oy);
    draw_blocks(ox, oy);

    float bx = ball_x + ox, by = ball_y + oy;                          // bola
    fellipse(bx, by + 6, 8, 3, RGBA(20, 70, 36), 10);
    fcircle(bx, by, 10, RGBA(20, 18, 44), 19);
    fcircle(bx, by, 8.6f, RGBA(255, 255, 255), 20);
    for (int k = 0; k < 3; k++) {
        float a = ball_rot + k * 2 * PI / 3;
        fcircle(bx + cosf(a) * 4.5f, by + sinf(a) * 4.5f, 2.6f, RGBA(40, 40, 60), 21);
    }
    draw_bullets(ox, oy);

    for (int i = 0; i < FUTE_N; i++) {                                 // jogadores
        const Actor *a = &actors[i];
        if (!a->alive || !visible_to(a, i, p)) continue;
        float sx = a->x + ox, sy = a->y + oy, spd = sqrtf(a->vx * a->vx + a->vy * a->vy);
        u64 tc = a->team == 0 ? RGBA(70, 150, 255) : RGBA(255, 90, 100);
        fellipse(sx, sy + 16, i == 0 ? 21 : 18, i == 0 ? 9 : 8, i == 0 ? RGBA(120, 255, 150) : tc, 12);
        int look = a->flash > 0 ? 1 : 0;
        if (!look && a->invis > 0 && !(a->invis < 120 && (anim / 6) % 2)) look = 2;
        draw_human(sx, sy, a->cls, a->face, a->aim, frame_count * 0.45f + i * 1.7f, spd > 0.6f, look, a->recoil, i == 0);
    }
    draw_bushes(ox, oy);
    if (p->alive && in_bush(p->x, p->y)) fcircle(p->x + ox, p->y + oy, 6, RGBA(53, 201, 245), 27);
    for (int i = 0; i < FUTE_N; i++) {
        const Actor *a = &actors[i];
        if (a->alive && visible_to(a, i, p)) draw_overhead(a, i, a->x + ox, a->y + oy);
    }
    for (int i = 0; i < MAX_PART; i++) if (parts[i].active) {
        const Particle *q = &parts[i];
        float s = q->size * (q->life / q->max) + 0.6f;
        rect(q->x + ox - s, q->y + oy - s, q->x + ox + s, q->y + oy + s, q->color, 28);
    }
    for (int i = 0; i < MAX_DMG; i++) if (dmgs[i].active) {
        char b[8];
        snprintf(b, sizeof(b), "%d", dmgs[i].val);
        text_sh((int)(dmgs[i].x + ox) - text_w(b, 2) / 2, (int)(dmgs[i].y + oy), 2, dmgs[i].color, b);
    }
    if (p->alive && p->aiming && fute_phase == 1) {                    // linha de mira
        const ClassDef *c = &CLS[p->cls];
        float len = c->lob ? p->aimd : c->bspeed * c->life, rr = c->lob ? 55.0f : 9.0f;
        for (float d = 28; d < len; d += 24)
            fcircle(p->x + ox + cosf(p->aim) * d, p->y + oy + sinf(p->aim) * d, 2.5f - d / len, RGBA(255, 255, 255), 29);
        for (int k = 0; k < 16; k++) {
            float a = 2 * PI * k / 16 + anim * 0.05f;
            fcircle(p->x + ox + cosf(p->aim) * len + cosf(a) * rr, p->y + oy + sinf(p->aim) * len + sinf(a) * rr, 2, ccol(p->cls, 40), 29);
        }
    }
    fute_hud();
}

static void render_game(void) { if (game_mode == 1) fute_render(); else render(); }

// ---------- menus ----------
static void draw_items(int y0, int sc, const char **items, int n, int sel) {
    for (int i = 0; i < n; i++) {
        int y = y0 + i * (sc * 7 + 18), w = text_w(items[i], sc);
        if (i == sel) {
            rect((SCR_W - w) / 2 - 16, y - 7, (SCR_W + w) / 2 + 16, y + sc * 7 + 7, RGBA(255, 210, 63), 52);
            draw_text((SCR_W - w) / 2, y, sc, RGBA(58, 26, 0), 60, items[i]);
        } else {
            ctext(y, sc, RGBA(255, 255, 255), items[i]);
        }
    }
}
static void nav(u32 pressed, int count) {
    if (pressed & PAD_UP) menu_sel = (menu_sel + count - 1) % count;
    if (pressed & PAD_DOWN) menu_sel = (menu_sel + 1) % count;
}

// ---------- menu principal "ARENA REIS" (abas: JOGAR | PERSONAGENS | COMO JOGAR) ----------
// camadas (z): fundo 1-4 | barras 6-7 | paineis 7-12 | personagem 13-22 | widgets 24-34 | retratos 39-45 | texto 49-50
static const char *TAB_NAME[3] = { "JOGAR", "PERSONAGENS", "COMO JOGAR" };
static const char *CLS_ROLE[NCLS] = { "CURTO ALCANCE", "ATIRADOR", "TANQUE", "ARTILHEIRO", "ASSASSINO" };
static const char *CLS_ATK[NCLS]  = { "3 PROJETEIS CURTOS", "1 TIRO FORTE LONGO", "TANQUE 2 PROJETEIS", "BOMBA EM ARCO AREA", "2 KUNAIS RAPIDOS" };
static const char *CLS_SUP[NCLS]  = { "ANEL DE TIROS", "LEQUE DE 5 TIROS", "ONDA DE CHOQUE", "CHUVA DE BOMBAS", "INVISIVEL POR 7S" };

static int lerpi(int a, int b, float t) { return (int)(a + (b - a) * t + 0.5f); }
static u64 bg_col(float y, int add) {                     // degrade vertical roxo -> azul escuro
    float t = clampf(y / SCR_H, 0, 1);
    if (bg_blue) return RGBA(lerpi(44, 26, t) + add, lerpi(116, 80, t) + add, lerpi(236, 200, t) + add);   // azul vivo (aba PERSONAGENS)
    return RGBA(lerpi(44, 14, t) + add, lerpi(26, 18, t) + add, lerpi(104, 48, t) + add * 2);
}
static u64 tint(int cls, float k) {                       // cor da classe misturada com o azul do menu
    return RGBA(lerpi(24, CLS[cls].r, k), lerpi(22, CLS[cls].g, k), lerpi(60, CLS[cls].b, k));
}

static void render_bg(void) {
    gsKit_clear(gs, RGBA(14, 16, 46));
    for (int i = 0; i < 16; i++) rect(0, i * 28, SCR_W, i * 28 + 28, bg_col(i * 28 + 14, 0), 1);
    int off = (anim / 3) % 128;
    for (int gx = -128; gx < SCR_W; gx += 64)
        for (int gy = -128; gy < SCR_H; gy += 64)
            if ((((gx + 128) / 64) + ((gy + 128) / 64)) % 2 == 0) {
                float x = gx + off, y = gy + off;
                rect(x, y, x + 64, y + 64, bg_col(y + 32, 10), 2);
            }
    for (int i = 0; i < 18; i++) {                        // faiscas douradas subindo
        float sp = 0.35f + (i % 5) * 0.14f;
        float y = SCR_H + 10 - fmodf(anim * sp + i * 61.0f, SCR_H + 20.0f);
        float x = fmodf(i * 83.0f + 17.0f, (float)SCR_W) + sinf(anim * 0.03f + i) * 10.0f;
        int s = 2 + i % 3;
        u64 c = (i % 3 == 0) ? RGBA(255, 210, 63) : (i % 3 == 1 ? RGBA(120, 100, 200) : RGBA(53, 201, 245));
        rect(x, y, x + s * 2, y + s * 2, c, 4);
    }
}

static void crown_shape(float cx, float cy, float s, u64 c, int z) {
    float yb = cy - 0.35f * s;
    rect(cx - s, yb, cx + s, cy, c, z);
    gsKit_prim_triangle(gs, cx - s, yb + 1, cx - 0.2f * s, yb + 1, cx - 0.9f * s, cy - 1.0f * s, z, c);
    gsKit_prim_triangle(gs, cx - 0.55f * s, yb + 1, cx + 0.55f * s, yb + 1, cx, cy - 1.3f * s, z, c);
    gsKit_prim_triangle(gs, cx + 0.2f * s, yb + 1, cx + s, yb + 1, cx + 0.9f * s, cy - 1.0f * s, z, c);
}
// coroa dourada; (cx, cy) = centro da base. Usa z .. z+3
static void draw_crown(float cx, float cy, float s, int z) {
    crown_shape(cx, cy + 0.08f * s, s * 1.12f, RGBA(40, 24, 10), z);
    crown_shape(cx, cy, s, RGBA(255, 200, 40), z + 1);
    rect(cx - s, cy - 0.35f * s, cx + s, cy - 0.25f * s, GOLD_HI, z + 2);
    rect(cx - s, cy - 0.16f * s, cx + s, cy, GOLD_LO, z + 2);
    fcircle(cx, cy - 0.2f * s, 0.12f * s, RGBA(230, 50, 70), z + 3);
    fcircle(cx - 0.6f * s, cy - 0.2f * s, 0.09f * s, RGBA(53, 201, 245), z + 3);
    fcircle(cx + 0.6f * s, cy - 0.2f * s, 0.09f * s, RGBA(53, 201, 245), z + 3);
    fcircle(cx - 0.9f * s, cy - 1.0f * s, 0.10f * s, RGBA(255, 255, 255), z + 2);
    fcircle(cx, cy - 1.3f * s, 0.12f * s, RGBA(255, 255, 255), z + 2);
    fcircle(cx + 0.9f * s, cy - 1.0f * s, 0.10f * s, RGBA(255, 255, 255), z + 2);
}

// titulo grande com contorno e sombra
static void title_text(int x, int y, int sc, u64 col, const char *s) {
    int o = sc / 2;
    u64 ink = RGBA(10, 8, 30);
    draw_text(x + o, y + o * 2, sc, ink, 46, s);
    draw_text(x - o, y, sc, ink, 47, s);
    draw_text(x + o, y, sc, ink, 47, s);
    draw_text(x, y - o, sc, ink, 47, s);
    draw_text(x, y + o, sc, ink, 47, s);
    draw_text(x, y, sc, col, 50, s);
}

static void panel(int x1, int y1, int x2, int y2, u64 border, u64 fill, int z) {
    rect(x1 + 4, y1 + 5, x2 + 4, y2 + 5, RGBA(8, 6, 24), z - 1);          // sombra
    rect(x1, y1, x2, y2, border, z);
    rect(x1 + 3, y1 + 3, x2 - 3, y2 - 3, fill, z + 1);
}
static void pill(int x, int y, int w, int h, u64 edge, u64 fill, int z) {
    rect(x, y, x + w, y + h, edge, z);
    rect(x + 2, y + 2, x + w - 2, y + h - 2, fill, z + 1);
}

static void render_tabs(void) {
    rect(0, 0, SCR_W, 44, RGBA(12, 10, 36), 6);
    rect(0, 44, SCR_W, 47, GOLD, 7);
    int gap = 8, pad = 14, w[3], total = -gap;
    for (int i = 0; i < 3; i++) { w[i] = text_w(TAB_NAME[i], 2) + pad * 2; total += w[i] + gap; }
    int x = (SCR_W - total) / 2;
    pill(x - 46, 11, 34, 24, RGBA(150, 146, 210), RGBA(50, 46, 110), 8);
    draw_text(x - 46 + 6, 17, 2, RGBA(255, 255, 255), 50, "L1");
    pill(x + total + 12, 11, 34, 24, RGBA(150, 146, 210), RGBA(50, 46, 110), 8);
    draw_text(x + total + 12 + 6, 17, 2, RGBA(255, 255, 255), 50, "R1");
    for (int i = 0; i < 3; i++) {
        if (i == tab) {
            rect(x, 5, x + w[i], 44, RGBA(10, 8, 30), 10);
            rect(x + 2, 7, x + w[i] - 2, 44, GOLD, 11);
            rect(x + 2, 7, x + w[i] - 2, 14, GOLD_HI, 12);
            draw_text(x + pad, 16, 2, GOLD_INK, 50, TAB_NAME[i]);
        } else {
            rect(x, 11, x + w[i], 44, RGBA(70, 64, 140), 10);
            rect(x + 2, 13, x + w[i] - 2, 44, RGBA(30, 28, 78), 11);
            draw_text(x + pad, 20, 2, MUTED, 50, TAB_NAME[i]);
        }
        x += w[i] + gap;
    }
}

static void render_footer(const char *hint) {
    rect(0, 408, SCR_W, SCR_H, RGBA(12, 10, 36), 6);
    rect(0, 405, SCR_W, 408, GOLD, 7);
    ctext(420, 2, MUTED, hint);
}

static void pedestal(float cx, float cy, float rx) {
    fellipse(cx, cy + 3, rx + 3, rx * 0.30f + 3, RGBA(8, 6, 24), 8);
    fellipse(cx, cy, rx, rx * 0.30f, GOLD, 9);
    fellipse(cx, cy, rx - 5, rx * 0.30f - 2, RGBA(72, 52, 150), 10);
}
static void preview_human(float cx, float cy, int cls) {
    float a = anim * 0.03f;
    pedestal(cx, cy + 22, 38);
    draw_human(cx, cy, cls, a, a, anim * 0.25f, 1, 0, 0, 0);
}

static void mode_card(int x, int y, int w, int h, int sel, int mode, const char *title, const char *sub) {
    int bob = sel ? (int)(sinf(anim * 0.1f) * 2.0f) : 0;
    y -= bob;
    u64 ink = RGBA(10, 8, 30);
    rect(x + 4, y + 6, x + w + 4, y + h + 6, RGBA(8, 6, 24), 7);
    rect(x, y, x + w, y + h, sel ? GOLD_INK : RGBA(70, 64, 140), 8);
    rect(x + 3, y + 3, x + w - 3, y + h - 3, sel ? GOLD : RGBA(30, 28, 78), 9);
    if (sel) { rect(x + 3, y + 3, x + w - 3, y + 14, GOLD_HI, 10); rect(x + 3, y + h - 13, x + w - 3, y + h - 3, GOLD_LO, 10); }
    float cx = x + 32, cy = y + h / 2;
    fcircle(cx, cy, 20, ink, 11);
    if (mode == 0) {
        fcircle(cx, cy, 18, RGBA(226, 48, 64), 12);
        draw_skull(cx, cy - 2, 10, RGBA(255, 255, 255), 13);
    } else {
        fcircle(cx, cy, 18, RGBA(255, 255, 255), 12);
        for (int k = 0; k < 3; k++) {
            float a = anim * 0.05f + k * 2 * PI / 3;
            fcircle(cx + cosf(a) * 9, cy + sinf(a) * 9, 4.5f, RGBA(40, 40, 60), 13);
        }
        fcircle(cx, cy, 3.5f, RGBA(40, 40, 60), 13);
    }
    draw_text(x + 62, y + 17, 2, sel ? GOLD_INK : RGBA(255, 255, 255), 50, title);
    draw_text(x + 62, y + 40, 1, sel ? GOLD_INK : MUTED, 50, sub);
}

static void render_tab_play(void) {
    char buf[48];
    fellipse(SCR_W / 2, 128, 250, 86, bg_col(128, 24), 3);                  // holofote atras do titulo
    int bob = (int)(sinf(anim * 0.06f) * 3.0f);
    draw_crown(SCR_W / 2, 98 + bob, 24, 20);
    title_text(143, 108, 6, GOLD, "ARENA");
    title_text(359, 108, 6, RGBA(53, 201, 245), "REIS");
    ctext(162, 2, MUTED, "ESCOLHA O MODO");
    rect(120, 168, 213, 170, GOLD, 12);
    rect(427, 168, 520, 170, GOLD, 12);

    mode_card(90, 188, 224, 64, game_mode == 0, 0, "BATTLE ROYALE", "10 JOGADORES");      // escolha do modo
    mode_card(326, 188, 224, 64, game_mode == 1, 1, "FUTE REIS 3X3", "2 GOLS OU 2 MINUTOS");
    ctext(258, 2, RGBA(255, 232, 120), "X OU START");

    panel(90, 282, 550, 392, NAVY_EDGE, NAVY_FILL, 7);                      // personagem atual
    draw_portrait(104, 296, 86, player_cls, GOLD);
    text_sh(206, 294, 1, MUTED, "SEU PERSONAGEM");
    text_sh(206, 308, 3, ccol(player_cls, 30), CLS_NAME[player_cls]);
    snprintf(buf, sizeof(buf), "ATAQUE: %s", CLS_ATK[player_cls]);
    text_sh(206, 338, 1, RGBA(255, 255, 255), buf);
    snprintf(buf, sizeof(buf), "SUPER: %s", CLS_SUP[player_cls]);
    text_sh(206, 352, 1, RGBA(255, 255, 255), buf);
    text_sh(206, 372, 1, GOLD, "L1 R1: ABA PERSONAGENS");
    preview_human(500, 338, player_cls);
}

// ---- aba PERSONAGENS: grade de cards (estilo selecao de lutadores) ----
#define N_SLOTS 6                                                  // 5 classes + 1 "em breve"
static void class_stats(int ci, float st[4]) {
    const ClassDef *c = &CLS[ci];
    st[0] = c->base_hp / 170.0f;
    st[1] = c->dmg * c->pellets / 60.0f;
    st[2] = c->speed / 2.7f;
    st[3] = sqrtf(c->bspeed * c->life / 825.0f);
}
static void draw_cup(int x, int y, u64 col) {                     // trofeu 10x10
    rect(x + 1, y, x + 9, y + 6, col, 47);
    rect(x, y + 1, x + 1, y + 4, col, 47);
    rect(x + 9, y + 1, x + 10, y + 4, col, 47);
    rect(x + 4, y + 6, x + 6, y + 8, col, 47);
    rect(x + 2, y + 8, x + 8, y + 10, col, 47);
}
static void mini_bar(int x, int y, float f, u64 fill, u64 hi) { bar(x, y, 52, 7, clampf(f, 0.08f, 1.0f), fill, hi, 24); }

static void char_card(int i, int x0, int y0, int sel) {
    const int W = 180, H = 104;
    int x1 = x0 + W, y1 = y0 + H, eq = (i == player_cls);
    float st[4]; class_stats(i, st);
    char buf[16];
    u64 ink = RGBA(10, 8, 30);

    if (sel) rect(x0 - 4, y0 - 4, x1 + 4, y1 + 4, (anim / 8) % 2 ? RGBA(255, 255, 255) : GOLD, 7);
    panel(x0, y0, x1, y1, ink, ccol(i, 15), 7);
    rect(x0 + 3, y0 + 40, x1 - 3, y1 - 3, ccol(i, -35), 9);                // degrade inferior
    rect(x0 + 148, y0 + 3, x1 - 3, y1 - 26, ccol(i, -100), 10);            // coluna da direita
    rect(x0 + 3, y1 - 26, x1 - 3, y1 - 3, ccol(i, -125), 10);              // faixa do nome

    draw_portrait(x0 + 8, y0 + 8, 66, i, ink);
    mini_bar(x0 + 86, y0 + 10, st[0], RGBA(70, 226, 100), RGBA(160, 255, 170));
    mini_bar(x0 + 86, y0 + 24, st[1], RGBA(255, 74, 90),  RGBA(255, 150, 150));
    mini_bar(x0 + 86, y0 + 38, st[2], RGBA(53, 201, 245), RGBA(150, 230, 255));
    mini_bar(x0 + 86, y0 + 52, st[3], RGBA(255, 190, 40), GOLD_HI);

    fcircle(x0 + 162, y0 + 22, 12, RGBA(255, 255, 255), 46);              // selo roxo: vitorias
    fcircle(x0 + 162, y0 + 22, 10, RGBA(150, 60, 230), 47);
    snprintf(buf, sizeof(buf), "%d", cls_wins[i] > 9 ? 9 : cls_wins[i]);
    text_sh(x0 + 162 - text_w(buf, 2) / 2, y0 + 15, 2, RGBA(255, 255, 255), buf);
    text_sh(x0 + 162 - text_w("VIT", 1) / 2, y0 + 38, 1, RGBA(255, 255, 255), "VIT");

    rect(x0 + 4, y0 + 58, x0 + 44, y0 + 74, ink, 46);                      // trofeu: recorde de abates
    draw_cup(x0 + 7, y0 + 61, GOLD);
    snprintf(buf, sizeof(buf), "%d", cls_best[i]);
    text_sh(x0 + 22, y0 + 63, 1, RGBA(255, 255, 255), buf);

    text_sh(x0 + W / 2 - text_w(CLS_NAME[i], 2) / 2, y1 - 22, 2, RGBA(255, 255, 255), CLS_NAME[i]);
    if (eq) draw_crown(x0 + 78, y0 + 4, 11, 28);
    if (i == NCLS - 1) {                                                   // etiqueta NOVO
        rect(x0 + 4, y0 - 6, x0 + 37, y0 + 8, ink, 46);
        rect(x0 + 5, y0 - 5, x0 + 36, y0 + 7, RGBA(226, 48, 64), 47);
        text_sh(x0 + 8, y0 - 2, 1, RGBA(255, 255, 255), "NOVO");
    }
}

static void locked_card(int x0, int y0) {
    const int W = 180, H = 104;
    int x1 = x0 + W, y1 = y0 + H, cx = x0 + 74, cy = y0 + 40;
    panel(x0, y0, x1, y1, RGBA(10, 8, 30), RGBA(36, 48, 112), 7);
    rect(x0 + 3, y1 - 26, x1 - 3, y1 - 3, RGBA(22, 30, 80), 10);
    rect(cx - 7, cy - 16, cx + 7, cy - 2, RGBA(150, 160, 190), 11);          // cadeado
    rect(cx - 4, cy - 13, cx + 4, cy - 2, RGBA(36, 48, 112), 12);
    rect(cx - 12, cy - 4, cx + 12, cy + 18, RGBA(10, 8, 30), 13);
    rect(cx - 10, cy - 2, cx + 10, cy + 16, GOLD, 14);
    rect(cx - 2, cy + 4, cx + 2, cy + 11, GOLD_INK, 15);
    text_sh(x0 + W / 2 - text_w("EM BREVE", 2) / 2, y1 - 22, 2, MUTED, "EM BREVE");
}

static void small_stat(int x, int y, const char *label, float f, u64 fill, u64 hi) {
    text_sh(x, y + 1, 1, RGBA(255, 255, 255), label);
    bar(x + 58, y, 84, 9, clampf(f, 0.08f, 1.0f), fill, hi, 24);
}

static void render_tab_chars(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "PERSONAGENS %d/%d", NCLS, N_SLOTS);
    text_sh(42, 54, 2, RGBA(255, 255, 255), buf);
    if (pick_flash > 0) {
        const char *m = "PERSONAGEM ESCOLHIDO!";
        text_sh(598 - text_w(m, 2), 54, 2, RGBA(150, 255, 170), m);
    }

    for (int i = 0; i < N_SLOTS; i++) {                                     // grade 3 x 2
        int x0 = 42 + (i % 3) * 188, y0 = 80 + (i / 3) * 112;
        if (i < NCLS) char_card(i, x0, y0, i == char_cur);
        else locked_card(x0, y0);
    }

    const ClassDef *c = &CLS[char_cur];                                     // detalhes do selecionado
    float st[4]; class_stats(char_cur, st);
    panel(42, 304, 598, 398, RGBA(10, 8, 30), RGBA(22, 36, 110), 7);
    preview_human(98, 346, char_cur);
    rect(152, 312, 154, 390, RGBA(60, 90, 190), 9);
    small_stat(164, 312, "VIDA",    st[0], RGBA(70, 226, 100), RGBA(160, 255, 170));
    small_stat(164, 332, "DANO",    st[1], RGBA(255, 74, 90),  RGBA(255, 150, 150));
    small_stat(164, 352, "VELOC",   st[2], RGBA(53, 201, 245), RGBA(150, 230, 255));
    small_stat(164, 372, "ALCANCE", st[3], RGBA(255, 190, 40), GOLD_HI);
    rect(322, 312, 324, 390, RGBA(60, 90, 190), 9);
    (void)c;
    text_sh(336, 310, 3, ccol(char_cur, 40), CLS_NAME[char_cur]);
    text_sh(336, 334, 1, MUTED, CLS_ROLE[char_cur]);
    text_sh(336, 346, 1, GOLD, "ATAQUE");
    text_sh(336, 355, 2, RGBA(255, 255, 255), CLS_ATK[char_cur]);
    text_sh(336, 374, 1, GOLD, "SUPER");
    text_sh(336, 383, 2, RGBA(255, 255, 255), CLS_SUP[char_cur]);
    if (char_cur == player_cls) {
        pill(512, 308, 80, 20, RGBA(20, 100, 50), RGBA(70, 226, 100), 10);
        draw_text(552 - text_w("EM USO", 2) / 2, 312, 2, RGBA(10, 50, 24), 50, "EM USO");
    } else if ((anim / 20) % 2 == 0) {
        pill(512, 308, 80, 20, GOLD_INK, GOLD, 10);
        draw_text(552 - text_w("X: ESCOLHER", 1) / 2, 315, 1, GOLD_INK, 50, "X: ESCOLHER");
    }
}

static void render_tab_help(void) {
    static const char *keys[5]  = { "ESQ", "DIR", "R1", "L1", "START" };
    static const char *descs[5] = { "MOVER COM INERCIA", "MIRAR. SOLTAR NAO ATIRA", "ATIRAR. SEGURE REPETE",
                                    "SUPER QUANDO AMARELO", "PAUSA" };
    static const char *tips[5]  = { "QUEBRE CAIXAS: CUBOS DE PODER", "ARBUSTOS ESCONDEM VOCE", "AGUA BLOQUEIA O PASSO",
                                    "FUJA DO GAS E VIRE O REI!", "FUTE: R1 ATIRA, R2 CHUTA, 2 GOLS!" };
    panel(40, 56, 600, 398, NAVY_EDGE, NAVY_FILL, 7);
    text_sh(64, 66, 2, GOLD, "CONTROLES");
    rect(64, 84, 576, 86, NAVY_EDGE, 9);
    for (int i = 0; i < 5; i++) {
        int y = 94 + i * 28;
        pill(64, y, 90, 22, GOLD_INK, GOLD, 9);
        draw_text(64 + (90 - text_w(keys[i], 2)) / 2, y + 4, 2, GOLD_INK, 50, keys[i]);
        text_sh(172, y + 4, 2, RGBA(255, 255, 255), descs[i]);
    }
    text_sh(64, 242, 2, GOLD, "DICAS");
    rect(64, 260, 576, 262, NAVY_EDGE, 9);
    for (int i = 0; i < 5; i++) {
        int y = 272 + i * 24;
        rect(68, y + 3, 76, y + 11, i % 2 ? RGBA(53, 201, 245) : GOLD, 9);
        text_sh(90, y, 2, RGBA(255, 255, 255), tips[i]);
    }
}

static void render_title(void) {
    tz = 50;
    bg_blue = (tab == 1);
    render_bg();
    render_tabs();
    if (tab == 0) {
        render_tab_play();
        render_footer("X: JOGAR   < >: MODO   L1 R1: ABAS");
    } else if (tab == 1) {
        render_tab_chars();
        render_footer("SETAS: VER   X: ESCOLHER   START: JOGAR");
    } else {
        render_tab_help();
        render_footer("L1 R1: ABAS   X: VOLTAR");
    }
    tz = 60;
}

static void draw_overlay(const char *title, const char *sub, const char **items, int n) {
    rect(110, 117, 538, 340, RGBA(8, 6, 24), 49);
    rect(106, 106, 534, 334, GOLD, 50);
    rect(110, 110, 530, 330, RGBA(30, 27, 76), 51);
    draw_crown(SCR_W / 2, 112, 14, 53);
    ctext(124, 4, GOLD, title);
    if (sub) ctext(166, 2, MUTED, sub);
    draw_items(200, 3, items, n, menu_sel);
}

static void start_game(void) {
    if (game_mode == 1) fute_reset(); else reset_game();
    state = S_PLAY;
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
    gs = gsKit_init_global_custom(1024 * 1024, 16 * 1024);                 // buffer maior para muitos desenhos por quadro
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
        case S_TITLE: {
            int old_tab = tab;
            if (pressed & PAD_L1) tab = (tab + 2) % 3;
            if (pressed & PAD_R1) tab = (tab + 1) % 3;
            if (tab != old_tab) { if (tab == 1) char_cur = player_cls; pick_flash = 0; }
            if (tab == 0) {                                                  // JOGAR
                if (pressed & (PAD_LEFT | PAD_RIGHT)) game_mode = !game_mode;
                if (pressed & (PAD_CROSS | PAD_START)) start_game();
            } else if (tab == 1) {                                           // PERSONAGENS
                if (pressed & PAD_LEFT) char_cur = (char_cur + NCLS - 1) % NCLS;
                if (pressed & PAD_RIGHT) char_cur = (char_cur + 1) % NCLS;
                if ((pressed & PAD_DOWN) && char_cur + 3 < NCLS) char_cur += 3;
                if ((pressed & PAD_UP) && char_cur >= 3) char_cur -= 3;
                if (pressed & PAD_CROSS) { player_cls = char_cur; pick_flash = 70; }
                if (pressed & PAD_START) { player_cls = char_cur; start_game(); }
                if (pressed & PAD_CIRCLE) tab = 0;
            } else {                                                         // COMO JOGAR
                if (pressed & (PAD_CROSS | PAD_CIRCLE | PAD_START)) tab = 0;
            }
            if (pick_flash > 0) pick_flash--;
            render_title();
            break;
        }

        case S_PLAY:
            if (pressed & PAD_START) { state = S_PAUSE; menu_sel = 0; }
            else {
                if (game_mode == 1) {
                    fute_update(&pad, btns, prev);
                    if (fute_done) { state = S_OVER; menu_sel = 0; }
                } else {
                    update(&pad, btns, prev);
                    if (winner >= 0 || dead_timer > 120) {
                        if (winner == 0) cls_wins[player_cls]++;
                        if (actors[0].kills > cls_best[player_cls]) cls_best[player_cls] = actors[0].kills;
                        state = S_OVER; menu_sel = 0;
                    }
                }
            }
            render_game();
            break;

        case S_PAUSE: {
            const char *items[3] = { "CONTINUAR", "REINICIAR", "MENU PRINCIPAL" };
            nav(pressed, 3);
            if (pressed & PAD_START) state = S_PLAY;
            else if (pressed & PAD_CROSS) {
                if (menu_sel == 0) state = S_PLAY;
                else if (menu_sel == 1) { start_game(); }
                else { state = S_TITLE; tab = 0; menu_sel = 0; }
            }
            render_game();
            if (state == S_PAUSE) draw_overlay("PAUSA", game_mode == 1 ? "FUTE REIS 3X3" : CLS_NAME[player_cls], items, 3);
            break;
        }

        case S_OVER: {
            char sub[48];
            const char *items[2] = { "JOGAR DE NOVO", "MENU PRINCIPAL" };
            nav(pressed, 2);
            if (pressed & PAD_CROSS) {
                if (menu_sel == 0) { start_game(); }
                else { state = S_TITLE; tab = 0; menu_sel = 0; }
            }
            render_game();
            if (state == S_OVER && game_mode == 1) {
                snprintf(sub, sizeof(sub), "PLACAR: %d X %d", score[0], score[1]);
                draw_overlay(fute_result ? "VITORIA!" : "DERROTA", sub, items, 2);
            } else if (state == S_OVER) {
                if (winner == 0) snprintf(sub, sizeof(sub), "ABATES: %d", actors[0].kills);
                else snprintf(sub, sizeof(sub), "POSICAO: %d/%d   ABATES: %d", actors[0].place ? actors[0].place : N_ACTORS, N_ACTORS, actors[0].kills);
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
