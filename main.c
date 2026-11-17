// PS2 Showdown - battle royale estilo "arena colorida" (top-down, 10 jogadores)
// PS2SDK + gsKit + libpad. Personagens e nomes sao originais.
//
// Menu: CIMA/BAIXO navega | X confirma | ESQ/DIR troca a classe | START pausa
// Jogo:
//   Analogico esquerdo : mover (com inercia)
//   Analogico direito  : mirar (solte para atirar)
//   QUADRADO           : super (cada classe tem o seu)
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

enum { WALL = 0, CRATE = 1, BUSH = 2, WATER = 3, ROCK = 4 };
enum { S_TITLE = 0, S_HELP, S_PLAY, S_PAUSE, S_OVER };

typedef struct {
    float x, y, vx, vy, aim, face, hp, ammo, super, cd, recoil;
    int alive, cls, cubes, aiming, place, kills, flash, since_hit, strafe_dir, strafe_t;
} Actor;
typedef struct { float x, y, dx, dy, life, dmg; int owner, active; } Bullet;
typedef struct { float x, y, w, h, hp; int kind; } Block;
typedef struct { float x, y; int active; } Cube;
typedef struct { float x, y, vx, vy, life, max, size; u64 color; int active; } Particle;
typedef struct { float x, y, vy, life; int val; u64 color; int active; } DmgNum;
typedef struct { float x, y; int type; } Decor;
typedef struct { char a[8], b[8]; int ca, cb, gas, ttl; } Feed;

// classes: 0 = Estilhaco (shotgun), 1 = Falcao (sniper), 2 = Muralha (tanque)
typedef struct {
    int r, g, b;
    float speed, bspeed, life, dmg, spread, base_hp, cd, reload, kb;
    int pellets;
} ClassDef;
static const ClassDef CLS[3] = {
    { 255, 150,  30, 2.5f,  8.5f, 22, 16, 0.30f, 100, 14, 70, 2.2f, 3 },
    { 170,  90, 255, 2.2f, 15.0f, 55, 52, 0.00f,  80, 30, 95, 3.5f, 1 },
    {  60, 200, 120, 2.0f,  9.5f, 18, 28, 0.22f, 170, 20, 80, 1.5f, 2 },
};
static const char *CLS_NAME[3] = { "ESTILHACO", "FALCAO", "MURALHA" };
static const char *CLS_DESC[3] = { "3 PROJETEIS. SUPER: ANEL DE TIROS", "TIRO FORTE. SUPER: LEQUE DE 5 TIROS", "MUITA VIDA. SUPER: ONDA DE CHOQUE" };

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
static float zone_r, zone_from, shake;

static char padBuf[256] __attribute__((aligned(64)));
static GSGLOBAL *gs;

#define RGBA(r, g, b) GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00)

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
        ac->cls = (i == 0) ? player_cls : rand() % 3;
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
    } else {
        for (int j = 0; j < N_ACTORS; j++) {
            if (j == who || !actors[j].alive) continue;
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
                    if (cd < 150 && fabsf(angle_diff(b->aim, a)) < 0.2f) try_shoot(i, b->aim);
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
            accel_move(b, cosf(a) * m, sinf(a) * m, c->speed * 0.62f);
            if (!has_aim) aim_t = a;
        } else accel_move(b, 0, 0, 0);
        b->aim = angle_lerp(b->aim, aim_t, 0.16f);
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
        accel_move(p, lx, ly, c->speed * (p->aiming ? 0.75f : 1.0f));
        if (fabsf(rx) > 0.35f || fabsf(ry) > 0.35f) { p->aim = atan2f(ry, rx); p->aiming = 1; }
        else if (p->aiming) { try_shoot(0, p->aim); p->aiming = 0; }   // solta = atira
        if ((btns & PAD_SQUARE) && !(prev & PAD_SQUARE)) use_super(0);
    }
    update_bots();

    for (int i = 0; i < N_ACTORS; i++) {
        Actor *a = &actors[i];
        if (!a->alive) continue;
        if (a->cd > 0) a->cd--;
        if (a->ammo < 3) a->ammo = clampf(a->ammo + 1.0f / CLS[a->cls].reload, 0, 3);
        if (a->flash > 0) a->flash--;
        if (a->recoil > 0) a->recoil *= 0.8f;
        a->since_hit++;
        float spd = sqrtf(a->vx * a->vx + a->vy * a->vy);
        float target = (a->aiming || (i != 0 && spd < 0.3f)) ? a->aim : (spd > 0.3f ? atan2f(a->vy, a->vx) : a->face);
        if (i != 0 && !a->aiming) target = a->aim;
        a->face = angle_lerp(a->face, target, 0.2f);
        if (spd > 1.2f && (frame_count + i) % 7 == 0)
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
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (!b->active) continue;
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
            for (int j = 0; j < N_ACTORS; j++) {
                if (j == b->owner || !actors[j].alive) continue;
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
static const int SKIN_C[3][3] = { { 255, 206, 160 }, { 242, 192, 152 }, { 196, 140, 98 } };
static const int HAIR_C[3][3] = { { 214, 74, 30 }, { 76, 44, 130 }, { 44, 32, 26 } };

static int visible_to(const Actor *a, int i, const Actor *p) {
    return !(i != 0 && in_bush(a->x, a->y) && dist(a->x, a->y, p->x, p->y) > 90);
}

static void draw_human(float sx, float sy, int cls, float face, float aim, float walk,
                       int moving, int flash, float recoil, int me) {
    u64 skin = RGBA(SKIN_C[cls][0], SKIN_C[cls][1], SKIN_C[cls][2]);
    u64 hair = RGBA(HAIR_C[cls][0], HAIR_C[cls][1], HAIR_C[cls][2]);
    u64 body = flash ? RGBA(255, 255, 255) : ccol(cls, 0);
    u64 dark = ccol(cls, -70);
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

    // braco que segura a arma (aponta para a mira) e braco solto
    float sh = cosf(aim) >= 0 ? 1.0f : -1.0f;
    float shx = sx + sh * 8, shy = by - 3;
    float hx = sx + cosf(aim) * 11, hy = by - 1 + sinf(aim) * 9;
    int gz = sinf(aim) > -0.3f ? 22 : 15;
    for (int t = 0; t <= 2; t++) fcircle(shx + (hx - shx) * t / 2.0f, shy + (hy - shy) * t / 2.0f, 3.4f, body, gz);
    fcircle(hx, hy, 3.0f, skin, gz);
    for (int d = 5; d <= 17; d += 4) {
        float g = d - recoil * 0.5f;
        fcircle(hx + cosf(aim) * g, hy + sinf(aim) * g, d == 17 ? 2.0f : 2.6f, d == 17 ? RGBA(120, 122, 140) : RGBA(60, 62, 78), gz);
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
    } else {
        rect(sx - 7, hy2 - 1, sx + 7, hy2 + 0.5f, ccol(2, 70), 21);           // aba do capacete
    }
}

// vida, municao, nome e cubos ficam em cima do personagem
static void draw_overhead(const Actor *a, int idx, float sx, float sy) {
    int old = tz; tz = 36;
    char buf[16];
    const int w = 46;
    float top = sy - 34, x0 = sx - w / 2;
    float f = clampf(a->hp / maxhp(a), 0, 1);
    u64 fill = idx == 0 ? RGBA(70, 226, 100) : RGBA(255, 74, 90);
    u64 hi = idx == 0 ? RGBA(160, 255, 170) : RGBA(255, 150, 150);
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
    text_sh((int)(sx - text_w(names[idx], 1) / 2.0f), (int)top - 29, 1, idx == 0 ? RGBA(130, 255, 160) : RGBA(255, 130, 140), names[idx]);
    snprintf(buf, sizeof(buf), "%d", a->cubes);
    float xs = sx - (9 + text_w(buf, 1)) / 2.0f;
    rect(xs, top - 39, xs + 8, top - 31, RGBA(60, 16, 104), 31);
    rect(xs + 1, top - 38, xs + 7, top - 32, RGBA(178, 78, 255), 32);
    text_sh((int)xs + 11, (int)top - 39, 1, RGBA(255, 255, 255), buf);
    tz = old;
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
    // blocos (sem arbustos)
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
    // cubos de poder
    for (int k = 0; k < MAX_CUBES; k++) if (cubes[k].active) {
        float bob = sinf(frame_count * 0.2f + cubes[k].x) * 2.5f, x = cubes[k].x + ox, y = cubes[k].y + oy + bob;
        fellipse(x, y + 12, 8, 3, RGBA(176, 142, 90), 8);
        fcircle(x, y, 13, RGBA(220, 180, 255), 8);
        rect(x - 8, y - 8, x + 8, y + 8, RGBA(60, 16, 104), 9);
        rect(x - 6, y - 6, x + 6, y + 6, RGBA(178, 78, 255), 10);
        rect(x - 4, y - 4, x, y, RGBA(228, 180, 255), 11);
    }
    // projeteis
    for (int i = 0; i < MAX_BULLETS; i++) if (bullets[i].active) {
        const Bullet *b = &bullets[i];
        int cl = actors[b->owner].cls;
        fcircle(b->x - b->dx * 0.7f + ox, b->y - b->dy * 0.7f + oy, 3, ccol(cl, -40), 11);
        fcircle(b->x + ox, b->y + oy, 5, ccol(cl, 0), 12);
        fcircle(b->x + ox, b->y + oy, 2.5f, RGBA(255, 255, 255), 13);
    }
    // personagens
    for (int i = 0; i < N_ACTORS; i++) {
        const Actor *a = &actors[i];
        if (!a->alive || !visible_to(a, i, p)) continue;
        float spd = sqrtf(a->vx * a->vx + a->vy * a->vy);
        draw_human(a->x + ox, a->y + oy, a->cls, a->face, a->aim, frame_count * 0.45f + i * 1.7f,
                   spd > 0.6f, a->flash > 0, a->recoil, i == 0);
    }
    // arbustos por cima
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
        float len = c->bspeed * c->life;
        for (float d = 28; d < len; d += 24)
            fcircle(p->x + ox + cosf(p->aim) * d, p->y + oy + sinf(p->aim) * d, 2.5f - d / len, RGBA(255, 255, 255), 29);
        for (int k = 0; k < 8; k++) {
            float a = 2 * PI * k / 8 + anim * 0.05f;
            fcircle(p->x + ox + cosf(p->aim) * len + cosf(a) * 9, p->y + oy + sinf(p->aim) * len + sinf(a) * 9, 2, ccol(p->cls, 40), 29);
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
        text_sh((int)(bx - text_w("QUADRADO", 1) / 2.0f), SCR_H - 30, 1, RGBA(255, 255, 255), "QUADRADO");
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
    if (p->alive && frame_count < 480) ctext(396, 2, RGBA(255, 232, 120), "ANALOGICO DIR: MIRE E SOLTE   QUADRADO: SUPER");
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
        fcircle(cx, cy, sel ? 22 : 15, ccol(i, 0), 18);
        fcircle(cx + 5, cy - 3, sel ? 5 : 3, RGBA(255, 255, 255), 19);
    }
    ctext(382, 2, RGBA(185, 180, 230), CLS_DESC[player_cls]);
    ctext(420, 2, RGBA(185, 180, 230), "CIMA BAIXO: NAVEGAR   X: ESCOLHER");
}

static void render_help(void) {
    static const char *lines[] = {
        "ANALOGICO ESQ: MOVER (COM INERCIA)",
        "ANALOGICO DIR: MIRAR E SOLTAR PARA ATIRAR",
        "SEM MIRA AUTOMATICA: VOCE MIRA E ATIRA",
        "QUADRADO: SUPER QUANDO A BARRA ESTIVER CHEIA",
        "START: PAUSA",
        "QUEBRE CAIXAS PARA PEGAR CUBOS DE PODER",
        "ARBUSTOS ESCONDEM VOCE. AGUA BLOQUEIA O PASSO",
        "VIDA E MUNICAO FICAM EM CIMA DO PERSONAGEM",
        "FUJA DO GAS VERDE E SEJA O ULTIMO DE PE",
    };
    gsKit_clear(gs, RGBA(22, 20, 58));
    render_bg_checker();
    ctext(26, 5, RGBA(255, 210, 63), "COMO JOGAR");
    for (int i = 0; i < 9; i++) text_sh(40, 100 + i * 29, 2, RGBA(255, 255, 255), lines[i]);
    ctext(410, 2, RGBA(185, 180, 230), "X: VOLTAR");
}

static void draw_overlay(const char *title, const char *sub, const char **items, int n) {
    rect(106, 106, 534, 334, RGBA(255, 210, 63), 50);
    rect(110, 110, 530, 330, RGBA(30, 27, 76), 51);
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
            char sub[48];
            const char *items[2] = { "JOGAR DE NOVO", "MENU PRINCIPAL" };
            nav(pressed, 2);
            if (pressed & PAD_CROSS) {
                if (menu_sel == 0) { reset_game(); state = S_PLAY; }
                else { state = S_TITLE; menu_sel = 0; }
            }
            render();
            if (state == S_OVER) {
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
