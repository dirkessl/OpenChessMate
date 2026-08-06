/*
 * micro_engine.cpp — Lightweight chess engine implementation
 *
 * Alpha-beta search with quiescence, move generation, and FEN support.
 * Designed to run on ESP32-S3 with minimal memory footprint.
 *
 * Inspired by Micro-Max by H.G. Muller — a complete chess engine in ~100 lines of C.
 *   http://home.hccnet.nl/h.g.muller/max-src2.html
 *   https://chessprogramming.wikispaces.com/Micro-Max
 *
 * Techniques used (common to many small engines, popularised by Micro-Max):
 *   - Fail-soft alpha-beta with quiescence search
 *   - MVV-LVA (Most Valuable Victim – Least Valuable Attacker) move ordering
 *   - Piece-square tables for positional evaluation
 *   - FEN parsing/generation
 *
 * This is an independent, readable reimplementation.  No source code from
 * Micro-Max was copied; only the general algorithmic approach is shared.
 */

#include "micro_engine.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Watchdog feeding and node limiting (ESP32 only).
// The engine task is NOT subscribed to the Arduino TWDT so WDT feeding here
// is only a safety measure; the real time bound is NODE_LIMIT.
#ifndef SIMULATOR
#include <esp_task_wdt.h>
// Must be a power of 2 for the bitmask trick (& (N-1) == % N only when N is power of 2)
#define WDT_FEED_INTERVAL 2048
#endif

// Hard node limit per search — keeps depth-5 searches within ~2-5 s on ESP32-S3.
#define NODE_LIMIT 100000

// Per-search node counter (set to 0 at the start of me_search, then shared
// through the recursive calls via a file-scope variable — safe because the
// engine runs on a single dedicated task).
static volatile int s_node_count = 0;
static volatile bool s_search_aborted = false;

// Piece values for evaluation (centipawns)
static const int PIECE_VAL[] = {0, 100, 320, 330, 500, 900, 20000};

// Piece-square tables for positional evaluation (from white's perspective)
static const int8_t PST_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};

static const int8_t PST_KNIGHT[64] = {
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50
};

static const int8_t PST_BISHOP[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -20,-10,-10,-10,-10,-10,-10,-20
};

static const int8_t PST_ROOK[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0
};

static const int8_t PST_QUEEN[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5,  5,  5,  5,  0,-10,
    -5,  0,  5,  5,  5,  5,  0, -5,
     0,  0,  5,  5,  5,  5,  0, -5,
   -10,  5,  5,  5,  5,  5,  0,-10,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20
};

static const int8_t PST_KING_MG[64] = {
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -10,-20,-20,-20,-20,-20,-20,-10,
    20, 20,  0,  0,  0,  0, 20, 20,
    20, 30, 10,  0,  0, 10, 30, 20
};

static inline int sq_row(int sq) { return sq / 8; }
static inline int sq_col(int sq) { return sq % 8; }
static inline int sq_make(int r, int c) { return r * 8 + c; }
static inline bool sq_valid(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }
static inline int sq_mirror(int sq) { return (7 - sq / 8) * 8 + (sq % 8); }

// Check if square is attacked by the given side
static bool is_attacked(const me_state_t* st, int sq, uint8_t by_color) {
    int r = sq_row(sq), c = sq_col(sq);

    // Knight attacks
    static const int kn_dr[] = {-2,-2,-1,-1, 1, 1, 2, 2};
    static const int kn_dc[] = {-1, 1,-2, 2,-2, 2,-1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_dr[i], nc = c + kn_dc[i];
        if (sq_valid(nr, nc)) {
            uint8_t p = st->board[sq_make(nr, nc)];
            if (p != ME_EMPTY && ME_COLOR(p) == by_color && ME_TYPE(p) == ME_KNIGHT)
                return true;
        }
    }

    // King attacks
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (sq_valid(nr, nc)) {
                uint8_t p = st->board[sq_make(nr, nc)];
                if (p != ME_EMPTY && ME_COLOR(p) == by_color && ME_TYPE(p) == ME_KING)
                    return true;
            }
        }

    // Pawn attacks
    int pawn_dr = (by_color == ME_WHITE) ? 1 : -1; // pawns attack "forward"
    for (int dc = -1; dc <= 1; dc += 2) {
        int nr = r + pawn_dr, nc = c + dc;
        if (sq_valid(nr, nc)) {
            uint8_t p = st->board[sq_make(nr, nc)];
            if (p != ME_EMPTY && ME_COLOR(p) == by_color && ME_TYPE(p) == ME_PAWN)
                return true;
        }
    }

    // Sliding pieces: bishop/queen (diagonals) and rook/queen (straights)
    static const int diag_dr[] = {-1,-1, 1, 1};
    static const int diag_dc[] = {-1, 1,-1, 1};
    for (int d = 0; d < 4; d++) {
        for (int dist = 1; dist < 8; dist++) {
            int nr = r + diag_dr[d] * dist;
            int nc = c + diag_dc[d] * dist;
            if (!sq_valid(nr, nc)) break;
            uint8_t p = st->board[sq_make(nr, nc)];
            if (p != ME_EMPTY) {
                if (ME_COLOR(p) == by_color &&
                    (ME_TYPE(p) == ME_BISHOP || ME_TYPE(p) == ME_QUEEN))
                    return true;
                break;
            }
        }
    }

    static const int str_dr[] = {-1, 1, 0, 0};
    static const int str_dc[] = { 0, 0,-1, 1};
    for (int d = 0; d < 4; d++) {
        for (int dist = 1; dist < 8; dist++) {
            int nr = r + str_dr[d] * dist;
            int nc = c + str_dc[d] * dist;
            if (!sq_valid(nr, nc)) break;
            uint8_t p = st->board[sq_make(nr, nc)];
            if (p != ME_EMPTY) {
                if (ME_COLOR(p) == by_color &&
                    (ME_TYPE(p) == ME_ROOK || ME_TYPE(p) == ME_QUEEN))
                    return true;
                break;
            }
        }
    }

    return false;
}

bool me_in_check(const me_state_t* st) {
    uint8_t king_sq = (st->side == ME_WHITE) ? st->wking : st->bking;
    uint8_t opp = (st->side == ME_WHITE) ? ME_BLACK : ME_WHITE;
    return is_attacked(st, king_sq, opp);
}

// Generate pseudo-legal moves (may leave king in check)
static int gen_pseudo_moves(const me_state_t* st, me_move_t* moves) {
    int n = 0;
    uint8_t side = st->side;
    uint8_t opp = (side == ME_WHITE) ? ME_BLACK : ME_WHITE;

    for (int sq = 0; sq < 64; sq++) {
        uint8_t p = st->board[sq];
        if (p == ME_EMPTY || ME_COLOR(p) != side) continue;
        int r = sq_row(sq), c = sq_col(sq);
        uint8_t type = ME_TYPE(p);

        if (type == ME_PAWN) {
            int dir = (side == ME_WHITE) ? -1 : 1;
            int start_row = (side == ME_WHITE) ? 6 : 1;
            int promo_row = (side == ME_WHITE) ? 0 : 7;

            // Single push
            int nr = r + dir;
            if (sq_valid(nr, c) && st->board[sq_make(nr, c)] == ME_EMPTY) {
                if (nr == promo_row) {
                    // Promotions
                    uint8_t promos[] = {ME_QUEEN, ME_ROOK, ME_BISHOP, ME_KNIGHT};
                    for (int pi = 0; pi < 4; pi++) {
                        moves[n].from = sq;
                        moves[n].to = sq_make(nr, c);
                        moves[n].promotion = promos[pi];
                        moves[n].flags = ME_FLAG_PROMOTION;
                        n++;
                    }
                } else {
                    moves[n].from = sq;
                    moves[n].to = sq_make(nr, c);
                    moves[n].promotion = 0;
                    moves[n].flags = 0;
                    n++;
                    // Double push
                    if (r == start_row) {
                        int nr2 = r + 2 * dir;
                        if (st->board[sq_make(nr2, c)] == ME_EMPTY) {
                            moves[n].from = sq;
                            moves[n].to = sq_make(nr2, c);
                            moves[n].promotion = 0;
                            moves[n].flags = 0;
                            n++;
                        }
                    }
                }
            }
            // Captures
            for (int dc = -1; dc <= 1; dc += 2) {
                int nc = c + dc;
                if (!sq_valid(nr, nc)) continue;
                int to = sq_make(nr, nc);
                uint8_t cap = st->board[to];
                if (cap != ME_EMPTY && ME_COLOR(cap) == opp) {
                    if (nr == promo_row) {
                        uint8_t promos[] = {ME_QUEEN, ME_ROOK, ME_BISHOP, ME_KNIGHT};
                        for (int pi = 0; pi < 4; pi++) {
                            moves[n].from = sq;
                            moves[n].to = to;
                            moves[n].promotion = promos[pi];
                            moves[n].flags = ME_FLAG_CAPTURE | ME_FLAG_PROMOTION;
                            n++;
                        }
                    } else {
                        moves[n].from = sq;
                        moves[n].to = to;
                        moves[n].promotion = 0;
                        moves[n].flags = ME_FLAG_CAPTURE;
                        n++;
                    }
                }
                // En passant
                if (to == st->ep_square && st->ep_square >= 0) {
                    moves[n].from = sq;
                    moves[n].to = to;
                    moves[n].promotion = 0;
                    moves[n].flags = ME_FLAG_CAPTURE | ME_FLAG_EP;
                    n++;
                }
            }
        } else if (type == ME_KNIGHT) {
            static const int kn_dr[] = {-2,-2,-1,-1, 1, 1, 2, 2};
            static const int kn_dc[] = {-1, 1,-2, 2,-2, 2,-1, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + kn_dr[i], nc = c + kn_dc[i];
                if (!sq_valid(nr, nc)) continue;
                int to = sq_make(nr, nc);
                uint8_t cap = st->board[to];
                if (cap != ME_EMPTY && ME_COLOR(cap) == side) continue;
                moves[n].from = sq;
                moves[n].to = to;
                moves[n].promotion = 0;
                moves[n].flags = (cap != ME_EMPTY) ? ME_FLAG_CAPTURE : 0;
                n++;
            }
        } else if (type == ME_KING) {
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = r + dr, nc = c + dc;
                    if (!sq_valid(nr, nc)) continue;
                    int to = sq_make(nr, nc);
                    uint8_t cap = st->board[to];
                    if (cap != ME_EMPTY && ME_COLOR(cap) == side) continue;
                    moves[n].from = sq;
                    moves[n].to = to;
                    moves[n].promotion = 0;
                    moves[n].flags = (cap != ME_EMPTY) ? ME_FLAG_CAPTURE : 0;
                    n++;
                }
            // Castling
            if (side == ME_WHITE) {
                if ((st->castle & ME_CASTLE_WK) && sq == 60 &&
                    st->board[61] == ME_EMPTY && st->board[62] == ME_EMPTY &&
                    !is_attacked(st, 60, opp) && !is_attacked(st, 61, opp) && !is_attacked(st, 62, opp)) {
                    moves[n].from = 60; moves[n].to = 62;
                    moves[n].promotion = 0; moves[n].flags = ME_FLAG_CASTLE;
                    n++;
                }
                if ((st->castle & ME_CASTLE_WQ) && sq == 60 &&
                    st->board[59] == ME_EMPTY && st->board[58] == ME_EMPTY && st->board[57] == ME_EMPTY &&
                    !is_attacked(st, 60, opp) && !is_attacked(st, 59, opp) && !is_attacked(st, 58, opp)) {
                    moves[n].from = 60; moves[n].to = 58;
                    moves[n].promotion = 0; moves[n].flags = ME_FLAG_CASTLE;
                    n++;
                }
            } else {
                if ((st->castle & ME_CASTLE_BK) && sq == 4 &&
                    st->board[5] == ME_EMPTY && st->board[6] == ME_EMPTY &&
                    !is_attacked(st, 4, opp) && !is_attacked(st, 5, opp) && !is_attacked(st, 6, opp)) {
                    moves[n].from = 4; moves[n].to = 6;
                    moves[n].promotion = 0; moves[n].flags = ME_FLAG_CASTLE;
                    n++;
                }
                if ((st->castle & ME_CASTLE_BQ) && sq == 4 &&
                    st->board[3] == ME_EMPTY && st->board[2] == ME_EMPTY && st->board[1] == ME_EMPTY &&
                    !is_attacked(st, 4, opp) && !is_attacked(st, 3, opp) && !is_attacked(st, 2, opp)) {
                    moves[n].from = 4; moves[n].to = 2;
                    moves[n].promotion = 0; moves[n].flags = ME_FLAG_CASTLE;
                    n++;
                }
            }
        } else {
            // Sliding pieces: bishop, rook, queen
            const int* drs;
            const int* dcs;
            int ndirs;
            static const int diag_dr[] = {-1,-1, 1, 1};
            static const int diag_dc[] = {-1, 1,-1, 1};
            static const int str_dr[]  = {-1, 1, 0, 0};
            static const int str_dc[]  = { 0, 0,-1, 1};
            static const int all_dr[]  = {-1,-1, 1, 1,-1, 1, 0, 0};
            static const int all_dc[]  = {-1, 1,-1, 1, 0, 0,-1, 1};

            if (type == ME_BISHOP)      { drs = diag_dr; dcs = diag_dc; ndirs = 4; }
            else if (type == ME_ROOK)   { drs = str_dr;  dcs = str_dc;  ndirs = 4; }
            else /* queen */            { drs = all_dr;  dcs = all_dc;  ndirs = 8; }

            for (int d = 0; d < ndirs; d++) {
                for (int dist = 1; dist < 8; dist++) {
                    int nr = r + drs[d] * dist;
                    int nc = c + dcs[d] * dist;
                    if (!sq_valid(nr, nc)) break;
                    int to = sq_make(nr, nc);
                    uint8_t cap = st->board[to];
                    if (cap != ME_EMPTY && ME_COLOR(cap) == side) break;
                    moves[n].from = sq;
                    moves[n].to = to;
                    moves[n].promotion = 0;
                    moves[n].flags = (cap != ME_EMPTY) ? ME_FLAG_CAPTURE : 0;
                    n++;
                    if (cap != ME_EMPTY) break; // can't go past capture
                }
            }
        }
    }
    return n;
}

void me_init(me_state_t* st) {
    me_load_fen(st, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

bool me_load_fen(me_state_t* st, const char* fen) {
    if (!st || !fen) return false;
    memset(st->board, ME_EMPTY, 64);
    st->castle = 0;
    st->ep_square = -1;
    st->halfmove = 0;
    st->fullmove = 1;
    st->wking = 0;
    st->bking = 0;

    int sq = 0;
    const char* p = fen;

    // Board
    while (*p && *p != ' ' && sq < 64) {
        char ch = *p++;
        if (ch == '/') continue;
        if (ch >= '1' && ch <= '8') {
            sq += ch - '0';
            continue;
        }
        uint8_t color = (ch >= 'a' && ch <= 'z') ? ME_BLACK : ME_WHITE;
        char lower = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
        uint8_t type = ME_EMPTY;
        switch (lower) {
            case 'p': type = ME_PAWN; break;
            case 'n': type = ME_KNIGHT; break;
            case 'b': type = ME_BISHOP; break;
            case 'r': type = ME_ROOK; break;
            case 'q': type = ME_QUEEN; break;
            case 'k': type = ME_KING; break;
            default: return false;
        }
        st->board[sq] = ME_PIECE(color, type);
        if (type == ME_KING) {
            if (color == ME_WHITE) st->wking = sq;
            else st->bking = sq;
        }
        sq++;
    }

    // Side to move
    while (*p == ' ') p++;
    st->side = (*p == 'b') ? ME_BLACK : ME_WHITE;
    if (*p) p++;

    // Castling
    while (*p == ' ') p++;
    while (*p && *p != ' ') {
        switch (*p) {
            case 'K': st->castle |= ME_CASTLE_WK; break;
            case 'Q': st->castle |= ME_CASTLE_WQ; break;
            case 'k': st->castle |= ME_CASTLE_BK; break;
            case 'q': st->castle |= ME_CASTLE_BQ; break;
        }
        p++;
    }

    // En passant
    while (*p == ' ') p++;
    if (*p == '-') {
        st->ep_square = -1;
        p++;
    } else if (*p >= 'a' && *p <= 'h') {
        int file = *p - 'a'; p++;
        int rank = 8 - (*p - '0'); p++;
        st->ep_square = sq_make(rank, file);
    }

    // Halfmove clock
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        st->halfmove = atoi(p);
        while (*p && *p != ' ') p++;
    }

    // Fullmove number
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        st->fullmove = atoi(p);
    }

    return true;
}

char* me_board_fen(const me_state_t* st, char* buf, int buf_size) {
    int idx = 0;
    for (int r = 0; r < 8; r++) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            uint8_t p = st->board[r * 8 + c];
            if (p == ME_EMPTY) {
                empty++;
            } else {
                if (empty > 0 && idx < buf_size - 1) {
                    buf[idx++] = '0' + empty;
                    empty = 0;
                }
                char ch = '?';
                switch (ME_TYPE(p)) {
                    case ME_PAWN:   ch = 'p'; break;
                    case ME_KNIGHT: ch = 'n'; break;
                    case ME_BISHOP: ch = 'b'; break;
                    case ME_ROOK:   ch = 'r'; break;
                    case ME_QUEEN:  ch = 'q'; break;
                    case ME_KING:   ch = 'k'; break;
                }
                if (ME_IS_WHITE(p)) ch -= 32;
                if (idx < buf_size - 1) buf[idx++] = ch;
            }
        }
        if (empty > 0 && idx < buf_size - 1) buf[idx++] = '0' + empty;
        if (r < 7 && idx < buf_size - 1) buf[idx++] = '/';
    }
    buf[idx] = '\0';
    return buf;
}

char* me_full_fen(const me_state_t* st, char* buf, int buf_size) {
    char board_buf[80];
    me_board_fen(st, board_buf, sizeof(board_buf));

    char castle_buf[5] = "-";
    {
        int ci = 0;
        if (st->castle & ME_CASTLE_WK) castle_buf[ci++] = 'K';
        if (st->castle & ME_CASTLE_WQ) castle_buf[ci++] = 'Q';
        if (st->castle & ME_CASTLE_BK) castle_buf[ci++] = 'k';
        if (st->castle & ME_CASTLE_BQ) castle_buf[ci++] = 'q';
        if (ci == 0) castle_buf[ci++] = '-';
        castle_buf[ci] = '\0';
    }

    char ep_buf[3] = "-";
    if (st->ep_square >= 0) {
        me_sq_to_alg(st->ep_square, ep_buf);
    }

    snprintf(buf, buf_size, "%s %c %s %s %d %d",
             board_buf,
             (st->side == ME_WHITE) ? 'w' : 'b',
             castle_buf, ep_buf,
             st->halfmove, st->fullmove);
    return buf;
}

bool me_make_move(me_state_t* st, me_move_t move) {
    uint8_t piece = st->board[move.from];
    if (piece == ME_EMPTY) return false;

    uint8_t side = ME_COLOR(piece);
    uint8_t opp = (side == ME_WHITE) ? ME_BLACK : ME_WHITE;
    uint8_t type = ME_TYPE(piece);
    uint8_t captured = st->board[move.to];

    // Save state for legality check
    me_state_t save = *st;

    // En passant capture
    if (move.flags & ME_FLAG_EP) {
        int ep_pawn_sq = (side == ME_WHITE) ? move.to + 8 : move.to - 8;
        st->board[ep_pawn_sq] = ME_EMPTY;
        captured = ME_PIECE(opp, ME_PAWN);
    }

    // Move piece
    st->board[move.to] = piece;
    st->board[move.from] = ME_EMPTY;

    // Promotion
    if (move.flags & ME_FLAG_PROMOTION) {
        st->board[move.to] = ME_PIECE(side, move.promotion);
    }

    // Castling — move the rook
    if (move.flags & ME_FLAG_CASTLE) {
        if (move.to == 62) { // White kingside
            st->board[61] = st->board[63]; st->board[63] = ME_EMPTY;
        } else if (move.to == 58) { // White queenside
            st->board[59] = st->board[56]; st->board[56] = ME_EMPTY;
        } else if (move.to == 6) { // Black kingside
            st->board[5] = st->board[7]; st->board[7] = ME_EMPTY;
        } else if (move.to == 2) { // Black queenside
            st->board[3] = st->board[0]; st->board[0] = ME_EMPTY;
        }
    }

    // Update king position
    if (type == ME_KING) {
        if (side == ME_WHITE) st->wking = move.to;
        else st->bking = move.to;
    }

    // Update castling rights
    if (type == ME_KING) {
        if (side == ME_WHITE) st->castle &= ~(ME_CASTLE_WK | ME_CASTLE_WQ);
        else st->castle &= ~(ME_CASTLE_BK | ME_CASTLE_BQ);
    }
    if (type == ME_ROOK) {
        if (move.from == 63) st->castle &= ~ME_CASTLE_WK;
        if (move.from == 56) st->castle &= ~ME_CASTLE_WQ;
        if (move.from == 7)  st->castle &= ~ME_CASTLE_BK;
        if (move.from == 0)  st->castle &= ~ME_CASTLE_BQ;
    }
    // Also update if rook is captured
    if (move.to == 63) st->castle &= ~ME_CASTLE_WK;
    if (move.to == 56) st->castle &= ~ME_CASTLE_WQ;
    if (move.to == 7)  st->castle &= ~ME_CASTLE_BK;
    if (move.to == 0)  st->castle &= ~ME_CASTLE_BQ;

    // En passant square
    if (type == ME_PAWN && abs(sq_row(move.from) - sq_row(move.to)) == 2) {
        st->ep_square = (move.from + move.to) / 2;
    } else {
        st->ep_square = -1;
    }

    // Halfmove clock
    if (type == ME_PAWN || captured != ME_EMPTY) {
        st->halfmove = 0;
    } else {
        st->halfmove++;
    }

    // Switch side
    if (side == ME_BLACK) st->fullmove++;
    st->side = opp;

    // Check legality: own king must not be in check
    uint8_t king_sq = (side == ME_WHITE) ? st->wking : st->bking;
    if (is_attacked(st, king_sq, opp)) {
        // Illegal — restore state
        *st = save;
        return false;
    }

    return true;
}

int me_generate_moves(const me_state_t* st, me_move_t* moves) {
    me_move_t pseudo[ME_MAX_MOVES];
    int np = gen_pseudo_moves(st, pseudo);
    int n = 0;

    for (int i = 0; i < np; i++) {
        me_state_t copy = *st;
        if (me_make_move(&copy, pseudo[i])) {
            moves[n++] = pseudo[i];
        }
    }
    return n;
}

bool me_is_legal(const me_state_t* st, me_move_t move) {
    me_state_t copy = *st;
    return me_make_move(&copy, move);
}

bool me_is_checkmate(const me_state_t* st) {
    if (!me_in_check(st)) return false;
    me_move_t moves[ME_MAX_MOVES];
    return me_generate_moves(st, moves) == 0;
}

bool me_is_stalemate(const me_state_t* st) {
    if (me_in_check(st)) return false;
    me_move_t moves[ME_MAX_MOVES];
    return me_generate_moves(st, moves) == 0;
}

// Evaluation
static int evaluate(const me_state_t* st) {
    int score = 0;
    for (int sq = 0; sq < 64; sq++) {
        uint8_t p = st->board[sq];
        if (p == ME_EMPTY) continue;
        int type = ME_TYPE(p);
        int val = PIECE_VAL[type];
        int pst = 0;
        int idx = ME_IS_WHITE(p) ? sq : sq_mirror(sq);
        switch (type) {
            case ME_PAWN:   pst = PST_PAWN[idx]; break;
            case ME_KNIGHT: pst = PST_KNIGHT[idx]; break;
            case ME_BISHOP: pst = PST_BISHOP[idx]; break;
            case ME_ROOK:   pst = PST_ROOK[idx]; break;
            case ME_QUEEN:  pst = PST_QUEEN[idx]; break;
            case ME_KING:   pst = PST_KING_MG[idx]; break;
        }
        if (ME_IS_WHITE(p))
            score += val + pst;
        else
            score -= val + pst;
    }
    return (st->side == ME_WHITE) ? score : -score;
}

// Move ordering score (for alpha-beta efficiency)
static int move_order_score(const me_state_t* st, me_move_t m) {
    int score = 0;
    if (m.flags & ME_FLAG_CAPTURE) {
        int victim = PIECE_VAL[ME_TYPE(st->board[m.to])];
        int attacker = PIECE_VAL[ME_TYPE(st->board[m.from])];
        score += 10000 + victim - attacker / 100; // MVV-LVA
    }
    if (m.flags & ME_FLAG_PROMOTION) score += 9000;
    return score;
}

// Simple insertion sort for move ordering
static void sort_moves(const me_state_t* st, me_move_t* moves, int n) {
    int scores[ME_MAX_MOVES];
    for (int i = 0; i < n; i++)
        scores[i] = move_order_score(st, moves[i]);
    for (int i = 1; i < n; i++) {
        me_move_t key_m = moves[i];
        int key_s = scores[i];
        int j = i - 1;
        while (j >= 0 && scores[j] < key_s) {
            moves[j + 1] = moves[j];
            scores[j + 1] = scores[j];
            j--;
        }
        moves[j + 1] = key_m;
        scores[j + 1] = key_s;
    }
}

// Alpha-beta search with quiescence
// qs_depth limits how many extra capture-only plies are searched beyond the horizon;
// prevents unbounded recursion in positions with long capture chains.
static int quiesce(const me_state_t* st, int alpha, int beta, int qs_depth = 6) {
    // Node count + WDT feed + abort check
    s_node_count++;
    if (s_node_count >= NODE_LIMIT) { s_search_aborted = true; }
    if (s_search_aborted) return evaluate(st);
#ifndef SIMULATOR
    if ((s_node_count & (WDT_FEED_INTERVAL - 1)) == 0)
        esp_task_wdt_reset();
#endif

    int stand_pat = evaluate(st);
    if (stand_pat >= beta) return beta;
    if (alpha < stand_pat) alpha = stand_pat;

    // If we've exhausted the quiescence budget, return the static eval
    if (qs_depth <= 0) return alpha;

    me_move_t moves[ME_MAX_MOVES];
    int n = me_generate_moves(st, moves);
    sort_moves(st, moves, n);

    for (int i = 0; i < n; i++) {
        if (!(moves[i].flags & ME_FLAG_CAPTURE)) continue;
        if (s_search_aborted) break;
        me_state_t copy = *st;
        if (!me_make_move(&copy, moves[i])) continue;
        int score = -quiesce(&copy, -beta, -alpha, qs_depth - 1);
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

static int alpha_beta(const me_state_t* st, int depth, int alpha, int beta) {
    if (s_search_aborted) return evaluate(st);
    if (depth <= 0) return quiesce(st, alpha, beta);

    // Node count + WDT feed
    s_node_count++;
#ifndef SIMULATOR
    if ((s_node_count & (WDT_FEED_INTERVAL - 1)) == 0)
        esp_task_wdt_reset();
#endif

    me_move_t moves[ME_MAX_MOVES];
    int n = me_generate_moves(st, moves);

    if (n == 0) {
        if (me_in_check(st)) return -30000 + (10 - depth); // checkmate
        return 0; // stalemate
    }

    sort_moves(st, moves, n);

    for (int i = 0; i < n; i++) {
        if (s_search_aborted) break;
        me_state_t copy = *st;
        if (!me_make_move(&copy, moves[i])) continue;
        int score = -alpha_beta(&copy, depth - 1, -beta, -alpha);
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

me_move_t me_search(const me_state_t* st, int depth) {
    // Reset per-search counters
    s_node_count = 0;
    s_search_aborted = false;

    me_move_t moves[ME_MAX_MOVES];
    int n = me_generate_moves(st, moves);
    me_move_t best = {0, 0, 0, 0};
    int best_score = -100000;

    if (n == 0) return best;

    sort_moves(st, moves, n);

    for (int i = 0; i < n; i++) {
        if (s_search_aborted) break;
        me_state_t copy = *st;
        if (!me_make_move(&copy, moves[i])) continue;
        int score = -alpha_beta(&copy, depth - 1, -100000, -best_score);
        if (score > best_score) {
            best_score = score;
            best = moves[i];
        }
    }
    return best;
}

void me_abort_search(void) {
    s_search_aborted = true;
}

char* me_move_to_uci(me_move_t move, char* buf, int buf_size) {
    if (buf_size < 6) { buf[0] = '\0'; return buf; }
    buf[0] = 'a' + (move.from % 8);
    buf[1] = '8' - (move.from / 8);
    buf[2] = 'a' + (move.to % 8);
    buf[3] = '8' - (move.to / 8);
    if (move.flags & ME_FLAG_PROMOTION) {
        switch (move.promotion) {
            case ME_QUEEN:  buf[4] = 'q'; break;
            case ME_ROOK:   buf[4] = 'r'; break;
            case ME_BISHOP: buf[4] = 'b'; break;
            case ME_KNIGHT: buf[4] = 'n'; break;
            default:        buf[4] = 'q'; break;
        }
        buf[5] = '\0';
    } else {
        buf[4] = '\0';
    }
    return buf;
}

bool me_parse_uci(const me_state_t* st, const char* uci, me_move_t* move) {
    if (!uci || strlen(uci) < 4) return false;
    if (uci[0] < 'a' || uci[0] > 'h') return false;
    if (uci[1] < '1' || uci[1] > '8') return false;
    if (uci[2] < 'a' || uci[2] > 'h') return false;
    if (uci[3] < '1' || uci[3] > '8') return false;

    int from = me_alg_to_sq(uci[0], uci[1]);
    int to = me_alg_to_sq(uci[2], uci[3]);

    uint8_t promo = 0;
    if (uci[4]) {
        switch (uci[4]) {
            case 'q': promo = ME_QUEEN; break;
            case 'r': promo = ME_ROOK; break;
            case 'b': promo = ME_BISHOP; break;
            case 'n': promo = ME_KNIGHT; break;
        }
    }

    // Find matching legal move
    me_move_t moves[ME_MAX_MOVES];
    int n = me_generate_moves(st, moves);
    for (int i = 0; i < n; i++) {
        if (moves[i].from == from && moves[i].to == to) {
            if (promo && (moves[i].flags & ME_FLAG_PROMOTION)) {
                if (moves[i].promotion != promo) continue;
            }
            *move = moves[i];
            return true;
        }
    }
    return false;
}
