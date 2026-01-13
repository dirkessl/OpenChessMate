#include "chess_moves.h"
#include "chess_utils.h"
#include <Arduino.h>

const char ChessMoves::INITIAL_BOARD[8][8] = {
    {'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'}, // row 0 = rank 8 (black pieces, top row)
    {'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'}, // row 1 = rank 7 (black pawns)
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}, // row 2 = rank 6
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}, // row 3 = rank 5
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}, // row 4 = rank 4
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}, // row 5 = rank 3
    {'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'}, // row 6 = rank 2 (white pawns)
    {'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'}  // row 7 = rank 1 (White pieces, bottom row)
};

ChessMoves::ChessMoves(BoardDriver* bd, ChessEngine* ce) : boardDriver(bd), chessEngine(ce) {
  // Initialize board state
  initializeBoard();
  currentTurn = 'w'; // White starts
}

void ChessMoves::begin() {
  Serial.println("Starting Chess Game Mode...");
  initializeBoard();
  waitForBoardSetup();
  Serial.println("Chess game ready to start!");
  boardDriver->fireworkAnimation();
  boardDriver->readSensors();
  boardDriver->updateSensorPrev();
}

void ChessMoves::update() {
  boardDriver->readSensors();

  // Look for a piece pickup
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      if (boardDriver->getSensorPrev(row, col) && !boardDriver->getSensorState(row, col)) {
        char piece = board[row][col];

        // Skip empty squares
        if (piece == ' ')
          continue;

        // Check if it's the correct player's turn
        char pieceColor = (piece >= 'a' && piece <= 'z') ? 'b' : 'w';
        if (pieceColor != currentTurn) {
          Serial.printf("Wrong turn! It's %s's turn to move.\n", (currentTurn == 'w') ? "White" : "Black");

          // Flash the square red to indicate wrong turn
          boardDriver->blinkSquare(row, col, 255, 0, 0, 2);

          continue; // Skip this piece
        }

        Serial.printf("Piece lifted from %c%d\n", (char)('a' + col), row + 1);

        // Generate possible moves
        int moveCount = 0;
        int moves[28][2]; // up to 28 moves (maximum for a queen)
        chessEngine->getPossibleMoves(board, row, col, moveCount, moves);

        // Light up current square and possible move squares
        boardDriver->setSquareLED(row, col, 0, 0, 0, 100); // Dimmer, but solid

        // Highlight possible move squares (including captures)
        for (int i = 0; i < moveCount; i++) {
          int r = moves[i][0];
          int c = moves[i][1];

          // Different highlighting for empty squares vs capture squares
          if (board[r][c] == ' ') {
            boardDriver->setSquareLED(r, c, 0, 0, 0, 50); // Soft white for moves
          } else {
            boardDriver->setSquareLED(r, c, 255, 0, 0, 50); // Red tint for captures
          }
        }
        boardDriver->showLEDs();

        // Wait for piece placement - handle both normal moves and captures
        int targetRow = -1, targetCol = -1;
        bool piecePlaced = false;
        bool captureInProgress = false;

        // Wait for a piece placement on any square
        while (!piecePlaced) {
          boardDriver->readSensors();

          // First check if the original piece was placed back
          if (boardDriver->getSensorState(row, col)) {
            targetRow = row;
            targetCol = col;
            piecePlaced = true;
            break;
          }

          // Then check all squares for a regular move or capture initiation
          for (int r2 = 0; r2 < 8; r2++) {
            for (int c2 = 0; c2 < 8; c2++) {
              // Skip the original square which was already checked
              if (r2 == row && c2 == col)
                continue;

              // Check if this would be a legal move
              bool isLegalMove = false;
              for (int i = 0; i < moveCount; i++) {
                if (moves[i][0] == r2 && moves[i][1] == c2) {
                  isLegalMove = true;
                  break;
                }
              }

              // If not a legal move, no need to check further
              if (!isLegalMove)
                continue;

              // For capture moves: detect when the target piece is removed
              if (board[r2][c2] != ' ' && !boardDriver->getSensorState(r2, c2) && boardDriver->getSensorPrev(r2, c2)) {
                Serial.printf("Capture initiated at %c%d\n", (char)('a' + c2), r2 + 1);

                // Store the target square and wait for the capturing piece to be placed there
                targetRow = r2;
                targetCol = c2;
                captureInProgress = true;

                // Flash the capture square to indicate waiting for piece placement
                boardDriver->setSquareLED(r2, c2, 255, 0, 0, 100);
                boardDriver->showLEDs();

                // Wait for the capturing piece to be placed
                bool capturePiecePlaced = false;
                while (!capturePiecePlaced) {
                  boardDriver->readSensors();
                  if (boardDriver->getSensorState(r2, c2)) {
                    capturePiecePlaced = true;
                    piecePlaced = true;
                    break;
                  }
                  delay(50);
                }
                break;
              }

              // For normal non-capture moves: detect when a piece is placed on an empty square
              else if (board[r2][c2] == ' ' && boardDriver->getSensorState(r2, c2) && !boardDriver->getSensorPrev(r2, c2)) {
                targetRow = r2;
                targetCol = c2;
                piecePlaced = true;
                break;
              }
            }
            if (piecePlaced || captureInProgress)
              break;
          }

          delay(50);
        }

        // Check if piece is replaced in the original spot
        if (targetRow == row && targetCol == col) {
          Serial.println("Piece replaced in original spot");
          // Blink once to confirm
          boardDriver->setSquareLED(row, col, 0, 0, 0, 255);
          boardDriver->showLEDs();
          delay(200);
          boardDriver->setSquareLED(row, col, 0, 0, 0, 100);
          boardDriver->showLEDs();

          // Clear all LED effects
          boardDriver->clearAllLEDs();

          continue; // Skip to next iteration
        }

        // Check if move is legal
        bool legalMove = false;
        bool isCapture = false;
        for (int i = 0; i < moveCount; i++) {
          if (moves[i][0] == targetRow && moves[i][1] == targetCol) {
            legalMove = true;
            // Check if this is a capture move
            if (board[targetRow][targetCol] != ' ') {
              isCapture = true;
            }
            break;
          }
        }

        if (legalMove) {
          Serial.printf("Legal move to %c%d\n", (char)('a' + targetCol), targetRow + 1);

          // Play capture animation if needed
          if (board[targetRow][targetCol] != ' ') {
            Serial.println("Performing capture animation");
            boardDriver->captureAnimation();
          }

          // Process the move
          processMove(row, col, targetRow, targetCol, piece);

          // Check for pawn promotion
          checkForPromotion(targetRow, targetCol, piece);

          // Confirmation: Double blink destination square
          for (int blink = 0; blink < 2; blink++) {
            boardDriver->setSquareLED(targetRow, targetCol, 0, 0, 0, 255);
            boardDriver->showLEDs();
            delay(200);
            boardDriver->setSquareLED(targetRow, targetCol, 0, 0, 0, 50);
            boardDriver->showLEDs();
            delay(200);
          }
        } else {
          Serial.println("Illegal move, reverting");
        }

        // Clear any remaining LED effects
        boardDriver->clearAllLEDs();
      }
    }
  }

  // Update previous sensor state
  boardDriver->updateSensorPrev();
}

void ChessMoves::initializeBoard() {
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      board[row][col] = INITIAL_BOARD[row][col];
    }
  }
}

void ChessMoves::waitForBoardSetup() {
  Serial.println("Waiting for pieces to be placed...");
  while (!boardDriver->checkInitialBoard(INITIAL_BOARD)) {
    boardDriver->updateSetupDisplay(INITIAL_BOARD);
    ChessUtils::printBoard(INITIAL_BOARD);
    delay(500);
  }
}

void ChessMoves::processMove(int fromRow, int fromCol, int toRow, int toCol, char piece) {
  // Update board state
  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';

  // Switch turns
  currentTurn = (currentTurn == 'w') ? 'b' : 'w';

  // Check for check, checkmate, or stalemate
  checkGameState();
}

void ChessMoves::checkForPromotion(int targetRow, int targetCol, char piece) {
  if (chessEngine->isPawnPromotion(piece, targetRow)) {
    char promotedPiece = chessEngine->getPromotedPiece(piece);
    Serial.println(String(piece == 'P' ? "White" : "Black") + " pawn promoted to Queen at " + String(((char)('a' + targetCol))) + String(targetRow + 1));
    boardDriver->promotionAnimation(targetCol);
    board[targetRow][targetCol] = promotedPiece;
    handlePromotion(targetRow, targetCol, piece);
  }
}

void ChessMoves::handlePromotion(int targetRow, int targetCol, char piece) {
  Serial.println("Please replace the pawn with a queen piece");

  // First wait for the pawn to be removed
  while (boardDriver->getSensorState(targetRow, targetCol)) {
    // Blink the square to indicate action needed
    boardDriver->setSquareLED(targetRow, targetCol, 255, 215, 0, 50);
    boardDriver->showLEDs();
    delay(250);
    boardDriver->setSquareLED(targetRow, targetCol, 0, 0, 0, 0);
    boardDriver->showLEDs();
    delay(250);

    // Read sensors
    boardDriver->readSensors();
  }

  Serial.println("Pawn removed, please place a queen");

  // Then wait for the queen to be placed
  while (!boardDriver->getSensorState(targetRow, targetCol)) {
    // Blink the square to indicate action needed
    boardDriver->setSquareLED(targetRow, targetCol, 255, 215, 0, 50);
    boardDriver->showLEDs();
    delay(250);
    boardDriver->setSquareLED(targetRow, targetCol, 0, 0, 0, 0);
    boardDriver->showLEDs();
    delay(250);

    // Read sensors
    boardDriver->readSensors();
  }

  Serial.println("Queen placed, promotion complete");

  // Final confirmation blink
  for (int i = 0; i < 3; i++) {
    boardDriver->setSquareLED(targetRow, targetCol, 255, 215, 0, 50);
    boardDriver->showLEDs();
    delay(100);
    boardDriver->setSquareLED(targetRow, targetCol, 0, 0, 0, 0);
    boardDriver->showLEDs();
    delay(100);
  }
}

bool ChessMoves::isActive() {
  return true; // Simple implementation for now
}

void ChessMoves::reset() {
  boardDriver->clearAllLEDs();
  initializeBoard();
}

void ChessMoves::getBoardState(char boardState[8][8]) {
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      boardState[row][col] = board[row][col];
    }
  }
}

void ChessMoves::setBoardState(char newBoardState[8][8]) {
  Serial.println("Board state updated via WiFi edit");
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      board[row][col] = newBoardState[row][col];
    }
  }
  // Update sensor previous state to match new board
  boardDriver->readSensors();
  boardDriver->updateSensorPrev();
  ChessUtils::printBoard(board);
}

// Check game state after a move
void ChessMoves::checkGameState() {
  const char* colorName = (currentTurn == 'w') ? "White" : "Black";

  // Check for checkmate
  if (chessEngine->isCheckmate(board, currentTurn)) {
    const char* winnerName = (currentTurn == 'w') ? "Black" : "White";
    Serial.printf("CHECKMATE! %s wins!\n", winnerName);
    boardDriver->fireworkAnimation();
    return;
  }

  // Check for stalemate
  if (chessEngine->isStalemate(board, currentTurn)) {
    Serial.println("STALEMATE! Game is a draw.");
    boardDriver->clearAllLEDs();
    return;
  }

  // Check for check (but not checkmate)
  if (chessEngine->isKingInCheck(board, currentTurn)) {
    Serial.printf("%s is in CHECK!\n", colorName);
    boardDriver->clearAllLEDs();
    // Find the king position and blink it red
    char kingPiece = (currentTurn == 'w') ? 'K' : 'k';
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        if (board[row][col] == kingPiece) {
          // Blink the king's square orange/yellow to indicate check
          boardDriver->blinkSquare(row, col, 255, 100, 0);
          return;
        }
      }
    }
  }
}