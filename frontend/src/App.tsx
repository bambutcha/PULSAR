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

interface ESPData {
  timestamp: number;
  position: Position;
  wifi: Record<string, BeaconData>;
  ble: Record<string, BeaconData>;
  fusion: Fusion;
  cv?: Position;
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

// -------------------- WEBSOCKET HOOK --------------------
function useWebSocket(url: string) {
  const [data, setData] = useState<ESPData | null>(null);
  const [connected, setConnected] = useState(false);
  const wsRef = useRef<WebSocket | null>(null);

  useEffect(() => {
    let ws: WebSocket;

    function connect() {
      ws = new WebSocket(url);
      wsRef.current = ws;

      ws.onopen = () => setConnected(true);
      ws.onmessage = (event) => {
        try {
          const json: ESPData = JSON.parse(event.data);
          setData(json);
        } catch (err) {
          console.error("Invalid JSON", err);
        }
      };
      ws.onclose = () => {
        setConnected(false);
        setTimeout(connect, 2000);
      };
      ws.onerror = (err) => {
        console.error("WebSocket error", err);
        ws.close();
      };
    }

    connect();
    return () => ws.close();
  }, [url]);

  return { data, connected };
}

// -------------------- ADAPTIVE MAP CANVAS --------------------
const getScale = (isMobile: boolean, isTablet: boolean): number => {
  if (isMobile) return 30;  // Меньше масштаб на мобильных
  if (isTablet) return 40;  // Средний масштаб на планшетах
  return 50; // Полный масштаб на десктопе
};

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

    const scale = getScale(isMobile, isTablet);

    function draw() {
      if (!ctx) return;

      // Линейная интерполяция
      if (data) {
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

      // Сетка (только если достаточно места)
      if (!isMobile || dimensions.width > 300) {
        ctx.strokeStyle = "#333";
        for (let x = 0; x <= dimensions.width; x += scale) {
          ctx.beginPath();
          ctx.moveTo(x, 0);
          ctx.lineTo(x, dimensions.height);
          ctx.stroke();
        }
        for (let y = 0; y <= dimensions.height; y += scale) {
          ctx.beginPath();
          ctx.moveTo(0, y);
          ctx.lineTo(dimensions.width, y);
          ctx.stroke();
        }
      }

      if (!data) return;

      // WiFi маяки (адаптивное расположение)
      Object.entries(data.wifi).forEach(([name, beacon], i) => {
        const bx = (i + 1) * scale * (isMobile ? 1.2 : 1.5);
        const by = isMobile ? 30 : 50;
        ctx.fillStyle = "gold";
        ctx.strokeStyle = "gold";
        ctx.beginPath();
        ctx.arc(bx, by, isMobile ? 4 : 6, 0, 2 * Math.PI);
        ctx.fill();

        // Радиус сигнала (только если не мобильный или большой экран)
        if (!isMobile || dimensions.width > 400) {
          ctx.globalAlpha = 0.2;
          ctx.beginPath();
          ctx.arc(bx, by, beacon.distance * scale, 0, 2 * Math.PI);
          ctx.stroke();
          ctx.globalAlpha = 1;
        }
      });

      // BLE маяки
      Object.entries(data.ble).forEach(([name, beacon], i) => {
        const bx = (i + 1) * scale * (isMobile ? 1.2 : 1.5);
        const by = isMobile ? dimensions.height - 30 : 150;
        ctx.fillStyle = "deepskyblue";
        ctx.strokeStyle = "deepskyblue";
        ctx.beginPath();
        ctx.arc(bx, by, isMobile ? 4 : 6, 0, 2 * Math.PI);
        ctx.fill();

        if (!isMobile || dimensions.width > 400) {
          ctx.globalAlpha = 0.2;
          ctx.beginPath();
          ctx.arc(bx, by, beacon.distance * scale, 0, 2 * Math.PI);
          ctx.stroke();
          ctx.globalAlpha = 1;
        }
      });

      // История движения
      if (showHistory && positionsRef.current.length > 1) {
        ctx.strokeStyle = "rgba(0,255,255,0.5)";
        ctx.lineWidth = isMobile ? 1.5 : 2;
        ctx.beginPath();
        positionsRef.current.forEach((pos, idx) => {
          const x = pos.x * scale;
          const y = pos.y * scale;
          if (idx === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        });
        ctx.stroke();
      }

      // Текущий объект
      const posX = currentPosRef.current.x * scale;
      const posY = currentPosRef.current.y * scale;

      ctx.fillStyle = currentPosRef.current.accuracy > 2 ? "red" : "lime";
      ctx.beginPath();
      ctx.arc(posX, posY, isMobile ? 8 : 10, 0, 2 * Math.PI);
      ctx.fill();

      // Accuracy circle
      ctx.fillStyle = currentPosRef.current.accuracy > 2 ? "rgba(255,0,0,0.2)" : "rgba(0,255,0,0.2)";
      ctx.beginPath();
      ctx.arc(posX, posY, currentPosRef.current.accuracy * scale, 0, 2 * Math.PI);
      ctx.fill();

      // CV точка
      if (data.cv) {
        ctx.fillStyle = "red";
        ctx.beginPath();
        ctx.arc(data.cv.x * scale, data.cv.y * scale, isMobile ? 4 : 6, 0, 2 * Math.PI);
        ctx.fill();
      }

      // Подписи координат (только на больших экранах)
      if (!isMobile) {
        ctx.fillStyle = "#fff";
        ctx.font = "12px Arial";
        ctx.fillText(
          `(${currentPosRef.current.x.toFixed(1)}, ${currentPosRef.current.y.toFixed(1)})`,
          posX + 15,
          posY - 10
        );
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

// -------------------- MOBILE SIDEBAR --------------------
interface MobileSidebarProps {
  data: ESPData | null;
  isOpen: boolean;
  onClose: () => void;
}

const MobileSidebar: React.FC<MobileSidebarProps> = ({ data, isOpen, onClose }) => {
  return (
    <div className={`mobile-sidebar ${isOpen ? 'open' : ''}`}>
      <div className="mobile-sidebar-header">
        <h3>Tracking Info</h3>
        <button className="close-btn" onClick={onClose}>✕</button>
      </div>
      <div className="mobile-sidebar-content">
        <PositionInfo position={data?.position || null} />
        <FusionInfo fusion={data?.fusion || null} />
        <BeaconList beacons={data?.wifi || null} type="WiFi" isMobile={true} />
        <BeaconList beacons={data?.ble || null} type="BLE" isMobile={true} />
      </div>
    </div>
  );
};

// -------------------- ADAPTIVE SIDEBAR COMPONENTS --------------------
const InfoCard: React.FC<{ title: string; isMobile?: boolean }> = ({ title, children, isMobile = false }) => (
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
        <div className={`coord-line accuracy ${position.accuracy > 2 ? 'low' : ''}`}>
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
            <div className={`status ${connected ? "connected" : "disconnected"}`}>
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
            <FusionInfo fusion={data?.fusion || null} />
            <BeaconList beacons={data?.wifi || null} type="WiFi" />
            <BeaconList beacons={data?.ble || null} type="BLE" />
          </aside>
        )}

        {/* Мобильный сайдбар */}
        {isMobile && (
          <MobileSidebar 
            data={data}
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
              Object
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
            Scale: 1m = {getScale(isMobile, isTablet)}px
          </div>
        </div>
      </footer>
    </div>
  );
}