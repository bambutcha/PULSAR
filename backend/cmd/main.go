package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"pulsar-backend/pkg/logger"

	"github.com/gorilla/websocket"
	"go.bug.st/serial"
)

// Модели данных
type Position struct {
	X        float64 `json:"x"`
	Y        float64 `json:"y"`
	Accuracy float64 `json:"accuracy"`
}

type BeaconData struct {
	RSSI     int     `json:"rssi"`
	Distance float64 `json:"distance"`
	Found    bool    `json:"found"`
}

type FusionData struct {
	WiFiWeight float64 `json:"wifi_weight"`
	BLEWeight  float64 `json:"ble_weight"`
}

type ESP32Data struct {
	Timestamp int64                 `json:"timestamp"`
	Position  Position              `json:"position"`
	WiFi      map[string]BeaconData `json:"wifi"`
	BLE       map[string]BeaconData `json:"ble"`
	Fusion    FusionData            `json:"fusion"`
}

type WSMessage struct {
	Type      string      `json:"type"`
	Data      interface{} `json:"data"`
	Timestamp int64       `json:"timestamp"`
}

// WebSocket Hub
type Hub struct {
	clients    map[*websocket.Conn]bool
	register   chan *websocket.Conn
	unregister chan *websocket.Conn
	broadcast  chan []byte
	logger     *logger.Logger
}

func NewHub(logger *logger.Logger) *Hub {
	return &Hub{
		clients:    make(map[*websocket.Conn]bool),
		register:   make(chan *websocket.Conn),
		unregister: make(chan *websocket.Conn),
		broadcast:  make(chan []byte),
		logger:     logger,
	}
}

func (h *Hub) Run() {
	for {
		select {
		case conn := <-h.register:
			h.clients[conn] = true
			h.logger.Info("WebSocket client connected. Total clients: %d", len(h.clients))

		case conn := <-h.unregister:
			if _, ok := h.clients[conn]; ok {
				delete(h.clients, conn)
				conn.Close()
				h.logger.Info("WebSocket client disconnected. Total clients: %d", len(h.clients))
			}

		case message := <-h.broadcast:
			for conn := range h.clients {
				if err := conn.WriteMessage(websocket.TextMessage, message); err != nil {
					h.logger.Debug("WebSocket write error (client disconnected): %v", err)
					delete(h.clients, conn)
					conn.Close()
				}
			}
		}
	}
}

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		origin := r.Header.Get("Origin")
		// Разрешаем подключения с localhost:5173 (Vite dev server) и другие localhost порты
		return origin == "http://localhost:5173" || 
			   origin == "http://127.0.0.1:5173" ||
			   origin == "http://localhost:3000" ||
			   origin == "http://127.0.0.1:3000" ||
			   origin == ""
	},
}

func handleWebSocket(hub *Hub, w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		hub.logger.Error("WebSocket upgrade error: %v", err)
		return
	}
	hub.register <- conn
}

// Serial Reader
type SerialReader struct {
	port   serial.Port
	hub    *Hub
	logger *logger.Logger
}

func NewSerialReader(hub *Hub, logger *logger.Logger) *SerialReader {
	return &SerialReader{
		hub:    hub,
		logger: logger,
	}
}

func (sr *SerialReader) Connect(portName string) error {
	mode := &serial.Mode{
		BaudRate: 115200,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}

	port, err := serial.Open(portName, mode)
	if err != nil {
		return fmt.Errorf("failed to open serial port %s: %v", portName, err)
	}

	sr.port = port
	sr.logger.Info("📡 Connected to serial port: %s", portName)
	return nil
}

func (sr *SerialReader) ReadAndBroadcast() {
	if sr.port == nil {
		sr.logger.Error("Serial port not connected")
		return
	}

	scanner := bufio.NewScanner(sr.port)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}

		// Проверяем, является ли строка JSON (начинается с {)
		if !strings.HasPrefix(line, "{") {
			// Это отладочное сообщение от ESP32, игнорируем
			continue
		}

		sr.logger.Debug("📥 Received JSON: %s", line)

		// Парсинг JSON от ESP32
		var esp32Data ESP32Data
		if err := json.Unmarshal([]byte(line), &esp32Data); err != nil {
			sr.logger.Error("❌ Failed to parse JSON: %v", err)
			continue
		}

		// Создание WebSocket сообщения
		wsMessage := WSMessage{
			Type:      "position_update",
			Data:      esp32Data,
			Timestamp: time.Now().UnixMilli(),
		}

		// Конвертация в JSON
		messageBytes, err := json.Marshal(wsMessage)
		if err != nil {
			sr.logger.Error("❌ Failed to marshal WebSocket message: %v", err)
			continue
		}

		// Отправка через WebSocket Hub
		sr.hub.broadcast <- messageBytes
		sr.logger.Debug("📤 Broadcasted position: (%.2f, %.2f) ±%.2fm", 
			esp32Data.Position.X, esp32Data.Position.Y, esp32Data.Position.Accuracy)
	}

	if err := scanner.Err(); err != nil {
		sr.logger.Error("❌ Serial scanner error: %v", err)
	}
}

func (sr *SerialReader) Close() error {
	if sr.port != nil {
		return sr.port.Close()
	}
	return nil
}

// Генератор тестовых данных для демонстрации
func (sr *SerialReader) GenerateTestData() {
	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()

	// Начальная позиция
	x, y := 1.0, 1.0
	direction := 1.0

	for range ticker.C {
		// Симуляция движения по кругу
		x += 0.1 * direction
		y += 0.05 * direction
		
		if x > 4.0 || x < 0.5 {
			direction *= -1
		}

		// Создание тестовых данных
		testData := ESP32Data{
			Timestamp: time.Now().UnixMilli(),
			Position: Position{
				X:        x,
				Y:        y,
				Accuracy: 0.5 + (x/10.0), // Изменяющаяся точность
			},
			WiFi: map[string]BeaconData{
				"beacon1": {RSSI: -45, Distance: 2.1, Found: true},
				"beacon2": {RSSI: -52, Distance: 3.4, Found: true},
				"beacon3": {RSSI: -48, Distance: 2.8, Found: true},
			},
			BLE: map[string]BeaconData{
				"beacon1": {RSSI: -65, Distance: 2.3, Found: true},
				"beacon2": {RSSI: -72, Distance: 3.6, Found: true},
				"beacon3": {RSSI: -68, Distance: 3.0, Found: true},
			},
			Fusion: FusionData{
				WiFiWeight: 0.6,
				BLEWeight:  0.4,
			},
		}

		// Создание WebSocket сообщения
		wsMessage := WSMessage{
			Type:      "position_update",
			Data:      testData,
			Timestamp: time.Now().UnixMilli(),
		}

		// Конвертация в JSON
		messageBytes, err := json.Marshal(wsMessage)
		if err != nil {
			sr.logger.Error("❌ Failed to marshal test message: %v", err)
			continue
		}

		// Отправка через WebSocket Hub
		sr.hub.broadcast <- messageBytes
		sr.logger.Debug("🧪 Test data: (%.2f, %.2f) ±%.2fm", 
			testData.Position.X, testData.Position.Y, testData.Position.Accuracy)
	}
}

func main() {
	// Инициализация логгера
	logger, err := logger.NewLogger("pulsar.log")
	if err != nil {
		fmt.Printf("Failed to create logger: %v\n", err)
		os.Exit(1)
	}
	defer logger.Close()

	logger.Info("🚀 Starting PULSAR Backend Server")

	// Создание WebSocket Hub
	hub := NewHub(logger)
	go hub.Run()

	// Создание Serial Reader
	serialReader := NewSerialReader(hub, logger)
	
	// Попытка подключения к Serial порту (по умолчанию /dev/ttyUSB0)
	portName := "/dev/ttyUSB0"
	if len(os.Args) > 1 {
		portName = os.Args[1]
	}
	
	if err := serialReader.Connect(portName); err != nil {
		logger.Warning("⚠️  Serial connection failed: %v", err)
		logger.Warning("💡 Usage: %s [serial_port]", os.Args[0])
		logger.Warning("💡 Example: %s /dev/ttyUSB0", os.Args[0])
		logger.Info("🧪 Starting test data generator for demo...")
		// Запуск генератора тестовых данных
		go serialReader.GenerateTestData()
	} else {
		defer serialReader.Close()
		// Запуск чтения Serial порта в отдельной горутине
		go serialReader.ReadAndBroadcast()
	}

	// HTTP сервер для WebSocket
	http.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		handleWebSocket(hub, w, r)
	})

	// Статический файл для тестирования
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `
<!DOCTYPE html>
<html>
<head>
    <title>PULSAR WebSocket Test</title>
</head>
<body>
    <h1>PULSAR Position Data</h1>
    <div id="messages"></div>
    <script>
        const ws = new WebSocket('ws://localhost:8080/ws');
        const messages = document.getElementById('messages');
        
        ws.onmessage = function(event) {
            const data = JSON.parse(event.data);
            messages.innerHTML += '<p>' + JSON.stringify(data, null, 2) + '</p>';
        };
    </script>
</body>
</html>`)
	})

	// Запуск HTTP сервера
	go func() {
		logger.Info("🌐 WebSocket server starting on :8080")
		if err := http.ListenAndServe(":8080", nil); err != nil {
			logger.Fatal("HTTP server error: %v", err)
		}
	}()

	// Обработка сигналов для graceful shutdown
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	logger.Info("✅ Server started successfully")
	logger.Info("📡 WebSocket endpoint: ws://localhost:8080/ws")
	logger.Info("🌐 Test page: http://localhost:8080/")
	logger.Info("📡 Serial port: %s", portName)
	logger.Info("🔄 Reading ESP32 data and broadcasting via WebSocket...")

	// Ожидание сигнала завершения
	<-sigChan
	logger.Info("🛑 Shutting down server...")
}
