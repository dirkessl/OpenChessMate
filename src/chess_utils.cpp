#include "chess_utils.h"

#if defined(ESP32)
extern "C" {
#include "nvs_flash.h"
}
#endif

String ChessUtils::castlingRightsToString(uint8_t rights) {
  String s = "";
  if (rights & 0x01) s += "K";
  if (rights & 0x02) s += "Q";
  if (rights & 0x04) s += "k";
  if (rights & 0x08) s += "q";
  if (s.length() == 0) s = "-";
  return s;
}

String ChessUtils::boardToFEN(const char board[8][8], bool isWhiteTurn, const char* castlingRights) {
  String fen = "";

  // Board position - FEN expects rank 8 (black pieces) first, rank 1 (white pieces) last
  // Our array: row 0 = rank 8 (black), row 7 = rank 1 (White)
  // boardToFEN loops from row 0 to row 7, so rank 8 is output first (correct for FEN)
  for (int row = 0; row < 8; row++) {
    int emptyCount = 0;
    for (int col = 0; col < 8; col++) {
      if (board[row][col] == ' ') {
        emptyCount++;
      } else {
        if (emptyCount > 0) {
          fen += String(emptyCount);
          emptyCount = 0;
        }
        fen += board[row][col];
      }
    }
    if (emptyCount > 0) {
      fen += String(emptyCount);
    }
    if (row < 7)
      fen += "/";
  }

  // Active color
  fen += isWhiteTurn ? " w" : " b";

  // Castling availability
  fen += " ";
  fen += castlingRights;

  // En passant target square (simplified - assume none)
  fen += " -";

  // Halfmove clock (simplified)
  fen += " 0";

  // Fullmove number (simplified)
  fen += " 1";

  return fen;
}

void ChessUtils::fenToBoard(String fen, char board[8][8], bool* isWhiteTurn, String* castlingRights) {
  // Parse FEN string and update board state
  // FEN format: "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

  // Split FEN into parts
  int firstSpace = fen.indexOf(' ');
  String boardPart = (firstSpace > 0) ? fen.substring(0, firstSpace) : fen;
  String remainingParts = (firstSpace > 0) ? fen.substring(firstSpace + 1) : "";

  // Clear board
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      board[row][col] = ' ';
    }
  }

  // Parse FEN ranks (rank 8 first, rank 1 last)
  // FEN: "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"
  // Our array: row 0 = rank 8 (black), row 7 = rank 1 (white)
  int row = 0; // Start with rank 8 (row 0 in our array)
  int col = 0;

  for (int i = 0; i < boardPart.length() && row < 8; i++) {
    char c = boardPart.charAt(i);

    if (c == '/') {
      // Next rank
      row++;
      col = 0;
    } else if (c >= '1' && c <= '8') {
      // Empty squares
      int emptyCount = c - '0';
      col += emptyCount;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      // Piece
      if (row >= 0 && row < 8 && col >= 0 && col < 8) {
        board[row][col] = c;
        col++;
      }
    }
  }

  // Parse active color if requested
  if (isWhiteTurn != nullptr && remainingParts.length() > 0) {
    int secondSpace = remainingParts.indexOf(' ');
    String activeColor = (secondSpace > 0) ? remainingParts.substring(0, secondSpace) : remainingParts;
    *isWhiteTurn = (activeColor == "w" || activeColor == "W");
    remainingParts = (secondSpace > 0) ? remainingParts.substring(secondSpace + 1) : "";
  }

  // Parse castling rights if requested
  if (castlingRights != nullptr && remainingParts.length() > 0) {
    int thirdSpace = remainingParts.indexOf(' ');
    *castlingRights = (thirdSpace > 0) ? remainingParts.substring(0, thirdSpace) : remainingParts;
  }
}

void ChessUtils::printBoard(const char board[8][8]) {
  Serial.println("====== BOARD STATE ======");
  Serial.println("  a b c d e f g h");
  for (int row = 0; row < 8; row++) {
    String rowStr = String(8 - row) + " ";
    for (int col = 0; col < 8; col++) {
      char piece = board[row][col];
      rowStr += (piece == ' ') ? String(". ") : String(piece) + " ";
    }
    rowStr += " " + String(8 - row);
    Serial.println(rowStr);
  }
  Serial.println("  a b c d e f g h");
  Serial.println("White pieces (uppercase): R N B Q K P");
  Serial.println("Black pieces (lowercase): r n b q k p");
  Serial.println("========================");
}

bool ChessUtils::ensureNvsInitialized() {
#if defined(ESP32)
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  return err == ESP_OK;
#else
  return false;
#endif
}
