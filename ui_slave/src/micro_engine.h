#pragma once
/*
 * micro_engine.h — Lightweight chess engine for practice mode
 *
 * Self-contained chess engine suitable for ESP32-S3.
 * Provides move generation, validation, alpha-beta search with
 * quiescence, and FEN import/export.
 *
 * Inspired by Micro-Max by H.G. Muller — a complete chess engine in ~100 lines of C.
 *   http://home.hccnet.nl/h.g.muller/max-src2.html
 *   https://chessprogramming.wikispaces.com/Micro-Max
 *
 * This is an independent, readable reimplementation sharing the same high-level
 * ideas (alpha-beta with quiescence, MVV-LVA move ordering, piece-square tables)
 * but written from scratch for clarity and ESP32 suitability.  No source code
 * from Micro-Max was copied.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Piece encoding (lower 3 bits = type, bit 3 = color: 0=white, 8=black)
#define ME_EMPTY   0
#define ME_PAWN    1
#define ME_KNIGHT  2
#define ME_BISHOP  3
#define ME_ROOK    4
#define ME_QUEEN   5
#define ME_KING    6
#define ME_WHITE   0
#define ME_BLACK   8
#define ME_COLOR_MASK 8
#define ME_TYPE_MASK  7

#define ME_PIECE(color, type) ((color) | (type))
#define ME_IS_WHITE(p)  (((p) & ME_COLOR_MASK) == ME_WHITE && (p) != ME_EMPTY)
#define ME_IS_BLACK(p)  (((p) & ME_COLOR_MASK) == ME_BLACK)
#define ME_TYPE(p)      ((p) & ME_TYPE_MASK)
#define ME_COLOR(p)     ((p) & ME_COLOR_MASK)

// Move representation
typedef struct {
    uint8_t from;       // source square (0-63, row*8+col)
    uint8_t to;         // destination square
    uint8_t promotion;  // promotion piece type (0 if none)
    uint8_t flags;      // ME_FLAG_* bits
} me_move_t;

#define ME_FLAG_CAPTURE   0x01
#define ME_FLAG_CASTLE    0x02
#define ME_FLAG_EP        0x04
#define ME_FLAG_PROMOTION 0x08
#define ME_FLAG_CHECK     0x10

#define ME_MAX_MOVES 128

// Game state
typedef struct {
    uint8_t board[64];      // row-major, [0]=a8, [63]=h1
    uint8_t side;           // ME_WHITE or ME_BLACK (side to move)
    uint8_t castle;         // castling rights: bits 0-3 = KQkq
    int8_t  ep_square;      // en passant target square (-1 if none)
    uint8_t halfmove;       // halfmove clock (50-move rule)
    uint16_t fullmove;      // fullmove number
    // King positions (cached for check detection)
    uint8_t wking;
    uint8_t bking;
} me_state_t;

// Castling right bits
#define ME_CASTLE_WK 0x01
#define ME_CASTLE_WQ 0x02
#define ME_CASTLE_BK 0x04
#define ME_CASTLE_BQ 0x08

/// Initialize the engine state to the standard starting position.
void me_init(me_state_t* st);

/// Load a FEN string into the state. Returns true on success.
bool me_load_fen(me_state_t* st, const char* fen);

/// Export the current position as FEN (board part only, no side/castling).
/// Writes to buf (must be >= 72 bytes). Returns buf.
char* me_board_fen(const me_state_t* st, char* buf, int buf_size);

/// Export full FEN string (with side, castling, ep, halfmove, fullmove).
/// Writes to buf (must be >= 100 bytes). Returns buf.
char* me_full_fen(const me_state_t* st, char* buf, int buf_size);

/// Generate all legal moves for the side to move.
/// Returns the number of moves written to `moves` (max ME_MAX_MOVES).
int me_generate_moves(const me_state_t* st, me_move_t* moves);

/// Check if a move is legal (validates and checks for leaving king in check).
bool me_is_legal(const me_state_t* st, me_move_t move);

/// Make a move on the board. Modifies state in place.
/// Returns true if the move was valid and applied.
bool me_make_move(me_state_t* st, me_move_t move);

/// Undo a move (requires the captured piece info).
/// For simplicity, prefer copying state before make_move instead.

/// Check if the current side's king is in check.
bool me_in_check(const me_state_t* st);

/// Check if the current side is checkmated.
bool me_is_checkmate(const me_state_t* st);

/// Check if the position is stalemate (no legal moves, not in check).
bool me_is_stalemate(const me_state_t* st);

/// Search for the best move using alpha-beta with the given depth.
/// Returns the best move found. If no legal move exists, returns a
/// move with from==to==0.
me_move_t me_search(const me_state_t* st, int depth);

/// Abort a running me_search() call as soon as possible.
/// Safe to call from any task/context — sets a volatile flag checked by the search.
void me_abort_search(void);

/// Convert a move to UCI string (e.g., "e2e4", "e7e8q").
/// Writes to buf (must be >= 6 bytes). Returns buf.
char* me_move_to_uci(me_move_t move, char* buf, int buf_size);

/// Parse a UCI move string into a me_move_t.
/// Returns true on success.
bool me_parse_uci(const me_state_t* st, const char* uci, me_move_t* move);

/// Convert square index (0-63) to algebraic notation (e.g., "e4").
static inline void me_sq_to_alg(uint8_t sq, char* buf) {
    buf[0] = 'a' + (sq % 8);
    buf[1] = '8' - (sq / 8);
    buf[2] = '\0';
}

/// Convert algebraic notation to square index.
static inline int me_alg_to_sq(char file, char rank) {
    return (8 - (rank - '0')) * 8 + (file - 'a');
}

#ifdef __cplusplus
}
#endif
