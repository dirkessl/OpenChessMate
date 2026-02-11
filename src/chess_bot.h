#ifndef CHESS_BOT_H
#define CHESS_BOT_H

#include "chess_game.h"
#include "chess_utils.h"
#include "stockfish_api.h"
#include "stockfish_settings.h"

// ESP32 WiFi includes
#include <WiFi.h>
#include <WiFiClientSecure.h>
#define WiFiSSLClient WiFiClientSecure

class ChessBot : public ChessGame {
 private:
  BotConfig botConfig;

  // WiFi and API (Stockfish-specific)
  String makeStockfishRequest(String fen);
  bool parseStockfishResponse(String response, String& bestMove, float& evaluation);

  // Game flow (Stockfish-specific)
  void makeBotMove();

 protected:
  float currentEvaluation; // Evaluation (in pawns, positive = white advantage)

  // Move handling - shared with subclasses (e.g., ChessLichess)
  void executeOpponentMove(int fromRow, int fromCol, int toRow, int toCol, char promotion = ' ');
  void showOpponentMoveIndicator(int fromRow, int fromCol, int toRow, int toCol, bool isCapture, bool isEnPassant = false, int enPassantCapturedPawnRow = -1);
  void waitForOpponentMoveCompletion(int fromRow, int fromCol, int toRow, int toCol, bool isCapture, bool isEnPassant = false, int enPassantCapturedPawnRow = -1);

 public:
  ChessBot(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, BotConfig cfg);
  void begin() override;
  void update() override;

  // Get current evaluation
  float getEvaluation() { return currentEvaluation; }
};

#endif // CHESS_BOT_H
