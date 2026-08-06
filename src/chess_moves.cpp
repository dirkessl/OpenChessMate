#include "chess_moves.h"
#include "chess_utils.h"
#include "led_colors.h"
#include "move_history.h"
#include "web_logger.h"
#include "wifi_manager_esp32.h"
#include <Arduino.h>

ChessMoves::ChessMoves(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, MoveHistory* mh) : ChessGame(bd, ce, wm, mh) {}

void ChessMoves::begin() {
  webLog.println("=== Starting Chess Moves Mode ===");
  initializeBoard();
  if (moveHistory->hasLiveGame()) {
    if (!resumeLiveGameIfBoardMatches()) {
      gameOver = true;
      return;
    }
  } else {
    moveHistory->startGame(GAME_MODE_CHESS_MOVES);
    moveHistory->addFen(currentFEN());
  }
  waitForBoardSetup(board);
  sendUiState();
}

void ChessMoves::update() {
  boardDriver->readSensors();

  // Check for physical resign/draw gesture (both kings lifted)
  if (checkPhysicalResignOrDraw()) return;

  int fromRow, fromCol, toRow, toCol;
  if (tryPlayerMove(currentTurn, fromRow, fromCol, toRow, toCol)) {
    applyMove(fromRow, fromCol, toRow, toCol);
    updateGameStatus();
    wifiManager->updateBoardState(currentFEN(), ChessUtils::evaluatePosition(board));
    sendUiState();
  }

  boardDriver->updateSensorPrev();
}
