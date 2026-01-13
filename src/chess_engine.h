#ifndef CHESS_ENGINE_H
#define CHESS_ENGINE_H

// ---------------------------
// Chess Engine Class
// ---------------------------
class ChessEngine {
 private:
  // Helper functions for move generation
  void addPawnMoves(const char board[8][8], int row, int col, char pieceColor, int& moveCount, int moves[][2]);
  void addRookMoves(const char board[8][8], int row, int col, char pieceColor, int& moveCount, int moves[][2]);
  void addKnightMoves(const char board[8][8], int row, int col, char pieceColor, int& moveCount, int moves[][2]);
  void addBishopMoves(const char board[8][8], int row, int col, char pieceColor, int& moveCount, int moves[][2]);
  void addQueenMoves(const char board[8][8], int row, int col, char pieceColor, int& moveCount, int moves[][2]);
  void addKingMoves(const char board[8][8], int row, int col, char pieceColor, int& moveCount, int moves[][2]);

  bool isSquareOccupiedByOpponent(const char board[8][8], int row, int col, char pieceColor);
  bool isSquareEmpty(const char board[8][8], int row, int col);
  bool isValidSquare(int row, int col);
  char getPieceColor(char piece);

  // Check detection helpers
  void getPseudoLegalMoves(const char board[8][8], int row, int col, int& moveCount, int moves[][2]);
  bool isSquareUnderAttack(const char board[8][8], int row, int col, char defendingColor);
  bool findKing(const char board[8][8], char kingColor, int& kingRow, int& kingCol);
  bool wouldMoveLeaveKingInCheck(const char board[8][8], int fromRow, int fromCol, int toRow, int toCol);
  void makeMove(char board[8][8], int fromRow, int fromCol, int toRow, int toCol, char& capturedPiece);

 public:
  ChessEngine();

  // Main move generation function
  void getPossibleMoves(const char board[8][8], int row, int col, int& moveCount, int moves[][2]);

  // Move validation
  bool isValidMove(const char board[8][8], int fromRow, int fromCol, int toRow, int toCol);

  // Game state checks
  bool isPawnPromotion(char piece, int targetRow);
  char getPromotedPiece(char piece);
  bool isKingInCheck(const char board[8][8], char kingColor);
  bool isCheckmate(const char board[8][8], char kingColor);
  bool isStalemate(const char board[8][8], char colorToMove);

  // Utility functions
  void printMove(int fromRow, int fromCol, int toRow, int toCol);
  char algebraicToCol(char file);
  int algebraicToRow(int rank);
};

#endif // CHESS_ENGINE_H