#include "chess_bot.h"
#include "chess_utils.h"
#include "stockfish_api.h"
#include "wifi_manager_esp32.h"
#include <Arduino.h>

ChessBot::ChessBot(BoardDriver* boardDriver, ChessEngine* chessEngine, WiFiManagerESP32* wifiManager, BotDifficulty diff, bool playerWhite) {
  _boardDriver = boardDriver;
  _chessEngine = chessEngine;
  _wifiManager = wifiManager;
  difficulty = diff;
  playerIsWhite = playerWhite;

  // Set difficulty settings
  switch (difficulty) {
    case BOT_EASY:
      settings = StockfishSettings::easy();
      break;
    case BOT_MEDIUM:
      settings = StockfishSettings::medium();
      break;
    case BOT_HARD:
      settings = StockfishSettings::hard();
      break;
    case BOT_EXPERT:
      settings = StockfishSettings::expert();
      break;
  }

  // Set initial turn: In chess, White always moves first
  // If player is White, player goes first (isWhiteTurn = true)
  // If player is Black, bot (White) goes first (isWhiteTurn = true)
  isWhiteTurn = true; // White always moves first in chess
  gameStarted = false;
  botThinking = false;
  wifiConnected = false;
  currentEvaluation = 0.0;
}

void ChessBot::begin() {
  Serial.println("=== Starting Chess Bot Mode ===");
  Serial.printf("Player plays: %s\n", playerIsWhite ? "White" : "Black");
  Serial.printf("Bot plays: %s\n", playerIsWhite ? "Black" : "White");
  Serial.print("Bot Difficulty: ");

  switch (difficulty) {
    case BOT_EASY:
      Serial.println("Easy (Depth 6)");
      break;
    case BOT_MEDIUM:
      Serial.println("Medium (Depth 10)");
      break;
    case BOT_HARD:
      Serial.println("Hard (Depth 14)");
      break;
    case BOT_EXPERT:
      Serial.println("Expert (Depth 16)");
      break;
  }

  _boardDriver->clearAllLEDs();
  _boardDriver->showLEDs();

  // Connect to WiFi
  if (_wifiManager->connectToWiFi(_wifiManager->getWiFiSSID(), _wifiManager->getWiFiPassword())) {
    Serial.println("WiFi connected! Bot mode ready.");
    wifiConnected = true;

    // Show success animation
    for (int i = 0; i < 3; i++) {
      _boardDriver->clearAllLEDs();
      _boardDriver->showLEDs();
      delay(200);

      // Light up entire board green briefly
      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          _boardDriver->setSquareLED(row, col, 0, 255, 0); // Green
        }
      }
      _boardDriver->showLEDs();
      delay(200);
    }

    initializeBoard();
    waitForBoardSetup();
  } else {
    Serial.println("Failed to connect to WiFi. Bot mode unavailable.");
    wifiConnected = false;

    // Show error animation (red flashing)
    for (int i = 0; i < 5; i++) {
      _boardDriver->clearAllLEDs();
      _boardDriver->showLEDs();
      delay(300);

      // Light up entire board red briefly
      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          _boardDriver->setSquareLED(row, col, 255, 0, 0); // Red
        }
      }
      _boardDriver->showLEDs();
      delay(300);
    }

    _boardDriver->clearAllLEDs();
    _boardDriver->showLEDs();
  }
}

void ChessBot::update() {
  if (!wifiConnected) {
    return; // No WiFi, can't play against bot
  }

  if (!gameStarted) {
    return; // Waiting for initial setup
  }

  if (botThinking) {
    showBotThinking();
    return;
  }

  _boardDriver->readSensors();

  // Detect piece movements (player's turn)
  bool isPlayerTurn = (playerIsWhite && isWhiteTurn) || (!playerIsWhite && !isWhiteTurn);
  if (isPlayerTurn) {
    static unsigned long lastTurnDebug = 0;
    if (millis() - lastTurnDebug > 5000) {
      Serial.printf("Your turn! Move a %s\n", playerIsWhite ? "WHITE piece (uppercase letters)" : "BLACK piece (lowercase letters)");
      lastTurnDebug = millis();
    }
    // Look for piece pickups and placements
    static int selectedRow = -1, selectedCol = -1;
    static bool piecePickedUp = false;

    // Check for piece pickup
    if (!piecePickedUp) {
      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          if (!_boardDriver->getSensorState(row, col) && _boardDriver->getSensorPrev(row, col)) {
            // Check what piece was picked up
            char piece = board[row][col];

            if (piece != ' ') {
              // Player should only be able to move their own pieces
              if ((playerIsWhite && piece >= 'A' && piece <= 'Z') || (!playerIsWhite && piece >= 'a' && piece <= 'z')) {
                selectedRow = row;
                selectedCol = col;
                piecePickedUp = true;

                Serial.printf("Player picked up %s piece '%c' at %c%d (array position %d,%d)\n", playerIsWhite ? "WHITE" : "BLACK", board[row][col], (char)('a' + col), 8 - row, row, col);

                // Show selected square
                _boardDriver->setSquareLED(row, col, 255, 0, 0); // Red

                // Show possible moves
                int moveCount = 0;
                int moves[27][2];
                _chessEngine->getPossibleMoves(board, row, col, moveCount, moves);

                for (int i = 0; i < moveCount; i++) {
                  _boardDriver->setSquareLED(moves[i][0], moves[i][1], 0, 0, 0, 255); // Bright white using W channel
                }
                _boardDriver->showLEDs();
                break;
              } else {
                // Player tried to pick up the wrong color piece
                Serial.printf("ERROR: You tried to pick up %s piece '%c' at %c%d. You can only move %s pieces!\n", (piece >= 'A' && piece <= 'Z') ? "WHITE" : "BLACK", piece, (char)('a' + col), 8 - row, playerIsWhite ? "WHITE" : "BLACK");

                // Flash red to indicate error
                _boardDriver->blinkSquare(row, col, 255, 0, 0); // Blink red
              }
            }
          }
        }
        if (piecePickedUp)
          break;
      }
    }

    // Check for piece placement
    if (piecePickedUp) {
      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          if (_boardDriver->getSensorState(row, col) && !_boardDriver->getSensorPrev(row, col)) {
            // Check if piece was returned to its original position
            if (row == selectedRow && col == selectedCol) {
              // Piece returned to original position - cancel selection
              Serial.println("Piece returned to original position. Selection cancelled.");
              piecePickedUp = false;
              selectedRow = selectedCol = -1;

              // Clear all indicators
              _boardDriver->clearAllLEDs();
              _boardDriver->showLEDs();
              break;
            }

            // Piece placed somewhere else - validate move
            int moveCount = 0;
            int moves[27][2];
            _chessEngine->getPossibleMoves(board, selectedRow, selectedCol, moveCount, moves);

            bool validMove = false;
            for (int i = 0; i < moveCount; i++) {
              if (moves[i][0] == row && moves[i][1] == col) {
                validMove = true;
                break;
              }
            }

            if (validMove) {
              char piece = board[selectedRow][selectedCol];

              // Complete LED animations BEFORE API request
              processPlayerMove(selectedRow, selectedCol, row, col, piece);

              // Flash confirmation on destination square for player move
              confirmSquareCompletion(row, col);

              piecePickedUp = false;
              selectedRow = selectedCol = -1;

              // Switch to bot's turn
              // If player is White, bot is Black (isWhiteTurn = false)
              // If player is Black, bot is White (isWhiteTurn = true)
              isWhiteTurn = !playerIsWhite;
              botThinking = true;

              Serial.println("Player move completed. Bot thinking...");

              // Start bot move calculation
              makeBotMove();
            } else {
              Serial.println("Invalid move! Please try again.");
              _boardDriver->blinkSquare(row, col, 255, 0, 0); // Blink red for invalid move

              // Restore move indicators - piece is still selected
              _boardDriver->clearAllLEDs();

              // Show selected square again
              _boardDriver->setSquareLED(selectedRow, selectedCol, 255, 0, 0); // Red

              // Show possible moves again
              int moveCount = 0;
              int moves[27][2];
              _chessEngine->getPossibleMoves(board, selectedRow, selectedCol, moveCount, moves);

              for (int i = 0; i < moveCount; i++) {
                _boardDriver->setSquareLED(moves[i][0], moves[i][1], 0, 0, 0, 255); // Bright white using W channel
              }
              _boardDriver->showLEDs();

              Serial.println("Piece is still selected. Place it on a valid move or return it to its original position.");
            }
            break;
          }
        }
      }
    }
  } else {
    // Bot's turn - if player is Black, bot (White) goes first
    if (!botThinking && !playerIsWhite && isWhiteTurn) {
      // Bot plays White and it's White's turn
      botThinking = true;
      makeBotMove();
    } else if (!botThinking && playerIsWhite && !isWhiteTurn) {
      // Bot plays Black and it's Black's turn
      botThinking = true;
      makeBotMove();
    }
  }

  _boardDriver->updateSensorPrev();
}

String ChessBot::makeStockfishRequest(String fen) {
  WiFiSSLClient client;
#if defined(ESP32) || defined(ESP8266)
  // ESP32/ESP8266: Set insecure mode for SSL (or add proper certificate validation)
  client.setInsecure();
#endif
  String path = StockfishAPI::buildRequestURL(fen, settings.depth);
  // Retry logic
  for (int attempt = 1; attempt <= settings.maxRetries; attempt++) {
    Serial.println("Attempt: " + String(attempt) + "/" + String(settings.maxRetries));
    if (client.connect(STOCKFISH_API_URL, STOCKFISH_API_PORT)) {
      client.println("GET " + path + " HTTP/1.1");
      client.println("Host: " STOCKFISH_API_URL);
      client.println("Connection: close");
      client.println();
      // Wait for response
      unsigned long startTime = millis();
      String response = "";
      bool gotResponse = false;
      while (client.connected() && (millis() - startTime < settings.timeoutMs)) {
        if (client.available()) {
          response = client.readString();
          gotResponse = true;
          break;
        }
        delay(10);
      }
      client.stop();

      if (gotResponse && response.length() > 0)
        return response;
    }

    Serial.println("API request timeout or empty response");
    if (attempt < settings.maxRetries) {
      Serial.println("Retrying...");
      delay(500);
    }
  }

  Serial.println("All API request attempts failed");
  return "";
}

bool ChessBot::parseStockfishResponse(String response, String& bestMove, float& evaluation) {
  StockfishResponse stockfishResp;
  if (!StockfishAPI::parseResponse(response, stockfishResp)) {
    Serial.printf("Failed to parse Stockfish response: %s\n", stockfishResp.errorMessage.c_str());
    return false;
  }
  bestMove = stockfishResp.bestMove;
  if (stockfishResp.hasMate) {
    Serial.printf("Mate in %d moves\n", stockfishResp.mateInMoves);
    // Convert mate to a large evaluation (positive or negative based on direction)
    evaluation = stockfishResp.mateInMoves > 0 ? 100.0f : -100.0f;
  } else {
    // Regular evaluation (already in pawns from API)
    evaluation = stockfishResp.evaluation;
  }
  return true;
}

void ChessBot::makeBotMove() {
  Serial.println("=== BOT MOVE CALCULATION ===");
  showBotThinking();
  String fen = ChessUtils::boardToFEN(board, isWhiteTurn);
  Serial.println("Sending FEN to Stockfish: " + fen);
  botThinking = false;
  String bestMove;
  String response = makeStockfishRequest(fen);
  if (parseStockfishResponse(response, bestMove, currentEvaluation)) {
    Serial.println("=== STOCKFISH EVALUATION ===");
    Serial.printf("%s advantage: %.2f pawns\n", currentEvaluation > 0 ? "White" : "Black", currentEvaluation);

    int fromRow, fromCol, toRow, toCol;
    String validationError;
    if (StockfishAPI::validateUCIMove(bestMove, validationError, fromRow, fromCol, toRow, toCol)) {
      Serial.printf("Move string: %s Parsed: %c%c -> %c%c | Array coords: (%d,%d) to (%d,%d)", bestMove.c_str(), bestMove[0], bestMove[1], bestMove[2], bestMove[3], fromRow, fromCol, toRow, toCol);
      if (bestMove.length() >= 5)
        Serial.printf(" Promotion to: %c", bestMove[4]);
      Serial.println("\n============================");
      // Verify the move is from the correct color piece
      char piece = board[fromRow][fromCol];
      bool botPlaysWhite = !playerIsWhite;
      bool isBotPiece = (botPlaysWhite && piece >= 'A' && piece <= 'Z') || (!botPlaysWhite && piece >= 'a' && piece <= 'z');
      if (!isBotPiece) {
        Serial.printf("ERROR: Bot tried to move a %s piece, but bot plays %s. Piece at source: %c\n", (piece >= 'A' && piece <= 'Z') ? "WHITE" : "BLACK", botPlaysWhite ? "WHITE" : "BLACK", piece);
        return;
      }
      if (piece == ' ') {
        Serial.println("ERROR: Bot tried to move from an empty square!");
        return;
      }
      // Execute the validated bot move
      executeBotMove(fromRow, fromCol, toRow, toCol);
      isWhiteTurn = playerIsWhite;
      Serial.println("Bot move completed. Your turn!");
    } else {
      Serial.printf("Failed to parse bot move - %s\n", validationError.c_str());
    }
  }
}

void ChessBot::executeBotMove(int fromRow, int fromCol, int toRow, int toCol) {
  char piece = board[fromRow][fromCol];
  char capturedPiece = board[toRow][toCol];

  // Update board state
  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';

  Serial.printf("Bot wants to move piece from %c%d to %c%d\nPlease make this move on the physical board...\n", (char)('a' + fromCol), 8 - fromRow, (char)('a' + toCol), 8 - toRow);

  // Show the move that needs to be made
  showBotMoveIndicator(fromRow, fromCol, toRow, toCol);

  // Wait for user to physically complete the bot's move
  waitForBotMoveCompletion(fromRow, fromCol, toRow, toCol);

  if (capturedPiece != ' ') {
    Serial.printf("Piece captured: %c\n", capturedPiece);
    _boardDriver->captureAnimation();
  }

  // Flash confirmation on the destination square
  confirmSquareCompletion(toRow, toCol);

  Serial.println("Bot move completed. Your turn!");
}

void ChessBot::showBotThinking() {
  int green = isWhiteTurn ? 255 : 0;
  _boardDriver->clearAllLEDs();
  // Corner LEDs blue if bot is Black, green/blue if bot is White
  _boardDriver->setSquareLED(0, 0, 0, green, 255);
  _boardDriver->setSquareLED(0, 7, 0, green, 255);
  _boardDriver->setSquareLED(7, 0, 0, green, 255);
  _boardDriver->setSquareLED(7, 7, 0, green, 255);
  _boardDriver->showLEDs();
}

void ChessBot::initializeBoard() {
  // Copy initial board state
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      board[row][col] = INITIAL_BOARD[row][col];
    }
  }
}

void ChessBot::waitForBoardSetup() {
  Serial.println("Please set up the chess board in starting position...");

  while (!_boardDriver->checkInitialBoard(INITIAL_BOARD)) {
    _boardDriver->readSensors();
    _boardDriver->updateSetupDisplay(INITIAL_BOARD);
    _boardDriver->showLEDs();
    delay(100);
  }

  Serial.println("Board setup complete! Game starting...");
  _boardDriver->fireworkAnimation();
  gameStarted = true;

  // Show initial board state
  ChessUtils::printBoard(board);
}

void ChessBot::processPlayerMove(int fromRow, int fromCol, int toRow, int toCol, char piece) {
  char capturedPiece = board[toRow][toCol];

  // Update board state
  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';

  Serial.printf("Player moved %c from %c%d to %c%d\n", piece, (char)('a' + fromCol), 8 - fromRow, (char)('a' + toCol), 8 - toRow);

  if (capturedPiece != ' ') {
    Serial.printf("Captured %c\n", capturedPiece);
    _boardDriver->captureAnimation();
  }

  // Check for pawn promotion
  if (_chessEngine->isPawnPromotion(piece, toRow)) {
    char promotedPiece = _chessEngine->getPromotedPiece(piece);
    board[toRow][toCol] = promotedPiece;
    Serial.printf("Pawn promoted to %c\n", promotedPiece);
    _boardDriver->promotionAnimation(toCol);
  }
}

void ChessBot::showBotMoveIndicator(int fromRow, int fromCol, int toRow, int toCol) {
  // Clear all LEDs first
  _boardDriver->clearAllLEDs();

  // Show source square flashing (where to pick up from)
  // Use white channel (W) for much brighter display
  _boardDriver->setSquareLED(fromRow, fromCol, 0, 0, 0, 255); // Bright white using W channel

  // Show destination square solid (where to place)
  _boardDriver->setSquareLED(toRow, toCol, 0, 0, 0, 255); // Bright white using W channel

  _boardDriver->showLEDs();
}

void ChessBot::waitForBotMoveCompletion(int fromRow, int fromCol, int toRow, int toCol) {
  bool piecePickedUp = false;
  bool moveCompleted = false;
  static unsigned long lastBlink = 0;
  static bool blinkState = false;

  Serial.println("Waiting for you to complete the bot's move...");

  while (!moveCompleted) {
    _boardDriver->readSensors();

    // Blink the source square
    if (millis() - lastBlink > 500) {
      _boardDriver->clearAllLEDs();
      if (blinkState && !piecePickedUp) {
        _boardDriver->setSquareLED(fromRow, fromCol, 255, 255, 255, 255); // Flash source - bright white using W channel
      }
      _boardDriver->setSquareLED(toRow, toCol, 0, 0, 0, 255); // Always show destination - bright white using W channel
      _boardDriver->showLEDs();

      blinkState = !blinkState;
      lastBlink = millis();
    }

    // Check if piece was picked up from source
    if (!piecePickedUp && !_boardDriver->getSensorState(fromRow, fromCol)) {
      piecePickedUp = true;
      Serial.println("Bot piece picked up, now place it on the destination...");

      // Stop blinking source, just show destination
      _boardDriver->clearAllLEDs();
      _boardDriver->setSquareLED(toRow, toCol, 0, 0, 0, 255); // Bright white using W channel
      _boardDriver->showLEDs();
    }

    // Check if piece was placed on destination
    if (piecePickedUp && _boardDriver->getSensorState(toRow, toCol)) {
      moveCompleted = true;
      Serial.println("Bot move completed on physical board!");
    }

    delay(50);
    _boardDriver->updateSensorPrev();
  }
}

void ChessBot::confirmSquareCompletion(int row, int col) {
  _boardDriver->blinkSquare(row, col, 0, 255, 0, 3); // Green flash
}

void ChessBot::setDifficulty(BotDifficulty diff) {
  difficulty = diff;
  switch (difficulty) {
    case BOT_EASY:
      settings = StockfishSettings::easy();
      break;
    case BOT_MEDIUM:
      settings = StockfishSettings::medium();
      break;
    case BOT_HARD:
      settings = StockfishSettings::hard();
      break;
    case BOT_EXPERT:
      settings = StockfishSettings::expert();
      break;
  }

  Serial.print("Bot difficulty changed to: ");
  switch (difficulty) {
    case BOT_EASY:
      Serial.println("Easy");
      break;
    case BOT_MEDIUM:
      Serial.println("Medium");
      break;
    case BOT_HARD:
      Serial.println("Hard");
      break;
    case BOT_EXPERT:
      Serial.println("Expert");
      break;
  }
}

void ChessBot::getBoardState(char boardState[8][8]) {
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      boardState[row][col] = board[row][col];
    }
  }
}

void ChessBot::setBoardState(char newBoardState[8][8]) {
  Serial.println("Board state updated via WiFi edit");
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      board[row][col] = newBoardState[row][col];
    }
  }
  // Update sensor previous state to match new board
  _boardDriver->readSensors();
  ChessUtils::printBoard(board);
}