import React, { useState, useEffect, useRef } from "react";
import "./App.css";

// -------------------- TYPES --------------------
interface BeaconData {
  rssi: number;
  distance: number;
  found: boolean;
}

interface Position {
  x: number;
  y: number;
  accuracy: number;
}

interface Fusion {
  wifi_weight: number;
  ble_weight: number;
}

interface Environment {
  temperature: number;
  humidity: number;
}

interface ESPData {
  timestamp: number;
  position: Position;
  wifi: Record<string, BeaconData>;
  ble: Record<string, BeaconData>;
  fusion: Fusion;
  cv?: Position;
  environment?: Environment;
}

// -------------------- RESPONSIVE HOOK --------------------
const useResponsive = () => {
  const [windowSize, setWindowSize] = useState({
    width: window.innerWidth,
    height: window.innerHeight,
  });
  const [isMobile, setIsMobile] = useState(window.innerWidth < 768);
  const [isTablet, setIsTablet] = useState(
    window.innerWidth >= 768 && window.innerWidth < 1024
  );

  useEffect(() => {
    const handleResize = () => {
      const width = window.innerWidth;
      const height = window.innerHeight;
      
      setWindowSize({ width, height });
      setIsMobile(width < 768);
      setIsTablet(width >= 768 && width < 1024);
    };

    window.addEventListener("resize", handleResize);
    return () => window.removeEventListener("resize", handleResize);
  }, []);

  return { windowSize, isMobile, isTablet };
};

// -------------------- WEBSOCKET SINGLETON --------------------
let globalWS: WebSocket | null = null;
let globalCallbacks: Set<(data: ESPData | null, connected: boolean) => void> = new Set();

function connectWebSocket(url: string) {
  if (globalWS && globalWS.readyState === WebSocket.OPEN) {
    return; // Уже подключен
  }

  if (globalWS) {
    globalWS.close();
  }

  globalWS = new WebSocket(url);
  
  globalWS.onopen = () => {
    globalCallbacks.forEach(callback => callback(null, true));
  };
  
  globalWS.onmessage = (event) => {
    try {
      const message = JSON.parse(event.data);
      if (message.type === "position_update" && message.data) {
        globalCallbacks.forEach(callback => callback(message.data, true));
      }
    } catch (err) {
      console.error("Invalid JSON", err);
    }
  };
  
  globalWS.onclose = () => {
    globalCallbacks.forEach(callback => callback(null, false));
    setTimeout(() => connectWebSocket(url), 2000);
  };
  
  globalWS.onerror = (err) => {
    console.error("WebSocket error", err);
    globalCallbacks.forEach(callback => callback(null, false));
  };
}

// -------------------- WEBSOCKET HOOK --------------------
function useWebSocket(url: string) {
  const [data, setData] = useState<ESPData | null>(null);
  const [connected, setConnected] = useState(false);

  useEffect(() => {
    // Подключаемся к глобальному WebSocket
    connectWebSocket(url);
    
    // Добавляем callback
    const callback = (newData: ESPData | null, isConnected: boolean) => {
      setData(newData);
      setConnected(isConnected);
    };
    
    globalCallbacks.add(callback);
    
    return () => {
      globalCallbacks.delete(callback);
    };
  }, [url]);

  return { data, connected };
}

// -------------------- ADAPTIVE MAP CANVAS --------------------
interface MapCanvasProps {
  data: ESPData | null;
  showHistory?: boolean;
  historyLength?: number;
  isMobile?: boolean;
  isTablet?: boolean;
}

const MapCanvas: React.FC<MapCanvasProps> = ({
  data,
  showHistory = true,
  historyLength = 30,
  isMobile = false,
  isTablet = false,
}) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  const positionsRef = useRef<Position[]>([]);

  const animationRef = useRef<number>();
  const currentPosRef = useRef<Position>({ x: 0, y: 0, accuracy: 0 });

  const [dimensions, setDimensions] = useState({ width: 600, height: 400 });

  // Константы комнаты
  const ROOM_WIDTH = 1.5; // meters
  const ROOM_HEIGHT = 1.5; // meters
  const GRID_STEP = 0.1; // meters (сетка каждые 10 см)

  // Позиции маячков в метрах (конвертировано из dm: 2=0.2m, 8=0.8m, etc.)
    const beaconPositions = [
    { x: 0.2, y: 1.3 }, // Beacon 1
    { x: 1.3, y: 1.3 }, // Beacon 2
    { x: 0.8, y: 0.2 }, // Beacon 3
    ];

  // Адаптивные размеры канваса
  useEffect(() => {
    const updateDimensions = () => {
      if (containerRef.current) {
        const containerWidth = containerRef.current.clientWidth;
        const containerHeight = containerRef.current.clientHeight;
        
        let width, height;
        
        if (isMobile) {
          // На мобильных - квадратный формат
          width = containerWidth - 20;
          height = Math.min(containerWidth - 20, 300);
        } else if (isTablet) {
          // На планшетах - умеренная высота
          width = containerWidth - 20;
          height = Math.min(containerHeight * 0.7, 400);
        } else {
          // На десктопе - полная высота
          width = containerWidth - 20;
          height = containerHeight - 20;
        }

        setDimensions({ width, height });
      }
    };

    updateDimensions();
    window.addEventListener('resize', updateDimensions);
    
    return () => {
      window.removeEventListener('resize', updateDimensions);
      if (animationRef.current) cancelAnimationFrame(animationRef.current);
    };
  }, [isMobile, isTablet]);

  useEffect(() => {
    if (!data) return;

    positionsRef.current.push(data.position);
    if (positionsRef.current.length > historyLength)
      positionsRef.current.shift();
  }, [data, historyLength]);

  useEffect(() => {
    const ctx = canvasRef.current?.getContext("2d");
    if (!ctx) return;

    function draw() {
      if (!ctx) return;

      // Линейная интерполяция
      if (data && data.position) {
        currentPosRef.current.x += (data.position.x - currentPosRef.current.x) * 0.1;
        currentPosRef.current.y += (data.position.y - currentPosRef.current.y) * 0.1;
        currentPosRef.current.accuracy += (data.position.accuracy - currentPosRef.current.accuracy) * 0.1;
      }

      // Очистка
      ctx.clearRect(0, 0, dimensions.width, dimensions.height);

      // Градиентное фоновое поле
      const gradient = ctx.createLinearGradient(0, 0, dimensions.width, dimensions.height);
      gradient.addColorStop(0, "#111");
      gradient.addColorStop(1, "#222");
      ctx.fillStyle = gradient;
      ctx.fillRect(0, 0, dimensions.width, dimensions.height);

      if (!data) return;

      // Вычисление масштаба (px per meter), сохраняя aspect ratio комнаты
      const scale = Math.min(
        dimensions.width / ROOM_WIDTH,
        dimensions.height / ROOM_HEIGHT
      );

      // Офсеты для центрирования комнаты на канвасе
      const offsetX = (dimensions.width - ROOM_WIDTH * scale) / 2;
      const offsetY = (dimensions.height - ROOM_HEIGHT * scale) / 2;

      // Сетка (каждые 0.1m, только если достаточно места)
      if (!isMobile || dimensions.width > 300) {
        ctx.strokeStyle = "#333";
        ctx.lineWidth = 1;

        // Вертикальные линии
        for (let x = 0; x <= ROOM_WIDTH; x += GRID_STEP) {
          const canvasX = offsetX + x * scale;
          ctx.beginPath();
          ctx.moveTo(canvasX, offsetY);
          ctx.lineTo(canvasX, offsetY + ROOM_HEIGHT * scale);
          ctx.stroke();
        }

        // Горизонтальные линии
        for (let y = 0; y <= ROOM_HEIGHT; y += GRID_STEP) {
          const canvasY = offsetY + y * scale;
          ctx.beginPath();
          ctx.moveTo(offsetX, canvasY);
          ctx.lineTo(offsetX + ROOM_WIDTH * scale, canvasY);
          ctx.stroke();
        }
      }

      // Отрисовка ESP-меток (WiFi и BLE вместе)
      beaconPositions.forEach((beaconPos, index) => {
        const beaconNumber = index + 1;
        const wifiBeacon = data.wifi[`beacon${beaconNumber}`];
        const bleBeacon = data.ble[`beacon${beaconNumber}`];

        // Позиция метки на канвасе (Y растет вниз, как в canvas)
        const posX = offsetX + beaconPos.x * scale;
        const posY = offsetY + beaconPos.y * scale;

        if (wifiBeacon) {
          // WiFi точка (слева от центра метки)
          const wifiX = posX - 8;
          ctx.fillStyle = "gold";
          ctx.beginPath();
          ctx.arc(wifiX, posY, isMobile ? 3 : 5, 0, 2 * Math.PI);
          ctx.fill();

          // Радиус сигнала WiFi
          if (!isMobile || dimensions.width > 400) {
            ctx.globalAlpha = 0.2;
            ctx.strokeStyle = "gold";
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.arc(wifiX, posY, wifiBeacon.distance * scale, 0, 2 * Math.PI);
            ctx.stroke();
            ctx.globalAlpha = 1;
          }
        }

        if (bleBeacon) {
          // BLE точка (справа от центра метки)
          const bleX = posX + 8;
          ctx.fillStyle = "deepskyblue";
          ctx.beginPath();
          ctx.arc(bleX, posY, isMobile ? 3 : 5, 0, 2 * Math.PI);
          ctx.fill();

          // Радиус сигнала BLE
          if (!isMobile || dimensions.width > 400) {
            ctx.globalAlpha = 0.2;
            ctx.strokeStyle = "deepskyblue";
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.arc(bleX, posY, bleBeacon.distance * scale, 0, 2 * Math.PI);
            ctx.stroke();
            ctx.globalAlpha = 1;
          }
        }

        // Подписи для ESP-меток (только на десктопе)
        if (!isMobile) {
          ctx.fillStyle = "#fff";
          ctx.font = "12px Arial";
          ctx.textAlign = "center";
          ctx.fillText(`ESP${beaconNumber}`, posX, posY - 20);
        }
      });

      // История движения
      if (showHistory && positionsRef.current.length > 1) {
        ctx.strokeStyle = "rgba(0,255,255,0.5)";
        ctx.lineWidth = isMobile ? 1.5 : 2;
        ctx.beginPath();
        positionsRef.current.forEach((pos, idx) => {
          const x = offsetX + pos.x * scale;
          const y = offsetY + pos.y * scale;
          if (idx === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        });
        ctx.stroke();
      }

      // Текущий объект (приемник)
      const posX = offsetX + currentPosRef.current.x * scale;
      const posY = offsetY + currentPosRef.current.y * scale;

      ctx.fillStyle = currentPosRef.current.accuracy > 0.4 ? "orange" : "lime";      
      ctx.beginPath();
      ctx.arc(posX, posY, isMobile ? 6 : 8, 0, 2 * Math.PI);
      ctx.fill();

      // Accuracy circle
      ctx.fillStyle = currentPosRef.current.accuracy > 0.3 ? "rgba(255,165,0,0.2)" : "rgba(0,255,0,0.2)";
      ctx.beginPath();
      ctx.arc(posX, posY, currentPosRef.current.accuracy * scale, 0, 2 * Math.PI);
      ctx.fill();

      // CV точка
      if (data.cv) {
        const cvX = offsetX + data.cv.x * scale;
        const cvY = offsetY + data.cv.y * scale;
        ctx.fillStyle = "red";
        ctx.beginPath();
        ctx.arc(cvX, cvY, isMobile ? 3 : 5, 0, 2 * Math.PI);
        ctx.fill();
      }

      // Подписи координат (только на больших экранах)
      if (!isMobile) {
        ctx.fillStyle = "#fff";
        ctx.font = "12px Arial";
        ctx.textAlign = "left";
        ctx.fillText(
          `(${currentPosRef.current.x.toFixed(1)}, ${currentPosRef.current.y.toFixed(1)})`,
          posX + 10,
          posY - 5
        );
      }

      // Подписи осей (координаты комнаты)
      if (!isMobile) {
        ctx.fillStyle = "#888";
        ctx.font = "10px Arial";
        ctx.textAlign = "center";

        // X labels
        for (let x = 0; x <= ROOM_WIDTH; x += 0.3) {
        const canvasX = offsetX + x * scale;
        ctx.fillText(x.toFixed(1), canvasX, offsetY + ROOM_HEIGHT * scale + 15);
        }

        // Y labels
        for (let y = 0; y <= ROOM_HEIGHT; y += 0.3) {
        const canvasY = offsetY + y * scale;
        ctx.save();
        ctx.textAlign = "right";
        ctx.fillText(y.toFixed(1), offsetX - 5, canvasY);
        ctx.restore();
        }
    }

      animationRef.current = requestAnimationFrame(draw);
    }

    draw();

    return () => {
      if (animationRef.current) cancelAnimationFrame(animationRef.current);
    };
  }, [data, dimensions, isMobile, isTablet, showHistory]);

  return (
    <div ref={containerRef} className="map-container">
      <canvas
        ref={canvasRef}
        width={dimensions.width}
        height={dimensions.height}
        className="map-canvas"
      />
    </div>
  );
};

// -------------------- HISTORY COMPONENT --------------------
const secondsToTime = (ms: number) => {
  const sec = Math.floor(ms / 1000);
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  return `${h.toString().padStart(2, '0')}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
};

interface HistoryProps {
  history: ESPData[];
  isMobile?: boolean;
}

const History: React.FC<HistoryProps> = ({ history, isMobile = false }) => {
  if (history.length === 0) return null;

  return (
    <InfoCard title="Movement History" isMobile={isMobile}>
      <ul className={`history-list ${isMobile ? 'mobile' : ''}`}>
        {history.map((entry, idx) => (
          <li key={idx} className="history-entry">
            <div className="history-time">Time: {secondsToTime(entry.timestamp)}</div>
            <div className="history-pos">
              Position: {entry.position.x.toFixed(2)}, {entry.position.y.toFixed(2)} m (acc: {entry.position.accuracy.toFixed(2)} m)
            </div>
            {entry.environment && (
              <>
                <div className="history-env">Temp: {entry.environment.temperature.toFixed(1)}°C</div>
                <div className="history-env">Humidity: {entry.environment.humidity.toFixed(1)}%</div>
              </>
            )}
            <div className="history-distances">
              Distances:
              {[1, 2, 3].map((b) => {
                const wifiD = entry.wifi[`beacon${b}`]?.distance ?? 0;
                const bleD = entry.ble[`beacon${b}`]?.distance ?? 0;
                const effD = entry.fusion.wifi_weight * wifiD + entry.fusion.ble_weight * bleD;
                return (
                  <div key={b} className="history-distance-item">
                    Beacon {b}: {effD.toFixed(2)} m
                  </div>
                );
              })}
            </div>
          </li>
        ))}
      </ul>
    </InfoCard>
  );
};

// -------------------- MOBILE SIDEBAR --------------------
interface MobileSidebarProps {
  data: ESPData | null;
  history: ESPData[];
  isOpen: boolean;
  onClose: () => void;
}

const MobileSidebar: React.FC<MobileSidebarProps> = ({ data, history, isOpen, onClose }) => {
  return (
    <div className={`mobile-sidebar ${isOpen ? 'open' : ''}`}>
      <div className="mobile-sidebar-header">
        <h3>Tracking Info</h3>
        <button className="close-btn" onClick={onClose}>✕</button>
      </div>
      <div className="mobile-sidebar-content">
        <PositionInfo position={data?.position || null} />
        <EnvironmentInfo environment={data?.environment || null} isMobile={true} />
        <FusionInfo fusion={data?.fusion || null} isMobile={true} />
        <BeaconList beacons={data?.wifi || null} type="WiFi" isMobile={true} />
        <BeaconList beacons={data?.ble || null} type="BLE" isMobile={true} />
        <History history={history} isMobile={true} />
      </div>
    </div>
  );
};

// -------------------- ENVIRONMENT INFO COMPONENT --------------------
const EnvironmentInfo: React.FC<{ environment: Environment | null; isMobile?: boolean }> = ({ 
  environment, 
  isMobile = false 
}) => {
  if (!environment) return null;
  
  return (
    <InfoCard title="Environment" isMobile={isMobile}>
      <div className="environment-data">
        <div className="environment-item">
          <span className="env-label">Temperature:</span>
          <span className="env-value temperature">
            {environment.temperature.toFixed(1)}°C
          </span>
        </div>
        <div className="environment-item">
          <span className="env-label">Humidity:</span>
          <span className="env-value humidity">
            {environment.humidity.toFixed(1)}%
          </span>
        </div>
      </div>
    </InfoCard>
  );
};

// -------------------- ADAPTIVE SIDEBAR COMPONENTS --------------------
interface InfoCardProps {
  title: string;
  isMobile?: boolean;
  children: React.ReactNode;
}

const InfoCard: React.FC<InfoCardProps> = ({ title, children, isMobile = false }) => (
  <div className={`info-card ${isMobile ? 'mobile' : ''}`}>
    <h3>{title}</h3>
    {children}
  </div>
);

const PositionInfo: React.FC<{ position: Position | null; isMobile?: boolean }> = ({ position, isMobile = false }) => {
  if (!position) return null;
  return (
    <InfoCard title="Current Position" isMobile={isMobile}>
      <div className="position-data">
        <div className="coord-line">
          <span className="label">X:</span>
          <span className="value">{position.x.toFixed(2)} m</span>
        </div>
        <div className="coord-line">
          <span className="label">Y:</span>
          <span className="value">{position.y.toFixed(2)} m</span>
        </div>
        <div className={`coord-line accuracy ${position.accuracy > 0.4 ? 'low' : ''}`}>
          <span className="label">Accuracy:</span>
          <span className="value">{position.accuracy.toFixed(2)} m</span>
        </div>
      </div>
    </InfoCard>
  );
};

const FusionInfo: React.FC<{ fusion: Fusion | null; isMobile?: boolean }> = ({ fusion, isMobile = false }) => {
  if (!fusion) return null;
  return (
    <InfoCard title="Fusion Weights" isMobile={isMobile}>
      <div className="fusion-bars">
        <div className="fusion-bar">
          <span className="fusion-label">WiFi: {Math.round(fusion.wifi_weight * 100)}%</span>
          <div className="bar-container">
            <div 
              className="bar wifi" 
              style={{ width: `${fusion.wifi_weight * 100}%` }} 
            />
          </div>
        </div>
        <div className="fusion-bar">
          <span className="fusion-label">BLE: {Math.round(fusion.ble_weight * 100)}%</span>
          <div className="bar-container">
            <div 
              className="bar ble" 
              style={{ width: `${fusion.ble_weight * 100}%` }} 
            />
          </div>
        </div>
      </div>
    </InfoCard>
  );
};

const BeaconList: React.FC<{ 
  beacons: Record<string, BeaconData> | null; 
  type: string;
  isMobile?: boolean;
}> = ({ beacons, type, isMobile = false }) => {
  if (!beacons) return null;
  
  return (
    <InfoCard title={`${type} Beacons`} isMobile={isMobile}>
      <div className={`beacon-table-container ${isMobile ? 'mobile' : ''}`}>
        <table className={isMobile ? 'mobile-table' : ''}>
          <thead>
            <tr>
              <th>Beacon</th>
              <th>RSSI</th>
              <th>Dist</th>
              <th>Status</th>
            </tr>
          </thead>
          <tbody>
            {Object.entries(beacons).map(([name, b]) => (
              <tr key={name} className={!b.found ? "not-found" : ""}>
                <td className="beacon-name">{isMobile ? name.replace('beacon', '') : name}</td>
                <td>{b.rssi}</td>
                <td>{b.distance.toFixed(1)}m</td>
                <td className="status-cell">
                  {b.found ? (
                    <span className="status-found">✓</span>
                  ) : (
                    <span className="status-lost">✗</span>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </InfoCard>
  );
};

// -------------------- ADAPTIVE APP --------------------
export default function App() {
  const { data, connected } = useWebSocket("ws://localhost:8080/ws");
  const { isMobile, isTablet } = useResponsive();
  const [sidebarOpen, setSidebarOpen] = useState(false);
  const [history, setHistory] = useState<ESPData[]>([]);
  const MOVEMENT_THRESHOLD = 0.075; // meters (adjusted for 1.5x1.5 room)
  const MAX_HISTORY = 50;

  // Update history on significant movement
  useEffect(() => {
    if (!data) return;

    setHistory((prev) => {
      if (prev.length === 0) {
        return [data];
      }

      const last = prev[0];
      const dx = data.position.x - last.position.x;
      const dy = data.position.y - last.position.y;
      const dist = Math.sqrt(dx * dx + dy * dy);

      let newHist = prev;
      if (dist > MOVEMENT_THRESHOLD) {
        newHist = [data, ...prev];
      }

      return newHist.slice(0, MAX_HISTORY);
    });
  }, [data]);

  // Закрываем сайдбар при переходе на десктоп
  useEffect(() => {
    if (!isMobile) {
      setSidebarOpen(false);
    }
  }, [isMobile]);

  return (
    <div className="app">
      <header className="header">
        <div className="header-content">
          <div className="header-main">
            <h1>Indoor Positioning System</h1>
            <div className={`status ${connected ? "connected" : "disconnected"}`} style={{ marginRight: '8px' }}>
              {connected ? "🟢 Connected" : "🔴 Disconnected"}
            </div>
          </div>
          
          {isMobile && (
            <button 
              className="menu-toggle"
              onClick={() => setSidebarOpen(!sidebarOpen)}
            >
              ☰
            </button>
          )}
        </div>
      </header>

      <div className="main-content">
        <div className="map-section">
          <MapCanvas 
            data={data}
            isMobile={isMobile}
            isTablet={isTablet}
          />
        </div>

        {/* Десктоп сайдбар */}
        {!isMobile && (
          <aside className="sidebar">
            <PositionInfo position={data?.position || null} />
            <EnvironmentInfo environment={data?.environment || null} />
            <FusionInfo fusion={data?.fusion || null} />
            <BeaconList beacons={data?.wifi || null} type="WiFi" />
            <BeaconList beacons={data?.ble || null} type="BLE" />
            <History history={history} />
          </aside>
        )}

        {/* Мобильный сайдбар */}
        {isMobile && (
          <MobileSidebar 
            data={data}
            history={history}
            isOpen={sidebarOpen}
            onClose={() => setSidebarOpen(false)}
          />
        )}

        {/* Overlay для мобильного сайдбара */}
        {isMobile && sidebarOpen && (
          <div 
            className="sidebar-overlay"
            onClick={() => setSidebarOpen(false)}
          />
        )}
      </div>

      <footer className="footer">
        <div className="footer-content">
          <div className="legend">
            <span className="legend-item">
              <span className="color-dot object-dot"></span>
              Receiver
            </span>
            <span className="legend-item">
              <span className="color-dot wifi-dot"></span>
              WiFi
            </span>
            <span className="legend-item">
              <span className="color-dot ble-dot"></span>
              BLE
            </span>
            {!isMobile && (
              <span className="legend-item">
                <span className="color-dot cv-dot"></span>
                CV
              </span>
            )}
          </div>
          <div className="scale-info">
            Room: 1.5m x 1.5m | Grid: 10cm
          </div>
        </div>
      </footer>
    </div>
  );
}