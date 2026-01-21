#include "board_driver.h"
#include "chess_bot.h"
#include "chess_engine.h"
#include "chess_moves.h"
#include "chess_utils.h"
#include "sensor_test.h"
#include "wifi_manager_esp32.h"

#define WiFiManager WiFiManagerESP32

// ---------------------------
// Game State and Configuration
// ---------------------------

enum GameMode {
  MODE_SELECTION = 0,
  MODE_CHESS_MOVES = 1,
  MODE_BOT = 2,
  MODE_SENSOR_TEST = 3
};

BotConfig botConfig = {StockfishSettings::medium(), true};

BoardDriver boardDriver;
ChessEngine chessEngine;
WiFiManager wifiManager(&boardDriver);
ChessMoves chessMoves(&boardDriver, &chessEngine, &wifiManager);
ChessBot* chessBot = nullptr;
SensorTest sensorTest(&boardDriver);

GameMode currentMode = MODE_SELECTION;
bool modeInitialized = false;

// ---------------------------
// Function Prototypes
// ---------------------------
void showGameSelection();
void handleGameSelection();
void showBotConfigSelection();
void handleBotConfigSelection();
void initializeSelectedMode(GameMode mode);

// ---------------------------
// SETUP
// ---------------------------
void setup() {
  Serial.begin(115200);
  delay(3000);

  Serial.println();
  Serial.println("================================================");
  Serial.println("         OpenChess Starting Up");
  Serial.println("================================================");
  if (!ChessUtils::ensureNvsInitialized())
    Serial.println("WARNING: NVS init failed (Preferences may not work)");
  boardDriver.begin();
  wifiManager.begin();
  Serial.println();
  Serial.println("=== Game Selection Mode ===");
  showGameSelection();
  Serial.println("Three LEDs are lit in the center of the board:");
  Serial.println("Gold:  Chess Moves (Human vs Human)");
  Serial.println("White: Chess Bot (Human vs AI)");
  Serial.println("Red:   Sensor Test");
  Serial.println();
  Serial.println("Place any chess piece on a LED to select that mode");
  Serial.println("================================================");
}

void loop() {
  // Check for pending board edits from WiFi
  char editBoard[8][8];
  if (wifiManager.getPendingBoardEdit(editBoard)) {
    Serial.println("Applying board edit from WiFi interface...");

    if (currentMode == MODE_CHESS_MOVES && modeInitialized) {
      chessMoves.setBoardState(editBoard);
      Serial.println("Board edit applied to Chess Moves mode");
    } else if (currentMode == MODE_BOT && modeInitialized && chessBot != nullptr) {
      chessBot->setBoardState(editBoard);
      Serial.println("Board edit applied to Chess Bot mode");
    } else {
      Serial.println("Warning: Board edit received but no active game mode");
    }

    wifiManager.clearPendingEdit();
  }

  // Check for WiFi game selection
  int selectedMode = wifiManager.getSelectedGameMode();
  if (selectedMode > 0) {
    Serial.printf("WiFi game selection detected: %d\n", selectedMode);
    switch (selectedMode) {
      case 1:
        currentMode = MODE_CHESS_MOVES;
        break;
      case 2:
        currentMode = MODE_BOT;
        botConfig = wifiManager.getBotConfig();
        break;
      case 3:
        currentMode = MODE_SENSOR_TEST;
        break;
      default:
        Serial.println("Invalid game mode selected via WiFi");
        selectedMode = 0;
        break;
    }
    if (selectedMode > 0) {
      modeInitialized = false;
      boardDriver.clearAllLEDs();
      wifiManager.resetGameSelection();
      boardDriver.flashBoardAnimation(LedColors::ConfirmGreen.r, LedColors::ConfirmGreen.g, LedColors::ConfirmGreen.b);
    }
  }

  if (currentMode == MODE_SELECTION) {
    handleGameSelection();
    return;
  }
  // Game mode selected
  if (!modeInitialized) {
    initializeSelectedMode(currentMode);
    modeInitialized = true;
  }
  switch (currentMode) {
    case MODE_CHESS_MOVES:
      chessMoves.update();
      break;
    case MODE_BOT:
      if (chessBot != nullptr)
        chessBot->update();
      break;
    case MODE_SENSOR_TEST:
      sensorTest.update();
      break;
    default:
      // Should not reach here
      currentMode = MODE_SELECTION;
      modeInitialized = false;
      showGameSelection();
      break;
  }

  delay(SENSOR_READ_DELAY_MS);
}

// ---------------------------
// GAME SELECTION FUNCTIONS
// ---------------------------

void showGameSelection() {
  boardDriver.clearAllLEDs();
  // Light up the 3 selector positions in the middle of the board
  // Each mode has a different color for easy identification
  // Position 1: Chess Moves (row 3, col 3) - Orange
  boardDriver.setSquareLED(3, 3, 255, 165, 0);
  // Position 2: Chess Bot (row 3, col 4) - White
  boardDriver.setSquareLED(3, 4, 0, 0, 0, 255);
  // Position 3: Sensor Test (row 4, col 4) - Red
  boardDriver.setSquareLED(4, 4, 255, 0, 0);
  boardDriver.showLEDs();
}

void handleGameSelection() {
  boardDriver.readSensors();

  // Check for piece placement on selector squares
  if (boardDriver.getSensorState(3, 3)) {
    Serial.println("Mode: 'Chess Moves' selected!");
    currentMode = MODE_CHESS_MOVES;
    modeInitialized = false;
    boardDriver.clearAllLEDs();
  } else if (boardDriver.getSensorState(3, 4)) {
    Serial.println("Mode: 'Chess Bot' Selected! Showing bot configuration...");
    currentMode = MODE_BOT;
    modeInitialized = false;
    boardDriver.clearAllLEDs();
    showBotConfigSelection();
  } else if (boardDriver.getSensorState(4, 4)) {
    Serial.println("Mode: 'Sensor Test' Selected!");
    currentMode = MODE_SENSOR_TEST;
    modeInitialized = false;
    boardDriver.clearAllLEDs();
  }

  delay(SENSOR_READ_DELAY_MS);
}

void initializeSelectedMode(GameMode mode) {
  switch (mode) {
    case MODE_CHESS_MOVES:
      Serial.println("Starting 'Chess Moves'...");
      chessMoves.begin();
      break;
    case MODE_BOT:
      Serial.printf("Starting 'Chess Bot' (Depth: %d, Player is %s)...\n", botConfig.stockfishSettings.depth, botConfig.playerIsWhite ? "White" : "Black");
      // Clean up any existing bot instance
      if (chessBot != nullptr)
        delete chessBot;
      // Create new bot with current configuration
      chessBot = new ChessBot(&boardDriver, &chessEngine, &wifiManager, botConfig);
      chessBot->begin();
      break;
    case MODE_SENSOR_TEST:
      Serial.println("Starting 'Sensor Test'...");
      sensorTest.begin();
      break;

    default:
      currentMode = MODE_SELECTION;
      modeInitialized = false;
      showGameSelection();
      break;
  }
}

// Bot configuration selection functions
void showBotConfigSelection() {
  boardDriver.clearAllLEDs();

  Serial.println("=== Bot Configuration Selection ===");
  Serial.println("Select Bot Color:");
  Serial.println("Row 2 (any square): Play as White (bot is Black)");
  Serial.println("Row 5 (any square): Play as Black (bot is White)");
  Serial.println("");
  Serial.println("Select Difficulty:");
  Serial.println("Col 1: Easy");
  Serial.println("Col 3: Medium");
  Serial.println("Col 5: Hard");
  Serial.println("Col 7: Expert");
  Serial.println("");
  Serial.println("Example: Place piece at row 2, col 3 = White + Medium");

  // Only show LEDs for valid difficulty selections (don't light up all squares)
  // Row 2 = Player plays White, Row 5 = Player plays Black
  // Easy (col 1) - Green
  boardDriver.setSquareLED(2, 1, 0, 255, 0);
  boardDriver.setSquareLED(5, 1, 0, 255, 0);

  // Medium (col 3) - Orange/Gold
  boardDriver.setSquareLED(2, 3, 255, 165, 0);
  boardDriver.setSquareLED(5, 3, 255, 165, 0);

  // Hard (col 5) - Red
  boardDriver.setSquareLED(2, 5, 255, 0, 0);
  boardDriver.setSquareLED(5, 5, 255, 0, 0);

  // Expert (col 7) - Purple
  boardDriver.setSquareLED(2, 7, 128, 0, 255);
  boardDriver.setSquareLED(5, 7, 128, 0, 255);

  boardDriver.showLEDs();

  // Wait for selection
  handleBotConfigSelection();
}

void handleBotConfigSelection() {
  Serial.println("Waiting for bot configuration selection...");

  while (true) {
    boardDriver.readSensors();

    // Check rows 2 and 5 for selections
    for (int row : {2, 5})
      for (int col = 0; col < 8; col++)
        if (boardDriver.getSensorState(row, col)) {
          // Determine player color based on row
          botConfig.playerIsWhite = (row == 2);
          const char* colorName = botConfig.playerIsWhite ? "White" : "Black";

          // Determine difficulty based on column
          if (col == 1) {
            botConfig.stockfishSettings = StockfishSettings::easy();
            Serial.printf("Configuration: Play as %s, Easy difficulty\n", colorName);
          } else if (col == 3) {
            botConfig.stockfishSettings = StockfishSettings::medium();
            Serial.printf("Configuration: Play as %s, Medium difficulty\n", colorName);
          } else if (col == 5) {
            botConfig.stockfishSettings = StockfishSettings::hard();
            Serial.printf("Configuration: Play as %s, Hard difficulty\n", colorName);
          } else if (col == 7) {
            botConfig.stockfishSettings = StockfishSettings::expert();
            Serial.printf("Configuration: Play as %s, Expert difficulty\n", colorName);
          } else {
            Serial.println("Invalid column for difficulty selection. Please try again.");
            continue;
          }

          boardDriver.clearAllLEDs();
          return;
        }

    delay(SENSOR_READ_DELAY_MS);
  }
}
