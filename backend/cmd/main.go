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

type EnvironmentData struct {
	Temperature float64 `json:"temperature"`
	Humidity    float64 `json:"humidity"`
}

type ESP32Data struct {
	Timestamp   int64                 `json:"timestamp"`
	Position    Position              `json:"position"`
	WiFi        map[string]BeaconData `json:"wifi"`
	BLE         map[string]BeaconData `json:"ble"`
	Fusion      FusionData            `json:"fusion"`
	Environment EnvironmentData        `json:"environment"`
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
	port         serial.Port
	portName     string
	hub          *Hub
	logger       *logger.Logger
	reconnect    chan bool
	stopReconnect chan bool
	stopReading  chan bool
	isReading    bool
}

func NewSerialReader(hub *Hub, logger *logger.Logger) *SerialReader {
	return &SerialReader{
		hub:           hub,
		logger:        logger,
		reconnect:     make(chan bool, 1),
		stopReconnect: make(chan bool, 1),
		stopReading:   make(chan bool, 1),
		isReading:     false,
	}
}

func (sr *SerialReader) Connect(portName string) error {
	// Сохраняем имя порта даже при неудачном подключении
	sr.portName = portName
	
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

func (sr *SerialReader) StopReading() {
	if sr.isReading {
		sr.logger.Debug("🛑 Stopping serial reading...")
		sr.stopReading <- true
		sr.isReading = false
	}
}

func (sr *SerialReader) StartReading() {
	if !sr.isReading {
		sr.isReading = true
		go sr.ReadAndBroadcast()
	}
}

// Функция автоматического переподключения
func (sr *SerialReader) StartReconnectLoop() {
	go func() {
		reconnectInterval := 5 * time.Second
		
		for {
			select {
			case <-sr.reconnect:
				sr.logger.Warning("🔄 Attempting to reconnect to serial port...")
				
				// Останавливаем текущее чтение
				sr.StopReading()
				
				// Закрываем текущее соединение если оно есть
				if sr.port != nil {
					sr.port.Close()
					sr.port = nil
				}
				
				// Попытка переподключения
				for {
					select {
					case <-sr.stopReconnect:
						sr.logger.Info("🛑 Stopping reconnect loop")
						return
					default:
						if err := sr.Connect(sr.portName); err != nil {
							sr.logger.Warning("⚠️  Reconnect failed to %s: %v. Retrying in %v...", sr.portName, err, reconnectInterval)
							time.Sleep(reconnectInterval)
							continue
						}
						
						sr.logger.Info("✅ Successfully reconnected to serial port")
						// Запускаем чтение данных снова
						sr.StartReading()
						return
					}
				}
			case <-sr.stopReconnect:
				sr.logger.Info("🛑 Stopping reconnect loop")
				return
			}
		}
	}()
}

func (sr *SerialReader) TriggerReconnect() {
	select {
	case sr.reconnect <- true:
	default:
		// Канал уже содержит сигнал переподключения
	}
}

func (sr *SerialReader) StopReconnect() {
	select {
	case sr.stopReconnect <- true:
	default:
	}
}

func (sr *SerialReader) ReadAndBroadcast() {
	if sr.port == nil {
		sr.logger.Error("Serial port not connected")
		sr.isReading = false
		return
	}

	scanner := bufio.NewScanner(sr.port)
	buf := make([]byte, 0, 64*1024) // 64KB буфер
	scanner.Buffer(buf, 1024*1024)   // Максимум 1MB
	
	for scanner.Scan() {
		// Проверяем сигнал остановки
		select {
		case <-sr.stopReading:
			sr.logger.Debug("🛑 Reading stopped by signal")
			sr.isReading = false
			return
		default:
		}
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
		sr.logger.Debug("📤 Broadcasted position: (%.2f, %.2f) ±%.2fm | Temp: %.1f°C, Humidity: %.1f%%", 
			esp32Data.Position.X, esp32Data.Position.Y, esp32Data.Position.Accuracy,
			esp32Data.Environment.Temperature, esp32Data.Environment.Humidity)
	}

	if err := scanner.Err(); err != nil {
		sr.logger.Error("❌ Serial scanner error: %v", err)
		sr.logger.Warning("🔄 Connection lost, triggering reconnect...")
		sr.isReading = false
		sr.TriggerReconnect()
	} else {
		sr.isReading = false
	}
}

func (sr *SerialReader) Close() error {
	sr.StopReconnect()
	sr.StopReading()
	if sr.port != nil {
		return sr.port.Close()
	}
	return nil
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
	
	// Запуск цикла переподключения
	serialReader.StartReconnectLoop()
	
	// Попытка первоначального подключения
	if err := serialReader.Connect(portName); err != nil {
		logger.Warning("⚠️  Initial serial connection failed: %v", err)
		logger.Info("🔄 Will attempt to reconnect automatically...")
		logger.Info("💡 Usage: %s [serial_port]", os.Args[0])
		logger.Info("💡 Example: %s /dev/ttyUSB0", os.Args[0])
		// Запускаем переподключение
		serialReader.TriggerReconnect()
	} else {
		// Запуск чтения Serial порта
		serialReader.StartReading()
	}
	
	defer serialReader.Close()

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
