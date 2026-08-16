/*
 * Block Blast pour NumWorks
 * -------------------------
 * Jeu de puzzle façon "1010!/Block Blast" : on pose des pièces sur une
 * grille 8x8, et une ligne (rangée ou colonne) entièrement remplie est
 * effacée. Pas de rotation, pas de chute : c'est le joueur qui choisit
 * où poser chaque pièce avec les flèches, puis valide avec OK.
 *
 * Principe anti-flicker :
 *   On ne DESSINE JAMAIS un grand rectangle pour "effacer" l'écran avant
 *   de le redessiner. On ne touche que les cases qui changent réellement
 *   (déplacement du curseur, pose d'une pièce, ligne effacée, score).
 *   Comme il n'y a qu'un seul framebuffer sur la calculatrice, dessiner
 *   une grande zone puis la redessiner par-dessus est ce qui provoque le
 *   scintillement visible. Ici chaque case est réécrite une seule fois
 *   avec sa couleur finale, jamais "effacée puis redessinée".
 */

#include <eadk.h>
#include <stdbool.h>
#include <stdint.h>

/* ---------- Métadonnées de l'application ----------
 * Si le Makefile de votre modèle définit déjà eadk_app_name /
 * eadk_app_api_level ailleurs (vous aurez alors une erreur de
 * l'éditeur de liens du type "multiple definition of eadk_app_name"),
 * supprimez simplement ces deux lignes : elles ne sont utiles que si
 * rien ne les définit déjà dans le projet cloné. */
const char eadk_app_name[] = "Block Blast";
const uint32_t eadk_app_api_level = 0;

/* ---------- Configuration de la grille ---------- */
#define GRID_SIZE   8
#define CELL        20
#define GRID_X      10
#define GRID_Y      34

#define RGB565(r, g, b) \
  ((eadk_color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

static const eadk_color_t COLOR_BG        = RGB565(20, 20, 28);
static const eadk_color_t COLOR_BORDER    = RGB565(60, 60, 72);
static const eadk_color_t COLOR_EMPTY     = RGB565(232, 232, 238);
static const eadk_color_t COLOR_INVALID   = RGB565(255, 90, 90);

static const eadk_color_t PALETTE[6] = {
    RGB565(230, 60, 60),   /* rouge   */
    RGB565(240, 150, 40),  /* orange  */
    RGB565(230, 210, 40),  /* jaune   */
    RGB565(60, 190, 90),   /* vert    */
    RGB565(60, 140, 230),  /* bleu    */
    RGB565(170, 90, 220),  /* violet  */
};
#define PALETTE_SIZE 6

/* ---------- Définition des pièces (pas de rotation) ---------- */
typedef struct {
  int8_t dr, dc;
} cell_offset_t;

typedef struct {
  cell_offset_t cells[4];
  uint8_t count;
  uint8_t h, w; /* hauteur / largeur de la boite englobante */
} shape_t;

static const shape_t SHAPES[9] = {
    {{{0, 0}}, 1, 1, 1},                                     /* carre simple  */
    {{{0, 0}, {0, 1}}, 2, 1, 2},                              /* domino H     */
    {{{0, 0}, {1, 0}}, 2, 2, 1},                              /* domino V     */
    {{{0, 0}, {0, 1}, {0, 2}}, 3, 1, 3},                       /* barre H      */
    {{{0, 0}, {1, 0}, {2, 0}}, 3, 3, 1},                       /* barre V      */
    {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}, 4, 2, 2},               /* carre 2x2    */
    {{{0, 0}, {1, 0}, {1, 1}}, 3, 2, 2},                       /* coin L       */
    {{{0, 1}, {1, 0}, {1, 1}}, 3, 2, 2},                       /* coin J       */
    {{{0, 0}, {0, 1}, {1, 1}, {2, 1}}, 4, 3, 2},               /* zigzag       */
};
#define SHAPE_COUNT 9

/* ---------- État du jeu ---------- */
static int8_t grid[GRID_SIZE][GRID_SIZE]; /* -1 = vide, sinon index couleur */
static int score = 0;
static bool game_over = false;
static const shape_t *cur_shape;
static int cur_color;

/* ---------- Petits utilitaires ---------- */

static void int_to_str(int value, char *out) {
  char tmp[12];
  int i = 0;
  if (value <= 0) {
    out[0] = '0';
    out[1] = '\0';
    return;
  }
  while (value > 0 && i < (int)sizeof(tmp)) {
    tmp[i++] = (char)('0' + (value % 10));
    value /= 10;
  }
  int j = 0;
  while (i > 0) {
    out[j++] = tmp[--i];
  }
  out[j] = '\0';
}

/* ---------- Dessin ---------- */

static eadk_color_t cell_color(int8_t v) {
  if (v < 0) {
    return COLOR_EMPTY;
  }
  return PALETTE[v];
}

/* Redessine une seule case avec une couleur donnee (bordure + interieur) */
static void draw_cell(int r, int c, eadk_color_t color) {
  eadk_rect_t outer = {(uint16_t)(GRID_X + c * CELL), (uint16_t)(GRID_Y + r * CELL), CELL, CELL};
  eadk_display_push_rect_uniform(outer, COLOR_BORDER);
  eadk_rect_t inner = {(uint16_t)(GRID_X + c * CELL + 1), (uint16_t)(GRID_Y + r * CELL + 1),
                        CELL - 2, CELL - 2};
  eadk_display_push_rect_uniform(inner, color);
}

/* Redessine une case avec son contenu REEL (utilise pour "effacer" un aperçu) */
static void redraw_true_cell(int r, int c) {
  draw_cell(r, c, cell_color(grid[r][c]));
}

static void redraw_full_grid(void) {
  for (int r = 0; r < GRID_SIZE; r++) {
    for (int c = 0; c < GRID_SIZE; c++) {
      redraw_true_cell(r, c);
    }
  }
}

static void redraw_score(void) {
  eadk_rect_t area = {200, 30, 110, 20};
  eadk_display_push_rect_uniform(area, COLOR_BG);
  char buf[20] = "Score: ";
  char num[12];
  int_to_str(score, num);
  int i = 7;
  int j = 0;
  while (num[j] != '\0') {
    buf[i++] = num[j++];
  }
  buf[i] = '\0';
  eadk_display_draw_string(buf, (eadk_point_t){200, 30}, false, eadk_color_white, COLOR_BG);
}

static void draw_static_ui(void) {
  eadk_display_push_rect_uniform(eadk_screen_rect, COLOR_BG);
  eadk_display_draw_string("Block Blast", (eadk_point_t){10, 8}, true, eadk_color_white, COLOR_BG);
  redraw_full_grid();
  redraw_score();
  eadk_display_draw_string("OK : poser", (eadk_point_t){200, 60}, false, eadk_color_white, COLOR_BG);
  eadk_display_draw_string("Fleches : bouger", (eadk_point_t){200, 80}, false, eadk_color_white,
                            COLOR_BG);
  eadk_display_draw_string("Retour : quitter", (eadk_point_t){200, 100}, false, eadk_color_white,
                            COLOR_BG);
}

static void draw_game_over(void) {
  eadk_rect_t area = {10, 205, 300, 30};
  eadk_display_push_rect_uniform(area, COLOR_BG);
  eadk_display_draw_string("GAME OVER - OK pour rejouer", (eadk_point_t){10, 210}, false,
                            PALETTE[0], COLOR_BG);
}

/* ---------- Logique du jeu ---------- */

static void init_grid(void) {
  for (int r = 0; r < GRID_SIZE; r++) {
    for (int c = 0; c < GRID_SIZE; c++) {
      grid[r][c] = -1;
    }
  }
}

static bool can_place(int ar, int ac, const shape_t *s) {
  for (int i = 0; i < s->count; i++) {
    int r = ar + s->cells[i].dr;
    int c = ac + s->cells[i].dc;
    if (r < 0 || r >= GRID_SIZE || c < 0 || c >= GRID_SIZE) {
      return false;
    }
    if (grid[r][c] != -1) {
      return false;
    }
  }
  return true;
}

static bool any_valid_placement(const shape_t *s) {
  for (int r = 0; r <= GRID_SIZE - s->h; r++) {
    for (int c = 0; c <= GRID_SIZE - s->w; c++) {
      if (can_place(r, c, s)) {
        return true;
      }
    }
  }
  return false;
}

static void spawn_piece(void) {
  cur_shape = &SHAPES[eadk_random() % SHAPE_COUNT];
  cur_color = (int)(eadk_random() % PALETTE_SIZE);
}

static void place_piece(int ar, int ac, const shape_t *s, int color) {
  for (int i = 0; i < s->count; i++) {
    int r = ar + s->cells[i].dr;
    int c = ac + s->cells[i].dc;
    grid[r][c] = (int8_t)color;
    redraw_true_cell(r, c);
  }
  score += s->count * 10;
  redraw_score();
}

static void clear_lines(void) {
  bool row_full[GRID_SIZE];
  bool col_full[GRID_SIZE];
  int cleared = 0;

  for (int r = 0; r < GRID_SIZE; r++) {
    bool full = true;
    for (int c = 0; c < GRID_SIZE; c++) {
      if (grid[r][c] == -1) {
        full = false;
        break;
      }
    }
    row_full[r] = full;
    if (full) cleared++;
  }

  for (int c = 0; c < GRID_SIZE; c++) {
    bool full = true;
    for (int r = 0; r < GRID_SIZE; r++) {
      if (grid[r][c] == -1) {
        full = false;
        break;
      }
    }
    col_full[c] = full;
    if (full) cleared++;
  }

  if (cleared == 0) {
    return;
  }

  for (int r = 0; r < GRID_SIZE; r++) {
    for (int c = 0; c < GRID_SIZE; c++) {
      if (row_full[r] || col_full[c]) {
        grid[r][c] = -1;
      }
    }
  }
  for (int r = 0; r < GRID_SIZE; r++) {
    for (int c = 0; c < GRID_SIZE; c++) {
      if (row_full[r] || col_full[c]) {
        redraw_true_cell(r, c);
      }
    }
  }

  score += cleared * 100;
  redraw_score();
}

/* Dessine (ou efface) l'aperçu de la piece courante a une position donnee */
static void draw_piece_preview(int ar, int ac, const shape_t *s, int color, bool erase) {
  bool valid = !erase && can_place(ar, ac, s);
  for (int i = 0; i < s->count; i++) {
    int r = ar + s->cells[i].dr;
    int c = ac + s->cells[i].dc;
    if (r < 0 || r >= GRID_SIZE || c < 0 || c >= GRID_SIZE) {
      continue;
    }
    if (erase) {
      redraw_true_cell(r, c);
    } else {
      draw_cell(r, c, valid ? PALETTE[color] : COLOR_INVALID);
    }
  }
}

/* ---------- Boucle principale ---------- */

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  init_grid();
  draw_static_ui();
  spawn_piece();

  int cursor_r = 0;
  int cursor_c = 0;
  draw_piece_preview(cursor_r, cursor_c, cur_shape, cur_color, false);

  eadk_keyboard_state_t prev_state = 0;

  while (1) {
    eadk_keyboard_state_t state = eadk_keyboard_scan();
    eadk_keyboard_state_t pressed = state & ~prev_state;
    prev_state = state;

    if (eadk_keyboard_key_down(pressed, eadk_key_back)) {
      break; /* quitte l'application */
    }

    if (game_over) {
      if (eadk_keyboard_key_down(pressed, eadk_key_ok)) {
        init_grid();
        score = 0;
        game_over = false;
        redraw_full_grid();
        redraw_score();
        spawn_piece();
        cursor_r = 0;
        cursor_c = 0;
        draw_piece_preview(cursor_r, cursor_c, cur_shape, cur_color, false);
      }
      eadk_timing_msleep(30);
      continue;
    }

    int new_r = cursor_r;
    int new_c = cursor_c;
    if (eadk_keyboard_key_down(pressed, eadk_key_left)) new_c--;
    if (eadk_keyboard_key_down(pressed, eadk_key_right)) new_c++;
    if (eadk_keyboard_key_down(pressed, eadk_key_up)) new_r--;
    if (eadk_keyboard_key_down(pressed, eadk_key_down)) new_r++;

    if (new_r < 0) new_r = 0;
    if (new_c < 0) new_c = 0;
    if (new_r > GRID_SIZE - cur_shape->h) new_r = GRID_SIZE - cur_shape->h;
    if (new_c > GRID_SIZE - cur_shape->w) new_c = GRID_SIZE - cur_shape->w;

    if (new_r != cursor_r || new_c != cursor_c) {
      draw_piece_preview(cursor_r, cursor_c, cur_shape, cur_color, true);
      cursor_r = new_r;
      cursor_c = new_c;
      draw_piece_preview(cursor_r, cursor_c, cur_shape, cur_color, false);
    }

    if (eadk_keyboard_key_down(pressed, eadk_key_ok)) {
      if (can_place(cursor_r, cursor_c, cur_shape)) {
        place_piece(cursor_r, cursor_c, cur_shape, cur_color);
        clear_lines();
        spawn_piece();
        cursor_r = 0;
        cursor_c = 0;
        if (!any_valid_placement(cur_shape)) {
          game_over = true;
          draw_game_over();
        } else {
          draw_piece_preview(cursor_r, cursor_c, cur_shape, cur_color, false);
        }
      }
    }

    eadk_timing_msleep(30);
  }

  return 0;
}
