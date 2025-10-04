package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"os/signal"
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
					h.logger.Error("WebSocket write error: %v", err)
					delete(h.clients, conn)
					conn.Close()
				}
			}
		}
	}
}

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true // Разрешаем все origin для разработки
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

	// Ожидание сигнала завершения
	<-sigChan
	logger.Info("🛑 Shutting down server...")
}
